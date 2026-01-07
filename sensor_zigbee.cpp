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

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include <esp_partition.h>
#include <vector>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include "esp_zigbee_core.h"
#include "Zigbee.h"

// Zigbee coordinator instance
static bool zigbee_initialized = false;

// Active Zigbee sensor (prevents concurrent access)
static int active_zigbee_sensor = 0;

// Discovered devices storage (dynamically allocated)
static std::vector<ZigbeeDeviceInfo> discovered_devices;

static void log_esp_err(const __FlashStringHelper* what, esp_err_t err) {
    DEBUG_PRINT(what);
    DEBUG_PRINT(F(": "));
    DEBUG_PRINTLN(esp_err_to_name(err));
}

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
    DEBUG_PRINT(F("New Zigbee device joined! Short: 0x"));
    DEBUG_PRINTF(F("%04X"), (unsigned int)short_addr);
    DEBUG_PRINT(F(", IEEE: 0x"));
    DEBUG_PRINTF(
        F("%08lX%08lX\n"),
        (unsigned long)(ieee_addr >> 32),
        (unsigned long)(ieee_addr & 0xFFFFFFFFUL)
    );
    
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

        DEBUG_PRINTLN(F("Device added to discovered list"));
    }
}

/**
 * @brief Actually start Zigbee coordinator (called after WiFi is connected)
 */
void sensor_zigbee_start() {
    if (zigbee_initialized) {
        return;
    }

    DEBUG_PRINTLN(F("Starting Zigbee coordinator..."));
    
    // Check if zb_storage partition exists (FAT subtype 0x81)
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        return;
    }

    DEBUG_PRINTLN(F("Found zb_storage partition"));
   
    // Configure Zigbee radio settings
    esp_zb_radio_config_t radio_config = {
        .radio_mode = ZB_RADIO_MODE_NATIVE,  // Native 802.15.4 mode (not BLE)
    };
    
    // Configure Zigbee host settings
    esp_zb_host_config_t host_config = {
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,  // No external host connection
    };
    
    // Apply radio and host configuration
    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);    

    DEBUG_PRINTLN(F("Zigbee radio and host configured"));

    // Enable WiFi/802.15.4 coexistence BEFORE starting Zigbee.
    // Without this, Zigbee init can break an already-established WiFi connection on ESP32-C5.
	DEBUG_PRINTLN(F("[COEX] Configuring WiFi/Zigbee coexistence"));
	WiFi.setSleep(false);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    (void)esp_coex_wifi_i154_enable();
    
    // Enable multiple endpoint binding for coordinator (Seeed Studio Wiki recommendation)
    Zigbee.allowMultiEndpointBinding(true);
    
    // Initialize Zigbee as coordinator (sensor data collector, not controller)
    Zigbee.begin(ZIGBEE_COORDINATOR);

    DEBUG_PRINTLN(F("Zigbee coordinator initialized"));
    
    // Note: Custom endpoint implementation depends on Arduino-Zigbee API
    // For now, use basic coordinator mode
    
    // Start Zigbee network

    Zigbee.start();
    DEBUG_PRINTLN(F("Zigbee network started"));
    
    zigbee_initialized = true;
}

bool sensor_zigbee_is_active() {
    return zigbee_initialized;
}

bool sensor_zigbee_ensure_started() {
    if (zigbee_initialized) return true;
    // Never start Zigbee in SOFTAP mode (RF conflict). Ethernet-only is handled elsewhere.
    if (os.get_wifi_mode() == WIFI_MODE_AP) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    sensor_zigbee_start();
    return zigbee_initialized;
}

// Note: sensor_zigbee_stop() removed - Zigbee stays active permanently
// WiFi-Zigbee coexistence handles RF coordination automatically

/**
 * @brief Zigbee attribute report callback - updates sensor data
 */
void ZigbeeSensor::zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint,
                                            uint16_t cluster_id, uint16_t attr_id,
                                            int32_t value, uint8_t lqi) {
    DEBUG_PRINT(F("Zigbee attribute report - IEEE: 0x"));


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
            
            DEBUG_PRINT(F("Updating sensor: "));
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
            
            DEBUG_PRINT(F("Raw value: "));
            DEBUG_PRINT(value);
            DEBUG_PRINT(F(" -> Converted: "));
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
    // Do NOT start Zigbee here - it will be started on-demand in read()
    // This prevents RF interference when no sensors are configured
    
    if (device_ieee == 0) {
        return false;
    }
    
    device_bound = true;
    return true;
}

void ZigbeeSensor::deinit() {
    if (device_bound && device_ieee != 0) {
        DEBUG_PRINTLN(F("ZigbeeSensor: Deinitializing device"));
        device_bound = false;
    }
}

