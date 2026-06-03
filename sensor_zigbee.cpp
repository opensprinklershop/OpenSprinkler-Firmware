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
#include "sensor_zigbee_client_expose.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"
#include "ieee802154_config.h"

extern OpenSprinkler os;

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include <esp_partition.h>
#include <vector>
#include <WiFi.h>
#include <esp_err.h>
extern "C" {
}
#include <esp_ieee802154.h>
#include "esp_zigbee_core.h"
#include "esp_zigbee_secur.h"
#include "Zigbee.h"
#include "ZigbeeEP.h"
#include <esp_attr.h>

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
static bool client_zigbee_start_failed = false;
static int client_active_zigbee_sensor = 0;
static std::vector<ZigbeeDeviceInfo> client_discovered_devices;
static constexpr size_t CLIENT_DISCOVERED_MAX = 128;
static unsigned long client_join_window_end = 0;
static constexpr bool ZIGBEE_CLIENT_MINIMAL_PAIRING = true;
static const char* ZIGBEE_CLIENT_INIT_FLAG = "/zb_client_init.flag";
static const char* ZIGBEE_CLIENT_RESET_FLAG = "/zb_client_reset.flag";
static bool client_zigbee_leave_in_progress = false;  // Flag to prevent snapshot access during leave operations

struct ClientZigbeeNetworkSnapshot {
    bool started;
    bool raw_connected;
    bool valid_join;
    bool factory_new;
    uint8_t channel;
    uint16_t pan_id;
    uint16_t short_addr;
    esp_zb_nwk_device_type_t role;
    uint32_t primary_mask;
    uint32_t secondary_mask;
    uint32_t active_mask;
    int8_t tx_power;
    uint64_t ext_pan;
    uint64_t own_ieee;
};


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
#define ZB_ZCL_CLUSTER_ID_METERING                  0x0702

// Tuya-specific protocol definitions (cluster 0xEF00)
#define ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC             0xEF00
#define TUYA_CMD_DATA_REQUEST   0x00
#define TUYA_CMD_DATA_RESPONSE  0x01
#define TUYA_CMD_DATA_REPORT    0x02
#define TUYA_TYPE_RAW    0x00
#define TUYA_TYPE_BOOL   0x01
#define TUYA_TYPE_VALUE  0x02  // 4-byte big-endian integer
#define TUYA_TYPE_STRING 0x03
#define TUYA_TYPE_ENUM   0x04
#define TUYA_TYPE_BITMAP 0x05
// Flag to mark Tuya reports as pre-scaled (no ZCL scaling needed)
#define TUYA_REPORT_FLAG_PRESCALED     0x8000
#define TUYA_REPORT_TYPE_SHIFT         8
#define TUYA_REPORT_TYPE_MASK          0x0F00
#define TUYA_REPORT_DP_MASK            0x00FF

static uint16_t tuya_report_attr(uint8_t dp_number, uint8_t dp_type) {
    return TUYA_REPORT_FLAG_PRESCALED |
           (((uint16_t)dp_type << TUYA_REPORT_TYPE_SHIFT) & TUYA_REPORT_TYPE_MASK) |
           (uint16_t)dp_number;
}

static uint16_t zigbee_report_attr_id(uint16_t attr_id) {
    return (attr_id & TUYA_REPORT_FLAG_PRESCALED) ? (attr_id & TUYA_REPORT_DP_MASK) : attr_id;
}

static uint8_t tuya_report_type(uint16_t attr_id) {
    return (attr_id & TUYA_REPORT_FLAG_PRESCALED) ? (uint8_t)((attr_id & TUYA_REPORT_TYPE_MASK) >> TUYA_REPORT_TYPE_SHIFT) : 0;
}

static uint32_t zigbee_battery_percent_from_report(bool is_tuya_report, uint16_t raw_attr_id, uint8_t tuya_type, int16_t configured_tuya_battery_dp, int32_t value) {
    if (is_tuya_report) {
        if (configured_tuya_battery_dp >= 0 && raw_attr_id == (uint16_t)configured_tuya_battery_dp && tuya_type == TUYA_TYPE_ENUM) {
            if (value <= 0) return 0;
            if (value == 1) return 50;
            return 100;
        }
        if (value < 0) return 0;
        return (value > 100) ? 100 : (uint32_t)value;
    }

    if (value < 0) return 0;
    uint32_t batt_pct = (uint32_t)(value / 2);
    return (batt_pct > 100) ? 100 : batt_pct;
}

// Basic Cluster attribute IDs
#define ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID       0x0004
#define ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID        0x0005
#define ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID            0x0007

class ClientZigbeeReportReceiver;
static ClientZigbeeReportReceiver* client_reportReceiver = nullptr;
static uint64_t client_resolve_ieee(uint16_t short_addr);

// Flag to track if we need to reset NVRAM
static bool client_zigbee_needs_nvram_reset = false;
static const uint8_t ZIGBEE_CLIENT_ENDPOINT_SCHEMA_VERSION = 2;
static const char* ZIGBEE_CLIENT_SCHEMA_FLAG = "/zb_client_schema.ver";

// Static storage for active attribute read requests.
// attr_field in esp_zb_zcl_read_attr_cmd_t is a pointer that ZBOSS
// processes asynchronously in the Zigbee_main task.  Using a stack
// variable would leave a dangling pointer → Load access fault.
static uint16_t s_read_attr_id = 0;
static bool     s_read_pending = false;
// true when data arrived as a response to our active ZCL Read Attributes request
// (vs. an unsolicited attribute report pushed by the device)
static bool     s_last_data_was_solicited = false;
// Set when any sensor's comm_mode is updated from the ZigBee callback task;
// causes sensor_save() to be called from the main loop on the next loop tick.
static bool     s_comm_mode_changed = false;

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
    uint8_t attempts;
};
static std::vector<ClientBasicQueryItem> client_basic_query_queue;

static bool client_is_basic_query_queued(uint64_t ieee_addr) {
    for (const auto& q : client_basic_query_queue) {
        if (q.ieee_addr == ieee_addr) return true;
    }
    return false;
}

