/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Zigbee sensor implementation - Unified runtime dispatcher
 * 
 * This file contains:
 * 1. All shared ZigbeeSensor class methods (only defined once)
 * 2. Client/End Device specific internals
 * 3. Public sensor_zigbee_*() API that dispatches to the correct mode
 *    at RUNTIME based on ieee802154_get_mode()
 *
 * The gateway-specific internals live in sensor_zigbee_gw.cpp and are
 * called via sensor_zigbee_gw.h when the runtime mode is ZIGBEE_GATEWAY.
 *
 * MUTUAL EXCLUSIVITY RULES (enforced at runtime):
 *   1. 802.15.4 DISABLED → Matter + Zigbee both disabled
 *   2. MATTER mode       → Zigbee disabled (both client and gateway)
 *   3. ZIGBEE_GATEWAY    → Matter disabled, Zigbee Client disabled
 *   4. ZIGBEE_CLIENT     → Matter disabled, Zigbee Gateway disabled
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
#include "sensor_zigbee_gw.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"
#include "ieee802154_config.h"
#include "radio_arbiter.h"

extern OpenSprinkler os;

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include <esp_partition.h>
#include <vector>
#include <WiFi.h>
#include <esp_err.h>
extern "C" {
#include <esp_coex_i154.h>
}
#include <esp_ieee802154.h>
#include "esp_zigbee_core.h"
#include "Zigbee.h"
#include "ZigbeeEP.h"

// Optional: Restrict Zigbee to a single channel (e.g. 25 = 2475 MHz) to
// minimise radio overlap with WiFi on dual-protocol ESP32-C5.
// If not defined, the Zigbee stack scans all channels 11-26 (default).
// #define ZIGBEE_COEX_CHANNEL_MASK  (1UL << 25)

// ==========================================================================
// CLIENT (End Device) specific state and implementation
// ==========================================================================

// Client state
static bool client_zigbee_initialized = false;
static bool client_zigbee_connected = false;
static int client_active_zigbee_sensor = 0;
static std::vector<ZigbeeDeviceInfo> client_discovered_devices;

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

// Tuya-specific protocol definitions (cluster 0xEF00)
#define ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC             0xEF00
#define TUYA_CMD_DATA_REQUEST   0x00
#define TUYA_CMD_DATA_RESPONSE  0x01
#define TUYA_CMD_DATA_REPORT    0x02
#define TUYA_DP_TYPE_RAW    0x00
#define TUYA_DP_TYPE_BOOL   0x01
#define TUYA_DP_TYPE_VALUE  0x02  // 4-byte big-endian integer
#define TUYA_DP_TYPE_STRING 0x03
#define TUYA_DP_TYPE_ENUM   0x04
#define TUYA_DP_TYPE_BITMAP 0x05
// Common Tuya DP numbers for soil/environment sensors
#define TUYA_DP_SOIL_MOISTURE     3
#define TUYA_DP_TEMPERATURE       5
#define TUYA_DP_TEMPERATURE_UNIT  9
#define TUYA_DP_BATTERY          15
// Flag to mark Tuya reports as pre-scaled (no ZCL scaling needed)
#define TUYA_REPORT_FLAG_PRESCALED  0x8000

// Basic Cluster attribute IDs
#define ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID       0x0004
#define ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID        0x0005
#define ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID            0x0007

class ClientZigbeeReportReceiver;
static ClientZigbeeReportReceiver* client_reportReceiver = nullptr;

// Flag to track if we need to reset NVRAM
static bool client_zigbee_needs_nvram_reset = false;

// Static storage for active attribute read requests.
// attr_field in esp_zb_zcl_read_attr_cmd_t is a pointer that ZBOSS
// processes asynchronously in the Zigbee_main task.  Using a stack
// variable would leave a dangling pointer → Load access fault.
static uint16_t s_read_attr_id = 0;
static bool     s_read_pending = false;

// Basic Cluster query state (Client mode)
// Used to read ManufacturerName (0x0004) and ModelIdentifier (0x0005)
// from newly discovered remote devices.
static uint16_t s_basic_query_attrs[2] = {
    ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
    ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID
};
static bool     s_basic_query_pending = false;
static unsigned long s_basic_query_time = 0;
#define CLIENT_BASIC_QUERY_TIMEOUT_MS   10000  // 10 seconds
#define CLIENT_BASIC_QUERY_DELAY_MS     5000   // Wait 5s after first contact

// Queue of devices whose Basic Cluster has not yet been read (Client mode)
struct ClientBasicQueryItem {
    uint64_t ieee_addr;
    uint16_t short_addr;
    uint8_t  endpoint;
    unsigned long discovered_time;
};
static std::vector<ClientBasicQueryItem> client_basic_query_queue;

/**
 * @brief Extract a ZCL CHAR_STRING attribute into a C string buffer
 * @param attr ZCL attribute (must be CHAR_STRING or LONG_CHAR_STRING type)
 * @param buf Output buffer
 * @param buf_size Size of output buffer
 * @return true if a string was extracted, false otherwise
 */
static bool extractStringAttribute(const esp_zb_zcl_attribute_t *attr, char *buf, size_t buf_size) {
    if (!attr || !attr->data.value || buf_size == 0) return false;
    
    if (attr->data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
        // ZCL CHAR_STRING: [length_byte][chars...] (NOT null-terminated)
        uint8_t *raw = (uint8_t*)attr->data.value;
        uint8_t len = raw[0];
        if (len == 0xFF) { buf[0] = '\0'; return false; }  // 0xFF = invalid
        if (len >= buf_size) len = buf_size - 1;
        memcpy(buf, raw + 1, len);
        buf[len] = '\0';
        return len > 0;
    } else if (attr->data.type == ESP_ZB_ZCL_ATTR_TYPE_LONG_CHAR_STRING) {
        // ZCL LONG_CHAR_STRING: [length_u16_le][chars...]
        uint8_t *raw = (uint8_t*)attr->data.value;
        uint16_t len = raw[0] | ((uint16_t)raw[1] << 8);
        if (len == 0xFFFF) { buf[0] = '\0'; return false; }
        if (len >= buf_size) len = buf_size - 1;
        memcpy(buf, raw + 2, len);
        buf[len] = '\0';
        return len > 0;
    }
    return false;
}