int ZigbeeSensor::read(unsigned long time) {
    // Data request cycle: Zigbee stays on permanently
    // First call (repeat_read == 0): Request data, set repeat_read = 1
    // Second call (repeat_read == 1): Return data, set repeat_read = 0
    
    // CRITICAL: Do NOT use Zigbee in WiFi SOFTAP mode (RF coexistence conflict)
    // Exception: Ethernet mode (no WiFi RF usage)
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
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
        
        // CRITICAL: Check device_ieee BEFORE starting Zigbee
        // This prevents Zigbee from starting but never stopping on configuration errors
        if (device_ieee == 0) {
            // Disable sensor permanently to prevent spam in log
            flags.enable = false;
            flags.data_ok = false;
            active_zigbee_sensor = 0; // Release ownership
            
            return HTTP_RQT_NOT_RECEIVED;
        }

        // Start Zigbee on-demand (best-effort)
        if (!zigbee_initialized && !sensor_zigbee_ensure_started()) {
            flags.data_ok = false;
            active_zigbee_sensor = 0;
            return HTTP_RQT_NOT_RECEIVED;
        }
        
        // Send read attribute request
        if (device_ieee != 0) {
            if (sensor_zigbee_read_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                last_poll_time = time;
                last_read = time;
                // Response will arrive via zigbee_attribute_callback which sets repeat_read = 1
                return HTTP_RQT_NOT_RECEIVED; // Wait for callback
            } else {
                flags.data_ok = false;
                flags.data_ok = false;
                active_zigbee_sensor = 0; // Release ownership on error
                return HTTP_RQT_NOT_RECEIVED;
            }
        } else {
            // This should never happen due to earlier check, but keep as safety
            flags.data_ok = false;
            active_zigbee_sensor = 0; // Release ownership
            return HTTP_RQT_NOT_RECEIVED;
        }
        
    } else {
        // Second call (repeat_read == 1): Data received, return it
        repeat_read = 0; // Reset for next cycle
        active_zigbee_sensor = 0; // Release ownership
        
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
        
        DEBUG_PRINT(F("sensor_zigbee_bind_device: "));
        DEBUG_PRINTLN(sensor->name);
        DEBUG_PRINT(F("IEEE: "));
        DEBUG_PRINTLN(device_ieee_str);
        
        uint64_t ieee = ZigbeeSensor::parseIeeeAddress(device_ieee_str);
        if (ieee == 0) {
            DEBUG_PRINTLN(F("Invalid IEEE address"));
            return;
        }
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        zb_sensor->device_ieee = ieee;
        
        if (zigbee_initialized) {
            // Reopen network for joining to allow new device to join
            Zigbee.openNetwork(60);
            DEBUG_PRINTLN(F("Zigbee network reopened for device pairing (60s)"));
        }
        
        DEBUG_PRINTLN(F("Device configured and ready for pairing"));
    }
}

void sensor_zigbee_unbind_device(uint nr, const char *device_ieee_str) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (device_ieee_str && device_ieee_str[0] && sensor) {
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        zb_sensor->device_ieee = 0;
        zb_sensor->device_bound = false;
        zb_sensor->flags.data_ok = false;
        
    }
}

void sensor_zigbee_open_network(uint16_t duration) {
    if (!sensor_zigbee_ensure_started()) return;
    
    // Limit discovery time to max 10 seconds to minimize WiFi interference
    const uint16_t MAX_DISCOVERY_TIME = 10;
    uint16_t actual_duration = (duration > MAX_DISCOVERY_TIME) ? MAX_DISCOVERY_TIME : duration;
    
    Zigbee.openNetwork(actual_duration);
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
    
    return count;
}

/**
 * @brief Clear new device flags
 */
void sensor_zigbee_clear_new_device_flags() {
    for (auto& dev : discovered_devices) {
        dev.is_new = false;
    }
}

/**
 * @brief Zigbee maintenance loop
 */
void sensor_zigbee_loop() {
    if (!zigbee_initialized) return;
    // Intentionally no periodic work here (no debug polling).
}

/**
 * @brief Actively read attribute from Zigbee device
 */
bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    if (!zigbee_initialized && !sensor_zigbee_ensure_started()) return false;
    
    // Find device short address from IEEE address
    uint16_t short_addr = 0xFFFF; // Invalid address
    for (const auto& dev : discovered_devices) {
        if (dev.ieee_addr == device_ieee) {
            short_addr = dev.short_addr;
            break;
        }
    }
    
    if (short_addr == 0xFFFF) {
        return false;
    }
    
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
        return false;
    }
    return true;
}

void ZigbeeSensor::emitJson(BufferFiller& bfill) const {
	SensorBase::emitJson(bfill);
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
