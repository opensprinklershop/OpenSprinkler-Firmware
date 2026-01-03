/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Zigbee sensor implementation - Arduino Zigbee integration
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

#include "sensor_zigbee.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

extern OpenSprinkler os;

// Zigbee sensor is only available when Zigbee is enabled and Matter is NOT enabled
#if defined(ESP32C5) && !defined(ZIGBEE_MODE_ZCZR) && !defined(ENABLE_MATTER)
#error "Zigbee coordinator mode is not selected. Set Zigbee Mode to 'Zigbee ZCZR (coordinator/router)' in platformio.ini"
#endif

#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR) && !defined(ENABLE_MATTER)

#include <esp_partition.h>
#include <vector>
#include "esp_zigbee_core.h"
#include "Zigbee.h"

// WiFi+Zigbee Coexistence API
extern "C" {
    #include "esp_coexist.h"
}

// Zigbee coordinator instance
static bool zigbee_initialized = false;

// Active Zigbee sensor (prevents concurrent access)
static int active_zigbee_sensor = 0;

// Discovered devices storage (dynamically allocated)
static std::vector<ZigbeeDeviceInfo> discovered_devices;

// Zigbee Cluster IDs (ZCL Specification 07-5123-06)
// See: ZIGBEE_CLUSTER_REFERENCE.md for detailed documentation
#define ZB_ZCL_CLUSTER_ID_BASIC                     0x0000  // Section 3.2: Basic cluster
#define ZB_ZCL_CLUSTER_ID_POWER_CONFIG              0x0001  // Section 3.3: Power Configuration
#define ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT   0x0400  // Section 4.2: Illuminance (Lux)
#define ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT          0x0402  // Section 4.4: Temperature (0.01°C)
#define ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT      0x0403  // Section 4.5: Pressure (0.1 kPa)
#define ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT          0x0404  // Section 4.6: Flow (0.1 m³/h)
#define ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT  0x0405  // Section 4.7: Humidity (0.01% RH)
#define ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING         0x0406  // Section 4.8: Occupancy/Motion
#define ZB_ZCL_CLUSTER_ID_LEAF_WETNESS              0x0407  // Section 4.9: Leaf Wetness (%)
#define ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE             0x0408  // Section 4.10: Soil Moisture (0.01%)

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
    
    // CRITICAL: Do NOT start Zigbee in WiFi SOFTAP mode (RF coexistence conflict)
    // Exception: Ethernet mode (no WiFi RF usage)
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        DEBUG_PRINTLN("ERROR: Cannot start Zigbee in SOFTAP mode (RF conflict)");
        return;
    }
    
    DEBUG_PRINTLN("Starting Zigbee coordinator...");
    
    // Enable WiFi+Zigbee coexistence (ESP32-C5 RF sharing)
    // Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html
    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (coex_err == ESP_OK) {
        DEBUG_PRINTLN("WiFi+Zigbee coexistence enabled (BALANCE mode)");
    } else {
        DEBUG_PRINT("WARNING: Coexistence setup failed: ");
        DEBUG_PRINTLN(coex_err);
    }
    
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
    
    // Note: Network is kept closed for security
    // Use sensor_zigbee_open_network() API to allow new devices to join
    
    zigbee_initialized = true;
}

/**
 * @brief Stop Zigbee coordinator (frees RF resources)
 * @note Called when WiFi needs full RF access (e.g., connection problems)
 */