static void client_queue_basic_cluster_query(uint64_t ieee_addr, uint16_t short_addr, uint8_t endpoint, unsigned long delay_ms) {
    if (ieee_addr == 0) return;
    if (endpoint == 0) endpoint = 1;
    for (auto& q : client_basic_query_queue) {
        if (q.ieee_addr == ieee_addr) {
            q.short_addr = short_addr;
            q.endpoint = endpoint;
            return;
        }
    }
    ClientBasicQueryItem item = {};
    item.ieee_addr = ieee_addr;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.discovered_time = millis() - CLIENT_BASIC_QUERY_DELAY_MS + delay_ms;
    item.attempts = 0;
    client_basic_query_queue.push_back(item);
}

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
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster attr 0x%04X: not a string (type=0x%02X)\n"),
                     // attribute->id, attribute->data.type);
        return;
    }
    
    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster attr 0x%04X = \"%s\" (from short=0x%04X)\n"),
                 // attribute->id, str_buf, short_addr);
    
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
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query: device 0x%016llX not in address table\n"),
                     // (unsigned long long)device_ieee);
        return false;
    }
    
    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Querying Basic Cluster: ieee=0x%016llX short=0x%04X ep=%d\n"),
                 // (unsigned long long)device_ieee, short_addr, endpoint);
    
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
    
    // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Basic Cluster read request sent (ManufacturerName + ModelIdentifier)"));
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
        
        // Identify cluster (mandatory)
        esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
        esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        if (!ZIGBEE_CLIENT_MINIMAL_PAIRING) {
            // Basic cluster CLIENT - needed to receive Read Attributes Responses
            // when we query remote devices' Basic Cluster (ManufacturerName, ModelIdentifier)
            esp_zb_attribute_list_t *basic_client_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_BASIC);
            esp_zb_cluster_list_add_custom_cluster(_cluster_list, basic_client_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

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

            esp_zb_attribute_list_t *meter_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_METERING);
            if (meter_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, meter_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
            }

            // Tuya-specific cluster 0xEF00 (CLIENT to receive Tuya DP reports)
            esp_zb_attribute_list_t *tuya_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC);
            if (tuya_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, tuya_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Tuya cluster 0xEF00 added (CLIENT)"));
            }
        }
        
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ZIGBEE_CLIENT_MINIMAL_PAIRING ? ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID : ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
        
        // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Report receiver endpoint created"));
    }

    // Called by ZigbeeCore when a device announces itself.
    // Track announced device in client_discovered_devices so /zd can show it.
    void findEndpoint(esp_zb_zdo_match_desc_req_param_t *cmd_req) override {
        if (!cmd_req) return;
        uint16_t short_addr = cmd_req->dst_nwk_addr;
        // DEBUG_PRINTF("[ZIGBEE-CLIENT] Device announced: short=0x%04X\n", short_addr);
        client_resolve_ieee(short_addr);
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
        // Capture whether this was solicited BEFORE clearing the flag so that
        // zigbee_attribute_callback can set comm_mode correctly.
        s_last_data_was_solicited = s_read_pending;
        s_read_pending = false;
        
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Report received: cluster=0x%04X, attr=0x%04X\n"),
                     // cluster_id, attribute->id);
        
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
            case 0x24:  // uint40
            case 0x25: { // uint48
                uint8_t len = (attr->data.type == 0x24) ? 5 : 6;
                const uint8_t* raw = (const uint8_t*)attr->data.value;
                uint32_t value = 0;
                for (uint8_t i = 0; i < len && i < 4; i++) {
                    value |= ((uint32_t)raw[i]) << (8 * i);
                }
                if (value > 0x7FFFFFFFUL) value = 0x7FFFFFFFUL;
                return (int32_t)value;
            }
            default:
                // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Unknown attribute type: 0x%02X\n"), attr->data.type);
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

    // If IEEE already known, update short addr and return
    for (auto& dev : client_discovered_devices) {
        if (dev.ieee_addr == ieee64) {
            dev.short_addr = short_addr;
            dev.endpoint = 1;
            dev.is_new = true;
            if ((dev.manufacturer[0] == '\0' || dev.model_id[0] == '\0') && !client_is_basic_query_queued(ieee64)) {
                client_queue_basic_cluster_query(ieee64, short_addr, 1, 1000UL);
            }
            return ieee64;
        }
    }

    // Auto-discover: add to device list
    ZigbeeDeviceInfo info = {};
    info.ieee_addr = ieee64;
    info.short_addr = short_addr;
    info.endpoint = 1;
    info.is_new = true;
    info.manufacturer[0] = '\0';
    info.model_id[0] = '\0';
    info.date_code[0] = '\0';
    info.sw_build_id[0] = '\0';
    info.app_version = 0xFF;
    info.stack_version = 0xFF;
    info.hw_version = 0xFF;
    if (client_discovered_devices.size() >= CLIENT_DISCOVERED_MAX) {
        client_discovered_devices.erase(client_discovered_devices.begin());
    }
    client_discovered_devices.push_back(info);

    // Queue for Basic Cluster query (get manufacturer/model asynchronously).
    // Use a short delay so the device has time to be ready for queries.
    client_queue_basic_cluster_query(ieee64, short_addr, 1, 2000UL);
    
    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Auto-discovered device: ieee=0x%016llX short=0x%04X, queued for Basic Cluster query\n"),
                 // (unsigned long long)ieee64, short_addr);
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
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Frame too short (%u bytes)\n"), ind.asdu_length);
        return true;
    }

    DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] APS ind: src=0x%04X ep=%u len=%u\n"),
                 ind.src_short_addr, ind.src_endpoint, ind.asdu_length);

    uint8_t command_id = ind.asdu[2];
    if (command_id != TUYA_CMD_DATA_RESPONSE && command_id != TUYA_CMD_DATA_REPORT) {
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Ignoring command 0x%02X\n"), command_id);
        return true;
    }

    uint64_t ieee_addr = client_resolve_ieee(ind.src_short_addr);

    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] Processing DP: cmd=0x%02X len=%u src=0x%04X\n"),
                 // command_id, ind.asdu_length, ind.src_short_addr);

    uint32_t offset = 5;  // After ZCL header (3) + Tuya seq (2)
    while (offset + 4 <= ind.asdu_length) {
        uint8_t dp_number = ind.asdu[offset];
        uint8_t dp_type = ind.asdu[offset + 1];
        uint16_t dp_len = ((uint16_t)ind.asdu[offset + 2] << 8) | ind.asdu[offset + 3];
        offset += 4;

        if (offset + dp_len > ind.asdu_length) break;

        int32_t dp_value = 0;
        if (dp_type == TUYA_TYPE_VALUE && dp_len == 4) {
            dp_value = (int32_t)(((uint32_t)ind.asdu[offset] << 24) |
                                 ((uint32_t)ind.asdu[offset + 1] << 16) |
                                 ((uint32_t)ind.asdu[offset + 2] << 8) |
                                 ((uint32_t)ind.asdu[offset + 3]));
        } else if (dp_type == TUYA_TYPE_ENUM || dp_type == TUYA_TYPE_BOOL) {
            dp_value = (int32_t)ind.asdu[offset];
        } else if (dp_len <= 4) {
            for (uint16_t j = 0; j < dp_len; j++) dp_value = (dp_value << 8) | ind.asdu[offset + j];
        }

        DEBUG_PRINTF(F("[ZIGBEE-CLIENT][TUYA] DP %u: type=%u len=%u value=%ld\n"),
                 dp_number, dp_type, dp_len, dp_value);

        ZigbeeSensor::zigbee_attribute_callback(
            ieee_addr, ind.src_endpoint, ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC,
            tuya_report_attr(dp_number, dp_type),
            dp_value, ind.lqi);

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
        // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] No zb_storage partition to erase"));
        return false;
    }
    
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Erasing Zigbee NVRAM partition..."));
    esp_err_t err = esp_partition_erase_range(zb_partition, 0, zb_partition->size);
    if (err != ESP_OK) {
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Failed to erase NVRAM: %s\n"), esp_err_to_name(err));
        return false;
    }

    // Remove persistent init flag so the next reboot treats this as a clean install
    if (file_exists(ZIGBEE_CLIENT_INIT_FLAG)) {
        remove_file(ZIGBEE_CLIENT_INIT_FLAG);
    }
    if (file_exists(ZIGBEE_CLIENT_SCHEMA_FLAG)) {
        remove_file(ZIGBEE_CLIENT_SCHEMA_FLAG);
    }

    // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] NVRAM erased successfully"));
    return true;
}

static bool client_zigbee_schema_matches() {
    uint8_t stored_schema = 0;
    if (!file_exists(ZIGBEE_CLIENT_SCHEMA_FLAG)) {
        return false;
    }
    file_read_block(ZIGBEE_CLIENT_SCHEMA_FLAG, &stored_schema, 0, sizeof(stored_schema));
    return stored_schema == ZIGBEE_CLIENT_ENDPOINT_SCHEMA_VERSION;
}

static void client_write_zigbee_schema_flags() {
    const char oneByte = '1';
    file_write_block(ZIGBEE_CLIENT_INIT_FLAG, &oneByte, 0, 1);
    file_write_block(ZIGBEE_CLIENT_SCHEMA_FLAG, &ZIGBEE_CLIENT_ENDPOINT_SCHEMA_VERSION, 0,
                     sizeof(ZIGBEE_CLIENT_ENDPOINT_SCHEMA_VERSION));
}

static void client_mark_zigbee_nvram_reset() {
    const char oneByte = '1';
    file_write_block(ZIGBEE_CLIENT_RESET_FLAG, &oneByte, 0, 1);
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

    if (client_zigbee_initialized || client_zigbee_start_failed) return;

    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Starting Zigbee End Device (join external coordinator)..."));
    
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage"
    );
    
    if (!zb_partition) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] ERROR: No zb_storage partition found!"));
        return;
    }

    // First-install NVRAM reset: erase only once (persistent flag survives reboots/OTA updates).
    // Do NOT use a static bool — it resets to false after every reboot/firmware update,
    // which would wipe ZigBee network join credentials on every restart.
    bool first_install = !file_exists(ZIGBEE_CLIENT_INIT_FLAG);
    bool reset_requested = file_exists(ZIGBEE_CLIENT_RESET_FLAG) || client_zigbee_needs_nvram_reset;
    bool schema_mismatch = !client_zigbee_schema_matches();
    bool reset_zigbee_nvram = first_install || reset_requested || schema_mismatch;
    if (reset_zigbee_nvram) {
        if (first_install) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] First install detected — erasing NVRAM for clean End Device state"));
        } else if (reset_requested) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Reset request detected — erasing NVRAM for fresh End Device pairing"));
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Endpoint schema changed — erasing stale Zigbee NVRAM"));
        }
        client_zigbee_needs_nvram_reset = false;
        client_erase_zigbee_nvram();
        if (file_exists(ZIGBEE_CLIENT_RESET_FLAG)) {
            remove_file(ZIGBEE_CLIENT_RESET_FLAG);
        }
        // Create the flag file so subsequent reboots skip the erase.
        // client_erase_zigbee_nvram() removes it, so recreate it after every intentional reset.
        client_write_zigbee_schema_flags();
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

    if (ZIGBEE_CLIENT_MINIMAL_PAIRING) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Minimal pairing mode: only endpoint 10 (Basic + Identify) is exposed"));
    } else {
        // Create ZCL endpoints that expose OS sensor values, zone control,
        // program control, and rain sensor state to the ZigBee hub.
        client_expose_create_endpoints();
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Created exposed OS ZCL endpoints"));
    }

    // Optionally restrict Zigbee to a specific channel.
#ifdef ZIGBEE_COEX_CHANNEL_MASK
    Zigbee.setPrimaryChannelMask(ZIGBEE_COEX_CHANNEL_MASK);
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Primary channel mask set to 0x%08X\n"),
                 (unsigned)ZIGBEE_COEX_CHANNEL_MASK);
#else
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Using default channel mask (all channels 11-26)"));
#endif
    
    // Configure ZBOSS memory before esp_zb_init() (called inside Zigbee.begin()).
    // End Device sizing — matches the values that worked in v240.196.
    // Coordinator-sized buffers (64/80/80) prevent the ZCZR stack from
    // completing network-steering on the ED path on ESP32-C5.
    esp_zb_overall_network_size_set(16);
    esp_zb_io_buffer_size_set(32);
    esp_zb_scheduler_queue_size_set(40);

    Zigbee.setTimeout(8000);
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Starting as END_DEVICE (ZCZR stack, joining external coordinator)"));
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] ZBOSS config: network_size=16, io_buffer=32, scheduler_queue=40\n"));
    esp_zb_secur_link_key_exchange_required_set(false);
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Install code disabled; using normal Zigbee commissioning"));
    
    if (!Zigbee.begin(ZIGBEE_END_DEVICE, reset_zigbee_nvram)) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] ERROR: Failed to start Zigbee End Device!"));
        client_zigbee_start_failed = true;
        // ZigbeeCore has already received endpoint pointers. The Arduino Zigbee
        // wrapper does not support removing endpoints after a failed begin(), so
        // keep the objects alive and block retries until reboot.
        return;
    }

    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Zigbee End Device started, awaiting network steering / join..."));
    client_zigbee_initialized = true;

    // Register APS handler for Tuya DP protocol (cluster 0xEF00)
    esp_zb_aps_data_indication_handler_register(client_tuya_aps_indication_handler);
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Tuya APS indication handler registered"));
}

