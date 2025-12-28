/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Zigbee sensor implementation - Arduino Zigbee integration
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

#include "sensor_zigbee.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

extern OpenSprinkler os;

#if defined(ESP32C5) && !defined(ZIGBEE_MODE_ZCZR)
#error "Zigbee coordinator mode is not selected. Set Zigbee Mode to 'Zigbee ZCZR (coordinator/router)' in platformio.ini"
#endif

#if defined(ESP32C5)

#include <esp_partition.h>
#include <vector>
#include "esp_zigbee_core.h"
#include "Zigbee.h"

// Zigbee coordinator instance
static bool zigbee_initialized = false;

// Discovered devices storage (dynamically allocated)
static std::vector<ZigbeeDeviceInfo> discovered_devices;

// Zigbee Cluster IDs (ZCL spec)
#define ZB_ZCL_CLUSTER_ID_BASIC                     0x0000
#define ZB_ZCL_CLUSTER_ID_POWER_CONFIG              0x0001
#define ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT          0x0402
#define ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT  0x0405
#define ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE             0x0408

/**
 * @brief Callback for new Zigbee device joining the network
 */
static void zigbee_device_joined_callback(uint16_t short_addr, uint64_t ieee_addr) {
    DEBUG_PRINT("New Zigbee device joined! Short: 0x");
    Serial.print(short_addr, HEX);
    DEBUG_PRINT(", IEEE: 0x");
    Serial.println((unsigned long)(ieee_addr >> 32), HEX);
    Serial.println((unsigned long)(ieee_addr & 0xFFFFFFFF), HEX);
    
    // Add to discovered devices list if not already present
    bool already_exists = false;
    for (auto& dev : discovered_devices) {
        if (dev.ieee_addr == ieee_addr) {
            already_exists = true;
            dev.is_new = true; // Mark as new again
            break;
        }
    }
    
    if (!already_exists) {
        ZigbeeDeviceInfo new_dev;
        new_dev.ieee_addr = ieee_addr;
        new_dev.short_addr = short_addr;
        new_dev.is_new = true;
        new_dev.endpoint = 1; // Default endpoint
        new_dev.device_id = 0; // Unknown until we read it
        strcpy(new_dev.model_id, "Unknown");
        strcpy(new_dev.manufacturer, "Unknown");
        discovered_devices.push_back(new_dev);
        
        DEBUG_PRINTLN("Device added to discovered list");
    }
}

/**
 * @brief Actually start Zigbee coordinator (called after WiFi is connected)
 */
void sensor_zigbee_start() {
    if (zigbee_initialized) {
        DEBUG_PRINTLN("Zigbee already running");
        return;
    }
    
    DEBUG_PRINTLN("Starting Zigbee coordinator...");
    
    // Check if zb_storage partition exists (FAT subtype 0x81)
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("WARNING: zb_storage partition not found - Zigbee disabled"));
        DEBUG_PRINTLN(F("Please ensure partition table includes zb_storage partition with FAT subtype"));
        return;
    }
    
    DEBUG_PRINTLN("Found zb_storage partition");
    
    // Initialize Zigbee as coordinator
    Zigbee.begin(ZIGBEE_COORDINATOR);
    
    DEBUG_PRINTLN("Zigbee coordinator initialized");
    
    // Note: Custom endpoint implementation depends on Arduino-Zigbee API
    // For now, use basic coordinator mode
    
    // Start Zigbee network
    Zigbee.start();
    DEBUG_PRINTLN("Zigbee network started");
    
    
    // Open network for joining (60 seconds initially)
    Zigbee.openNetwork(60);
    DEBUG_PRINTLN("Zigbee network open for joining (60s)");
    
    zigbee_initialized = true;
}

/**
 * @brief Zigbee attribute report callback - updates sensor data
 */