void sensor_zigbee_stop() {
    if (!zigbee_initialized) {
        DEBUG_PRINTLN("Zigbee not running, nothing to stop");
        return;
    }
    
    DEBUG_PRINTLN("Stopping Zigbee coordinator to free RF resources...");
    
    // Stop Zigbee stack
    Zigbee.stop();
    
    zigbee_initialized = false;
    DEBUG_PRINTLN("Zigbee stopped - RF resources freed for WiFi");
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
            
            // Convert raw Zigbee value according to ZCL Specification 07-5123-06
            // See ZIGBEE_CLUSTER_REFERENCE.md for conversion formulas
            zb_sensor->last_native_data = value; // Store raw value
            double converted_value = (double)value;
            
            // Apply ZCL standard conversions for known clusters
            if (cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && attr_id == 0x0000) {
                // ZCL 4.10: Soil Moisture MeasuredValue (uint16, 0-10000 = 0-100%)
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == 0x0000) {
                // ZCL 4.4: Temperature MeasuredValue (int16, 0.01°C resolution)
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr_id == 0x0000) {
                // ZCL 4.7: Humidity MeasuredValue (uint16, 0-10000 = 0-100% RH)
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && attr_id == 0x0000) {
                // ZCL 4.5: Pressure MeasuredValue (int16, 0.1 kPa resolution)
                converted_value = value / 10.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT && attr_id == 0x0000) {
                // ZCL 4.6: Flow MeasuredValue (uint16, 0.1 m³/h resolution)
                converted_value = value / 10.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT && attr_id == 0x0000) {
                // ZCL 4.2: Illuminance MeasuredValue (uint16, logarithmic: 10^((value-1)/10000) Lux)
                if (value > 0 && value <= 65534) {
                    converted_value = pow(10.0, (value - 1.0) / 10000.0);
                } else {
                    converted_value = 0.0; // Invalid/unknown
                }
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_LEAF_WETNESS && attr_id == 0x0000) {
                // ZCL 4.9: Leaf Wetness (uint16, 0-100%)
                converted_value = (double)value; // Direct value
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING && attr_id == 0x0000) {
                // ZCL 4.8: Occupancy bitmap (bit 0 = occupied)
                converted_value = (value & 0x01) ? 1.0 : 0.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && attr_id == 0x0021) {
                // ZCL 3.3: Battery Percentage Remaining (uint8, 0-200 = 0-100%, 0.5% per unit)
                converted_value = value / 2.0;
                zb_sensor->last_battery = (uint32_t)converted_value;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && attr_id == 0x0020) {
                // ZCL 3.3: Battery Voltage (uint8, 0.1V resolution)
                converted_value = value / 10.0;
            }
            
            // Apply user-defined factor/divider/offset (like SENSOR_USERDEF)
            // offset_mv: adjust zero-point offset in millivolt (applied before factor/divider)
            // factor/divider: multiplication/division for scaling
            // offset2: offset in 0.01 unit (applied after factor/divider)
            converted_value -= (double)zb_sensor->offset_mv / 1000.0;
            
            if (zb_sensor->factor && zb_sensor->divider)
                converted_value *= (double)zb_sensor->factor / (double)zb_sensor->divider;
            else if (zb_sensor->divider)
                converted_value /= (double)zb_sensor->divider;
            else if (zb_sensor->factor)
                converted_value *= (double)zb_sensor->factor;
            
            converted_value += zb_sensor->offset2 / 100.0;
            
            // Update sensor data
            zb_sensor->last_data = converted_value;
            zb_sensor->last_lqi = lqi;
            zb_sensor->flags.data_ok = true;
            zb_sensor->repeat_read = 1;  // Signal that new data is available
            
            DEBUG_PRINT("Raw value: ");
            DEBUG_PRINT(value);
            DEBUG_PRINT(" -> Converted: ");
            DEBUG_PRINTLN(converted_value);
            
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
    
    // User-defined conversion parameters (like AsbSensor SENSOR_USERDEF)
    if (obj.containsKey("factor")) {
        factor = obj["factor"].as<int32_t>();
    }
    if (obj.containsKey("divider")) {
        divider = obj["divider"].as<int32_t>();
    }
    if (obj.containsKey("offset_mv")) {
        offset_mv = obj["offset_mv"].as<int32_t>();
    }
    if (obj.containsKey("offset2")) {
        offset2 = obj["offset2"].as<int32_t>();
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
    
    // User-defined conversion parameters
    if (factor != 0) obj["factor"] = factor;
    if (divider != 0) obj["divider"] = divider;
    if (offset_mv != 0) obj["offset_mv"] = offset_mv;
    if (offset2 != 0) obj["offset2"] = offset2;
}

bool ZigbeeSensor::init() {
    DEBUG_PRINTLN("ZigbeeSensor::init()");
    
    // Do NOT start Zigbee here - it will be started on-demand in read()
    // This prevents RF interference when no sensors are configured
    
    if (device_ieee == 0) {
        DEBUG_PRINTLN("ZigbeeSensor: No device IEEE address configured");
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
    DEBUG_PRINTLN("  NOTE: Zigbee will be started on-demand during first read()");
    
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
    // Power-saving mode: Zigbee is turned on/off dynamically
    // First call (repeat_read == 0): Turn on Zigbee, request data, set repeat_read = 1
    // Second call (repeat_read == 1): Return data, set repeat_read = 0, turn off Zigbee
    
    // CRITICAL: Do NOT use Zigbee in WiFi SOFTAP mode (RF coexistence conflict)
    // Exception: Ethernet mode (no WiFi RF usage)
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        DEBUG_PRINTLN("Zigbee disabled in SOFTAP mode (WiFi setup)");
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Prevent concurrent access from multiple Zigbee sensors (like sensor_rs485_i2c)
    if (active_zigbee_sensor > 0 && active_zigbee_sensor != (int)nr) {
        repeat_read = 1;  // Will be reset when sensor gets its turn
        SensorBase *t = sensor_by_nr(active_zigbee_sensor);
        if (!t || !t->flags.enable) {
            active_zigbee_sensor = 0; // Breakout if active sensor is gone
        }
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (repeat_read == 0) {
        // Claim ownership
        if (active_zigbee_sensor != (int)nr) {
            active_zigbee_sensor = nr;
        }
        
        // First call: Turn on Zigbee and request data
        DEBUG_PRINT("Zigbee sensor read (turn on): ");
        DEBUG_PRINTLN(name);
        
        // CRITICAL: Check device_ieee BEFORE starting Zigbee
        // This prevents Zigbee from starting but never stopping on configuration errors
        if (device_ieee == 0) {
            DEBUG_PRINT("ERROR: Zigbee sensor '");
            DEBUG_PRINT(name);
            DEBUG_PRINTLN("' has no IEEE address configured!");
            DEBUG_PRINTLN("Please configure device_ieee via web interface (e.g., 0x00124B001F8E5678)");
            DEBUG_PRINTLN("Sensor will be disabled to prevent repeated errors.");
            
            // Disable sensor permanently to prevent spam in log
            flags.enable = false;
            flags.data_ok = false;
            active_zigbee_sensor = 0; // Release ownership
            
            // Stop Zigbee if it's running (cleanup from previous valid sensor)
            if (zigbee_initialized) {
                DEBUG_PRINTLN("Stopping Zigbee due to invalid configuration");
                sensor_zigbee_stop();
            }
            
            return HTTP_RQT_NOT_RECEIVED;
        }
        
        // Start Zigbee if not running
        if (!zigbee_initialized) {
            sensor_zigbee_start();
            if (!zigbee_initialized) {
                DEBUG_PRINTLN("ERROR: Failed to start Zigbee");
                flags.data_ok = false;
                active_zigbee_sensor = 0; // Release ownership
                return HTTP_RQT_NOT_RECEIVED;
            }
        }
        
        // Send read attribute request
        if (device_ieee != 0) {
            if (sensor_zigbee_read_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                last_poll_time = time;
                last_read = time;
                // Response will arrive via zigbee_attribute_callback which sets repeat_read = 1
                DEBUG_PRINTLN("Read request sent, waiting for response...");
                return HTTP_RQT_NOT_RECEIVED; // Wait for callback
            } else {
                DEBUG_PRINTLN("Failed to send read request");
                flags.data_ok = false;
                flags.data_ok = false;
                active_zigbee_sensor = 0; // Release ownership on error
                return HTTP_RQT_NOT_RECEIVED;
            }
        } else {
            // This should never happen due to earlier check, but keep as safety
            DEBUG_PRINTLN("ERROR: No device IEEE address configured");
            flags.data_ok = false;
            active_zigbee_sensor = 0; // Release ownership
            return HTTP_RQT_NOT_RECEIVED;
        }
        
    } else {
        // Second call (repeat_read == 1): Data received, return it and turn off Zigbee
        DEBUG_PRINT("Zigbee sensor read (turn off): ");
        DEBUG_PRINTLN(name);
        
        repeat_read = 0; // Reset for next cycle
        active_zigbee_sensor = 0; // Release ownership
        
        // Turn off Zigbee to free RF resources for WiFi
        sensor_zigbee_stop();
        
        last_read = time;
        
        if (flags.data_ok) {
            return HTTP_RQT_SUCCESS; // Adds data to log
        } else {
            return HTTP_RQT_NOT_RECEIVED;
        }
    }
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
    // Determine unit based on ZCL cluster type (auto-assignment)
    switch(cluster_id) {
        case ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE:       // 0x0408
        case ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:  // 0x0405
        case ZB_ZCL_CLUSTER_ID_LEAF_WETNESS:        // 0x0407
            return UNIT_PERCENT;  // % (1)
            
        case ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:    // 0x0402
            return UNIT_DEGREE;   // °C (2)
            
        case ZB_ZCL_CLUSTER_ID_POWER_CONFIG:        // 0x0001
            return UNIT_PERCENT;  // % (1) for battery
            
        case ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT:  // 0x0400
            return UNIT_LX;       // Lux (13)
            
        case ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT:     // 0x0403
            return UNIT_USERDEF;  // kPa (custom, 99)
            
        case ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT:         // 0x0404
            return UNIT_USERDEF;  // m³/h (custom, 99)
            
        case ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:        // 0x0406
            return UNIT_LEVEL;    // Boolean/Level (10)
    }
    
    // Default to user-assigned unit if specified
    if (assigned_unitid > 0) {
        return assigned_unitid;
    }
    
    return UNIT_NONE;
}


#endif // ESP32C5
