/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Bluetooth LE sensor implementation - Arduino ESP32 BLE
 * 2025 @ OpenSprinklerShop
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

#include "sensor_ble.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "sensor_payload_decoder.h"
#include <vector>

extern OpenSprinkler os;

#if defined(ESP32)

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// BLE scan instance
static bool ble_initialized = false;
static BLEScan* pBLEScan = nullptr;
static uint32_t scan_end_time = 0;
static bool scanning_active = false;

// Discovered devices storage (dynamically allocated)
static std::vector<BLEDeviceInfo> discovered_ble_devices;

/**
 * @brief Callback class for BLE scan results
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        DEBUG_PRINT("BLE Device found: ");
        DEBUG_PRINTLN(advertisedDevice.getAddress().toString().c_str());
        
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
        } else {
            // Add new device
            BLEDeviceInfo new_dev;
            memcpy(new_dev.address, addr_bytes, 6);
            
            // Get device name
            if (advertisedDevice.haveName()) {
                strncpy(new_dev.name, advertisedDevice.getName().c_str(), sizeof(new_dev.name) - 1);
                new_dev.name[sizeof(new_dev.name) - 1] = 0;
            } else {
                strcpy(new_dev.name, "Unknown");
            }
            
            new_dev.rssi = advertisedDevice.getRSSI();
            new_dev.is_new = true;
            new_dev.last_seen = millis();
            
            discovered_ble_devices.push_back(new_dev);
            
            DEBUG_PRINT("New BLE device added: ");
            DEBUG_PRINTLN(new_dev.name);
        }
    }
};

/**
 * @brief Initialize BLE sensor subsystem
 */
void sensor_ble_init() {
    if (ble_initialized) {
        DEBUG_PRINTLN("BLE already initialized");
        return;
    }
    
    DEBUG_PRINTLN("Initializing BLE scanner...");
    
    // Initialize BLE
    BLEDevice::init("OpenSprinkler");
    
    // Create BLE scan instance
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true); // Active scan uses more power but gets more info
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    
    DEBUG_PRINTLN("BLE scanner initialized");
    
    ble_initialized = true;
}

/**
 * @brief Start BLE scanning
 */
void sensor_ble_start_scan(uint16_t duration) {
    if (!ble_initialized) {
        sensor_ble_init();
    }
    
    if (scanning_active) {
        DEBUG_PRINTLN("BLE scan already in progress");
        return;
    }
    
    DEBUG_PRINT("Starting BLE scan for ");
    DEBUG_PRINT(duration);
    DEBUG_PRINTLN(" seconds...");
    
    // Clear old scan results
    pBLEScan->clearResults();
    
    // Start scan (non-blocking)
    pBLEScan->start(duration, false);
    scanning_active = true;
    scan_end_time = millis() + (duration * 1000);
}

/**
 * @brief BLE maintenance loop
 */
void sensor_ble_loop() {
    if (!ble_initialized) {
        return;
    }
    
    // Check if scan has completed
    if (scanning_active && millis() >= scan_end_time) {
        if (pBLEScan->isScanning()) {
            pBLEScan->stop();
        }
        scanning_active = false;
        DEBUG_PRINT("BLE scan completed. Found ");
        DEBUG_PRINT(discovered_ble_devices.size());
        DEBUG_PRINTLN(" devices.");
    }
    
    // Remove stale devices (not seen in 5 minutes)
    uint32_t now = millis();
    discovered_ble_devices.erase(
        std::remove_if(discovered_ble_devices.begin(), discovered_ble_devices.end(),
            [now](const BLEDeviceInfo& dev) {
                return (now - dev.last_seen > 300000);  // 5 minutes
            }),
        discovered_ble_devices.end()
    );
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
 */
int BLESensor::read(unsigned long time) {
    if (!flags.enable) return HTTP_RQT_NOT_RECEIVED;
    
    if (!ble_initialized) {
        sensor_ble_init();
    }
    
    // Parse sensor configuration from JSON
    // Expected format in name: MAC address (e.g., "AA:BB:CC:DD:EE:FF")
    // Expected format in userdef_unit: Characteristic UUID (optionally with format: "UUID|format_id")
    //   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
    
    const char* mac_address = name; // Use name field for MAC
    char characteristic_uuid[128] = {0};
    PayloadFormat format = FORMAT_TEMP_001; // Default auto-detect
    
    // Parse userdef_unit field: "UUID" or "UUID|format"
    if (userdef_unit && strlen(userdef_unit) > 0) {
        strncpy(characteristic_uuid, userdef_unit, sizeof(characteristic_uuid) - 1);
        
        // Check for format specifier after |
        char* pipe = strchr(characteristic_uuid, '|');
        if (pipe) {
            *pipe = '\0'; // Terminate UUID
            pipe++; // Move to format part
            int fmt = atoi(pipe);
            if (fmt >= 0 && fmt <= 30) {
                format = (PayloadFormat)fmt;
            }
        }
    }
    
    if (!mac_address || strlen(mac_address) == 0) {
        DEBUG_PRINTLN("ERROR: BLE MAC address not configured in name field");
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (strlen(characteristic_uuid) == 0) {
        DEBUG_PRINTLN("ERROR: BLE characteristic UUID not configured in userdef_unit field");
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    DEBUG_PRINT("Reading BLE sensor: ");
    DEBUG_PRINT(mac_address);
    DEBUG_PRINT(" characteristic: ");
    DEBUG_PRINTLN(characteristic_uuid);
    
    // Parse MAC address
    BLEAddress bleAddress(mac_address);
    
    // Connect to BLE device
    BLEClient* pClient = BLEDevice::createClient();
    if (!pClient->connect(bleAddress)) {
        DEBUG_PRINTLN("Failed to connect to BLE device");
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    DEBUG_PRINTLN("Connected to BLE device");
    
    // Get remote service (use generic service if not specified)
    BLERemoteService* pRemoteService = pClient->getService(BLEUUID((uint16_t)0x181A)); // Environmental Sensing
    if (pRemoteService == nullptr) {
        DEBUG_PRINTLN("Failed to find service, trying primary service...");
        std::map<std::string, BLERemoteService*>* services = pClient->getServices();
        if (services->size() > 0) {
            pRemoteService = services->begin()->second;
        }
    }
    
    if (pRemoteService == nullptr) {
        DEBUG_PRINTLN("Failed to find any service");
        pClient->disconnect();
        delete pClient;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Get characteristic
    BLERemoteCharacteristic* pRemoteCharacteristic = pRemoteService->getCharacteristic(BLEUUID(characteristic_uuid));
    if (pRemoteCharacteristic == nullptr) {
        DEBUG_PRINTLN("Failed to find characteristic");
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
        DEBUG_PRINTLN("No data received from BLE device");
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Parse payload using decoder
    double parsed_value = 0.0;
    
    if (!decode_payload((const uint8_t*)value_str.c_str(), value_str.length(), format, &parsed_value)) {
        DEBUG_PRINTLN("Failed to decode BLE payload");
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
    repeat_read = 1;
    
    DEBUG_PRINT("BLE sensor value: ");
    DEBUG_PRINTLN(parsed_value);
    
    return HTTP_RQT_SUCCESS;
}

/**
 * @brief Get measurement unit for BLE sensor
 */
unsigned char BLESensor::getUnitId() const {
    // Unit is configured via sensor JSON userdef field
    // Return UNIT_NONE here, actual unit will be set during sensor configuration
    return UNIT_NONE;
}

#endif // ESP32