/**
 * @brief Handle Basic Cluster (0x0000) attribute read response in Client mode
 * Updates discovered device info and matching sensor configurations.
 */
static void client_handleBasicClusterResponse(uint16_t short_addr, const esp_zb_zcl_attribute_t *attribute) {
    if (!attribute || !attribute->data.value) return;
    
    char str_buf[32] = {0};
    if (!extractStringAttribute(attribute, str_buf, sizeof(str_buf))) {
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster attr 0x%04X: not a string (type=0x%02X)\n"),
                     attribute->id, attribute->data.type);
        return;
    }
    
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster attr 0x%04X = \"%s\" (from short=0x%04X)\n"),
                 attribute->id, str_buf, short_addr);
    
    // Find device in discovered list and update
    uint64_t ieee_addr = 0;
    for (auto& dev : client_discovered_devices) {
        if (dev.short_addr == short_addr) {
            ieee_addr = dev.ieee_addr;
            if (attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) {
                strncpy(dev.manufacturer, str_buf, sizeof(dev.manufacturer) - 1);
                dev.manufacturer[sizeof(dev.manufacturer) - 1] = '\0';
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
                strncpy(dev.model_id, str_buf, sizeof(dev.model_id) - 1);
                dev.model_id[sizeof(dev.model_id) - 1] = '\0';
            }
            DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Updated device 0x%016llX: mfr=\"%s\" model=\"%s\"\n"),
                         (unsigned long long)dev.ieee_addr, dev.manufacturer, dev.model_id);
            break;
        }
    }
    
    // Update matching sensor configurations
    if (ieee_addr != 0) {
        const char* mfr = (attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) ? str_buf : nullptr;
        const char* mdl = (attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) ? str_buf : nullptr;
        ZigbeeSensor::updateBasicClusterInfo(ieee_addr, mfr, mdl);
    }
}

/**
 * @brief Send a ZCL Read Attributes request for Basic Cluster to a remote device (Client mode)
 * Reads ManufacturerName (0x0004) and ModelIdentifier (0x0005) in one request.
 */
static bool client_query_basic_cluster(uint64_t device_ieee, uint8_t endpoint) {
    if (!client_zigbee_initialized || !client_reportReceiver) return false;
    if (!Zigbee.started() || !Zigbee.connected()) return false;
    if (device_ieee == 0) return false;
    if (s_basic_query_pending) return false;  // Already in progress
    
    // Convert IEEE to little-endian array
    esp_zb_ieee_addr_t ieee_le;
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }
    
    esp_zb_lock_acquire(portMAX_DELAY);
    
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    if (short_addr == 0xFFFF || short_addr == 0xFFFE) {
        esp_zb_lock_release();
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query: device 0x%016llX not in address table\n"),
                     (unsigned long long)device_ieee);
        return false;
    }
    
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Querying Basic Cluster: ieee=0x%016llX short=0x%04X ep=%d\n"),
                 (unsigned long long)device_ieee, short_addr, endpoint);
    
    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));
    read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;  // our endpoint
    read_req.clusterID = ZB_ZCL_CLUSTER_ID_BASIC;
    read_req.attr_number = 2;
    read_req.attr_field = s_basic_query_attrs;  // static memory
    
    esp_zb_zcl_read_attr_cmd_req(&read_req);
    s_basic_query_pending = true;
    s_basic_query_time = millis();
    
    esp_zb_lock_release();
    
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Basic Cluster read request sent (ManufacturerName + ModelIdentifier)"));
    return true;
}

/**
 * @brief Custom Zigbee Endpoint to receive attribute reports (Client/End Device mode)
 */