void ZigbeeSensor::zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint,
                                            uint16_t cluster_id, uint16_t attr_id,
                                            int32_t value, uint8_t lqi) {
    // Find sensor by IEEE address, endpoint, cluster, and attribute
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) {
            continue;
        }
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        if (zb_sensor->device_ieee == ieee_addr &&
            zb_sensor->endpoint == endpoint &&
            zb_sensor->cluster_id == cluster_id &&
            zb_sensor->attribute_id == attr_id) {
            
            DEBUG_PRINT("Updating sensor: ");
            DEBUG_PRINTLN(sensor->name);
            
            // Update sensor data
            zb_sensor->last_data = value;
            zb_sensor->last_lqi = lqi;
            zb_sensor->flags.data_ok = true;
            zb_sensor->repeat_read = 1;  // Signal that new data is available
            
            // Battery level (cluster 0x0001, attribute 0x0021)
            if (cluster_id == 0x0001 && attr_id == 0x0021) {
                zb_sensor->last_battery = value / 2;  // Battery percentage
            }
            
            break;
        }
    }
}

uint64_t ZigbeeSensor::parseIeeeAddress(const char* ieee_str) {
    if (!ieee_str || !ieee_str[0]) return 0;
    
    // Remove "0x" prefix if present
    if (ieee_str[0] == '0' && (ieee_str[1] == 'x' || ieee_str[1] == 'X')) {
        ieee_str += 2;
    }
    
    // Parse hex string (supports both "00124B001F8E5678" and "00:12:4B:00:1F:8E:56:78")
    uint64_t addr = 0;
    for (int i = 0; ieee_str[i] != '\0'; i++) {
        char c = ieee_str[i];
        if (c == ':' || c == '-' || c == ' ') continue; // Skip separators
        
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else {
            return 0; // Invalid character
        }
        addr = (addr << 4) | nibble;
    }
    return addr;
}

const char* ZigbeeSensor::getIeeeString(char* buffer, size_t bufferSize) const {
    if (bufferSize < 19) return ""; // Need at least 18 chars + null
    snprintf(buffer, bufferSize, "0x%016llX", (unsigned long long)device_ieee);
    return buffer;
}

void ZigbeeSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    SensorBase::fromJson(obj);
    
    // Zigbee-specific fields
    if (obj.containsKey("device_ieee")) {
        const char *ieee_str = obj["device_ieee"].as<const char*>();
        if (ieee_str) {
            device_ieee = parseIeeeAddress(ieee_str);
            if (device_ieee != 0) {
                sensor_zigbee_bind_device(nr, ieee_str);
            }
        }
    }
    if (obj.containsKey("endpoint")) {
        endpoint = obj["endpoint"].as<uint8_t>();
    }
    if (obj.containsKey("cluster_id")) {
        cluster_id = obj["cluster_id"].as<uint16_t>();
    }
    if (obj.containsKey("attribute_id")) {
        attribute_id = obj["attribute_id"].as<uint16_t>();
    }
    if (obj.containsKey("poll_interval")) {
        poll_interval = obj["poll_interval"].as<uint32_t>();
    }
}

void ZigbeeSensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    
    // Zigbee-specific fields
    if (device_ieee != 0) {
        char ieee_str[20];
        obj["device_ieee"] = getIeeeString(ieee_str, sizeof(ieee_str));
    }
    if (endpoint != 1) obj["endpoint"] = endpoint;
    if (cluster_id != 0x0408) obj["cluster_id"] = cluster_id;
    if (attribute_id != 0x0000) obj["attribute_id"] = attribute_id;
    if (poll_interval != 60000) obj["poll_interval"] = poll_interval;
    if (last_battery < 100) obj["battery"] = last_battery;
    if (last_lqi > 0) obj["lqi"] = last_lqi;
}

bool ZigbeeSensor::init() {
    DEBUG_PRINTLN("ZigbeeSensor::init()");
    // Start Zigbee coordinator now
    if (!zigbee_initialized)
        sensor_zigbee_start();
    
    if (device_ieee == 0) {
        DEBUG_PRINTLN("ZigbeeSensor: No device IEEE address configured");
        return false;
    }
    
    if (!zigbee_initialized) {
        DEBUG_PRINTLN("ZigbeeSensor: Zigbee stack not initialized");
        return false;
    }
    
    char ieee_str[20];
    DEBUG_PRINT("ZigbeeSensor: Configured for device ");
    DEBUG_PRINTLN(getIeeeString(ieee_str, sizeof(ieee_str)));
    DEBUG_PRINT("  Endpoint: ");
    DEBUG_PRINTLN(endpoint);
    DEBUG_PRINT("  Cluster: 0x");
    Serial.println(cluster_id, HEX);
    DEBUG_PRINT("  Attribute: 0x");
    Serial.println(attribute_id, HEX);
    
    device_bound = true;
    return true;
}