static void client_zigbee_stop_internal() {
    // Zigbee End Device stays running permanently (non-Matter mode).
    // The Arduino Zigbee library does NOT support stop+restart.
    if (!client_zigbee_initialized) return;
    // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] stop() called — Zigbee stays active (permanent mode)"));
}

static bool client_zigbee_ensure_started_internal() {
    if (client_zigbee_initialized) {
        return true;
    }

    if (client_zigbee_start_failed) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Previous start failed - retry blocked until reboot"));
        return false;
    }
    
    // Never start Zigbee in SOFTAP mode (RF conflict)
    wifi_mode_t wmode = WiFi.getMode();
    if (wmode == WIFI_MODE_AP) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Cannot start in SOFTAP mode"));
        return false;
    }
    
    bool is_ethernet = (wmode == WIFI_MODE_NULL);
    if (!is_ethernet && WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] WiFi not connected yet - starting Zigbee anyway"));
    } else if (is_ethernet && !os.network_connected()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Ethernet not connected yet - starting Zigbee anyway"));
    }
    
    client_zigbee_start_internal();
    return client_zigbee_initialized;
}

static ClientZigbeeNetworkSnapshot client_zigbee_get_network_snapshot();
static void client_zigbee_log_network_info();

static bool client_zigbee_start_network_steering(uint16_t duration) {
    if (!client_zigbee_ensure_started_internal()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] network steering: Zigbee not started"));
        return false;
    }

    uint16_t dur = duration;
    if (dur < 1) dur = 1;
    if (dur > 120) dur = 120;

    client_join_window_end = millis() + (unsigned long)dur * 1000UL;

    ClientZigbeeNetworkSnapshot snapshot = client_zigbee_get_network_snapshot();
    if (snapshot.raw_connected) {
        client_zigbee_log_network_info();
        if (!snapshot.valid_join) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Network steering blocked: stale/self network state must be cleared first"));
            return false;
        }
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Already connected to a valid Zigbee network"));
        return true;
    }

    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Starting network steering for %u seconds (join search)\n"), dur);
    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Waiting for coordinator signal... (check Fritzbox/hub logs)"));
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    bool factory_new = esp_zb_bdb_is_factory_new();
    esp_zb_lock_release();

    if (err != ESP_OK) {
        if (factory_new) {
            DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Network steering already active in factory-new state, keeping join window: %u seconds\n"), dur);
            return true;
        }
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Network steering failed to start: %s\n"), esp_err_to_name(err));
        return false;
    }

    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Network steering initiated, join window: %u seconds\n"), dur);
    return true;
}

static uint64_t client_zigbee_ieee_to_u64(const esp_zb_ieee_addr_t addr) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | addr[i];
    }
    return value;
}

static ClientZigbeeNetworkSnapshot client_zigbee_get_network_snapshot() {
    ClientZigbeeNetworkSnapshot snapshot = {};
    snapshot.started = Zigbee.started();
    snapshot.raw_connected = snapshot.started && Zigbee.connected();
    snapshot.valid_join = false;
    snapshot.factory_new = false;
    snapshot.role = ESP_ZB_DEVICE_TYPE_NONE;
    snapshot.pan_id = 0xFFFF;
    snapshot.short_addr = 0xFFFF;

    if (!snapshot.started) {
        return snapshot;
    }

    // Prevent snapshot access during leave operations to avoid ZBOSS assertion crashes
    if (client_zigbee_leave_in_progress) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Snapshot blocked: leave operation in progress"));
        return snapshot;
    }

    esp_zb_ieee_addr_t ext_pan_raw;
    esp_zb_ieee_addr_t own_ieee_raw;

    esp_zb_lock_acquire(portMAX_DELAY);
    snapshot.channel = esp_zb_get_current_channel();
    snapshot.pan_id = esp_zb_get_pan_id();
    snapshot.short_addr = esp_zb_get_short_address();
    snapshot.role = esp_zb_get_network_device_role();
    snapshot.primary_mask = esp_zb_get_primary_network_channel_set();
    snapshot.secondary_mask = esp_zb_get_secondary_network_channel_set();
    snapshot.active_mask = esp_zb_get_channel_mask();
    snapshot.factory_new = esp_zb_bdb_is_factory_new();
    esp_zb_get_extended_pan_id(ext_pan_raw);
    esp_zb_get_long_address(own_ieee_raw);
    esp_zb_get_tx_power(&snapshot.tx_power);
    esp_zb_lock_release();

    snapshot.ext_pan = client_zigbee_ieee_to_u64(ext_pan_raw);
    snapshot.own_ieee = client_zigbee_ieee_to_u64(own_ieee_raw);
    snapshot.valid_join = snapshot.raw_connected &&
        snapshot.role == ESP_ZB_DEVICE_TYPE_ED &&
        snapshot.short_addr != 0x0000 &&
        snapshot.short_addr != 0xFFFE &&
        snapshot.short_addr != 0xFFFF &&
        snapshot.pan_id != 0xFFFF &&
        snapshot.channel >= 11 && snapshot.channel <= 26 &&
        snapshot.ext_pan != 0 &&
        snapshot.ext_pan != 0xFFFFFFFFFFFFFFFFULL &&
        snapshot.ext_pan != snapshot.own_ieee;

    return snapshot;
}

static void client_zigbee_log_network_info() {
    ClientZigbeeNetworkSnapshot snapshot = client_zigbee_get_network_snapshot();

    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Network: rawConnected=%d validJoin=%d factoryNew=%d channel=%u PAN=0x%04X extPAN=0x%08lX%08lX short=0x%04X role=%d ownIEEE=0x%08lX%08lX\n"),
                 snapshot.raw_connected ? 1 : 0,
                 snapshot.valid_join ? 1 : 0,
                 snapshot.factory_new ? 1 : 0,
                 snapshot.channel,
                 snapshot.pan_id,
                 (unsigned long)(snapshot.ext_pan >> 32), (unsigned long)(snapshot.ext_pan & 0xFFFFFFFF),
                 snapshot.short_addr,
                 (int)snapshot.role,
                 (unsigned long)(snapshot.own_ieee >> 32), (unsigned long)(snapshot.own_ieee & 0xFFFFFFFF));
    DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Radio: primaryMask=0x%08lX secondaryMask=0x%08lX activeMask=0x%08lX txPower=%d dBm ZBOSS=%s\n"),
                 (unsigned long)snapshot.primary_mask, (unsigned long)snapshot.secondary_mask,
                 (unsigned long)snapshot.active_mask, (int)snapshot.tx_power,
                 esp_zb_get_version_string());

    if (snapshot.raw_connected && !snapshot.valid_join) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] WARNING: ZBOSS reports connected, but network state is not a valid End Device join. Use /zl or /zj?reset=1 before retrying pairing."));
    }
}

