/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Bluetooth LE sensor implementation - Arduino ESP32 BLE
 * 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

 #if defined(ESP32) && defined(OS_ENABLE_BLE)

#include "sensor_ble.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "sensor_payload_decoder.h"
#include <vector>
#include <ctype.h>

extern OpenSprinkler os;

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// BLE scan instance
static bool ble_initialized = false;
static BLEScan* pBLEScan = nullptr;
static uint32_t scan_end_time = 0;
static bool scanning_active = false;

// Auto-stop timer for discovery mode (to free WiFi RF resources)
static unsigned long ble_auto_stop_time = 0;
static bool ble_discovery_mode = false;

// Discovered devices storage (dynamically allocated)
static std::vector<BLEDeviceInfo> discovered_ble_devices;

static void ble_sanitize_json_string(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    dst[0] = 0;

    if (!src || *src == 0) {
        strncpy(dst, "Unknown", dst_len - 1);
        dst[dst_len - 1] = 0;
        return;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != 0 && di < dst_len - 1; si++) {
        unsigned char c = (unsigned char)src[si];

        // Drop ASCII control chars (incl. \n/\r/\t) and DEL
        if (c < 0x20 || c == 0x7F) {
            continue;
        }

        // Keep it ASCII-printable only to avoid invalid UTF-8 in JSON consumers
        if (c > 0x7E) {
            continue;
        }

        // Prevent breaking JSON if downstream formatter does not escape
        if (c == '"' || c == '\\') {
            dst[di++] = '\'';
            continue;
        }

        dst[di++] = (char)c;
    }
    dst[di] = 0;

    // If everything got stripped, fall back
    if (dst[0] == 0) {
        strncpy(dst, "Unknown", dst_len - 1);
        dst[dst_len - 1] = 0;
    }
}

static void ble_parse_uuid_and_format_legacy(const char* unit_str, char* out_uuid, size_t out_uuid_len, uint8_t* out_fmt) {
    if (out_uuid && out_uuid_len) {
        out_uuid[0] = 0;
    }
    if (out_fmt) {
        *out_fmt = (uint8_t)FORMAT_TEMP_001;
    }
    if (!unit_str || !unit_str[0] || !out_uuid || out_uuid_len == 0) {
        return;
    }

    char tmp[64] = {0};
    strncpy(tmp, unit_str, sizeof(tmp) - 1);

    char* pipe = strchr(tmp, '|');
    if (pipe) {
        *pipe = 0;
        pipe++;
        int fmt = atoi(pipe);
        if (fmt >= 0 && fmt <= 30 && out_fmt) {
            *out_fmt = (uint8_t)fmt;
        }
    }

    // copy UUID part
    strncpy(out_uuid, tmp, out_uuid_len - 1);
    out_uuid[out_uuid_len - 1] = 0;
}

void BLESensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    SensorBase::fromJson(obj);

    // Optional explicit BLE config (preferred)
    if (obj.containsKey("char_uuid")) {
        const char* u = obj["char_uuid"].as<const char*>();
        if (u) strncpy(characteristic_uuid_cfg, u, sizeof(characteristic_uuid_cfg) - 1);
    } else if (obj.containsKey("uuid")) {
        const char* u = obj["uuid"].as<const char*>();
        if (u) strncpy(characteristic_uuid_cfg, u, sizeof(characteristic_uuid_cfg) - 1);
    }

    if (obj.containsKey("format")) {
        int fmt = obj["format"].as<int>();
        if (fmt >= 0 && fmt <= 30) {
            payload_format_cfg = (uint8_t)fmt;
        }
    }

    // Backward-compat: parse legacy `unit` field (userdef_unit) if no explicit uuid provided
    if (!characteristic_uuid_cfg[0] && userdef_unit[0]) {
        uint8_t fmt = (uint8_t)FORMAT_TEMP_001;
        ble_parse_uuid_and_format_legacy(userdef_unit, characteristic_uuid_cfg, sizeof(characteristic_uuid_cfg), &fmt);
        payload_format_cfg = fmt;
    }
}

void BLESensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    if (!obj) return;

    // BLE-specific fields
    if (characteristic_uuid_cfg[0]) obj["char_uuid"] = characteristic_uuid_cfg;
    if (payload_format_cfg != (uint8_t)FORMAT_TEMP_001) obj["format"] = payload_format_cfg;
}