class ClientZigbeeReportReceiver : public ZigbeeEP {
public:
    ClientZigbeeReportReceiver(uint8_t endpoint) : ZigbeeEP(endpoint) {
        _cluster_list = esp_zb_zcl_cluster_list_create();
        
        // Basic cluster (mandatory - SERVER for our identity)
        esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
        esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        
        // Basic cluster CLIENT - needed to receive Read Attributes Responses
        // when we query remote devices' Basic Cluster (ManufacturerName, ModelIdentifier)
        esp_zb_attribute_list_t *basic_client_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_BASIC);
        esp_zb_cluster_list_add_custom_cluster(_cluster_list, basic_client_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Identify cluster (mandatory)
        esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
        esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        
        // CLIENT-side clusters to receive reports from sensors
        esp_zb_attribute_list_t *temp_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
        esp_zb_cluster_list_add_temperature_meas_cluster(_cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        esp_zb_attribute_list_t *humidity_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
        esp_zb_cluster_list_add_humidity_meas_cluster(_cluster_list, humidity_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        esp_zb_attribute_list_t *soil_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE);
        esp_zb_cluster_list_add_custom_cluster(_cluster_list, soil_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        esp_zb_attribute_list_t *light_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT);
        esp_zb_cluster_list_add_illuminance_meas_cluster(_cluster_list, light_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        esp_zb_attribute_list_t *pressure_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT);
        esp_zb_cluster_list_add_pressure_meas_cluster(_cluster_list, pressure_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        esp_zb_attribute_list_t *power_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
        esp_zb_cluster_list_add_power_config_cluster(_cluster_list, power_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
        
        // Tuya-specific cluster 0xEF00 (CLIENT to receive Tuya DP reports)
        esp_zb_attribute_list_t *tuya_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC);
        if (tuya_cluster) {
            esp_zb_cluster_list_add_custom_cluster(_cluster_list, tuya_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Tuya cluster 0xEF00 added (CLIENT)"));
        }
        
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
        
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Report receiver endpoint created"));
    }
    
    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute, 
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) return;
        
        // Handle Basic Cluster responses (string attributes: manufacturer/model)
        if (cluster_id == ZB_ZCL_CLUSTER_ID_BASIC) {
            s_basic_query_pending = false;
            client_handleBasicClusterResponse(src_address.u.short_addr, attribute);
            return;  // Don't process as sensor data
        }
        
        // Clear pending flag – response arrived (or unsolicited report)
        s_read_pending = false;
        
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Report received: cluster=0x%04X, attr=0x%04X\n"),
                     cluster_id, attribute->id);
        
        uint64_t ieee_addr = 0;
        for (const auto& dev : client_discovered_devices) {
            if (dev.short_addr == src_address.u.short_addr) {
                ieee_addr = dev.ieee_addr;
                break;
            }
        }
        
        ZigbeeSensor::zigbee_attribute_callback(
            ieee_addr,
            src_endpoint,
            cluster_id,
            attribute->id,
            extractAttributeValue(attribute),
            0
        );
    }
    
private:
    int32_t extractAttributeValue(const esp_zb_zcl_attribute_t *attr) {
        if (!attr || !attr->data.value) return 0;
        switch (attr->data.type) {
            case ESP_ZB_ZCL_ATTR_TYPE_S8:  return (int32_t)(*(int8_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_S16: return (int32_t)(*(int16_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_S32: return *(int32_t*)attr->data.value;
            case ESP_ZB_ZCL_ATTR_TYPE_U8:  return (int32_t)(*(uint8_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_U16: return (int32_t)(*(uint16_t*)attr->data.value);
            case ESP_ZB_ZCL_ATTR_TYPE_U32: return (int32_t)(*(uint32_t*)attr->data.value);
            default:
                DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// ========== Client Tuya support ==========

/**
 * @brief Resolve IEEE address from short address in Client mode
 * Checks local cache first, then queries the Zigbee stack.
 */
static uint64_t client_resolve_ieee(uint16_t short_addr) {
    for (const auto& dev : client_discovered_devices) {
        if (dev.short_addr == short_addr) return dev.ieee_addr;
    }
    esp_zb_ieee_addr_t raw_ieee;
    if (esp_zb_ieee_address_by_short(short_addr, raw_ieee) != ESP_OK) return 0;
    uint64_t ieee64 = 0;
    for (int i = 7; i >= 0; i--) ieee64 = (ieee64 << 8) | raw_ieee[i];
    if (ieee64 == 0) return 0;

    // Auto-discover: add to device list
    ZigbeeDeviceInfo info = {};
    info.ieee_addr = ieee64;
    info.short_addr = short_addr;
    info.endpoint = 1;
    info.is_new = true;
    strncpy(info.manufacturer, "unknown", sizeof(info.manufacturer) - 1);
    strncpy(info.model_id, "unknown", sizeof(info.model_id) - 1);
    client_discovered_devices.push_back(info);
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Auto-discovered device: ieee=0x%016llX short=0x%04X\n"),
                 (unsigned long long)ieee64, short_addr);
    return ieee64;
}

/**
 * @brief APS indication handler for Tuya DP protocol (Client mode)
 * Intercepts Tuya cluster 0xEF00 frames and converts them to standard
 * attribute reports that the sensor system can process.
 */
static bool client_tuya_aps_indication_handler(esp_zb_apsde_data_ind_t ind) {
    if (ind.cluster_id != ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) return false;

    if (!ind.asdu || ind.asdu_length < 9) {
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Frame too short (%u bytes)\n"), ind.asdu_length);
        return true;
    }

    uint8_t command_id = ind.asdu[2];
    if (command_id != TUYA_CMD_DATA_RESPONSE && command_id != TUYA_CMD_DATA_REPORT) {
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Ignoring command 0x%02X\n"), command_id);
        return true;
    }

    uint64_t ieee_addr = client_resolve_ieee(ind.src_short_addr);

    DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Processing DP: cmd=0x%02X len=%u src=0x%04X\n"),
                 command_id, ind.asdu_length, ind.src_short_addr);

    uint32_t offset = 5;  // After ZCL header (3) + Tuya seq (2)
    while (offset + 4 <= ind.asdu_length) {
        uint8_t dp_number = ind.asdu[offset];
        uint8_t dp_type = ind.asdu[offset + 1];
        uint16_t dp_len = ((uint16_t)ind.asdu[offset + 2] << 8) | ind.asdu[offset + 3];
        offset += 4;

        if (offset + dp_len > ind.asdu_length) break;

        int32_t dp_value = 0;
        if (dp_type == TUYA_DP_TYPE_VALUE && dp_len == 4) {
            dp_value = (int32_t)(((uint32_t)ind.asdu[offset] << 24) |
                                 ((uint32_t)ind.asdu[offset + 1] << 16) |
                                 ((uint32_t)ind.asdu[offset + 2] << 8) |
                                 ((uint32_t)ind.asdu[offset + 3]));
        } else if (dp_type == TUYA_DP_TYPE_ENUM || dp_type == TUYA_DP_TYPE_BOOL) {
            dp_value = (int32_t)ind.asdu[offset];
        } else if (dp_len <= 4) {
            for (uint16_t j = 0; j < dp_len; j++) dp_value = (dp_value << 8) | ind.asdu[offset + j];
        }

        DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] DP %d: type=%d len=%d value=%ld\n"),
                     dp_number, dp_type, dp_len, dp_value);

        // Map Tuya DPs to ZCL cluster/attribute and route through the sensor callback
        uint16_t mapped_cluster = 0;
        uint16_t mapped_attr = 0;
        bool mapped = true;
        switch (dp_number) {
            case TUYA_DP_SOIL_MOISTURE:
                mapped_cluster = ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE;
                mapped_attr = 0x0000 | TUYA_REPORT_FLAG_PRESCALED;
                break;
            case TUYA_DP_TEMPERATURE:
                mapped_cluster = ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT;
                mapped_attr = 0x0000 | TUYA_REPORT_FLAG_PRESCALED;
                break;
            case TUYA_DP_BATTERY:
                mapped_cluster = ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
                mapped_attr = 0x0021 | TUYA_REPORT_FLAG_PRESCALED;
                break;
            case TUYA_DP_TEMPERATURE_UNIT:
                mapped = false;
                break;
            default:
                DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Unhandled DP %d\n"), dp_number);
                mapped = false;
                break;
        }

        if (mapped) {
            ZigbeeSensor::zigbee_attribute_callback(
                ieee_addr, ind.src_endpoint, mapped_cluster,
                mapped_attr, dp_value, ind.lqi);
        }

        offset += dp_len;
    }

    return true;  // Consumed
}

// ========== Client NVRAM erase ==========

static bool client_erase_zigbee_nvram() {
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] No zb_storage partition to erase"));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Erasing Zigbee NVRAM partition..."));
    esp_err_t err = esp_partition_erase_range(zb_partition, 0, zb_partition->size);
    if (err != ESP_OK) {
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Failed to erase NVRAM: %s\n"), esp_err_to_name(err));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] NVRAM erased successfully"));
    return true;
}

// ========== Client-specific start/stop ==========

static void client_zigbee_start_internal() {
    if (!ieee802154_is_zigbee_client()) {
        static bool mode_warning_shown = false;
        if (!mode_warning_shown) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Not in ZIGBEE_CLIENT mode - End Device disabled"));
            mode_warning_shown = true;
        }
        return;
    }