bool sensor_zigbee_client_factory_new() {
    if (ieee802154_get_mode() != IEEE802154Mode::IEEE_ZIGBEE_CLIENT || !Zigbee.started()) {
        return false;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    bool factory_new = esp_zb_bdb_is_factory_new();
    esp_zb_lock_release();
    return factory_new;
}

static void client_zigbee_loop_internal() {
    if (!client_zigbee_initialized) return;

    static unsigned long last_list_log = 0;
    if (millis() - last_list_log > 60000) {
        last_list_log = millis();
        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Status: connected=%d, discovered=%u devices\n"),
                     client_zigbee_connected ? 1 : 0,
                     (unsigned)client_discovered_devices.size());
    }

    // End temporary join/scan window.
    if (client_join_window_end != 0 && millis() > client_join_window_end) {
        client_join_window_end = 0;
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Join window closed"));
    }

    // NOTE: No idle timeout - Arduino Zigbee library does NOT support
    // Zigbee.stop() + Zigbee.begin() restart (causes Load access fault).
    // Once started, Zigbee stays running until reboot.

    // Persist comm_mode changes detected by the ZigBee callback task.
    // sensor_save() must run on the main loop thread, not the ZigBee task.
    if (s_comm_mode_changed) {
        s_comm_mode_changed = false;
        sensor_save();
    }

    static bool last_connected = false;
    ClientZigbeeNetworkSnapshot snapshot = client_zigbee_get_network_snapshot();
    bool connected = snapshot.valid_join;
    static bool stale_connected_logged = false;
    if (snapshot.raw_connected && !snapshot.valid_join && !stale_connected_logged) {
        stale_connected_logged = true;
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Detected invalid/stale Zigbee connected state"));
        client_zigbee_log_network_info();
    } else if (!snapshot.raw_connected) {
        stale_connected_logged = false;
    }

    if (connected != last_connected) {
        if (connected) {
            DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Connected to Zigbee network!"));
            client_zigbee_log_network_info();
            // On connect: immediately queue Basic Cluster queries for all paired sensors
            // that are still missing manufacturer/model info (e.g. after reboot before
            // the device has re-announced itself).
            if (!ZIGBEE_CLIENT_MINIMAL_PAIRING) {
                SensorIterator it_boot = sensors_iterate_begin();
                SensorBase* sb;
                while ((sb = sensors_iterate_next(it_boot)) != NULL) {
                    if (!sb || sb->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sb);
                    if (zb->device_ieee == 0 || zb->basic_cluster_queried) continue;
                    // Already has info — just mark queried
                    if (zb->zb_manufacturer[0] != '\0' || zb->zb_model[0] != '\0') {
                        zb->basic_cluster_queried = true;
                        continue;
                    }
                    bool already_queued = false;
                    for (const auto& q : client_basic_query_queue) {
                        if (q.ieee_addr == zb->device_ieee) { already_queued = true; break; }
                    }
                    if (!already_queued) {
                        ClientBasicQueryItem item = {};
                        item.ieee_addr = zb->device_ieee;
                        item.short_addr = 0;
                        item.endpoint = zb->endpoint;
                        // Use a short delay (2s) so the stack has time to settle
                        item.discovered_time = millis() - CLIENT_BASIC_QUERY_DELAY_MS + 2000UL;
                        client_basic_query_queue.push_back(item);
                        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Boot: queued Basic Cluster query for sensor '%s' (0x%016llX)\n"),
                                     zb->name, (unsigned long long)zb->device_ieee);
                    }
                }
            }
        } else {
            if (snapshot.raw_connected) {
                DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Ignoring invalid/stale Zigbee connected state"));
                client_zigbee_log_network_info();
            } else {
                DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Disconnected from Zigbee network"));
            }
        }
        last_connected = connected;
        client_zigbee_connected = connected;
    }
    
    // Process pending Basic Cluster queries for newly discovered devices
    if (connected && !ZIGBEE_CLIENT_MINIMAL_PAIRING && !client_basic_query_queue.empty()) {
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
                    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query sent for 0x%016llX\n"),
                                 // (unsigned long long)item.ieee_addr);
                    client_basic_query_queue.erase(client_basic_query_queue.begin());
                } else {
                    item.attempts++;
                    if (item.attempts >= 6) {
                        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query send failed for 0x%016llX after %u attempts\n"),
                                     (unsigned long long)item.ieee_addr, item.attempts);
                        client_basic_query_queue.erase(client_basic_query_queue.begin());
                    } else {
                        item.discovered_time = millis() - CLIENT_BASIC_QUERY_DELAY_MS + 5000UL;
                        DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Basic Cluster query send failed for 0x%016llX, retry %u queued\n"),
                                     (unsigned long long)item.ieee_addr, item.attempts);
                    }
                }
            }
        }
    }
    
    // Auto-discover: scan configured sensors for devices needing Basic Cluster query
    // Start at millis()-25000 so first check fires ~5s after boot rather than 30s.
    static unsigned long last_basic_scan = (unsigned long)-25000UL;
    if (connected && !ZIGBEE_CLIENT_MINIMAL_PAIRING && millis() - last_basic_scan > 30000) {  // Check every 30s
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
                // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Queued Basic Cluster query for sensor '%s' (0x%016llX)\n"),
                             // zb->name, (unsigned long long)zb->device_ieee);
            }
        }
    }

    // Update exposed ZCL endpoints (sensor values, zone states, rain sensor)
    if (connected && !ZIGBEE_CLIENT_MINIMAL_PAIRING) {
        client_expose_update_loop();
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
        // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Previous read still pending, skipping"));
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
        // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Device 0x%016llX not in address table (short=0x%04X)\n"),
                     // (unsigned long long)device_ieee, short_addr);
        // DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Device must first join the same Zigbee network"));
        return false;
    }
    
    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Reading attr: ieee=0x%016llX short=0x%04X ep=%d cluster=0x%04X attr=0x%04X\n"),
                 // (unsigned long long)device_ieee, short_addr, endpoint, cluster_id, attribute_id);
    
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
    
    // DEBUG_PRINTF(F("[ZIGBEE-CLIENT] Read request sent (TSN=%d)\n"), tsn);
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
        client_mark_zigbee_nvram_reset();
        DEBUG_PRINTLN(F("[ZIGBEE] Client factory reset scheduled for next start"));
    }
    // DISABLED or MATTER: no-op
}

bool sensor_zigbee_leave_network() {
    if (ieee802154_get_mode() != IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return false;
    }

    client_join_window_end = 0;
    client_zigbee_connected = false;
    client_basic_query_queue.clear();
    s_basic_query_pending = false;
    s_read_pending = false;

    if (client_zigbee_initialized && Zigbee.started()) {
        DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Leaving Zigbee network via local reset"));
        client_zigbee_leave_in_progress = true;
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_bdb_reset_via_local_action();
        vTaskDelay(100 / portTICK_PERIOD_MS);  // Give ZBOSS time to process reset
        esp_zb_lock_release();
        client_zigbee_leave_in_progress = false;
        return true;
    }

    DEBUG_PRINTLN(F("[ZIGBEE-CLIENT] Zigbee not running; erasing client NVRAM now"));
    return client_erase_zigbee_nvram();
}


void sensor_zigbee_pause() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY || mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        Zigbee.stop();
        esp_ieee802154_sleep();
    }
}

void sensor_zigbee_resume() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY || mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        esp_ieee802154_receive();
        Zigbee.start();
    }
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
        // DEBUG_PRINTLN(F("[ZIGBEE] 802.15.4 disabled - Zigbee not available"));
        return;
    }
    if (mode == IEEE802154Mode::IEEE_MATTER) {
        // DEBUG_PRINTLN(F("[ZIGBEE] Matter mode active - Zigbee not available"));
        return;
    }
    
    // ZigBee gateway requires Ethernet — WiFi shares the 2.4GHz radio
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY && !useEth) {
        DEBUG_PRINTLN(F("[ZIGBEE] Gateway mode requires Ethernet — not starting"));
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

bool sensor_zigbee_is_connected() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        return Zigbee.started() && Zigbee.connected();
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return client_zigbee_get_network_snapshot().valid_join;
    }
    return false;
}

uint16_t sensor_zigbee_get_join_window_remaining() {
    if (ieee802154_get_mode() == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        return sensor_zigbee_gw_get_join_window_remaining();
    }
    if (ieee802154_get_mode() != IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return 0;
    }
    if (client_join_window_end == 0) {
        return 0;
    }
    unsigned long now = millis();
    if ((long)(client_join_window_end - now) <= 0) {
        return 0;
    }
    unsigned long remaining_ms = client_join_window_end - now;
    return (uint16_t)((remaining_ms + 999UL) / 1000UL);
}

bool sensor_zigbee_ensure_started() {
    IEEE802154Mode mode = ieee802154_get_mode();

    // ZigBee gateway requires Ethernet
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY && !useEth) {
        return false;
    }

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

bool sensor_zigbee_open_network(uint16_t duration) {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        sensor_zigbee_gw_open_network(duration);
        return true;
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        if (duration == 0) {
            // Explicit close semantics are only needed for gateway mode.
            // For client mode, just report success to keep API symmetric.
            return true;
        }
        return client_zigbee_start_network_steering(duration);
    }
    // DEBUG_PRINTLN(F("[ZIGBEE] open_network only available in ZIGBEE mode"));
    return false;
}

// ---------------------------------------------------------------------------
// Vendor API lookup — called from sensor_zigbee_loop() once per interval.
// Queries opensprinklershop.de/zigbee/devices_api.php?firmware=1 for sensors
// that have mfr+model but no vendor name yet.  One request per call so we do
// not block the main loop.
// ---------------------------------------------------------------------------
#if defined(ESP32)
static void sensor_zigbee_do_vendor_lookups() {
    if (WiFi.status() != WL_CONNECTED) return;
    static unsigned long s_last_attempt_ms = 0;
    // Don't hammer the API — try at most once every 15 s
    if (s_last_attempt_ms != 0 && millis() - s_last_attempt_ms < 15000UL) return;

    SensorIterator it = sensors_iterate_begin();
    SensorBase* s;
    while ((s = sensors_iterate_next(it)) != NULL) {
        if (!s || s->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(s);
        if (!zb->zb_vendor_pending) continue;
        if (!zb->zb_manufacturer[0] || !zb->zb_model[0]) {
            DEBUG_PRINTF(F("[ZB] Vendor lookup skipped for '%s': incomplete Basic Cluster data mfr=\"%s\" model=\"%s\" ieee=0x%016llX\n"),
                         zb->name,
                         zb->zb_manufacturer[0] ? zb->zb_manufacturer : "",
                         zb->zb_model[0] ? zb->zb_model : "",
                         (unsigned long long)zb->device_ieee);
            zb->zb_vendor_pending = false; // nothing to look up
            continue;
        }

        zb->zb_vendor_pending = false; // clear flag regardless of result
        s_last_attempt_ms = millis();

        // URL-encode the manufacturer and model strings (simple: replace space
        // with %20; the values from Basic Cluster rarely contain other chars)
        char mfr_enc[64], mdl_enc[64];
        // Cheap percent-encoding for space and a few common chars
        auto pct_encode = [](const char* src, char* dst, size_t dsz) {
            size_t j = 0;
            for (size_t i = 0; src[i] && j + 4 < dsz; i++) {
                unsigned char c = (unsigned char)src[i];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
                    dst[j++] = (char)c;
                } else {
                    j += snprintf(dst + j, dsz - j, "%%%02X", c);
                }
            }
            dst[j] = '\0';
        };
        pct_encode(zb->zb_manufacturer, mfr_enc, sizeof(mfr_enc));
        pct_encode(zb->zb_model,        mdl_enc, sizeof(mdl_enc));

        snprintf(ether_buffer, ETHER_BUFFER_SIZE - 1,
            "GET /zigbee/devices_api.php?manufacturer=%s&model=%s&firmware=1"
            " HTTP/1.0\r\nHost: opensprinklershop.de\r\n"
            "User-Agent: %s\r\nConnection: close\r\n\r\n",
            mfr_enc, mdl_enc, user_agent_string);

        DEBUG_PRINTF(F("[ZB] Vendor lookup: mfr=%s model=%s\n"), zb->zb_manufacturer, zb->zb_model);

        int ret = os.send_http_request("opensprinklershop.de", 443, ether_buffer, NULL, true, 8000);
        if (ret == HTTP_RQT_SUCCESS) {
            // Response body is in ether_buffer — find "vendor":"..."
            const char *p = strstr(ether_buffer, "\"vendor\":");
            if (p) {
                p += 9;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    char vnd[32];
                    int i = 0;
                    while (*p && *p != '"' && i < (int)sizeof(vnd) - 1)
                        vnd[i++] = *p++;
                    vnd[i] = '\0';
                    if (i > 0) {
                        DEBUG_PRINTF(F("[ZB] Vendor found: \"%s\" for %s|%s\n"),
                                     vnd, zb->zb_manufacturer, zb->zb_model);
                        // Apply to all sensors sharing the same IEEE address
                        uint64_t ieee = zb->device_ieee;
                        SensorIterator it2 = sensors_iterate_begin();
                        SensorBase* s2;
                        while ((s2 = sensors_iterate_next(it2)) != NULL) {
                            if (!s2 || s2->type != SENSOR_ZIGBEE) continue;
                            ZigbeeSensor* zb2 = static_cast<ZigbeeSensor*>(s2);
                            if (zb2->device_ieee != ieee) continue;
                            strncpy(zb2->zb_vendor, vnd, sizeof(zb2->zb_vendor) - 1);
                            zb2->zb_vendor[sizeof(zb2->zb_vendor) - 1] = '\0';
                            zb2->zb_vendor_pending = false;
                        }
                        sensor_save();
                    }
                }
            } else {
                DEBUG_PRINTF(F("[ZB] Vendor lookup returned no vendor for %s|%s. Response: %.120s\n"),
                             zb->zb_manufacturer, zb->zb_model, ether_buffer);
            }
        } else {
            DEBUG_PRINTF(F("[ZB] Vendor lookup failed (ret=%d)\n"), ret);
        }
        return; // one lookup per call
    }
}
#endif // ESP32