void ZigbeeSensor::deinit() {
    if (device_bound && device_ieee != 0) {
        DEBUG_PRINTLN("ZigbeeSensor: Deinitializing device");
        device_bound = false;
    }
}

int ZigbeeSensor::read(unsigned long time) {
    if (!zigbee_initialized) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // If data was received via Zigbee callback (passive report)
    if (repeat_read > 0) {
        DEBUG_PRINT("read_sensor_zigbee: push data for ");
        DEBUG_PRINTLN(name);
        repeat_read = 0;
        return HTTP_RQT_SUCCESS; // Adds data to log
    }
    
    // Active polling: check if poll interval has elapsed
    if (poll_interval > 0 && device_ieee != 0) {
        uint32_t elapsed = time - last_poll_time;
        if (last_poll_time == 0 || elapsed >= poll_interval) {
            DEBUG_PRINT("Active polling sensor: ");
            DEBUG_PRINTLN(name);
            
            // Send read attribute request
            if (sensor_zigbee_read_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                last_poll_time = time;
                // Response will arrive via zigbee_attribute_callback
            } else {
                DEBUG_PRINTLN("Failed to send read request");
            }
        }
    }
    
    last_read = time;
    
    // Return NOT_RECEIVED - actual data comes via callback
    return HTTP_RQT_NOT_RECEIVED;
}

void sensor_zigbee_bind_device(uint nr, const char *device_ieee_str) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (device_ieee_str && device_ieee_str[0] && sensor) {
        
        DEBUG_PRINT("sensor_zigbee_bind_device: ");
        DEBUG_PRINTLN(sensor->name);
        DEBUG_PRINT("IEEE: ");
        DEBUG_PRINTLN(device_ieee_str);
        
        uint64_t ieee = ZigbeeSensor::parseIeeeAddress(device_ieee_str);
        if (ieee == 0) {
            DEBUG_PRINTLN("Invalid IEEE address");
            return;
        }
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        zb_sensor->device_ieee = ieee;
        
        if (zigbee_initialized) {
            // Reopen network for joining to allow new device to join
            Zigbee.openNetwork(60);
            DEBUG_PRINTLN("Zigbee network reopened for device pairing (60s)");
        }
        
        DEBUG_PRINTLN("Device configured and ready for pairing");
    }
}

void sensor_zigbee_unbind_device(uint nr, const char *device_ieee_str) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (device_ieee_str && device_ieee_str[0] && sensor) {
        
        DEBUG_PRINT("sensor_zigbee_unbind_device: ");
        DEBUG_PRINTLN(sensor->name);
        DEBUG_PRINT("IEEE: ");
        DEBUG_PRINTLN(device_ieee_str);
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        zb_sensor->device_ieee = 0;
        zb_sensor->device_bound = false;
        zb_sensor->flags.data_ok = false;
        
        DEBUG_PRINTLN("Device unbound");
    }
}

void sensor_zigbee_open_network(uint16_t duration) {
    if (!zigbee_initialized) {
        DEBUG_PRINTLN("ERROR: Zigbee not initialized");
        return;
    }
    
    Zigbee.openNetwork(duration);
    DEBUG_PRINT("Zigbee network opened for pairing (");
    DEBUG_PRINT(duration);
    DEBUG_PRINTLN("s)");
}

/**
 * @brief Get list of discovered Zigbee devices
 */
int sensor_zigbee_get_discovered_devices(ZigbeeDeviceInfo* devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;
    
    int count = (discovered_devices.size() < (size_t)max_devices) ? discovered_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &discovered_devices[i], sizeof(ZigbeeDeviceInfo));
    }
    
    DEBUG_PRINT("Returning ");
    DEBUG_PRINT(count);
    DEBUG_PRINTLN(" discovered devices");
    
    return count;
}