    if (client_zigbee_initialized) return;

    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Starting Zigbee END DEVICE..."));
    
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] ERROR: No zb_storage partition found!"));
        return;
    }

    // First-boot NVRAM reset for clean End Device state
    static bool first_boot_check_done = false;
    if (!first_boot_check_done) {
        first_boot_check_done = true;
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] First boot - erasing NVRAM for clean End Device state"));
        client_erase_zigbee_nvram();
    }
    
    if (client_zigbee_needs_nvram_reset) {
        client_zigbee_needs_nvram_reset = false;
        client_erase_zigbee_nvram();
    }
   
    esp_zb_radio_config_t radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE };
    esp_zb_host_config_t host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE };
    
    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);    

    if (WiFi.getMode() != WIFI_MODE_NULL) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] WiFi active - coexistence base already configured"));
        // NOTE: Per-packet PTI must be set AFTER Zigbee.begin() — ieee802154_mac_init()
        // inside esp_zb_start() resets all PTI values to defaults.
    } else {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] No WiFi - Zigbee has full radio access (Ethernet mode)"));
    }
    
    client_reportReceiver = new ClientZigbeeReportReceiver(10);
    Zigbee.addEndpoint(client_reportReceiver);
    client_reportReceiver->setManufacturerAndModel("OpenSprinkler", "ZigbeeReceiver");

    // Optionally restrict Zigbee to a specific channel.
#ifdef ZIGBEE_COEX_CHANNEL_MASK
    Zigbee.setPrimaryChannelMask(ZIGBEE_COEX_CHANNEL_MASK);
    DEBUG_PRINTF("[ZIGBEE-CLIENT] Primary channel mask set to 0x%08X\n",
                 (unsigned)ZIGBEE_COEX_CHANNEL_MASK);
#else
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Using default channel mask (all channels 11-26)"));
#endif
    
    // Configure ZBOSS memory before esp_zb_init() (called inside Zigbee.begin()).
    // End Devices need smaller tables than the default (64).
    esp_zb_overall_network_size_set(16);
    esp_zb_io_buffer_size_set(32);
    esp_zb_scheduler_queue_size_set(40);
    
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Starting as END DEVICE (WiFi coexistence supported)"));
    
    if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] ERROR: Failed to start Zigbee End Device!"));
        delete client_reportReceiver;
        client_reportReceiver = nullptr;
        return;
    }

    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Zigbee End Device started, searching for network..."));
    client_zigbee_initialized = true;

    // Register APS handler for Tuya DP protocol (cluster 0xEF00)
    esp_zb_aps_data_indication_handler_register(client_tuya_aps_indication_handler);
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Tuya APS indication handler registered"));

    // Apply persistent 802.15.4 coexistence config AFTER Zigbee.begin().
    // ieee802154_mac_init() resets all PTI to defaults during init.
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        esp_ieee802154_coex_config_t coex_cfg = {
            .idle    = IEEE802154_IDLE,
            .txrx    = IEEE802154_LOW,
            .txrx_at = IEEE802154_LOW,
        };
        esp_ieee802154_set_coex_config(coex_cfg);
        esp_coex_ieee802154_ack_pti_set(IEEE802154_LOW);
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] 802.15.4 coex PTI set to LOW (persistent, post-init)"));
    }
}