void sensor_zigbee_loop() {
#if defined(ESP32)
    sensor_zigbee_do_vendor_lookups();
#endif
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
    // DEBUG_PRINTF(F("[ZIGBEE] Attribute callback: cluster=0x%04X, attr=0x%04X, value=%d\n"),
                 // cluster_id, attr_id, value);

    // Check Tuya pre-scaled flag: Tuya DP values don't need ZCL conversion
    bool is_tuya_prescaled = (attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0;
    uint16_t raw_attr_id = zigbee_report_attr_id(attr_id);
    uint8_t tuya_type = tuya_report_type(attr_id);

    DEBUG_PRINTF(F("[ZIGBEE-DATA] RX ieee=0x%016llX ep=%u cluster=0x%04X attr=0x%04X raw=%ld lqi=%u tuya=%u\n"),
                 (unsigned long long)ieee_addr,
                 (unsigned)endpoint,
                 (unsigned)cluster_id,
                 (unsigned)raw_attr_id,
                 (long)value,
                 (unsigned)lqi,
                 (unsigned)(is_tuya_prescaled ? 1 : 0));

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

        bool ieee_match = !(zb_sensor->device_ieee != 0 && ieee_addr != 0) || (zb_sensor->device_ieee == ieee_addr);
        bool ep_match = !(zb_sensor->endpoint != 1 && endpoint != 0) || (zb_sensor->endpoint == endpoint);
        bool is_tuya_dp_report = is_tuya_prescaled && cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC;
        if (!matches && is_tuya_dp_report && ieee_match && ep_match &&
            zb_sensor->tuya_dp_value >= 0 && raw_attr_id == (uint16_t)zb_sensor->tuya_dp_value) {
            matches = true;
        }
        
        if (matches) {
            DEBUG_PRINTF(F("[ZIGBEE] Updating sensor: %s%s\n"), sensor->name,
                         is_tuya_prescaled ? " (Tuya)" : "");

            // Battery reports must only refresh last_battery and must never
            // overwrite the sensor's primary measurement channel (last_data).
            if ((cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && raw_attr_id == 0x0021) ||
                (is_tuya_dp_report && zb_sensor->tuya_dp_battery >= 0 && raw_attr_id == (uint16_t)zb_sensor->tuya_dp_battery)) {
                uint32_t batt_pct = zigbee_battery_percent_from_report(is_tuya_prescaled, raw_attr_id, tuya_type, zb_sensor->tuya_dp_battery, value);
                zb_sensor->last_battery = batt_pct;
                zb_sensor->last_lqi = lqi;
                DEBUG_PRINTF(F("[ZIGBEE-BATT] Sensor='%s' ieee=0x%016llX ep=%u raw=%ld -> batt=%u%% lqi=%u (no last_data overwrite)\n"),
                             sensor->name,
                             (unsigned long long)ieee_addr,
                             (unsigned)endpoint,
                             (long)value,
                             (unsigned)batt_pct,
                             (unsigned)lqi);
                continue;
            }
            
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
            } else if (is_tuya_dp_report && zb_sensor->tuya_dp_value >= 0 && raw_attr_id == (uint16_t)zb_sensor->tuya_dp_value) {
                converted_value = value;
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
            zb_sensor->last_read = os.now_tz();
            zb_sensor->last = zb_sensor->last_read;

            // Always update the UNIX anchor to the actual reception time so
            // the next prediction uses real data, not the initial estimate.
            {
                uint32_t new_anchor = (uint32_t)zb_sensor->last_read;
                if (zb_sensor->join_anchor_ts != new_anchor) {
                    zb_sensor->join_anchor_ts = new_anchor;
                    s_comm_mode_changed = true;
                    DEBUG_PRINTF(F("[ZIGBEE] Predictive anchor updated for '%s': %lu (ri=%u)\n"),
                                 zb_sensor->name, (unsigned long)zb_sensor->join_anchor_ts,
                                 zb_sensor->read_interval);
                }
            }

            // Data received — release predictive boost lock early so WiFi is restored.

            // Update communication mode: distinguish unsolicited reports from
            // responses to active Read Attributes requests.
            // Battery/power reports are excluded — they don't determine data-channel mode.
            bool is_battery_report = (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                                      raw_attr_id == 0x0021);
            if (!is_battery_report) {
                ZbCommMode new_mode = s_last_data_was_solicited ? ZB_COMM_ACTIVE : ZB_COMM_REPORT;
                // Transition: UNKNOWN→any, or ACTIVE→REPORT (upgrade when device starts reporting)
                if (zb_sensor->comm_mode == ZB_COMM_UNKNOWN ||
                    (new_mode == ZB_COMM_REPORT && zb_sensor->comm_mode == ZB_COMM_ACTIVE)) {
                    if (zb_sensor->comm_mode != new_mode) {
                        zb_sensor->comm_mode = new_mode;
                        s_comm_mode_changed = true;
                        DEBUG_PRINTF(F("[ZIGBEE] '%s' comm_mode → %s\n"), sensor->name,
                                     new_mode == ZB_COMM_REPORT ? "REPORT" : "ACTIVE");
                    }
                }
            }

            DEBUG_PRINTF(F("[ZIGBEE] Raw: %d -> Converted: %.2f\n"), value, converted_value);
            DEBUG_PRINTF(F("[ZIGBEE-DATA] Sensor='%s' ieee=0x%016llX ep=%u cluster=0x%04X attr=0x%04X native=%ld data=%.3f batt=%u lqi=%u\n"),
                         sensor->name,
                         (unsigned long long)ieee_addr,
                         (unsigned)endpoint,
                         (unsigned)cluster_id,
                         (unsigned)raw_attr_id,
                         (long)zb_sensor->last_native_data,
                         converted_value,
                         (unsigned)zb_sensor->last_battery,
                         (unsigned)zb_sensor->last_lqi);
            // Don't break — multiple logical sensors may reference the
            // same physical device (same IEEE/cluster/attr).  Continue
            // iterating so every matching sensor is updated.
        } else if (is_tuya_dp_report && zb_sensor->device_ieee != 0 && zb_sensor->device_ieee == ieee_addr &&
               zb_sensor->tuya_dp_unit >= 0 && raw_attr_id == (uint16_t)zb_sensor->tuya_dp_unit) {
            zb_sensor->tuya_unit = (value < 0) ? 0xFF : (uint8_t)value;
            zb_sensor->last_lqi = lqi;
            DEBUG_PRINTF(F("[ZIGBEE-UNIT] Sensor='%s' ieee=0x%016llX ep=%u dp=%u unit=%ld lqi=%u\n"),
                 sensor->name,
                 (unsigned long long)ieee_addr,
                 (unsigned)endpoint,
                 (unsigned)raw_attr_id,
                 (long)value,
                 (unsigned)lqi);
        } else if (((cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && raw_attr_id == 0x0021) ||
                (is_tuya_dp_report && zb_sensor->tuya_dp_battery >= 0 && raw_attr_id == (uint16_t)zb_sensor->tuya_dp_battery)) &&
               zb_sensor->device_ieee != 0 && zb_sensor->device_ieee == ieee_addr) {
            // Battery report for same device: update last_battery on non-battery sensors too
            uint32_t batt_pct = zigbee_battery_percent_from_report(is_tuya_prescaled, raw_attr_id, tuya_type, zb_sensor->tuya_dp_battery, value);
            zb_sensor->last_battery = batt_pct;
            DEBUG_PRINTF(F("[ZIGBEE-BATT] Linked sensor='%s' ieee=0x%016llX ep=%u raw=%ld -> batt=%u%% lqi=%u\n"),
                         sensor->name,
                         (unsigned long long)ieee_addr,
                         (unsigned)endpoint,
                         (long)value,
                         (unsigned)batt_pct,
                         (unsigned)lqi);
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

bool sensor_zigbee_get_station_control_config(uint64_t device_ieee, ZigbeeStationControlConfig* config, uint8_t target_endpoint, uint8_t target_dp) {
    if (!config) return false;

    *config = ZigbeeStationControlConfig{};
    if (device_ieee == 0) return false;

    // 1. Try to find the configuration in the Logical Devices map first
    if (OpenSprinkler::zigbee_logical_devices_map) {
        char ieee_str[17];
        snprintf(ieee_str, sizeof(ieee_str), "%016llX", (unsigned long long)device_ieee);
        
        const ZigBeeLogicalDevice* best_logdev = nullptr;
        int best_logdev_score = -1;
        
        for (const auto& entry : *OpenSprinkler::zigbee_logical_devices_map) {
            const ZigBeeLogicalDevice& dev = entry.second.device;
            if (strncmp(dev.ieee, ieee_str, 16) != 0) continue;
            
            int score = 0;
            if (target_endpoint != 0 && dev.endpoint == target_endpoint) score += 100;
            if (target_dp != 0 && (dev.tuya_dp_value == target_dp || dev.tuya_dp_status == target_dp)) score += 200;
            
            // Check if it is a valve/switch/control device by its name or kind
            bool is_control = (strstr(dev.name, "valve") != nullptr || strstr(dev.name, "switch") != nullptr || strstr(dev.name, "state") != nullptr);
            if (is_control) score += 50;
            
            if (score > best_logdev_score) {
                best_logdev = &dev;
                best_logdev_score = score;
            }
        }
        
        if (best_logdev) {
            config->found = true;
            config->endpoint = best_logdev->endpoint ? best_logdev->endpoint : 1;
            config->control_mode = best_logdev->is_tuya ? ZB_STATION_CTRL_TUYA : ZB_STATION_CTRL_STANDARD;
            config->protocol_type = best_logdev->is_tuya ? 1 : 0;
            
            // "Wenn es für den Status kein DP gibt bzw DP < 0 ist, dann das DP des Values verwenden."
            config->dp_value = (best_logdev->tuya_dp_value >= 0) ? (uint8_t)best_logdev->tuya_dp_value : 0;
            config->dp_status = (best_logdev->tuya_dp_status >= 0) ? (uint8_t)best_logdev->tuya_dp_status : config->dp_value;
            
            DEBUG_PRINTF(F("[ZIGBEE] Resolved station control config from logical device: %s (ep=%d dp_val=%d dp_stat=%d)\n"),
                         best_logdev->name, config->endpoint, config->dp_value, config->dp_status);
            return true;
        }
    }

    // 2. Fall back to existing sensor-iteration code
    auto score_candidate = [target_endpoint, target_dp](const ZigbeeSensor* zb) -> int {
        if (!zb) return -1;
        int score = 0;
        if (target_endpoint != 0 && zb->endpoint == target_endpoint) score += 100;
        if (target_dp != 0 && (zb->tuya_dp_value == target_dp || zb->tuya_dp_status == target_dp)) score += 200;

        if (zb->control_mode == ZB_STATION_CTRL_TUYA) score += 50;
        if (zb->zb_type == 1) score += 40;
        if (zb->tuya_dp_status >= 0) score += 25;
        if (zb->tuya_dp_value >= 0) score += 20;
        if (zb->cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) score += 10;
        if (zb->endpoint != 0) score += 2;
        return score;
    };

    ZigbeeSensor* best = nullptr;
    int best_score = -1;

    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
        if (zb->device_ieee != device_ieee) continue;

        int score = score_candidate(zb);
        if (!best || score > best_score) {
            best = zb;
            best_score = score;
        }
    }

    if (!best) return false;

    config->found = true;
    config->endpoint = best->endpoint ? best->endpoint : 1;
    config->control_mode = best->control_mode;
    config->protocol_type = (best->zb_type <= 2) ? best->zb_type : 0;
    
    // Only use best sensor's Tuya DP mappings if they actually match target_dp,
    // or if target_dp is 0.
    if (target_dp == 0 || best->tuya_dp_value == target_dp || best->tuya_dp_status == target_dp) {
        config->dp_value = (best->tuya_dp_value >= 0) ? (uint8_t)best->tuya_dp_value : 0;
        config->dp_status = (best->tuya_dp_status >= 0) ? (uint8_t)best->tuya_dp_status : config->dp_value;
    } else {
        config->dp_value = target_dp;
        config->dp_status = target_dp;
    }
    return true;
}

void ZigbeeSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    // Capture current read_interval before base-class update so we can detect a change.
    uint old_ri = read_interval;
    SensorBase::fromJson(obj);
    
    // NEW: Load logical device reference (preferred method)
    if (obj.containsKey(F("zb_ieee_ref"))) {
        const char *ieee_ref = obj[F("zb_ieee_ref")].as<const char*>();
        if (ieee_ref && ieee_ref[0]) {
            strncpy(zigbee_device_ieee, ieee_ref, sizeof(zigbee_device_ieee) - 1);
            zigbee_device_ieee[sizeof(zigbee_device_ieee) - 1] = '\0';
        }
    }
    if (obj.containsKey(F("zb_logical_name"))) {
        const char *logical_name = obj[F("zb_logical_name")].as<const char*>();
        if (logical_name && logical_name[0]) {
            strncpy(zigbee_logical_name, logical_name, sizeof(zigbee_logical_name) - 1);
            zigbee_logical_name[sizeof(zigbee_logical_name) - 1] = '\0';
        }
    }
    
    // DEPRECATED: Load old fields (for backward compatibility)
    if (obj.containsKey(F("device_ieee"))) {
        ArduinoJson::JsonVariantConst val = obj[F("device_ieee")];
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
    if (obj.containsKey(F("endpoint"))) {
        endpoint = obj[F("endpoint")].as<uint8_t>();
    }
    if (obj.containsKey(F("cluster_id"))) {
        ArduinoJson::JsonVariantConst val = obj[F("cluster_id")];
        if (val.is<const char*>()) {
            const char *cluster_str = val.as<const char*>();
            if (cluster_str && (strncmp(cluster_str, "0x", 2) == 0 || strncmp(cluster_str, "0X", 2) == 0)) {
                cluster_id = (uint16_t)strtoul(cluster_str, nullptr, 16);
            } else {
                cluster_id = (uint16_t)strtoul(cluster_str, nullptr, 10);
            }
        } else {
            cluster_id = val.as<uint16_t>();
        }
    }
    if (obj.containsKey(F("attribute_id"))) {
        ArduinoJson::JsonVariantConst val = obj[F("attribute_id")];
        if (val.is<const char*>()) {
            const char *attr_str = val.as<const char*>();
            if (attr_str && (strncmp(attr_str, "0x", 2) == 0 || strncmp(attr_str, "0X", 2) == 0)) {
                attribute_id = (uint16_t)strtoul(attr_str, nullptr, 16);
            } else {
                attribute_id = (uint16_t)strtoul(attr_str, nullptr, 10);
            }
        } else {
            attribute_id = val.as<uint16_t>();
        }
    }
    if (obj.containsKey(F("zb_type"))) {
        uint8_t t = obj[F("zb_type")].as<uint8_t>();
        zb_type = (t <= 2) ? t : 0;
    } else if (obj.containsKey(F("zigbee_type"))) {
        uint8_t t = obj[F("zigbee_type")].as<uint8_t>();
        zb_type = (t <= 2) ? t : 0;
    }
    if (obj.containsKey(F("control_mode"))) {
        ArduinoJson::JsonVariantConst val = obj[F("control_mode")];
        if (val.is<const char*>()) {
            const char *mode_str = val.as<const char*>();
            if (mode_str && mode_str[0]) {
                if (!strcmp(mode_str, "tuya")) {
                    control_mode = ZB_STATION_CTRL_TUYA;
                } else if (!strcmp(mode_str, "standard")) {
                    control_mode = ZB_STATION_CTRL_STANDARD;
                } else {
                    control_mode = (uint8_t)strtoul(mode_str, nullptr, 10);
                }
            }
        } else {
            control_mode = val.as<uint8_t>();
        }
        if (control_mode > ZB_STATION_CTRL_TUYA) {
            control_mode = ZB_STATION_CTRL_TUYA;;
        }
    } else if (obj.containsKey(F("use_tuya_control"))) {
        control_mode = obj[F("use_tuya_control")].as<bool>() ? ZB_STATION_CTRL_TUYA : ZB_STATION_CTRL_STANDARD;
    }
    // Optional per-sensor Tuya DP overrides
    if (obj.containsKey(F("tuya_dp"))) {
        tuya_dp_value = obj[F("tuya_dp")].as<int16_t>();
    } else if (obj.containsKey(F("tuya_dp_value"))) {
        tuya_dp_value = obj[F("tuya_dp_value")].as<int16_t>();
    } else if (obj.containsKey(F("dp_value"))) {
        tuya_dp_value = obj[F("dp_value")].as<int16_t>();
    } else if (obj.containsKey(F("dp"))) {
        tuya_dp_value = obj[F("dp")].as<int16_t>();
    }
    if (obj.containsKey(F("tuya_dp_batt"))) {
        tuya_dp_battery = obj[F("tuya_dp_batt")].as<int16_t>();
    } else if (obj.containsKey(F("tuya_dp_battery"))) {
        tuya_dp_battery = obj[F("tuya_dp_battery")].as<int16_t>();
    } else if (obj.containsKey(F("dp_battery"))) {
        tuya_dp_battery = obj[F("dp_battery")].as<int16_t>();
    } else if (obj.containsKey(F("battery_dp"))) {
        tuya_dp_battery = obj[F("battery_dp")].as<int16_t>();
    }
    if (obj.containsKey(F("tuya_dp_unit"))) {
        tuya_dp_unit = obj[F("tuya_dp_unit")].as<int16_t>();
    } else if (obj.containsKey(F("unit_dp"))) {
        tuya_dp_unit = obj[F("unit_dp")].as<int16_t>();
    } else if (obj.containsKey(F("dp_unit"))) {
        tuya_dp_unit = obj[F("dp_unit")].as<int16_t>();
    }
    if (obj.containsKey(F("tuya_dp_status"))) {
        tuya_dp_status = obj[F("tuya_dp_status")].as<int16_t>();
    } else if (obj.containsKey(F("dp_status"))) {
        tuya_dp_status = obj[F("dp_status")].as<int16_t>();
    }
    if (obj.containsKey(F("tuya_dp_consumption"))) {
        tuya_dp_consumption = obj[F("tuya_dp_consumption")].as<int16_t>();
    } else if (obj.containsKey(F("dp_consumption"))) {
        tuya_dp_consumption = obj[F("dp_consumption")].as<int16_t>();
    }
    if (obj.containsKey(F("tuya_unit"))) {
        tuya_unit = obj[F("tuya_unit")].as<uint8_t>();
    }
    // Basic Cluster info (persisted from device query)
    if (obj.containsKey(F("zb_manufacturer"))) {
        const char *mfr = obj[F("zb_manufacturer")].as<const char*>();
        if (mfr) {
            strncpy(zb_manufacturer, mfr, sizeof(zb_manufacturer) - 1);
            zb_manufacturer[sizeof(zb_manufacturer) - 1] = '\0';
        }
    }
    if (obj.containsKey(F("zb_model"))) {
        const char *mdl = obj[F("zb_model")].as<const char*>();
        if (mdl) {
            strncpy(zb_model, mdl, sizeof(zb_model) - 1);
            zb_model[sizeof(zb_model) - 1] = '\0';
        }
    }
    if (obj.containsKey(F("zb_vendor"))) {
        const char *vnd = obj[F("zb_vendor")].as<const char*>();
        if (vnd) {
            strncpy(zb_vendor, vnd, sizeof(zb_vendor) - 1);
            zb_vendor[sizeof(zb_vendor) - 1] = '\0';
        }
    }
    // Restore battery level (UINT32_MAX = not yet measured)
    if (obj.containsKey(F("battery"))) {
        last_battery = obj[F("battery")].as<uint32_t>();
    }
    // Restore communication mode (active read / spontaneous report / unknown)
    if (obj.containsKey(F("comm_mode"))) {
        uint8_t cm = obj[F("comm_mode")].as<uint8_t>();
        if (cm <= (uint8_t)ZB_COMM_ACTIVE) {
            comm_mode = (ZbCommMode)cm;
        }
    }
    // Configured ZCL report interval (for predictive boost scheduling)
    if (obj.containsKey(F("rpt_intv"))) {
        report_interval_s = obj[F("rpt_intv")].as<uint32_t>();
    }
    // Phase anchor for UNIX-based predictive boost (survives reboots)
    if (obj.containsKey(F("join_anchor"))) {
        join_anchor_ts = obj[F("join_anchor")].as<uint32_t>();
    }
    // If the user changed read_interval, invalidate the prediction anchor and ZCL
    // report configuration — the new interval will be sent via ConfigureReporting
    // and the anchor will be re-established on the next successful report.
    if (old_ri != 0 && obj.containsKey(F("ri")) && read_interval != old_ri) {
        join_anchor_ts = 0;
        report_interval_s = 0;
        DEBUG_PRINTF(F("[ZIGBEE] read_interval changed %u\u2192%u \u2014 prediction anchor cleared\n"),
                     old_ri, read_interval);
    }
    // If we already have Basic Cluster info from config, mark as queried
    if (zb_manufacturer[0] != '\0' || zb_model[0] != '\0') {
        basic_cluster_queried = true;
    }
    // Schedule vendor API lookup if we have mfr+model but no vendor yet
    if (zb_manufacturer[0] != '\0' && zb_model[0] != '\0' && zb_vendor[0] == '\0') {
        zb_vendor_pending = true;
    }
   
}

void ZigbeeSensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    
    // NEW: Save logical device reference (preferred)
    if (zigbee_device_ieee[0] != '\0') {
        obj[F("zb_ieee_ref")] = zigbee_device_ieee;
    }
    if (zigbee_logical_name[0] != '\0') {
        obj[F("zb_logical_name")] = zigbee_logical_name;
    }
    
    // DEPRECATED: Keep saving old fields for fallback/migration
    if (device_ieee != 0) {
        char ieee_str[20];
        getIeeeString(ieee_str, sizeof(ieee_str));
        obj[F("device_ieee")] = String(ieee_str);  // String() forces ArduinoJson to copy
    } else if (zigbee_device_ieee[0] != '\0') {
        obj[F("device_ieee")] = String(zigbee_device_ieee);
    }
    obj[F("endpoint")] = endpoint;
    
    // Store cluster_id and attribute_id as hex strings for readability
    char cluster_hex[10];
    snprintf(cluster_hex, sizeof(cluster_hex), "0x%04X", cluster_id);
    obj[F("cluster_id")] = String(cluster_hex);
    
    char attr_hex[10];
    snprintf(attr_hex, sizeof(attr_hex), "0x%04X", attribute_id);
    obj[F("attribute_id")] = String(attr_hex);
    if (zb_type <= 2) obj[F("zb_type")] = zb_type;
    obj[F("control_mode")] = control_mode;

    if (tuya_dp_value >= 0) obj[F("tuya_dp")] = tuya_dp_value;
    if (tuya_dp_battery >= 0) obj[F("tuya_dp_batt")] = tuya_dp_battery;
    if (tuya_dp_unit >= 0) obj[F("tuya_dp_unit")] = tuya_dp_unit;
    if (tuya_unit != 0xFF) obj[F("tuya_unit")] = tuya_unit;
    if (tuya_dp_status >= 0) obj[F("tuya_dp_status")] = tuya_dp_status;
    if (tuya_dp_consumption >= 0) obj[F("tuya_dp_consumption")] = tuya_dp_consumption;
    
    // Battery level — only persist when actually measured
    if (last_battery != UINT32_MAX) obj[F("battery")] = last_battery;
    obj[F("lqi")] = last_lqi;
    // Communication mode (0=unknown, 1=report, 2=active) — always persist
    obj[F("comm_mode")] = (uint8_t)comm_mode;
    // Configured ZCL report interval (for predictive boost)
    if (report_interval_s > 0) obj[F("rpt_intv")] = report_interval_s;
    // Phase anchor for UNIX-based predictive boost
    if (join_anchor_ts > 0) obj[F("join_anchor")] = join_anchor_ts;
    // Basic Cluster info (persisted)
    if (zb_manufacturer[0] != '\0') {
        obj[F("zb_manufacturer")] = zb_manufacturer;
    }
    if (zb_model[0] != '\0') {
        obj[F("zb_model")] = zb_model;
    }
    if (zb_vendor[0] != '\0') {
        obj[F("zb_vendor")] = zb_vendor;
    }
}

ZigBeeLogicalDevice* ZigbeeSensor::getLogicalDevice() const {
    // Try new method first: lookup by IEEE string + logical name
    if (zigbee_device_ieee[0] != '\0' && zigbee_logical_name[0] != '\0') {
        ZigBeeLogicalDevice* logdev = OpenSprinkler::zigbee_logical_lookup(zigbee_device_ieee, zigbee_logical_name);
        if (logdev) {
            return logdev;
        }
        // If not found, log warning but don't fail silently — requires migration
        DEBUG_PRINTF(F("[ZIGBEE] WARN: Logical device not found for sensor %d: %s#%s\n"),
                     nr, zigbee_device_ieee, zigbee_logical_name);
    }
     
    // Fallback: try deprecated fields (for backward compatibility during migration)
    if (device_ieee != 0) {
        char ieee_str[17];
        snprintf(ieee_str, sizeof(ieee_str), "%016llX", (unsigned long long)device_ieee);
        // Use cluster_id as part of a fallback name (temporary)
        char fallback_name[30];
        snprintf(fallback_name, sizeof(fallback_name), "fallback_%04X_%04X", cluster_id, attribute_id);
        ZigBeeLogicalDevice* logdev = OpenSprinkler::zigbee_logical_lookup(ieee_str, fallback_name);
        if (logdev) {
            DEBUG_PRINTF(F("[ZIGBEE] Using fallback logical device for sensor %d: %s#%s\n"),
                         nr, ieee_str, fallback_name);
            return logdev;
        }
    }
     
    return nullptr;
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
        if (last_read == 0 || time >= last_read + 60) {
            DEBUG_PRINTF(F("[ZB] Sensor #%d NOT in Zigbee mode\n"), nr);
            last_read = time;
        }
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // CRITICAL: Do NOT use Zigbee in WiFi SOFTAP mode
    if (WiFi.getMode() == WIFI_MODE_AP) {
        if (last_read == 0 || time >= last_read + 60) {
            DEBUG_PRINTF(F("[ZB] Sensor #%d WiFi in AP mode - REJECTED\n"), nr);
            last_read = time;
        }
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Start Zigbee if not yet running
    if (!sensor_zigbee_is_active() && !sensor_zigbee_ensure_started()) {
        // Back off to avoid tight spin loop (re-check every 60s)
        if (last_read == 0 || time >= last_read + 60) {
            DEBUG_PRINTF(F("[ZB] Sensor #%d FAILED to start Zigbee\n"), nr);
            last_read = time;
        }
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // =========================================================================
    // GATEWAY MODE: Reports arrive asynchronously via gw_updateSensorFromReport
    // which sets data_ok=true. Trust passive reports - no forced active reads.
    // =========================================================================
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        if (flags.data_ok) {
            // Have valid report data — return it directly.
            // Do NOT consume data_ok here: the sensor should keep returning the
            // last known value until a fresh ZigBee report overwrites last_data.
            // Consuming it would leave data_ok=false between reports, making the
            // sensor appear to have "no data" for the entire inter-report interval.
            repeat_read = 0;
            return HTTP_RQT_SUCCESS;
        }

        // REPORT mode: device is known to push reports — never actively poll it.
        // It will update the sensor on its own schedule; just wait.
        // Set last_read so the outer sensor loop respects read_interval instead
        // of calling us on every tick while we're passively waiting.
        if (comm_mode == ZB_COMM_REPORT) {
            repeat_read = 0;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        // UNKNOWN / ACTIVE: use active read as fallback when poll interval expires
        uint poll_interval = read_interval ? read_interval : 60;
        if (poll_interval < 15) poll_interval = 15;
        bool should_read = (last_read == 0 || time >= last_read + poll_interval);

        if (device_ieee != 0 && should_read) {
            if (sensor_zigbee_gw_read_attribute(device_ieee, endpoint, cluster_id, attribute_id)) {
                last_read = time;
                repeat_read = 0;
            } else {
                repeat_read = 1;
            }
        } else {
            // Not bound or interval not yet elapsed — update last_read so we are
            // not polled again on the very next tick.
            if (!should_read || device_ieee == 0) last_read = time;
            repeat_read = 0;
        }

        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // =========================================================================
    // CLIENT MODE: Passive (report devices) or active 2-phase read (active-only)
    // =========================================================================

    // REPORT mode: device sends unsolicited reports — skip active read entirely.
    // Data will be updated by zigbee_attribute_callback when the next report arrives.
    if (comm_mode == ZB_COMM_REPORT) {
        if (flags.data_ok) {
            // Report-mode sensor has fresh data — return it.
            // Keep data_ok=true so the sensor continues to serve the last known
            // value until an updated ZigBee report arrives.
            repeat_read = 0;
            return HTTP_RQT_SUCCESS;
        }
        // No data yet from a reporting device — wait passively.
        // Set last_read so the outer sensor loop respects read_interval instead
        // of calling us on every tick while we have nothing to do.
        repeat_read = 0;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // UNKNOWN / ACTIVE: Active 2-phase read (request → wait → return)
    
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
        
        if (flags.data_ok) {
            flags.data_ok = false;  // consume — next active cycle sends a fresh request
            return HTTP_RQT_SUCCESS;
        }
        return HTTP_RQT_NOT_RECEIVED;
    }
}

// Compatibility functions for old API
void sensor_zigbee_bind_device(uint nr, const char *device_ieee_str) {
    // DEBUG_PRINTF(F("[ZIGBEE] Bind request for sensor %d: %s\n"), nr, device_ieee_str ? device_ieee_str : "null");
    
    if (device_ieee_str && device_ieee_str[0]) {
        SensorBase* sensor = sensor_by_nr(nr);
        if (sensor && sensor->type == SENSOR_ZIGBEE) {
            ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
            zb_sensor->device_ieee = ZigbeeSensor::parseIeeeAddress(device_ieee_str);
            sensor_save();
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

    int count = 0;
    if (ieee802154_is_zigbee_gw()) {
        // Gateway mode: delegate to the GW module's own device list
        count = sensor_zigbee_gw_get_discovered_devices(devices, max_devices);
    } else {
        // Client mode: use the local client list
        count = ((int)client_discovered_devices.size() < max_devices)
                    ? (int)client_discovered_devices.size() : max_devices;
        for (int i = 0; i < count; i++) {
            memcpy(&devices[i], &client_discovered_devices[i], sizeof(ZigbeeDeviceInfo));
        }
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

void sensor_zigbee_request_configure_reporting(uint nr) {
    if (!ieee802154_is_zigbee_gw()) return;

    SensorBase* sensor = sensor_by_nr(nr);
    if (!sensor || sensor->type != SENSOR_ZIGBEE) return;

    ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
    if (zb->device_ieee == 0) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] ConfigReport skipped: sensor %u has no IEEE\n"), nr);
        return;
    }

    if (!sensor_zigbee_is_active() || !sensor_zigbee_is_connected()) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] ConfigReport skipped: Zigbee not ready (sensor %u)\n"), nr);
        return;
    }

    uint ri = zb->read_interval ? zb->read_interval : 60;
    uint16_t max_interval = (ri >= 15 && ri <= 3600) ? (uint16_t)ri : 120;
    uint16_t min_interval = 10;

    DEBUG_PRINTF(F("[ZIGBEE-GW] ConfigReport now: sensor %u ieee=%016llX ep=%u c=0x%04X a=0x%04X min=%u max=%u\n"),
                 nr, (unsigned long long)zb->device_ieee, zb->endpoint,
                 zb->cluster_id, zb->attribute_id, min_interval, max_interval);

    sensor_zigbee_gw_configure_reporting(zb->device_ieee, zb->endpoint,
                                         zb->cluster_id, zb->attribute_id,
                                         min_interval, max_interval);
}

void sensor_zigbee_request_dp_query(uint nr) {
    if (!ieee802154_is_zigbee_gw()) return;

    SensorBase* sensor = sensor_by_nr(nr);
    if (!sensor || sensor->type != SENSOR_ZIGBEE) return;

    ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
    if (zb->device_ieee == 0) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] DP query skipped: sensor %u has no IEEE\n"), nr);
        return;
    }

    if (!sensor_zigbee_is_active() || !sensor_zigbee_is_connected()) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] DP query skipped: Zigbee not ready (sensor %u)\n"), nr);
        return;
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW] DP query now: sensor %u ieee=%016llX ep=%u\n"),
                 nr, (unsigned long long)zb->device_ieee, zb->endpoint);
    sensor_zigbee_gw_request_dp_query(zb->device_ieee, zb->endpoint);
}