/**
 * @brief Clear new device flags
 */
void sensor_zigbee_clear_new_device_flags() {
    for (auto& dev : discovered_devices) {
        dev.is_new = false;
    }
    DEBUG_PRINTLN("Cleared new device flags");
}

/**
 * @brief Zigbee maintenance loop
 */
void sensor_zigbee_loop() {
    if (!zigbee_initialized) return;
    
    // Check for new devices
    static unsigned long last_check = 0;
    unsigned long now = millis();
    if (now - last_check > 5000) { // Check every 5 seconds
        last_check = now;
        
        // Count new devices
        int new_count = 0;
        for (const auto& dev : discovered_devices) {
            if (dev.is_new) new_count++;
        }
        
        if (new_count > 0) {
            DEBUG_PRINT("Found ");
            DEBUG_PRINT(new_count);
            DEBUG_PRINTLN(" new Zigbee device(s)");
        }
    }
}

/**
 * @brief PRINTLN("s)");
}

/**
 * @brief Actively read attribute from Zigbee device
 */
bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    if (!zigbee_initialized) {
        DEBUG_PRINTLN(F("ERROR: Zigbee not initialized"));
        return false;
    }
    
    // Find device short address from IEEE address
    uint16_t short_addr = 0xFFFF; // Invalid address
    for (const auto& dev : discovered_devices) {
        if (dev.ieee_addr == device_ieee) {
            short_addr = dev.short_addr;
            break;
        }
    }
    
    if (short_addr == 0xFFFF) {
        DEBUG_PRINTLN(F("ERROR: Device not found in network"));
        return false;
    }
    
    DEBUG_PRINT("Reading attribute from device: 0x");
    Serial.print((unsigned long)(device_ieee >> 32), HEX);
    Serial.println((unsigned long)(device_ieee & 0xFFFFFFFF), HEX);
    DEBUG_PRINT("  Short address: 0x");
    Serial.println(short_addr, HEX);
    DEBUG_PRINT("  Endpoint: ");
    DEBUG_PRINTLN(endpoint);
    DEBUG_PRINT("  Cluster: 0x");
    Serial.println(cluster_id, HEX);
    DEBUG_PRINT("  Attribute: 0x");
    Serial.println(attribute_id, HEX);
    
    // Use native ESP-Zigbee SDK API to send ZCL Read Attributes command
    esp_zb_zcl_read_attr_cmd_t read_req;
    read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10; // Our gateway endpoint
    read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    read_req.clusterID = cluster_id;
    read_req.attr_number = 1;
    read_req.attr_field = &attribute_id; // Single attribute to read
    
    // Send the read attribute command (returns transaction sequence number)
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&read_req);
    
    if (tsn == 0xFF) {
        DEBUG_PRINTLN(F("Failed to send read attribute command"));
        return false;
    }
    
    DEBUG_PRINTLN("Read attribute command sent successfully");
    return true;
}

void ZigbeeSensor::emitJson(BufferFiller& bfill) const {
    ArduinoJson::JsonDocument doc;
    ArduinoJson::JsonObject obj = doc.to<ArduinoJson::JsonObject>();
    toJson(obj);
    
    // Serialize to string and output
    String jsonStr;
    ArduinoJson::serializeJson(doc, jsonStr);
    bfill.emit_p(PSTR("$S"), jsonStr.c_str());
}

unsigned char ZigbeeSensor::getUnitId() const {
    // Determine unit based on cluster type
    if (cluster_id == 0x0408 || cluster_id == 0x0405) { // Soil moisture or humidity
        return UNIT_PERCENT;
    } else if (cluster_id == 0x0402) { // Temperature
        return UNIT_DEGREE;
    } else if (cluster_id == 0x0001) { // Power/battery
        return UNIT_PERCENT;
    }
    
    // Default to assigned unit if specified
    if (assigned_unitid > 0) {
        return assigned_unitid;
    }
    
    return UNIT_NONE;
}


#endif // ESP32C5