static void client_zigbee_stop_internal() {
    // Zigbee End Device stays running permanently (non-Matter mode).
    // The Arduino Zigbee library does NOT support stop+restart.
    if (!client_zigbee_initialized) return;
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] stop() called — Zigbee stays active (permanent mode)"));
}

static bool client_zigbee_ensure_started_internal() {
    if (client_zigbee_initialized) {
        return true;
    }
    
    // Never start Zigbee in SOFTAP mode (RF conflict)
    wifi_mode_t wmode = WiFi.getMode();
    if (wmode == WIFI_MODE_AP) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Cannot start in SOFTAP mode"));
        return false;
    }
    
    bool is_ethernet = (wmode == WIFI_MODE_NULL);
    if (!is_ethernet && WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] WiFi not connected, waiting..."));
        return false;
    }
    
    if (is_ethernet && !os.network_connected()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Ethernet not connected, waiting..."));
        return false;
    }
    
    client_zigbee_start_internal();
    return client_zigbee_initialized;
}

static void client_zigbee_loop_internal() {
    if (!client_zigbee_initialized) return;

    if (radio_arbiter_is_web_priority_active()) {
        return;
    }

    // NOTE: No idle timeout - Arduino Zigbee library does NOT support
    // Zigbee.stop() + Zigbee.begin() restart (causes Load access fault).
    // Once started, Zigbee stays running until reboot.
    
    static bool last_connected = false;
    bool connected = Zigbee.started() && Zigbee.connected();
    
    if (connected != last_connected) {
        if (connected) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Connected to Zigbee network!"));
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Disconnected from Zigbee network"));
        }
        last_connected = connected;
        client_zigbee_connected = connected;
    }
    
    // Process pending Basic Cluster queries for newly discovered devices
    if (connected && !client_basic_query_queue.empty()) {
        // Timeout stale queries
        if (s_basic_query_pending && millis() - s_basic_query_time > CLIENT_BASIC_QUERY_TIMEOUT_MS) {
            s_basic_query_pending = false;
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Basic Cluster query timed out"));
        }
        
        if (!s_basic_query_pending && !s_read_pending) {
            auto& item = client_basic_query_queue.front();
            // Wait for delay after discovery before querying
            if (millis() - item.discovered_time >= CLIENT_BASIC_QUERY_DELAY_MS) {
                if (client_query_basic_cluster(item.ieee_addr, item.endpoint)) {
                    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query sent for 0x%016llX\n"),
                                 (unsigned long long)item.ieee_addr);
                }
                // Remove from queue regardless of success (avoid infinite retries)
                client_basic_query_queue.erase(client_basic_query_queue.begin());
            }
        }
    }
    
    // Auto-discover: scan configured sensors for devices needing Basic Cluster query
    static unsigned long last_basic_scan = 0;
    if (connected && millis() - last_basic_scan > 30000) {  // Check every 30s
        last_basic_scan = millis();
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
            ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
            if (zb->device_ieee == 0 || zb->basic_cluster_queried) continue;
            if (zb->zb_manufacturer[0] != '\0' || zb->zb_model[0] != '\0') {
                zb->basic_cluster_queried = true;  // Already have info
                continue;
            }
            // Check if already queued
            bool already_queued = false;
            for (const auto& q : client_basic_query_queue) {
                if (q.ieee_addr == zb->device_ieee) { already_queued = true; break; }
            }
            if (!already_queued) {
                ClientBasicQueryItem item = {};
                item.ieee_addr = zb->device_ieee;
                item.short_addr = 0;  // Will be resolved by query function
                item.endpoint = zb->endpoint;
                item.discovered_time = millis();
                client_basic_query_queue.push_back(item);
                DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Queued Basic Cluster query for sensor '%s' (0x%016llX)\n"),
                             zb->name, (unsigned long long)zb->device_ieee);
            }
        }
    }
}

/**
 * @brief Send a ZCL Read Attributes request to a remote Zigbee device (Client mode)
 *
 * Converts the 64-bit IEEE address to a network short address, then sends
 * a unicast ZCL Read Attributes command. The response arrives via the
 * Arduino Zigbee library's action handler and is dispatched to
 * ClientZigbeeReportReceiver::zbAttributeRead(), which calls
 * ZigbeeSensor::zigbee_attribute_callback().
 *
 * IMPORTANT SAFETY NOTES:
 *   - ALL ZBOSS API calls (incl. address lookup) must run under esp_zb_lock
 *   - attr_field must point to STATIC memory because ZBOSS processes the
 *     command asynchronously in the Zigbee_main task; a stack pointer would
 *     be dangling by the time the task picks up the request → crash
 *   - Only one outstanding read request at a time (guarded by _pending flag)
 *
 * @param device_ieee  64-bit IEEE address of the target device
 * @param endpoint     Target endpoint on the remote device
 * @param cluster_id   ZCL cluster to read from
 * @param attribute_id ZCL attribute to read
 * @return true if request was sent, false on error
 */