static bool ble_uuid_extract_16bit(const char* uuid_in, uint16_t* out_uuid16) {
    if (!uuid_in || !out_uuid16) return false;

    // Copy until '|' or end, trim leading/trailing whitespace
    char buf[64];
    size_t n = 0;
    while (uuid_in[n] && uuid_in[n] != '|' && n < sizeof(buf) - 1) {
        buf[n] = uuid_in[n];
        n++;
    }
    buf[n] = '\0';

    // Trim left
    char* s = buf;
    while (*s && isspace((unsigned char)*s)) s++;
    // Trim right
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end[-1] = '\0';
        end--;
    }

    if (*s == '\0') return false;

    // Accept 0xNNNN
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    // Check Bluetooth base UUID form: 0000xxxx-0000-1000-8000-00805f9b34fb
    // (case-insensitive)
    const char* base_suffix = "-0000-1000-8000-00805f9b34fb";
    const size_t base_suffix_len = strlen(base_suffix);
    size_t s_len = strlen(s);
    if (s_len == 36) {
        // Must end with base suffix
        const char* suffix = s + (36 - base_suffix_len);
        bool suffix_ok = true;
        for (size_t i = 0; i < base_suffix_len; i++) {
            char a = (char)tolower((unsigned char)suffix[i]);
            char b = (char)tolower((unsigned char)base_suffix[i]);
            if (a != b) { suffix_ok = false; break; }
        }
        // Must start with 0000 + 4 hex
        if (suffix_ok &&
            s[0] == '0' && s[1] == '0' && s[2] == '0' && s[3] == '0' &&
            isxdigit((unsigned char)s[4]) && isxdigit((unsigned char)s[5]) &&
            isxdigit((unsigned char)s[6]) && isxdigit((unsigned char)s[7]) &&
            s[8] == '-') {
            char hex4[5] = { s[4], s[5], s[6], s[7], 0 };
            *out_uuid16 = (uint16_t)strtoul(hex4, nullptr, 16);
            return true;
        }
    }

    // Accept plain 4-hex UUIDs
    if (strlen(s) == 4 &&
        isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) &&
        isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3])) {
        *out_uuid16 = (uint16_t)strtoul(s, nullptr, 16);
        return true;
    }

    return false;
}

static const char* ble_uuid16_to_name(uint16_t uuid16) {
    switch (uuid16) {
        // Common services
        case 0x1800: return "Generic Access";
        case 0x1801: return "Generic Attribute";
        case 0x180A: return "Device Information";
        case 0x180F: return "Battery Service";
        case 0x181A: return "Environmental Sensing";

        // Common characteristics
        case 0x2A00: return "Device Name";
        case 0x2A01: return "Appearance";
        case 0x2A19: return "Battery Level";
        case 0x2A24: return "Model Number String";
        case 0x2A25: return "Serial Number String";
        case 0x2A26: return "Firmware Revision String";
        case 0x2A27: return "Hardware Revision String";
        case 0x2A28: return "Software Revision String";
        case 0x2A29: return "Manufacturer Name String";
        case 0x2A6D: return "Pressure";
        case 0x2A6E: return "Temperature";
        case 0x2A6F: return "Humidity";
        case 0x2A73: return "Barometric Pressure Trend";

        default: return "Unknown";
    }
}

const char* ble_uuid_to_name(const char* uuid) {
    uint16_t uuid16 = 0;
    if (ble_uuid_extract_16bit(uuid, &uuid16)) {
        return ble_uuid16_to_name(uuid16);
    }
    return "Unknown";
}

