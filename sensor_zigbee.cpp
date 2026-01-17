/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Zigbee sensor implementation - ESP32-C5 End Device Mode
 * Uses WiFi + Zigbee Coexistence (End Device mode only!)
 * 
 * IMPORTANT: This implementation runs as Zigbee END DEVICE, not Coordinator!
 * - Coordinator/Router + WiFi is NOT supported on single-chip ESP32-C5
 * - End Device + WiFi STA is supported with stable performance
 * - Requires an external Zigbee Coordinator (e.g., Zigbee2MQTT)
 *
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
#include "ZigbeeEP.h"

// Zigbee End Device state
static bool zigbee_initialized = false;
static bool zigbee_connected = false;

// Active Zigbee sensor (prevents concurrent access)
static int active_zigbee_sensor = 0;

// Discovered devices storage (for compatibility with old API)
static std::vector<ZigbeeDeviceInfo> discovered_devices;

// Zigbee Cluster IDs (ZCL Specification 07-5123-06)
#define ZB_ZCL_CLUSTER_ID_BASIC                     0x0000
#define ZB_ZCL_CLUSTER_ID_POWER_CONFIG              0x0001
#define ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT   0x0400
#define ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT          0x0402
#define ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT      0x0403
#define ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT          0x0404
#define ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT  0x0405
#define ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING         0x0406
#define ZB_ZCL_CLUSTER_ID_LEAF_WETNESS              0x0407
#define ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE             0x0408

// Forward declaration
class ZigbeeReportReceiver;
static ZigbeeReportReceiver* reportReceiver = nullptr;

/**
 * @brief Custom Zigbee Endpoint to receive attribute reports from other devices
 * 
 * This endpoint acts as a "sensor collector" - it doesn't send data itself,
 * but receives reports from other Zigbee sensors that are bound to it.
 * 
 * The binding is typically done by the Zigbee coordinator (e.g., Zigbee2MQTT).
 */