static bool client_read_remote_attribute(uint64_t device_ieee, uint8_t endpoint,
                                         uint16_t cluster_id, uint16_t attribute_id) {
    if (!radio_arbiter_allow_zigbee_active_ops()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Active read deferred: web priority"));
        return false;
    }

    if (!client_zigbee_initialized || !client_reportReceiver) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Cannot read: Zigbee not initialized"));
        return false;
    }
    
    if (!Zigbee.started() || !Zigbee.connected()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Cannot read: not connected to network"));
        return false;
    }
    
    if (device_ieee == 0) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Cannot read: no device IEEE address configured"));
        return false;
    }
    
    // Prevent overlapping requests – the static attr buffer can only hold one
    if (s_read_pending) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Previous read still pending, skipping"));
        return false;
    }
    
    // Convert uint64_t IEEE to esp_zb_ieee_addr_t (uint8_t[8], little-endian)
    esp_zb_ieee_addr_t ieee_le;
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }
    
    // EVERYTHING that touches ZBOSS internals must run under the lock,
    // including the address-table lookup.
    esp_zb_lock_acquire(portMAX_DELAY);
    
    // Look up short address from the network address table
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    if (short_addr == 0xFFFF || short_addr == 0xFFFE) {
        esp_zb_lock_release();
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Device 0x%016llX not in address table (short=0x%04X)\n"),
                     (unsigned long long)device_ieee, short_addr);
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Device must first join the same Zigbee network"));
        return false;
    }
    
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Reading attr: ieee=0x%016llX short=0x%04X ep=%d cluster=0x%04X attr=0x%04X\n"),
                 (unsigned long long)device_ieee, short_addr, endpoint, cluster_id, attribute_id);
    
    // Store attribute_id in static memory so the pointer remains valid
    // until Zigbee_main processes the request.
    s_read_attr_id = attribute_id;
    
    // Build ZCL Read Attributes request
    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));
    
    read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;  // unicast by short addr
    read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;  // our endpoint
    read_req.clusterID = cluster_id;
    read_req.attr_number = 1;
    read_req.attr_field = &s_read_attr_id;  // MUST be static – not a stack variable!
    
    // Send request (lock already held)
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&read_req);
    s_read_pending = true;
    
    esp_zb_lock_release();
    
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Read request sent (TSN=%d)\n"), tsn);
    return true;
}

// ==========================================================================
// PUBLIC API: Runtime dispatch based on ieee802154_get_mode()
//
// These functions enforce the mutual exclusivity rules:
//   DISABLED       → no-op
//   MATTER         → no-op (Zigbee not allowed)
//   ZIGBEE_GATEWAY → delegates to sensor_zigbee_gw_*()
//   ZIGBEE_CLIENT  → delegates to client_*_internal()
// ==========================================================================

void sensor_zigbee_factory_reset() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_factory_reset();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        client_zigbee_needs_nvram_reset = true;
        DEBUG_PRINTLN(F("[ZIGBEE] Factory reset scheduled for next start"));
    }
    // DISABLED or MATTER: no-op
}

void sensor_zigbee_stop() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_stop();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        client_zigbee_stop_internal();
    }
}

void sensor_zigbee_start() {
    IEEE802154Mode mode = ieee802154_get_mode();
    
    // Enforce mutual exclusivity: only start Zigbee in Zigbee modes
    if (mode == IEEE802154Mode::IEEE_DISABLED) {
        DEBUG_PRINTLN(F("[ZIGBEE] 802.15.4 disabled - Zigbee not available"));
        return;
    }
    if (mode == IEEE802154Mode::IEEE_MATTER) {
        DEBUG_PRINTLN(F("[ZIGBEE] Matter mode active - Zigbee not available"));
        return;
    }
    
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_start();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        client_zigbee_start_internal();
    }
}

bool sensor_zigbee_is_active() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        return sensor_zigbee_gw_is_active();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return client_zigbee_initialized;
    }
    return false;
}

bool sensor_zigbee_ensure_started() {
    IEEE802154Mode mode = ieee802154_get_mode();

    // If Zigbee is already running, allow immediate access.
    // Otherwise, block until sensor_api_connect() has been called.
    // This prevents Zigbee from auto-starting (via sensor reads) before
    // Block Zigbee auto-start until BLE has been initialized first.
    // sensor_radio_early_init() or sensor_api_connect() handles this.
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY && !sensor_zigbee_gw_is_active()) {
        if (!is_radio_early_init_done() && !is_sensor_api_connected()) return false;
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT && !client_zigbee_initialized) {
        if (!is_radio_early_init_done() && !is_sensor_api_connected()) return false;
    }

    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        return sensor_zigbee_gw_ensure_started();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return client_zigbee_ensure_started_internal();
    }
    // DISABLED or MATTER: Zigbee not available
    return false;
}

void sensor_zigbee_open_network(uint16_t duration) {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_open_network(duration);
    } else {
        DEBUG_PRINTLN(F("[ZIGBEE] open_network only available in ZIGBEE_GATEWAY mode"));
    }
}

void sensor_zigbee_loop() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_loop();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        client_zigbee_loop_internal();
    }
}

// ==========================================================================
// SHARED ZigbeeSensor class methods (defined once, used by both modes)
// ==========================================================================

/**
 * @brief Zigbee attribute report callback - updates sensor data
 * In Gateway mode, this is called via gw_process_reports cache flush.
 * In Client mode, this is called directly from the ZigbeeEP callback.
 */