/**
 * @brief Callback class for BLE scan results
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        DEBUG_PRINT("BLE Device found: ");
        DEBUG_PRINTLN(advertisedDevice.getAddress().toString().c_str());

        if (advertisedDevice.haveServiceUUID()) {
            String svc = advertisedDevice.getServiceUUID().toString();
            DEBUG_PRINT("BLE Service UUID: ");
            DEBUG_PRINT(svc.c_str());
            DEBUG_PRINT(" (");
            DEBUG_PRINT(ble_uuid_to_name(svc.c_str()));
            DEBUG_PRINTLN(")");
        }
        
        // Get device address (format: "aa:bb:cc:dd:ee:ff")
        String addr_str = advertisedDevice.getAddress().toString();
        uint8_t addr_bytes[6];
        
        // Parse address string to bytes
        if (sscanf(addr_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &addr_bytes[0], &addr_bytes[1], &addr_bytes[2],
                   &addr_bytes[3], &addr_bytes[4], &addr_bytes[5]) != 6) {
            return; // Invalid address format
        }
        
        // Check if device already exists in list
        bool already_exists = false;
        BLEDeviceInfo* existing_device = nullptr;
        for (auto& dev : discovered_ble_devices) {
            if (memcmp(dev.address, addr_bytes, 6) == 0) {
                already_exists = true;
                existing_device = &dev;
                break;
            }
        }
        
        if (already_exists) {
            // Update existing device
            existing_device->rssi = advertisedDevice.getRSSI();
            existing_device->last_seen = millis();
            existing_device->is_new = true; // Mark as seen again

            if (advertisedDevice.haveName()) {
                ble_sanitize_json_string(existing_device->name, sizeof(existing_device->name),
                                         advertisedDevice.getName().c_str());
            }

            if (advertisedDevice.haveServiceUUID()) {
                String svc = advertisedDevice.getServiceUUID().toString();
                strncpy(existing_device->service_uuid, svc.c_str(), sizeof(existing_device->service_uuid) - 1);
                existing_device->service_uuid[sizeof(existing_device->service_uuid) - 1] = 0;
            }
        } else {
            // Add new device
            BLEDeviceInfo new_dev;
            memcpy(new_dev.address, addr_bytes, 6);
            
            // Get device name
            if (advertisedDevice.haveName()) {
                ble_sanitize_json_string(new_dev.name, sizeof(new_dev.name), advertisedDevice.getName().c_str());
            } else {
                strcpy(new_dev.name, "Unknown");
            }
            
            new_dev.rssi = advertisedDevice.getRSSI();
            new_dev.is_new = true;
            new_dev.last_seen = millis();

            new_dev.service_uuid[0] = 0;
            if (advertisedDevice.haveServiceUUID()) {
                String svc = advertisedDevice.getServiceUUID().toString();
                strncpy(new_dev.service_uuid, svc.c_str(), sizeof(new_dev.service_uuid) - 1);
                new_dev.service_uuid[sizeof(new_dev.service_uuid) - 1] = 0;
            }
            
            discovered_ble_devices.push_back(new_dev);

            DEBUG_PRINT("New BLE device added: ");
            DEBUG_PRINTLN(new_dev.name);
        }
    }
};

static MyAdvertisedDeviceCallbacks ble_scan_callbacks;

/**
 * @brief Initialize BLE sensor subsystem
 */
void sensor_ble_init() {
    if (ble_initialized) {
        return;
    }
    
    // Note: WiFi mode check removed from init - will be checked in read() instead
    // This prevents exceptions when sensor_api_init() is called before WiFi is started

    DEBUG_PRINTLN("Initializing BLE...");
    
    // Initialize BLE
    BLEDevice::init("OpenSprinkler");

    DEBUG_PRINTLN("BLE initialized");
    
    ble_initialized = true;
}

/**
 * @brief Stop BLE subsystem (frees RF resources)
 */
void sensor_ble_stop() {
    if (!ble_initialized) {
        return;
    }
    DEBUG_PRINTLN("Stopping BLE to free RF resources...");
    
    // Stop scanning if active
    if (scanning_active && pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        scanning_active = false;
    }
    
    // Deinitialize BLE
    BLEDevice::deinit(false);
    
    ble_initialized = false;
    pBLEScan = nullptr;

    DEBUG_PRINTLN("BLE stopped - RF resources freed for WiFi");
}

/**
 * @brief Start BLE scanning
 */
void sensor_ble_start_scan(uint16_t duration) {
    if (!ble_initialized) {
        sensor_ble_init();
        if (!ble_initialized) {
            return;
        }
    }

    if (!pBLEScan) {
        // Configure scan only when discovery is requested.
        pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks);
        pBLEScan->setActiveScan(true); // Active scan uses more power but gets more info
        pBLEScan->setInterval(100);
        pBLEScan->setWindow(99);
    }
    
    if (scanning_active) {
        return;
    }
    
    // Limit discovery time to max 10 seconds to minimize WiFi interference
    const uint16_t MAX_DISCOVERY_TIME = 10;
    uint16_t actual_duration = (duration > MAX_DISCOVERY_TIME) ? MAX_DISCOVERY_TIME : duration;

    DEBUG_PRINT("Starting BLE scan for ");
    DEBUG_PRINT(actual_duration);
    DEBUG_PRINTLN(" seconds (will auto-stop to free WiFi RF)...");
    
    // Clear old scan results
    pBLEScan->clearResults();
    
    // Start scan (non-blocking)
    pBLEScan->start(actual_duration, false);
    scanning_active = true;
    scan_end_time = millis() + (actual_duration * 1000);
    
    // Set auto-stop timer
    ble_discovery_mode = true;
    ble_auto_stop_time = millis() + (actual_duration * 1000UL);
}