class ZigbeeReportReceiver : public ZigbeeEP {
public:
    ZigbeeReportReceiver(uint8_t endpoint) : ZigbeeEP(endpoint) {
        // Create cluster list with client-side clusters to receive reports
        _cluster_list = esp_zb_zcl_cluster_list_create();
        
        // Add basic cluster (mandatory for all Zigbee devices)
        esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
        esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        
        // Add identify cluster (mandatory)
        esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
        esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        
        // Add CLIENT-side clusters to receive reports from sensors
        // Temperature Measurement (0x0402)
        esp_zb_attribute_list_t *temp_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
        esp_zb_cluster_list_add_temperature_meas_cluster(_cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Relative Humidity (0x0405)
        esp_zb_attribute_list_t *humidity_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
        esp_zb_cluster_list_add_humidity_meas_cluster(_cluster_list, humidity_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Soil Moisture (0x0408) - custom cluster
        esp_zb_attribute_list_t *soil_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE);
        esp_zb_cluster_list_add_custom_cluster(_cluster_list, soil_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Illuminance (0x0400)
        esp_zb_attribute_list_t *light_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT);
        esp_zb_cluster_list_add_illuminance_meas_cluster(_cluster_list, light_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Pressure (0x0403)
        esp_zb_attribute_list_t *pressure_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT);
        esp_zb_cluster_list_add_pressure_meas_cluster(_cluster_list, pressure_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Power Configuration (0x0001) - for battery reports
        esp_zb_attribute_list_t *power_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
        esp_zb_cluster_list_add_power_config_cluster(_cluster_list, power_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Configure endpoint
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID; // Configuration tool can receive reports
        _ep_config.app_device_version = 0;
        
        DEBUG_PRINTLN(F("[ZIGBEE] Report receiver endpoint created"));
    }
    
    /**
     * @brief Callback when an attribute report is received from another device
     * 
     * This is called by the Zigbee stack when a bound device sends an attribute report.
     * We forward the data to the appropriate ZigbeeSensor instance.
     */
    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute, 
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) return;
        
        DEBUG_PRINTF(F("[ZIGBEE] Report received: cluster=0x%04X, attr=0x%04X, src_ep=%d, src_addr=0x%04X\n"),
                     cluster_id, attribute->id, src_endpoint, src_address.u.short_addr);
        
        // Find the IEEE address from short address (if we have it)
        uint64_t ieee_addr = 0;
        for (const auto& dev : discovered_devices) {
            if (dev.short_addr == src_address.u.short_addr) {
                ieee_addr = dev.ieee_addr;
                break;
            }
        }
        
        // Forward to static callback for sensor matching
        ZigbeeSensor::zigbee_attribute_callback(
            ieee_addr,
            src_endpoint,
            cluster_id,
            attribute->id,
            extractAttributeValue(attribute),
            0  // LQI not available in this callback
        );
    }
    
private:
    /**
     * @brief Extract numeric value from ZCL attribute
     */
    int32_t extractAttributeValue(const esp_zb_zcl_attribute_t *attr) {
        if (!attr || !attr->data.value) return 0;
        
        switch (attr->data.type) {
            case ESP_ZB_ZCL_ATTR_TYPE_S8:
                return (int32_t)(*(int8_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_S16:
                return (int32_t)(*(int16_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_S32:
                return *(int32_t*)attr->data.value;
            case ESP_ZB_ZCL_ATTR_TYPE_U8:
                return (int32_t)(*(uint8_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_U16:
                return (int32_t)(*(uint16_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_U32:
                return (int32_t)(*(uint32_t*)attr->data.value);
            default:
                DEBUG_PRINTF(F("[ZIGBEE] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// Flag to track if we need to reset NVRAM (e.g., after switching modes)
static bool zigbee_needs_nvram_reset = false;

/**
 * @brief Force a factory reset of Zigbee NVRAM on next start
 * Call this when switching device modes or if NVRAM is corrupted
 */
void sensor_zigbee_factory_reset() {
    zigbee_needs_nvram_reset = true;
    DEBUG_PRINTLN(F("[ZIGBEE] Factory reset scheduled for next start"));
}

/**
 * @brief Erase Zigbee NVRAM partition completely
 * This is needed when switching between Coordinator and End Device modes
 */
static bool erase_zigbee_nvram() {
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("[ZIGBEE] No zb_storage partition to erase"));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE] Erasing Zigbee NVRAM partition..."));
    esp_err_t err = esp_partition_erase_range(zb_partition, 0, zb_partition->size);
    if (err != ESP_OK) {
        DEBUG_PRINTF(F("[ZIGBEE] Failed to erase NVRAM: %s\n"), esp_err_to_name(err));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE] NVRAM erased successfully"));
    return true;
}

/**
 * @brief Start Zigbee as End Device (called after WiFi is connected)
 * 
 * IMPORTANT: This starts Zigbee in END DEVICE mode, which is the only mode
 * that supports WiFi coexistence on ESP32-C5/C6.
 */
void sensor_zigbee_start() {
    if (zigbee_initialized) {
        return;
    }

    DEBUG_PRINTLN(F("[ZIGBEE] Starting Zigbee END DEVICE..."));
    DEBUG_PRINTLN(F("[ZIGBEE] NOTE: Connect to existing Zigbee network (e.g., Zigbee2MQTT)"));
    
    // Check if zb_storage partition exists
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("[ZIGBEE] ERROR: No zb_storage partition found!"));
        return;
    }

    DEBUG_PRINTLN(F("[ZIGBEE] Found zb_storage partition"));
    
    // Check if NVRAM reset is needed (e.g., first run or mode switch)
    // We use a simple marker: if partition starts with 0xFF, it's either fresh or erased
    // The ZBOSS stack writes its own magic bytes when initialized
    uint8_t nvram_magic[4];
    esp_partition_read(zb_partition, 0, nvram_magic, sizeof(nvram_magic));
    
    // ZBOSS NVRAM has specific headers - if they look corrupt or wrong mode, reset
    // The assertion error means incompatible data, so we always reset on first End Device boot
    static bool first_boot_check_done = false;
    if (!first_boot_check_done) {
        first_boot_check_done = true;
        // For safety, always erase on first boot to ensure clean End Device state
        // This prevents "Zigbee stack assertion failed" errors from old Coordinator data
        DEBUG_PRINTLN(F("[ZIGBEE] First boot - erasing NVRAM for clean End Device state"));
        erase_zigbee_nvram();
    }
    
    // Handle explicit factory reset request
    if (zigbee_needs_nvram_reset) {
        zigbee_needs_nvram_reset = false;
        erase_zigbee_nvram();
    }
   
    // Configure Zigbee radio settings
    esp_zb_radio_config_t radio_config = {
        .radio_mode = ZB_RADIO_MODE_NATIVE,
    };
    
    esp_zb_host_config_t host_config = {
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
    };
    
    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);    

    // Enable WiFi/802.15.4 coexistence
    DEBUG_PRINTLN(F("[ZIGBEE] Configuring WiFi/Zigbee coexistence"));
    WiFi.setSleep(false);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);  // WiFi has priority
    (void)esp_coex_wifi_i154_enable();
    
    // Create report receiver endpoint
    reportReceiver = new ZigbeeReportReceiver(10);  // Endpoint 10
    Zigbee.addEndpoint(reportReceiver);
    
    // Set manufacturer and model for identification
    reportReceiver->setManufacturerAndModel("OpenSprinkler", "ZigbeeReceiver");
    
    // Start Zigbee as END DEVICE (WiFi compatible!)
    // This will join an existing network controlled by a Coordinator
    DEBUG_PRINTLN(F("[ZIGBEE] Starting as END DEVICE (WiFi coexistence supported)"));
    
    // Use resetToFactoryDefaults=true on first start to ensure clean state
    if (!Zigbee.begin(ZIGBEE_END_DEVICE, true)) {  // true = reset to factory defaults
        DEBUG_PRINTLN(F("[ZIGBEE] ERROR: Failed to start Zigbee End Device!"));
        delete reportReceiver;
        reportReceiver = nullptr;
        return;
    }

    DEBUG_PRINTLN(F("[ZIGBEE] Zigbee End Device started, searching for network..."));
    zigbee_initialized = true;
}

bool sensor_zigbee_is_active() {
    return zigbee_initialized;
}

bool sensor_zigbee_ensure_started() {
    if (zigbee_initialized) return true;
    
    // Never start Zigbee in SOFTAP mode (RF conflict)
    if (os.get_wifi_mode() == WIFI_MODE_AP) {
        DEBUG_PRINTLN(F("[ZIGBEE] Cannot start in SOFTAP mode"));
        return false;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[ZIGBEE] WiFi not connected, waiting..."));
        return false;
    }
    
    sensor_zigbee_start();
    return zigbee_initialized;
}

/**
 * @brief Zigbee attribute report callback - updates sensor data
 */
void ZigbeeSensor::zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint,
                                            uint16_t cluster_id, uint16_t attr_id,
                                            int32_t value, uint8_t lqi) {
    DEBUG_PRINTF(F("[ZIGBEE] Attribute callback: cluster=0x%04X, attr=0x%04X, value=%d\n"),
                 cluster_id, attr_id, value);

    // Find sensor by cluster and attribute (IEEE matching is optional)
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) {
            continue;
        }
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        
        // Match by cluster_id and attribute_id
        // If IEEE address is specified, also match that
        bool matches = (zb_sensor->cluster_id == cluster_id &&
                       zb_sensor->attribute_id == attr_id);
        
        if (zb_sensor->device_ieee != 0 && ieee_addr != 0) {
            matches = matches && (zb_sensor->device_ieee == ieee_addr);
        }
        
        if (matches) {
            DEBUG_PRINTF(F("[ZIGBEE] Updating sensor: %s\n"), sensor->name);
            
            // Convert raw Zigbee value according to ZCL Specification
            zb_sensor->last_native_data = value;
            double converted_value = (double)value;
            
            // Apply ZCL standard conversions
            if (cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && attr_id == 0x0000) {
                converted_value = value / 100.0;  // 0-10000 -> 0-100%
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 100.0;  // 0.01°C resolution
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 100.0;  // 0-10000 -> 0-100% RH
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 10.0;   // 0.1 kPa resolution
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT && attr_id == 0x0000) {
                if (value > 0 && value <= 65534) {
                    converted_value = pow(10.0, (value - 1.0) / 10000.0);  // Logarithmic
                } else {
                    converted_value = 0.0;
                }
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && attr_id == 0x0021) {
                converted_value = value / 2.0;  // Battery percentage
                zb_sensor->last_battery = (uint32_t)converted_value;
            }
            
            // Apply user-defined factor/divider/offset
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
            zb_sensor->repeat_read = 1;
            
            DEBUG_PRINTF(F("[ZIGBEE] Raw: %d -> Converted: %.2f\n"), value, converted_value);
            break;
        }
    }
}

uint64_t ZigbeeSensor::parseIeeeAddress(const char* ieee_str) {
    if (!ieee_str || !ieee_str[0]) return 0;
    
    if (ieee_str[0] == '0' && (ieee_str[1] == 'x' || ieee_str[1] == 'X')) {
        ieee_str += 2;
    }
    
    uint64_t addr = 0;
    for (int i = 0; ieee_str[i] != '\0'; i++) {
        char c = ieee_str[i];
        if (c == ':' || c == '-' || c == ' ') continue;
        
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else {
            return 0;
        }
        addr = (addr << 4) | nibble;
    }
    return addr;
}

const char* ZigbeeSensor::getIeeeString(char* buffer, size_t bufferSize) const {
    if (bufferSize < 19) return "";
    snprintf(buffer, bufferSize, "0x%016llX", (unsigned long long)device_ieee);
    return buffer;
}

void ZigbeeSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    SensorBase::fromJson(obj);
    
    if (obj.containsKey("device_ieee")) {
        const char *ieee_str = obj["device_ieee"].as<const char*>();
        if (ieee_str) {
            device_ieee = parseIeeeAddress(ieee_str);
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
    
    if (device_ieee != 0) {
        char ieee_str[20];
        obj["device_ieee"] = getIeeeString(ieee_str, sizeof(ieee_str));
    }
    if (endpoint != 1) obj["endpoint"] = endpoint;
    if (cluster_id != 0x0402) obj["cluster_id"] = cluster_id;
    if (attribute_id != 0x0000) obj["attribute_id"] = attribute_id;
    if (poll_interval != 60000) obj["poll_interval"] = poll_interval;
    if (last_battery < 100) obj["battery"] = last_battery;
    if (last_lqi > 0) obj["lqi"] = last_lqi;
    
    if (factor != 0) obj["factor"] = factor;
    if (divider != 0) obj["divider"] = divider;
    if (offset_mv != 0) obj["offset_mv"] = offset_mv;
    if (offset2 != 0) obj["offset2"] = offset2;
}

bool ZigbeeSensor::init() {
    // Zigbee End Device will be started on-demand in read()
    return true;
}

void ZigbeeSensor::deinit() {
    device_bound = false;
    flags.data_ok = false;
}

int ZigbeeSensor::read(unsigned long time) {
    // CRITICAL: Do NOT use Zigbee in WiFi SOFTAP mode
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Prevent concurrent access
    if (active_zigbee_sensor > 0 && active_zigbee_sensor != (int)nr) {
        repeat_read = 1;
        SensorBase *t = sensor_by_nr(active_zigbee_sensor);
        if (!t || !t->flags.enable) {
            active_zigbee_sensor = 0;
        }
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (repeat_read == 0) {
        if (active_zigbee_sensor != (int)nr) {
            active_zigbee_sensor = nr;
        }
        
        // Start Zigbee End Device if not already started
        if (!zigbee_initialized && !sensor_zigbee_ensure_started()) {
            flags.data_ok = false;
            active_zigbee_sensor = 0;
            return HTTP_RQT_NOT_RECEIVED;
        }
        
        // Check if connected to network
        if (!Zigbee.started()) {
            DEBUG_PRINTLN(F("[ZIGBEE] Not connected to network yet"));
            flags.data_ok = false;
            active_zigbee_sensor = 0;
            return HTTP_RQT_NOT_RECEIVED;
        }
        
        // In End Device mode, we passively receive reports from bound devices
        // The data arrives via zbAttributeRead callback
        // Set repeat_read to wait for data
        repeat_read = 1;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
        
    } else {
        // Second call: check if data was received
        repeat_read = 0;
        active_zigbee_sensor = 0;
        last_read = time;
        
        if (flags.data_ok) {
            return HTTP_RQT_SUCCESS;
        } else {
            return HTTP_RQT_NOT_RECEIVED;
        }
    }
}

// Compatibility functions for old API
void sensor_zigbee_bind_device(uint nr, const char *device_ieee_str) {
    DEBUG_PRINTF(F("[ZIGBEE] Bind request for sensor %d: %s\n"), nr, device_ieee_str ? device_ieee_str : "null");
    DEBUG_PRINTLN(F("[ZIGBEE] NOTE: Binding must be done via Zigbee coordinator (e.g., Zigbee2MQTT)"));
    
    if (device_ieee_str && device_ieee_str[0]) {
        SensorBase* sensor = sensor_by_nr(nr);
        if (sensor && sensor->type == SENSOR_ZIGBEE) {
            ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
            zb_sensor->device_ieee = ZigbeeSensor::parseIeeeAddress(device_ieee_str);
        }
    }
}

void sensor_zigbee_unbind_device(uint nr, const char *device_ieee_str) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (sensor && sensor->type == SENSOR_ZIGBEE) {
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        zb_sensor->device_ieee = 0;
        zb_sensor->device_bound = false;
        zb_sensor->flags.data_ok = false;
    }
}

void sensor_zigbee_open_network(uint16_t duration) {
    DEBUG_PRINTLN(F("[ZIGBEE] NOTE: Network joining is controlled by coordinator"));
    DEBUG_PRINTLN(F("[ZIGBEE] Use Zigbee2MQTT to pair new devices"));
}

int sensor_zigbee_get_discovered_devices(ZigbeeDeviceInfo* devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;
    int count = (discovered_devices.size() < (size_t)max_devices) ? discovered_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &discovered_devices[i], sizeof(ZigbeeDeviceInfo));
    }
    return count;
}

void sensor_zigbee_clear_new_device_flags() {
    for (auto& dev : discovered_devices) {
        dev.is_new = false;
    }
}

void sensor_zigbee_loop() {
    if (!zigbee_initialized) return;
    
    // Check connection status
    static bool last_connected = false;
    bool connected = Zigbee.started() && Zigbee.connected();
    
    if (connected != last_connected) {
        if (connected) {
            DEBUG_PRINTLN(F("[ZIGBEE] Connected to Zigbee network!"));
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE] Disconnected from Zigbee network"));
        }
        last_connected = connected;
        zigbee_connected = connected;
    }
}

bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    // In End Device mode, we typically rely on reports rather than active polling
    DEBUG_PRINTLN(F("[ZIGBEE] Active attribute reading not supported in End Device mode"));
    DEBUG_PRINTLN(F("[ZIGBEE] Configure reporting via Zigbee coordinator instead"));
    return false;
}

void ZigbeeSensor::emitJson(BufferFiller& bfill) const {
    SensorBase::emitJson(bfill);
}

unsigned char ZigbeeSensor::getUnitId() const {
    // Auto-assign unit based on ZCL cluster
    if (assigned_unitid > 0) {
        return assigned_unitid;
    }
    
    switch(cluster_id) {
        case ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE:
        case ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
        case ZB_ZCL_CLUSTER_ID_LEAF_WETNESS:
        case ZB_ZCL_CLUSTER_ID_POWER_CONFIG:  // Battery %
            return UNIT_PERCENT;
            
        case ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
            return UNIT_DEGREE;
            
        case ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT:
            return UNIT_LX;
            
        case ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT:
        case ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT:
            return UNIT_USERDEF;
            
        case ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
            return UNIT_LEVEL;
    }
    
    return UNIT_NONE;
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