void ZigbeeSensor::zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint,
                                            uint16_t cluster_id, uint16_t attr_id,
                                            int32_t value, uint8_t lqi) {
    // In Gateway mode, delegate to the cache-based processor
    if (ieee802154_is_zigbee_gw()) {
        sensor_zigbee_gw_process_reports(ieee_addr, endpoint, cluster_id, attr_id, value, lqi);
        return;
    }

    // Client mode: direct processing
    DEBUG_PRINTF(F("[ZIGBEE] Attribute callback: cluster=0x%04X, attr=0x%04X, value=%d\n"),
                 cluster_id, attr_id, value);

    // Check Tuya pre-scaled flag: Tuya DP values don't need ZCL conversion
    bool is_tuya_prescaled = (attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0;
    uint16_t raw_attr_id = attr_id & ~TUYA_REPORT_FLAG_PRESCALED;

    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) {
            continue;
        }
        
        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        
        bool matches = (zb_sensor->cluster_id == cluster_id &&
                       zb_sensor->attribute_id == raw_attr_id);
        
        if (zb_sensor->device_ieee != 0 && ieee_addr != 0) {
            matches = matches && (zb_sensor->device_ieee == ieee_addr);
        }
        // Match endpoint if the sensor specifies a non-default one
        if (zb_sensor->endpoint != 1 && endpoint != 0) {
            matches = matches && (zb_sensor->endpoint == endpoint);
        }
        
        if (matches) {
            DEBUG_PRINTF(F("[ZIGBEE] Updating sensor: %s%s\n"), sensor->name,
                         is_tuya_prescaled ? " (Tuya)" : "");
            
            zb_sensor->last_native_data = value;
            double converted_value = (double)value;
            
            // Apply ZCL standard conversions (skip for Tuya pre-scaled values)
            if (!is_tuya_prescaled) {
                if (cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && raw_attr_id == 0x0000) {
                    converted_value = value / 100.0;
                } else if (cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && raw_attr_id == 0x0000) {
                    converted_value = value / 100.0;
                } else if (cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && raw_attr_id == 0x0000) {
                    converted_value = value / 100.0;
                } else if (cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && raw_attr_id == 0x0000) {
                    converted_value = value / 10.0;
                } else if (cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT && raw_attr_id == 0x0000) {
                    if (value > 0 && value <= 65534) {
                        converted_value = pow(10.0, (value - 1.0) / 10000.0);
                    } else {
                        converted_value = 0.0;
                    }
                } else if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && raw_attr_id == 0x0021) {
                    converted_value = value / 2.0;
                    zb_sensor->last_battery = (uint32_t)converted_value;
                }
            } else {
                // Tuya: battery is already raw %
                if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && raw_attr_id == 0x0021) {
                    zb_sensor->last_battery = (uint32_t)converted_value;
                }
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
            
            zb_sensor->last_data = converted_value;
            zb_sensor->last_lqi = lqi;
            zb_sensor->flags.data_ok = true;
            zb_sensor->repeat_read = 1;
            
            DEBUG_PRINTF(F("[ZIGBEE] Raw: %d -> Converted: %.2f\n"), value, converted_value);
            // Don't break — multiple logical sensors may reference the
            // same physical device (same IEEE/cluster/attr).  Continue
            // iterating so every matching sensor is updated.
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
        ArduinoJson::JsonVariantConst val = obj["device_ieee"];
        if (val.is<const char*>()) {
            const char *ieee_str = val.as<const char*>();
            if (ieee_str && ieee_str[0]) {
                device_ieee = parseIeeeAddress(ieee_str);
            }
        } else if (val.is<uint64_t>() || val.is<long long>() || val.is<unsigned long long>()) {
            device_ieee = val.as<uint64_t>();
        } else if (val.is<long>()) {
            device_ieee = (uint64_t)val.as<long>();
        }
    }
    // Backward compatibility: old firmware may have used "ieee" or "ieee_addr"
    if (device_ieee == 0) {
        for (const char* altKey : {"ieee", "ieee_addr"}) {
            if (obj.containsKey(altKey)) {
                ArduinoJson::JsonVariantConst val = obj[altKey];
                if (val.is<const char*>()) {
                    const char *s = val.as<const char*>();
                    if (s && s[0]) { device_ieee = parseIeeeAddress(s); break; }
                } else {
                    uint64_t v = val.as<uint64_t>();
                    if (v != 0) { device_ieee = v; break; }
                }
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
    // Basic Cluster info (persisted from device query)
    if (obj.containsKey("zb_manufacturer")) {
        const char *mfr = obj["zb_manufacturer"].as<const char*>();
        if (mfr) {
            strncpy(zb_manufacturer, mfr, sizeof(zb_manufacturer) - 1);
            zb_manufacturer[sizeof(zb_manufacturer) - 1] = '\0';
        }
    }
    if (obj.containsKey("zb_model")) {
        const char *mdl = obj["zb_model"].as<const char*>();
        if (mdl) {
            strncpy(zb_model, mdl, sizeof(zb_model) - 1);
            zb_model[sizeof(zb_model) - 1] = '\0';
        }
    }
    // Restore battery level (UINT32_MAX = not yet measured)
    if (obj.containsKey("battery")) {
        last_battery = obj["battery"].as<uint32_t>();
    }
    // If we already have Basic Cluster info from config, mark as queried
    if (zb_manufacturer[0] != '\0' || zb_model[0] != '\0') {
        basic_cluster_queried = true;
    }
}

void ZigbeeSensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    
    if (device_ieee != 0) {
        char ieee_str[20];
        getIeeeString(ieee_str, sizeof(ieee_str));
        obj["device_ieee"] = String(ieee_str);  // String() forces ArduinoJson to copy
    }
    obj["endpoint"] = endpoint;
    obj["cluster_id"] = cluster_id;
    obj["attribute_id"] = attribute_id;
    // Battery level — only persist when actually measured
    if (last_battery != UINT32_MAX) obj["battery"] = last_battery;
    obj["lqi"] = last_lqi;
    // Basic Cluster info (persisted)
    if (zb_manufacturer[0] != '\0') {
        obj["zb_manufacturer"] = zb_manufacturer;
    }
    if (zb_model[0] != '\0') {
        obj["zb_model"] = zb_model;
    }
}

bool ZigbeeSensor::init() {
    return true;
}

void ZigbeeSensor::deinit() {
    device_bound = false;
    flags.data_ok = false;
}

int ZigbeeSensor::read(unsigned long time) {
    IEEE802154Mode mode = ieee802154_get_mode();
    
    // Enforce mutual exclusivity: reject reads in non-Zigbee modes
    if (mode != IEEE802154Mode::IEEE_ZIGBEE_GATEWAY && mode != IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // CRITICAL: Do NOT use Zigbee in WiFi SOFTAP mode
    if (WiFi.getMode() == WIFI_MODE_AP) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Start Zigbee if not yet running
    if (!sensor_zigbee_is_active() && !sensor_zigbee_ensure_started()) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // =========================================================================
    // GATEWAY MODE: Reports arrive asynchronously via gw_updateSensorFromReport
    // which sets data_ok=true and repeat_read=1.  No active read is needed.
    // Simply return the current data_ok status.
    // =========================================================================
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        if (flags.data_ok) {
            repeat_read = 0;
            return HTTP_RQT_SUCCESS;
        }

        // No data yet: in gateway mode we normally rely on passive reports,
        // but some devices do not report reliably. Trigger a best-effort
        // active read as fallback at a bounded interval.
        uint poll_interval = read_interval ? read_interval : 60;
        if (poll_interval < 15) poll_interval = 15;
        if (radio_arbiter_allow_zigbee_active_ops() && device_ieee != 0 && (last_read == 0 || time >= last_read + poll_interval)) {
            if (sensor_zigbee_gw_read_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                last_read = time;
            }
        }

        // Keep repeat_read so we retry quickly
        repeat_read = 1;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // =========================================================================
    // CLIENT MODE: Active 2-phase read (request → wait → return)
    // =========================================================================
    
    // Prevent concurrent access
    int& active_sensor = client_active_zigbee_sensor;
    
    if (active_sensor > 0 && active_sensor != (int)nr) {
        repeat_read = 1;
        SensorBase *t = sensor_by_nr(active_sensor);
        if (!t || !t->flags.enable) {
            active_sensor = 0;
        }
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (repeat_read == 0) {
        if (active_sensor != (int)nr) {
            active_sensor = nr;
        }
        
        if (!Zigbee.started()) {
            flags.data_ok = false;
            active_sensor = 0;
            return HTTP_RQT_NOT_RECEIVED;
        }
        
        // Client mode: actively poll the remote device
        if (device_ieee != 0) {
            if (!client_read_remote_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                DEBUG_PRINTLN(F("[ZIGBEE] Active read request failed"));
            }
        }
        
        repeat_read = 1;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
        
    } else {
        repeat_read = 0;
        active_sensor = 0;
        last_read = time;
        
        return flags.data_ok ? HTTP_RQT_SUCCESS : HTTP_RQT_NOT_RECEIVED;
    }
}

// Compatibility functions for old API
void sensor_zigbee_bind_device(uint nr, const char *device_ieee_str) {
    DEBUG_PRINTF(F("[ZIGBEE] Bind request for sensor %d: %s\n"), nr, device_ieee_str ? device_ieee_str : "null");
    
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

int sensor_zigbee_get_discovered_devices(ZigbeeDeviceInfo* devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;
    
    if (ieee802154_is_zigbee_gw()) {
        // Gateway mode: delegate to the GW module's own device list
        return sensor_zigbee_gw_get_discovered_devices(devices, max_devices);
    }
    // Client mode: use the local client list
    int count = ((int)client_discovered_devices.size() < max_devices)
                    ? (int)client_discovered_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &client_discovered_devices[i], sizeof(ZigbeeDeviceInfo));
    }
    return count;
}

void sensor_zigbee_clear_new_device_flags() {
    if (ieee802154_is_zigbee_gw()) {
        sensor_zigbee_gw_clear_new_device_flags();
        return;
    }
    for (auto& dev : client_discovered_devices) {
        dev.is_new = false;
    }
}

bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    IEEE802154Mode mode = ieee802154_get_mode();
    
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return client_read_remote_attribute(device_ieee, endpoint, cluster_id, attribute_id);
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        // Gateway mode: not needed, reports arrive passively
        DEBUG_PRINTLN(F("[ZIGBEE] Gateway mode uses passive reports, no active read needed"));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE] Active attribute reading not available in current mode"));
    return false;
}