/**
 * @brief BLE maintenance loop
 */
void sensor_ble_loop() {
    if (!ble_initialized) {
        return;
    }
    
    uint32_t now = millis();
    
    // Check if discovery mode auto-stop timer expired
    if (ble_discovery_mode && ble_auto_stop_time > 0) {
        if (now >= ble_auto_stop_time) {
            DEBUG_PRINTLN("BLE discovery time expired - stopping BLE to free WiFi RF");
            sensor_ble_stop();
            ble_discovery_mode = false;
            ble_auto_stop_time = 0;
            scanning_active = false;
            return; // Exit early since we just stopped
        }
    }
    
    // Check if scan has completed
    if (scanning_active && now >= scan_end_time) {
        if (pBLEScan->isScanning()) {
            pBLEScan->stop();
        }
        scanning_active = false;
        DEBUG_PRINT("BLE scan completed. Found ");
        DEBUG_PRINT(discovered_ble_devices.size());
        DEBUG_PRINTLN(" devices.");
    }
    
    // Remove stale devices (not seen in 5 minutes)
    discovered_ble_devices.erase(
        std::remove_if(discovered_ble_devices.begin(), discovered_ble_devices.end(),
            [now](const BLEDeviceInfo& dev) {
                return (now - dev.last_seen > 300000);  // 5 minutes
            }),
        discovered_ble_devices.end()
    );
}

bool sensor_ble_is_active() {
    return ble_initialized;
}

/**
 * @brief Get list of discovered BLE devices
 */
int sensor_ble_get_discovered_devices(BLEDeviceInfo* devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;
    
    int count = (discovered_ble_devices.size() < (size_t)max_devices) ? discovered_ble_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &discovered_ble_devices[i], sizeof(BLEDeviceInfo));
    }
    return count;
}

/**
 * @brief Clear new device flags
 */
void sensor_ble_clear_new_device_flags() {
    for (auto& dev : discovered_ble_devices) {
        dev.is_new = false;
    }
}

/**
 * @brief Read value from BLE sensor
 * Power-saving mode: BLE is turned on/off dynamically
 * First call (repeat_read == 0): Turn on BLE, read data, set repeat_read = 1
 * Second call (repeat_read == 1): Return data, set repeat_read = 0, turn off BLE
 */