void sensor_zigbee_request_active_read(uint nr) {
    if (!ieee802154_is_zigbee_gw()) return;

    SensorBase* sensor = sensor_by_nr(nr);
    if (!sensor || sensor->type != SENSOR_ZIGBEE) return;

    ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
    if (zb->device_ieee == 0) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Active read skipped: sensor %u has no IEEE\n"), nr);
        return;
    }

    if (!sensor_zigbee_is_active() || !sensor_zigbee_is_connected()) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Active read skipped: Zigbee not ready (sensor %u)\n"), nr);
        return;
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW] Active read now: sensor %u ieee=%016llX ep=%u c=0x%04X a=0x%04X\n"),
                 nr, (unsigned long long)zb->device_ieee, zb->endpoint,
                 zb->cluster_id, zb->attribute_id);
    sensor_zigbee_gw_read_attribute(zb->device_ieee, zb->endpoint,
                                    zb->cluster_id, zb->attribute_id);
}

bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    IEEE802154Mode mode = ieee802154_get_mode();
    
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        return client_read_remote_attribute(device_ieee, endpoint, cluster_id, attribute_id);
    } else if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY) {
        // Gateway mode: not needed, reports arrive passively
        // DEBUG_PRINTLN(F("[ZIGBEE] Gateway mode uses passive reports, no active read needed"));
        return false;
    }
    
    // DEBUG_PRINTLN(F("[ZIGBEE] Active attribute reading not available in current mode"));
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
        // If we now have mfr+model but no vendor yet, schedule an API lookup
        if (zb->zb_manufacturer[0] != '\0' && zb->zb_model[0] != '\0' && zb->zb_vendor[0] == '\0') {
            zb->zb_vendor_pending = true;
        }

        DEBUG_PRINTF(F("[ZIGBEE] Basic Cluster for '%s': ieee=0x%016llX mfr=\"%s\" model=\"%s\" vendor_pending=%d\n"),
                     zb->name,
                     (unsigned long long)zb->device_ieee,
                     zb->zb_manufacturer,
                     zb->zb_model,
                     zb->zb_vendor_pending ? 1 : 0);
    }
    
    if (updated) {
        sensor_save();
        DEBUG_PRINTLN(F("[ZIGBEE] Sensor config saved with Basic Cluster info"));
    }
}