void ZigbeeSensor::emitJson(BufferFiller& bfill) const {
    SensorBase::emitJson(bfill);
}

void ZigbeeSensor::updateBasicClusterInfo(uint64_t ieee_addr, const char* manufacturer, const char* model) {
    if (ieee_addr == 0) return;
    
    bool updated = false;
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
        if (zb->device_ieee != ieee_addr) continue;
        
        if (manufacturer && manufacturer[0]) {
            strncpy(zb->zb_manufacturer, manufacturer, sizeof(zb->zb_manufacturer) - 1);
            zb->zb_manufacturer[sizeof(zb->zb_manufacturer) - 1] = '\0';
            updated = true;
        }
        if (model && model[0]) {
            strncpy(zb->zb_model, model, sizeof(zb->zb_model) - 1);
            zb->zb_model[sizeof(zb->zb_model) - 1] = '\0';
            updated = true;
        }
        zb->basic_cluster_queried = true;
        
        DEBUG_PRINTF(F("[ZIGBEE] Updated sensor '%s' Basic Cluster info: mfr=\"%s\" model=\"%s\"\n"),
                     zb->name, zb->zb_manufacturer, zb->zb_model);
    }
    
    if (updated) {
        sensor_save();
        DEBUG_PRINTLN(F("[ZIGBEE] Sensor config saved with Basic Cluster info"));
    }
}

unsigned char ZigbeeSensor::getUnitId() const {
    if (assigned_unitid > 0) {
        return assigned_unitid;
    }
    
    switch(cluster_id) {
        case ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE:
        case ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
        case ZB_ZCL_CLUSTER_ID_LEAF_WETNESS:
        case ZB_ZCL_CLUSTER_ID_POWER_CONFIG:
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