int BLESensor::read(unsigned long time) {
    if (!flags.enable) return HTTP_RQT_NOT_RECEIVED;
    
    // CRITICAL: Do NOT use BLE in WiFi SOFTAP mode (RF coexistence conflict)
    // Exception: Ethernet mode (no WiFi RF usage)
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (repeat_read == 0) {
        // First call: Turn on BLE and read data
        DEBUG_PRINT("BLE sensor read (turn on): ");
        DEBUG_PRINTLN(name);
        
        // Start BLE if not running
        if (!ble_initialized) {
            sensor_ble_init();
            if (!ble_initialized) {
                flags.data_ok = false;
                return HTTP_RQT_NOT_RECEIVED;
            }
        }
    
    // Parse sensor configuration from JSON
    // Expected format in name: MAC address (e.g., "AA:BB:CC:DD:EE:FF")
    // Expected format in userdef_unit: Characteristic UUID (optionally with format: "UUID|format_id")
    //   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
    
    const char* mac_address = name; // Use name field for MAC
    char characteristic_uuid[128] = {0};
    PayloadFormat format = (PayloadFormat)payload_format_cfg;

    // Preferred: explicit BLE config fields
    if (characteristic_uuid_cfg[0]) {
        strncpy(characteristic_uuid, characteristic_uuid_cfg, sizeof(characteristic_uuid) - 1);
    } else if (userdef_unit && strlen(userdef_unit) > 0) {
        // Backward-compat: parse legacy userdef_unit field: "UUID" or "UUID|format"
        uint8_t fmt = (uint8_t)FORMAT_TEMP_001;
        ble_parse_uuid_and_format_legacy(userdef_unit, characteristic_uuid, sizeof(characteristic_uuid), &fmt);
        format = (PayloadFormat)fmt;
    }
    
    if (!mac_address || strlen(mac_address) == 0) {
        DEBUG_PRINTLN(F("ERROR: BLE MAC address not configured in name field"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (strlen(characteristic_uuid) == 0) {
        DEBUG_PRINTLN(F("ERROR: BLE characteristic UUID not configured (char_uuid/uuid or legacy unit)"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    DEBUG_PRINT("Reading BLE sensor: ");
    DEBUG_PRINT(mac_address);
    DEBUG_PRINT(" characteristic: ");
    DEBUG_PRINT(characteristic_uuid);
    DEBUG_PRINT(" (");
    DEBUG_PRINT(ble_uuid_to_name(characteristic_uuid));
    DEBUG_PRINTLN(")");
    
    // Parse MAC address
    BLEAddress bleAddress(mac_address);
    
    // Connect to BLE device
    BLEClient* pClient = BLEDevice::createClient();
    if (!pClient->connect(bleAddress)) {
        DEBUG_PRINTLN(F("Failed to connect to BLE device"));
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    DEBUG_PRINTLN(F("Connected to BLE device"));
    
    // Get remote service (use generic service if not specified)
    BLERemoteService* pRemoteService = pClient->getService(BLEUUID((uint16_t)0x181A)); // Environmental Sensing
    if (pRemoteService == nullptr) {
        DEBUG_PRINTLN(F("Failed to find service, trying primary service..."));
        std::map<std::string, BLERemoteService*>* services = pClient->getServices();
        if (services->size() > 0) {
            pRemoteService = services->begin()->second;
        }
    }
    
    if (pRemoteService == nullptr) {
        DEBUG_PRINTLN(F("Failed to find any service"));
        pClient->disconnect();
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Get characteristic
    BLERemoteCharacteristic* pRemoteCharacteristic = pRemoteService->getCharacteristic(BLEUUID(characteristic_uuid));
    if (pRemoteCharacteristic == nullptr) {
        DEBUG_PRINTLN(F("Failed to find characteristic"));
        pClient->disconnect();
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Read value
    if (!pRemoteCharacteristic->canRead()) {
        DEBUG_PRINTLN("Characteristic is not readable");
        pClient->disconnect();
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    String value_str = pRemoteCharacteristic->readValue().c_str();
    
    DEBUG_PRINT("Read ");
    DEBUG_PRINT(value_str.length());
    DEBUG_PRINTLN(" bytes from BLE characteristic");
    
    // Disconnect
    pClient->disconnect();
    delete pClient;
    
    if (value_str.length() == 0) {
        DEBUG_PRINTLN(F("No data received from BLE device"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Parse payload using decoder
    double parsed_value = 0.0;
    
    if (!decode_payload((const uint8_t*)value_str.c_str(), value_str.length(), format, &parsed_value)) {
        DEBUG_PRINTLN(F("Failed to decode BLE payload"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
        // Store value
        flags.data_ok = 1;
        last_data = (int32_t)(parsed_value * 100.0); // Store as integer with 0.01 precision
        last_native_data = last_data;
        last_read = time;
        
        // Reset averaging counters
        repeat_data = last_data;
        repeat_native = last_native_data;
        repeat_read = 1; // Signal that data is available
        
        DEBUG_PRINT("BLE sensor value: ");
        DEBUG_PRINTLN(parsed_value);
        
        // Don't turn off BLE yet - wait for second read() call
        return HTTP_RQT_NOT_RECEIVED; // Will return data on next call
        
    } else {
        // Second call (repeat_read == 1): Return data and turn off BLE
        DEBUG_PRINT("BLE sensor read (turn off): ");
        DEBUG_PRINTLN(name);
        
        repeat_read = 0; // Reset for next cycle
        
        // Turn off BLE to free RF resources for WiFi
        sensor_ble_stop();
        
        last_read = time;
        
        if (flags.data_ok) {
            return HTTP_RQT_SUCCESS; // Adds data to log
        } else {
            return HTTP_RQT_NOT_RECEIVED;
        }
    }
}

/**
 * @brief Get measurement unit for BLE sensor
 */
unsigned char BLESensor::getUnitId() const {
    // Unit is configured via sensor JSON assigned_unitid field
    return assigned_unitid > 0 ? assigned_unitid : UNIT_USERDEF;
}

#endif // ESP32