void ZigbeeSensor::updateProfileInfo(uint64_t ieee_addr, const char* manufacturer, const char* model, const char* vendor) {
    if (ieee_addr == 0) return;

    bool updated = false;
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
        if (zb->device_ieee != ieee_addr) continue;

        bool mfr_missing = (zb->zb_manufacturer[0] == '\0' || strcmp(zb->zb_manufacturer, "unknown") == 0);
        bool model_missing = (zb->zb_model[0] == '\0' || strcmp(zb->zb_model, "unknown") == 0);
        bool vendor_missing = (zb->zb_vendor[0] == '\0' || strcmp(zb->zb_vendor, "unknown") == 0);

        if (manufacturer && manufacturer[0] && mfr_missing) {
            strncpy(zb->zb_manufacturer, manufacturer, sizeof(zb->zb_manufacturer) - 1);
            zb->zb_manufacturer[sizeof(zb->zb_manufacturer) - 1] = '\0';
            updated = true;
        }
        if (model && model[0] && model_missing) {
            strncpy(zb->zb_model, model, sizeof(zb->zb_model) - 1);
            zb->zb_model[sizeof(zb->zb_model) - 1] = '\0';
            updated = true;
        }
        if (vendor && vendor[0] && vendor_missing) {
            strncpy(zb->zb_vendor, vendor, sizeof(zb->zb_vendor) - 1);
            zb->zb_vendor[sizeof(zb->zb_vendor) - 1] = '\0';
            zb->zb_vendor_pending = false;
            updated = true;
        } else if (zb->zb_manufacturer[0] != '\0' && zb->zb_model[0] != '\0' && zb->zb_vendor[0] == '\0') {
            zb->zb_vendor_pending = true;
        }

        if (zb->zb_manufacturer[0] != '\0' && zb->zb_model[0] != '\0') {
            zb->basic_cluster_queried = true;
        }

        DEBUG_PRINTF(F("[ZIGBEE] Profile for '%s': ieee=0x%016llX mfr=\"%s\" model=\"%s\" vendor=\"%s\"\n"),
                     zb->name,
                     (unsigned long long)zb->device_ieee,
                     zb->zb_manufacturer,
                     zb->zb_model,
                     zb->zb_vendor);
    }

    if (updated) {
        sensor_save();
        DEBUG_PRINTLN(F("[ZIGBEE] Sensor config saved with profile info"));
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

        case ZB_ZCL_CLUSTER_ID_METERING:
            return UNIT_LITER;
            
        case ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT:
        case ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT:
            return UNIT_USERDEF;
            
        case ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
            return UNIT_LEVEL;
    }
    
    return UNIT_NONE;
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
