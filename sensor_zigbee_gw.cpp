/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Zigbee sensor implementation - Gateway/Coordinator mode (internal)
 *
 * This file provides gateway-specific functions called by the runtime
 * dispatcher in sensor_zigbee.cpp. It does NOT define ZigbeeSensor methods
 * (those live in sensor_zigbee.cpp to avoid duplicate symbols).
 *
 * IMPORTANT: sensor_zigbee.cpp and sensor_zigbee_gw.cpp are BOTH compiled.
 * The runtime IEEE 802.15.4 mode determines which set of functions is used:
 *   - IEEE_ZIGBEE_GATEWAY → sensor_zigbee_gw_*() functions (this file)
 *   - IEEE_ZIGBEE_CLIENT  → client functions in sensor_zigbee.cpp
 *   - IEEE_MATTER          → neither (Zigbee disabled)
 *   - IEEE_DISABLED        → neither (radio off)
 */

#include "sensor_zigbee.h"
#include "sensor_zigbee_gw.h"
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
#include <esp_wifi.h>
#include <esp_coexist.h>
extern "C" {
#include <esp_coex_i154.h>
}
#include <esp_ieee802154.h>
#include "esp_zigbee_core.h"
#include "nwk/esp_zigbee_nwk.h"
#include "Zigbee.h"
#include "ZigbeeEP.h"
#include <esp_heap_caps.h>

// Optional: Restrict Zigbee to a single channel (e.g. 25 = 2475 MHz) to
// minimise radio overlap with WiFi on dual-protocol ESP32-C5.
// If not defined, the Zigbee stack scans all channels 11-26 (default).
// #define ZIGBEE_COEX_CHANNEL_MASK  (1UL << 25)

// Zigbee Gateway state
static bool gw_zigbee_initialized = false;
static bool gw_zigbee_connected = false;
static bool gw_zigbee_needs_nvram_reset = false;
// NOTE: The Arduino Zigbee library does NOT support Zigbee.stop() + Zigbee.begin()
// restart cycles (causes Load access fault). Once started, Zigbee stays running.
static bool gw_zigbee_was_stopped = false;  // guard against restart after stop

// Active Zigbee sensor (prevents concurrent access)
static int gw_active_zigbee_sensor = 0;

// Discovered devices storage
static std::vector<ZigbeeDeviceInfo> gw_discovered_devices;

// Zigbee Cluster IDs
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

// Tuya manufacturer-specific cluster (manuSpecificTuya)
#define ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC             0xEF00

// Tuya DP (DataPoint) command IDs within cluster 0xEF00
#define TUYA_CMD_DATA_REQUEST   0x00  // Gateway → device
#define TUYA_CMD_DATA_RESPONSE  0x01  // Device → gateway (response)
#define TUYA_CMD_DATA_REPORT    0x02  // Device → gateway (unsolicited)

// Tuya DP data types
#define TUYA_DP_TYPE_RAW    0x00
#define TUYA_DP_TYPE_BOOL   0x01
#define TUYA_DP_TYPE_VALUE  0x02  // 4-byte big-endian integer
#define TUYA_DP_TYPE_STRING 0x03
#define TUYA_DP_TYPE_ENUM   0x04
#define TUYA_DP_TYPE_BITMAP 0x05

// Tuya DP numbers for GIEX GX-04 / Soanalarm SNT858Z soil moisture sensors
#define TUYA_DP_SOIL_MOISTURE     3   // raw % (0-100)
#define TUYA_DP_TEMPERATURE       5   // raw °C (integer, already scaled)
#define TUYA_DP_TEMPERATURE_UNIT  9   // 0 = Celsius, 1 = Fahrenheit
#define TUYA_DP_BATTERY          15   // raw % (0-100)

// Flag to mark reports originating from Tuya DP parsing (value already scaled)
#define TUYA_REPORT_FLAG_PRESCALED  0x8000

// Lazy-loading report cache
struct ZigbeeAttributeReport {
    uint64_t ieee_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    int32_t value;
    uint8_t lqi;
    unsigned long timestamp;
    bool consumed;   // true once matched to a sensor — skip on next iteration
};

static constexpr size_t MAX_PENDING_REPORTS = 16;
static ZigbeeAttributeReport pending_reports[MAX_PENDING_REPORTS];
static size_t pending_report_count = 0;
static constexpr unsigned long REPORT_VALIDITY_MS = 60000;

class GwZigbeeReportReceiver;
static GwZigbeeReportReceiver* gw_reportReceiver = nullptr;

// ========== IEEE address resolution & device auto-discovery ==========
// Resolve IEEE from short address using the Zigbee stack's internal address
// table.  If the device is not yet in gw_discovered_devices, add it so the
// status line reports the correct device count.

static uint64_t gw_resolve_ieee(uint16_t short_addr) {
    // 1) Check our own cache first (fast path)
    for (const auto& dev : gw_discovered_devices) {
        if (dev.short_addr == short_addr) {
            return dev.ieee_addr;
        }
    }

    // 2) Ask the Zigbee stack's address map
    esp_zb_ieee_addr_t raw_ieee;  // 8-byte little-endian
    if (esp_zb_ieee_address_by_short(short_addr, raw_ieee) != ESP_OK) {
        return 0;  // Not in the stack's table (yet)
    }

    // Convert 8-byte little-endian array → uint64_t
    uint64_t ieee64 = 0;
    for (int i = 7; i >= 0; i--) {
        ieee64 = (ieee64 << 8) | raw_ieee[i];
    }
    if (ieee64 == 0) return 0;

    // 3) Auto-discover: add to our list so "devices=" counter is accurate
    ZigbeeDeviceInfo info = {};
    info.ieee_addr = ieee64;
    info.short_addr = short_addr;
    info.endpoint = 1;
    info.is_new = true;
    strncpy(info.manufacturer, "unknown", sizeof(info.manufacturer) - 1);
    strncpy(info.model_id, "unknown", sizeof(info.model_id) - 1);
    gw_discovered_devices.push_back(info);

    DEBUG_PRINTF(F("[ZIGBEE-GW] Auto-discovered device: short=0x%04X ieee=%08lX%08lX\n"),
                short_addr, (unsigned long)(ieee64 >> 32), (unsigned long)(ieee64 & 0xFFFFFFFF));
    return ieee64;
}

// ========== GW discovered-devices accessors (called from sensor_zigbee.cpp) ==

int sensor_zigbee_gw_get_discovered_devices(ZigbeeDeviceInfo* out, int max_devices) {
    if (!out || max_devices <= 0) return 0;
    int count = (gw_discovered_devices.size() < (size_t)max_devices)
                    ? (int)gw_discovered_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&out[i], &gw_discovered_devices[i], sizeof(ZigbeeDeviceInfo));
    }
    return count;
}

void sensor_zigbee_gw_clear_new_device_flags() {
    for (auto& dev : gw_discovered_devices) {
        dev.is_new = false;
    }
}

// ========== Tuya DP protocol handler (APS layer) ==========
// Tuya devices using cluster 0xEF00 send proprietary DataPoint messages
// instead of standard ZCL attribute reports. This APS indication handler
// intercepts those frames, parses the Tuya DP payload, and injects them
// into the existing report cache as if they were standard ZCL reports.
//
// Tuya ZCL frame layout (within asdu):
//   [0] frame_control  (0x09 = cluster-specific, client-to-server, disable default response)
//   [1] seq_number
//   [2] command_id     (0x01 = dataResponse, 0x02 = dataReport)
//   [3..4] tuya_seq    (big-endian, Tuya sequence number — ignored)
//   [5..N] DP records, each:
//       [0] dp_number
//       [1] dp_type
//       [2..3] dp_length (big-endian, length of dp_value)
//       [4..4+dp_length-1] dp_value (big-endian for numeric types)

static void gw_cache_tuya_report(uint64_t ieee_addr, uint8_t src_endpoint,
                                  uint16_t mapped_cluster, uint16_t mapped_attr,
                                  int32_t value, int lqi) {
    uint16_t flagged_attr = mapped_attr | TUYA_REPORT_FLAG_PRESCALED;

    // Update existing report in-place if we already have one for the same
    // ieee + cluster + attr.  This prevents the cache from filling up with
    // duplicate Tuya reports when no sensor is configured yet.
    for (size_t i = 0; i < pending_report_count; i++) {
        ZigbeeAttributeReport& r = pending_reports[i];
        if (r.cluster_id == mapped_cluster && r.attr_id == flagged_attr &&
            r.ieee_addr == ieee_addr) {
            r.value = value;
            r.lqi = (uint8_t)(lqi & 0xFF);
            r.endpoint = src_endpoint;
            r.timestamp = millis();
            r.consumed = false;  // Allow re-processing with updated value
            return;  // Updated in-place — no new slot needed
        }
    }

    // No existing entry — append a new one
    if (pending_report_count < MAX_PENDING_REPORTS) {
        ZigbeeAttributeReport& report = pending_reports[pending_report_count++];
        report.ieee_addr = ieee_addr;
        report.endpoint = src_endpoint;
        report.cluster_id = mapped_cluster;
        report.attr_id = flagged_attr;
        report.value = value;
        report.lqi = (uint8_t)(lqi & 0xFF);
        report.timestamp = millis();
        report.consumed = false;

        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Cached DP report: cluster=0x%04X attr=0x%04X value=%ld lqi=%d\n"),
                    mapped_cluster, mapped_attr, value, lqi);
    } else {
        DEBUG_PRINTLN(F("[ZIGBEE-GW][TUYA] Report cache full — dropping Tuya DP"));
    }
}

static bool gw_tuya_aps_indication_handler(esp_zb_apsde_data_ind_t ind) {
    // Log ALL incoming APS frames for debugging
    DEBUG_PRINTF(F("[ZIGBEE-GW][APS] Indication: cluster=0x%04X src=0x%04X ep=%d len=%u prof=0x%04X\n"),
                ind.cluster_id, ind.src_short_addr, ind.src_endpoint, ind.asdu_length, ind.profile_id);
    
    // Only intercept Tuya cluster 0xEF00
    if (ind.cluster_id != ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) {
        return false;  // Let the stack handle non-Tuya clusters normally
    }

    // Minimum ZCL header (3 bytes) + Tuya header (2 bytes) + 1 DP record (4+ bytes)
    if (!ind.asdu || ind.asdu_length < 9) {
        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Frame too short (%u bytes), ignoring\n"), ind.asdu_length);
        return true;  // Consume anyway — it's on cluster 0xEF00
    }

    uint8_t command_id = ind.asdu[2];
    // Only process dataResponse (0x01) and dataReport (0x02)
    if (command_id != TUYA_CMD_DATA_RESPONSE && command_id != TUYA_CMD_DATA_REPORT) {
        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Ignoring command 0x%02X\n"), command_id);
        return true;  // Consume — unknown Tuya command
    }

    // Resolve IEEE address from short address via the stack's address table.
    // This also auto-adds unknown devices to gw_discovered_devices.
    uint64_t ieee_addr = gw_resolve_ieee(ind.src_short_addr);

    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Processing DP frame: cmd=0x%02X len=%u src=0x%04X\n"),
                command_id, ind.asdu_length, ind.src_short_addr);

    // Parse DP records starting after ZCL header (3 bytes) + Tuya seq (2 bytes) = offset 5
    uint32_t offset = 5;
    while (offset + 4 <= ind.asdu_length) {  // Minimum DP record: 4 bytes header
        uint8_t dp_number = ind.asdu[offset];
        uint8_t dp_type = ind.asdu[offset + 1];
        uint16_t dp_len = ((uint16_t)ind.asdu[offset + 2] << 8) | ind.asdu[offset + 3];
        offset += 4;

        if (offset + dp_len > ind.asdu_length) {
            DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %d truncated (need %u, have %u)\n"),
                        dp_number, dp_len, ind.asdu_length - offset);
            break;
        }

        // Extract value (big-endian for numeric types)
        int32_t dp_value = 0;
        if (dp_type == TUYA_DP_TYPE_VALUE && dp_len == 4) {
            dp_value = (int32_t)(((uint32_t)ind.asdu[offset] << 24) |
                                 ((uint32_t)ind.asdu[offset + 1] << 16) |
                                 ((uint32_t)ind.asdu[offset + 2] << 8) |
                                 ((uint32_t)ind.asdu[offset + 3]));
        } else if (dp_type == TUYA_DP_TYPE_ENUM || dp_type == TUYA_DP_TYPE_BOOL) {
            dp_value = (int32_t)ind.asdu[offset];
        } else if (dp_len <= 4) {
            // Generic big-endian extraction for short payloads
            for (uint16_t j = 0; j < dp_len; j++) {
                dp_value = (dp_value << 8) | ind.asdu[offset + j];
            }
        }

        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %d: type=%d len=%d value=%ld\n"),
                    dp_number, dp_type, dp_len, dp_value);

        // Map Tuya DPs to standard ZCL cluster/attribute pairs
        switch (dp_number) {
            case TUYA_DP_SOIL_MOISTURE:
                // Tuya soil_moisture is raw % (0-100), map to soil moisture cluster
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE, 0x0000,
                                     dp_value, ind.lqi);
                break;

            case TUYA_DP_TEMPERATURE:
                // Tuya temperature is in tenths of °C (e.g. 227 = 22.7°C)
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, 0x0000,
                                     dp_value, ind.lqi);
                break;

            case TUYA_DP_BATTERY:
                // Tuya battery is raw % (0-100), map to power config cluster
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0021,
                                     dp_value, ind.lqi);
                break;

            case TUYA_DP_TEMPERATURE_UNIT:
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Temperature unit: %s\n"),
                            dp_value == 0 ? "Celsius" : "Fahrenheit");
                // Informational only — not cached as a sensor value
                break;

            default:
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Unhandled DP %d, value=%ld\n"),
                            dp_number, dp_value);
                break;
        }

        offset += dp_len;
    }

    return true;  // Consumed — do not let ZCL stack process cluster 0xEF00
}

class GwZigbeeReportReceiver : public ZigbeeEP {
public:
    GwZigbeeReportReceiver(uint8_t endpoint) : ZigbeeEP(endpoint) {
        _cluster_list = esp_zb_zcl_cluster_list_create();
        
        if (_cluster_list) {
            // SERVER-side mandatory clusters
            esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
            if (basic_cluster) {
                esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
            }
            
            esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
            if (identify_cluster) {
                esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
            }

            // CLIENT-side measurement clusters — required so the ZCL layer
            // routes incoming attribute reports to our zbAttributeRead() callback.
            // Without these, standard ZCL reports are dropped by the stack.
            esp_zb_attribute_list_t *temp_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
            if (temp_cluster) {
                esp_zb_cluster_list_add_temperature_meas_cluster(_cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Temp cluster 0x0402 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *humidity_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
            if (humidity_cluster) {
                esp_zb_cluster_list_add_humidity_meas_cluster(_cluster_list, humidity_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Humidity cluster 0x0405 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *soil_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE);
            if (soil_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, soil_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Soil moisture cluster 0x0408 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *pressure_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT);
            if (pressure_cluster) {
                esp_zb_cluster_list_add_pressure_meas_cluster(_cluster_list, pressure_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Pressure cluster 0x0403 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *light_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT);
            if (light_cluster) {
                esp_zb_cluster_list_add_illuminance_meas_cluster(_cluster_list, light_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Illuminance cluster 0x0400 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *power_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
            if (power_cluster) {
                esp_zb_cluster_list_add_power_config_cluster(_cluster_list, power_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Power config cluster 0x0001 added (CLIENT)"));
            }

            // Tuya manufacturer-specific cluster (0xEF00) as CLIENT so we receive
            // incoming DP reports from Tuya devices (GIEX GX-04, etc.)
            esp_zb_attribute_list_t *tuya_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC);
            if (tuya_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, tuya_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya cluster 0xEF00 added (CLIENT)"));
            }
        }
        
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
        
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Report receiver endpoint created (full cluster config)"));
    }
    
    virtual ~GwZigbeeReportReceiver() = default;

    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute,
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] zbAttributeRead called with NULL attribute!"));
            return;
        }
        
        DEBUG_PRINTF(F("[ZIGBEE-GW] >>> zbAttributeRead: cluster=0x%04X attr=0x%04X type=0x%02X src_ep=%d src_short=0x%04X\n"),
                    cluster_id, attribute->id, attribute->data.type, src_endpoint, src_address.u.short_addr);
        
        // Resolve IEEE from short address via the stack's address table
        uint64_t ieee_addr = gw_resolve_ieee(src_address.u.short_addr);
        int32_t value = extractAttributeValue(attribute);
        
        DEBUG_PRINTF(F("[ZIGBEE-GW] >>> resolved ieee=%08lX%08lX value=%ld\n"),
                    (unsigned long)(ieee_addr >> 32), (unsigned long)(ieee_addr & 0xFFFFFFFF), value);
        
        if (pending_report_count < MAX_PENDING_REPORTS) {
            ZigbeeAttributeReport& report = pending_reports[pending_report_count++];
            report.ieee_addr = ieee_addr;
            report.endpoint = src_endpoint;
            report.cluster_id = cluster_id;
            report.attr_id = attribute->id;
            report.value = value;
            report.lqi = 0;
            report.timestamp = millis();
            report.consumed = false;
            
            DEBUG_PRINTF(F("[ZIGBEE-GW] Report cached [%d/%d]: cluster=0x%04X attr=0x%04X value=%ld\n"),
                        (int)pending_report_count, (int)MAX_PENDING_REPORTS, cluster_id, attribute->id, value);
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Report cache full - dropping report!"));
        }
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
                DEBUG_PRINTF(F("[ZIGBEE-GW] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// ========== Helper: update sensor from cached report ==========

static void gw_updateSensorFromReport(ZigbeeSensor* zb_sensor, const ZigbeeAttributeReport& report) {
    if (!zb_sensor) return;
    
    zb_sensor->last_native_data = report.value;
    double converted_value = (double)report.value;

    // Check if this report came from the Tuya DP parser (values already in natural units)
    bool is_tuya = (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0;
    
    if (is_tuya) {
        // Tuya DP values need conversion to natural units
        if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
            converted_value = report.value / 10.0;  // Tuya sends tenths of °C (e.g. 227 = 22.7°C)
        } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
            converted_value = report.value / 10.0;  // Tuya sends tenths of %RH
        } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG) {
            // Tuya battery is raw % (0-100), keep as-is
            zb_sensor->last_battery = (uint32_t)report.value;
        }
        // Soil moisture from Tuya is raw % (0-100), already in natural units
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 10.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT && report.attr_id == 0x0000) {
        if (report.value > 0 && report.value <= 65534) {
            converted_value = pow(10.0, (report.value - 1.0) / 10000.0);
        } else {
            converted_value = 0.0;
        }
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && report.attr_id == 0x0021) {
        converted_value = report.value / 2.0;
        zb_sensor->last_battery = (uint32_t)converted_value;
    }
    
    converted_value -= (double)zb_sensor->offset_mv / 1000.0;
    if (zb_sensor->factor && zb_sensor->divider)
        converted_value *= (double)zb_sensor->factor / (double)zb_sensor->divider;
    else if (zb_sensor->divider)
        converted_value /= (double)zb_sensor->divider;
    else if (zb_sensor->factor)
        converted_value *= (double)zb_sensor->factor;
    converted_value += zb_sensor->offset2 / 100.0;
    
    zb_sensor->last_data = converted_value;
    zb_sensor->last_lqi = report.lqi;
    zb_sensor->flags.data_ok = true;
    zb_sensor->repeat_read = 1;
    
    DEBUG_PRINTF(F("[ZIGBEE-GW] Sensor updated: cluster=0x%04X raw=%ld conv=%.2f factor=%d div=%d offset=%d\n"),
                report.cluster_id, report.value, converted_value, zb_sensor->factor, zb_sensor->divider, zb_sensor->offset_mv);
}

// ========== NVRAM erase ==========

static bool gw_erase_zigbee_nvram() {
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage");
    if (!zb_partition) return false;
    esp_err_t err = esp_partition_erase_range(zb_partition, 0, zb_partition->size);
    return err == ESP_OK;
}

// ========== Public Gateway API ==========

void sensor_zigbee_gw_factory_reset() {
    gw_zigbee_needs_nvram_reset = true;
}

void sensor_zigbee_gw_stop() {
    if (!gw_zigbee_initialized) return;
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Stopping Zigbee Coordinator (WARNING: cannot restart without reboot!)"));
    gw_zigbee_initialized = false;
    gw_zigbee_connected = false;
    gw_zigbee_was_stopped = true;  // Mark as stopped - restart is NOT possible
    if (gw_reportReceiver) {
        delete gw_reportReceiver;
        gw_reportReceiver = nullptr;
    }
    Zigbee.stop();
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Zigbee stopped"));
}

void sensor_zigbee_gw_start() {
    DEBUG_PRINTLN(F("[ZIGBEE-GW] sensor_zigbee_gw_start() called"));
    DEBUG_PRINTF("[ZIGBEE-GW] ieee802154 mode: %d (%s)\n",
                (int)ieee802154_get_mode(), ieee802154_mode_name(ieee802154_get_mode()));

    if (!ieee802154_is_zigbee_gw()) {
        static bool mode_warning_shown = false;
        if (!mode_warning_shown) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Not in ZIGBEE_GATEWAY mode - Zigbee GW disabled"));
            mode_warning_shown = true;
        }
        return;
    }

    if (gw_zigbee_initialized) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Already initialized, skipping"));
        return;
    }

    // Arduino Zigbee library cannot restart after Zigbee.stop() - guard against crash
    if (gw_zigbee_was_stopped) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Cannot restart Zigbee after stop (library limitation) - reboot required"));
        return;
    }
    
    if (Zigbee.started()) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Stopping previous Zigbee instance..."));
        Zigbee.stop();
        if (gw_reportReceiver) {
            delete gw_reportReceiver;
            gw_reportReceiver = nullptr;
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));

    if (gw_zigbee_needs_nvram_reset) {
        gw_zigbee_needs_nvram_reset = false;
        gw_erase_zigbee_nvram();
    }

    esp_zb_radio_config_t radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE };
    esp_zb_host_config_t host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE };
    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);

    if (WiFi.getMode() != WIFI_MODE_NULL) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] WiFi active - configuring coexistence"));
        WiFi.setSleep(false);
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        (void)esp_coex_wifi_i154_enable();
        // NOTE: Per-packet PTI must be set AFTER Zigbee.begin() — ieee802154_mac_init()
        // inside esp_zb_start() resets all PTI values to defaults.
    } else {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] No WiFi - Zigbee has full radio access (Ethernet mode)"));
    }

    gw_reportReceiver = new GwZigbeeReportReceiver(10);
    if (!gw_reportReceiver) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] ERROR: Failed to allocate GwZigbeeReportReceiver!"));
        return;
    }
    Zigbee.addEndpoint(gw_reportReceiver);
    gw_reportReceiver->setManufacturerAndModel("OpenSprinkler", "ZigbeeGateway");

    // Optionally restrict Zigbee to a specific channel to reduce
    // radio contention with WiFi.  When ZIGBEE_COEX_CHANNEL_MASK is
    // not defined, the Zigbee stack uses the default all-channel scan.
#ifdef ZIGBEE_COEX_CHANNEL_MASK
    Zigbee.setPrimaryChannelMask(ZIGBEE_COEX_CHANNEL_MASK);
    DEBUG_PRINTF("[ZIGBEE-GW] Primary channel mask set to 0x%08X\n",
                 (unsigned)ZIGBEE_COEX_CHANNEL_MASK);
#else
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Using default channel mask (all channels 11-26)"));
#endif

    // Log heap state before Zigbee init for diagnostics
    DEBUG_PRINTF(F("[ZIGBEE-GW] Heap before init: internal free=%u, largest=%u, PSRAM free=%u, largest=%u\n"),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Configure ZBOSS memory BEFORE esp_zb_init() (which is called inside Zigbee.begin()).
    // The Coordinator/Router role allocates neighbor, routing, and source-route tables
    // whose sizes are derived from overall_network_size (default=64).  That is too large
    // for the available SRAM on ESP32-C5, triggering:
    //   "No memory for NWK src route table"  /  zb_memconfig.c:271
    //
    // IMPORTANT: ZBOSS uses calloc() for its tables.  With CONFIG_SPIRAM_USE_MALLOC=y
    // the allocator CAN fall back to PSRAM, but the async Zigbee_main task competes
    // with the main loop for internal RAM.  Keeping tables small prevents LittleFS
    // "Unable to allocate FD" failures that occur when internal RAM is exhausted.
    //
    // Size 10 supports up to 10 direct Zigbee children — enough for OpenSprinkler.
    esp_zb_overall_network_size_set(10);
    esp_zb_io_buffer_size_set(20);
    esp_zb_scheduler_queue_size_set(30);

    DEBUG_PRINTLN(F("[ZIGBEE-GW] Starting as COORDINATOR..."));
    if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] ERROR: Zigbee.begin(COORDINATOR) FAILED!"));
        delete gw_reportReceiver;
        gw_reportReceiver = nullptr;
        return;
    }

    gw_zigbee_initialized = true;
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Zigbee Coordinator started successfully!"));
    DEBUG_PRINTF("[ZIGBEE-GW] Zigbee.started()=%d, Zigbee.connected()=%d\n",
                Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0);

    // Register APS-layer indication handler for Tuya DP protocol (cluster 0xEF00).
    // This intercepts raw Tuya frames before ZCL processing and converts them
    // into standard report-cache entries for sensors like GIEX GX-04.
    esp_zb_aps_data_indication_handler_register(gw_tuya_aps_indication_handler);
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya APS indication handler registered"));

    // Apply persistent 802.15.4 coexistence config AFTER Zigbee.begin().
    // ieee802154_mac_init() (called inside esp_zb_start()) resets all PTI to
    // defaults.  esp_ieee802154_set_coex_config() updates the persistent
    // s_coex_config struct so every subsequent TX/RX operation uses the set
    // priority, balancing WiFi and Zigbee radio time.
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        // Default: LOW priority for Zigbee so WiFi is not starved.
        // PTI is boosted to HIGH during openNetwork() for device joining.
        esp_ieee802154_coex_config_t coex_cfg = {
            .idle    = IEEE802154_IDLE,    // lowest when radio idle
            .txrx    = IEEE802154_LOW,     // low priority: WiFi gets preference
            .txrx_at = IEEE802154_LOW,     // low priority for timed TX/RX
        };
        esp_ieee802154_set_coex_config(coex_cfg);
        esp_coex_ieee802154_ack_pti_set(IEEE802154_LOW);
        DEBUG_PRINTLN(F("[ZIGBEE-GW] 802.15.4 coex PTI set to LOW (WiFi-friendly, post-init)"));
    }
    DEBUG_PRINTF(F("[ZIGBEE-GW] Heap AFTER init: internal free=%u, largest=%u, PSRAM free=%u, largest=%u\n"),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Log channel and PAN ID for diagnostics
    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t zb_channel = esp_zb_get_current_channel();
    uint16_t zb_pan_id = esp_zb_get_pan_id();
    esp_zb_ieee_addr_t ext_pan_raw;
    esp_zb_get_extended_pan_id(ext_pan_raw);
    esp_zb_lock_release();
    uint64_t zb_ext_pan = 0;
    for (int i = 7; i >= 0; i--) zb_ext_pan = (zb_ext_pan << 8) | ext_pan_raw[i];
    DEBUG_PRINTF(F("[ZIGBEE-GW] Network: channel=%d PAN=0x%04X extPAN=%08lX%08lX\n"),
                 zb_channel, zb_pan_id,
                 (unsigned long)(zb_ext_pan >> 32), (unsigned long)(zb_ext_pan & 0xFFFFFFFF));

    // Network stays closed after init. Use the HTTP API ("zj" command)
    // to open the network for joining when pairing new devices.
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Network closed — use API to open for joining"));
}

bool sensor_zigbee_gw_is_active() {
    return gw_zigbee_initialized;
}

bool sensor_zigbee_gw_ensure_started() {
    if (gw_zigbee_initialized) {
        return true;
    }

    // Never start Zigbee in SOFTAP mode (RF conflict with 802.15.4)
    wifi_mode_t wmode = WiFi.getMode();
    if (wmode == WIFI_MODE_AP) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Cannot start in SOFTAP mode"));
        return false;
    }

    // Wait for network to be connected BEFORE starting Zigbee Coordinator.
    // The Coordinator does a full channel scan on startup which conflicts
    // with WiFi STA scanning/association if both run simultaneously.
    bool is_ethernet = (wmode == WIFI_MODE_NULL);
    if (!is_ethernet && WiFi.status() != WL_CONNECTED) {
        static bool wifi_wait_logged = false;
        if (!wifi_wait_logged) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Waiting for WiFi to connect before starting Zigbee..."));
            wifi_wait_logged = true;
        }
        return false;
    }
    if (is_ethernet && !os.network_connected()) {
        static bool eth_wait_logged = false;
        if (!eth_wait_logged) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Waiting for Ethernet to connect before starting Zigbee..."));
            eth_wait_logged = true;
        }
        return false;
    }

    DEBUG_PRINTLN(F("[ZIGBEE-GW] ensure_started: network ready, starting Zigbee GW..."));
    sensor_zigbee_gw_start();
    DEBUG_PRINTF("[ZIGBEE-GW] ensure_started result: initialized=%d\n", gw_zigbee_initialized ? 1 : 0);
    return gw_zigbee_initialized;
}

void sensor_zigbee_gw_process_reports(uint64_t ieee_addr, uint8_t endpoint,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       int32_t value, uint8_t lqi) {
    
    for (size_t i = 0; i < pending_report_count; i++) {
        ZigbeeAttributeReport& report = pending_reports[i];
        
        // Skip already-consumed or expired reports
        if (report.consumed || millis() - report.timestamp > REPORT_VALIDITY_MS) {
            continue;
        }
        
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        bool found = false;
        
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
            ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
            
            uint16_t report_attr_unmasked = report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED;
            bool cluster_match = (zb_sensor->cluster_id == report.cluster_id);
            bool attr_match = (zb_sensor->attribute_id == report_attr_unmasked);
            bool ieee_match = true;
            if (zb_sensor->device_ieee != 0 && report.ieee_addr != 0) {
                ieee_match = (zb_sensor->device_ieee == report.ieee_addr);
            }
            bool ep_match = true;
            // Endpoint 10 is the coordinator's local endpoint, not a remote one.
            // Treat ep=10 or ep=1 in sensor config as "match any endpoint".
            if (zb_sensor->endpoint != 1 && zb_sensor->endpoint != 10 && report.endpoint != 0) {
                ep_match = (zb_sensor->endpoint == report.endpoint);
            }
            bool matches = cluster_match && attr_match && ieee_match && ep_match;
            
            if (!matches && cluster_match) {
                // Cluster matches but something else doesn't — log for debugging
                DEBUG_PRINTF(F("[ZIGBEE-GW] Match fail sensor '%s': cluster=%d attr=%d|%d ieee=%d ep=%d\n"),
                            sensor->name,
                            cluster_match ? 1 : 0, attr_match ? 1 : 0,
                            (int)zb_sensor->attribute_id, (int)report_attr_unmasked,
                            ieee_match ? 1 : 0, ep_match ? 1 : 0);
                if (!ieee_match) {
                    DEBUG_PRINTF(F("[ZIGBEE-GW]   sensor_ieee=%08lX%08lX report_ieee=%08lX%08lX\n"),
                                (unsigned long)(zb_sensor->device_ieee >> 32),
                                (unsigned long)(zb_sensor->device_ieee & 0xFFFFFFFF),
                                (unsigned long)(report.ieee_addr >> 32),
                                (unsigned long)(report.ieee_addr & 0xFFFFFFFF));
                }
            }
            
            if (matches) {
                gw_updateSensorFromReport(zb_sensor, report);
                report.consumed = true;  // Don't re-process this report
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Mark as consumed to prevent re-processing every loop iteration
            report.consumed = true;
            
            // If this is a battery report (cluster 0x0001, attr 0x0021), update
            // battery on any sensor matching the IEEE address
            if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && 
                (report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED) == 0x0021 && report.ieee_addr != 0) {
                uint8_t battery_pct = (uint8_t)(report.value / 2);  // ZCL BatteryPercentageRemaining is 0-200
                SensorIterator it2 = sensors_iterate_begin();
                SensorBase* s2;
                while ((s2 = sensors_iterate_next(it2)) != NULL) {
                    if (s2 && s2->type == SENSOR_ZIGBEE) {
                        ZigbeeSensor* zb2 = static_cast<ZigbeeSensor*>(s2);
                        if (zb2->device_ieee == report.ieee_addr) {
                            zb2->last_battery = battery_pct;
                        }
                    }
                }
                DEBUG_PRINTF(F("[ZIGBEE-GW] Battery report: ieee=%08lX%08lX battery=%d%%\n"),
                            (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                            battery_pct);
            } else {
                // Throttle unmatched (non-battery) report logging
                static unsigned long last_unmatched_log = 0;
                static uint16_t last_unmatched_cluster = 0;
                static uint16_t last_unmatched_attr = 0;
                static int unmatched_count = 0;
                
                if (report.cluster_id != last_unmatched_cluster || 
                    (report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED) != last_unmatched_attr ||
                    millis() - last_unmatched_log > 30000) {
                    if (unmatched_count > 1) {
                        DEBUG_PRINTF(F("[ZIGBEE-GW]   (%d identical unmatched reports suppressed)\n"), unmatched_count - 1);
                    }
                    DEBUG_PRINTF(F("[ZIGBEE-GW] Unmatched report: ieee=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld%s\n"),
                                (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                                report.cluster_id, report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED, report.value,
                                (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) ? " (Tuya)" : "");
                    last_unmatched_log = millis();
                    last_unmatched_cluster = report.cluster_id;
                    last_unmatched_attr = report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED;
                    unmatched_count = 1;
                } else {
                    unmatched_count++;
                }
            }
        }
    }
    
    // Clear consumed and expired reports
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < pending_report_count; read_idx++) {
        if (!pending_reports[read_idx].consumed &&
            millis() - pending_reports[read_idx].timestamp <= REPORT_VALIDITY_MS) {
            pending_reports[write_idx++] = pending_reports[read_idx];
        }
    }
    pending_report_count = write_idx;
}

// Track when the join window closes so PTI can be lowered
static unsigned long gw_join_window_end = 0;

static void gw_set_zigbee_pti_high() {
    if (WiFi.getMode() == WIFI_MODE_NULL) return;
    esp_ieee802154_coex_config_t coex_cfg = {
        .idle    = IEEE802154_IDLE,
        .txrx    = IEEE802154_HIGH,
        .txrx_at = IEEE802154_HIGH,
    };
    esp_ieee802154_set_coex_config(coex_cfg);
    esp_coex_ieee802154_ack_pti_set(IEEE802154_HIGH);
    DEBUG_PRINTLN(F("[ZIGBEE-GW] PTI boosted to HIGH for device joining"));
}

static void gw_set_zigbee_pti_low() {
    if (WiFi.getMode() == WIFI_MODE_NULL) return;
    esp_ieee802154_coex_config_t coex_cfg = {
        .idle    = IEEE802154_IDLE,
        .txrx    = IEEE802154_LOW,
        .txrx_at = IEEE802154_LOW,
    };
    esp_ieee802154_set_coex_config(coex_cfg);
    esp_coex_ieee802154_ack_pti_set(IEEE802154_LOW);
    DEBUG_PRINTLN(F("[ZIGBEE-GW] PTI restored to LOW (WiFi-friendly)"));
}

void sensor_zigbee_gw_open_network(uint16_t duration) {
    if (!gw_zigbee_initialized) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] open_network: Zigbee not initialized!"));
        sensor_zigbee_gw_ensure_started();
        if (!gw_zigbee_initialized) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] open_network: Failed to start Zigbee!"));
            return;
        }
    }
    uint8_t dur = (duration > 254) ? 254 : (uint8_t)duration;
    DEBUG_PRINTF("[ZIGBEE-GW] Opening network for %d seconds (permit join)\n", dur);

    // Boost Zigbee priority during join window so devices can pair reliably
    gw_set_zigbee_pti_high();
    gw_join_window_end = millis() + (unsigned long)dur * 1000UL;

    // Must acquire Zigbee lock — this function is called from the HTTP server
    // task, but esp_zb_bdb_open_network() must run in the Zigbee task context.
    esp_zb_lock_acquire(portMAX_DELAY);
    Zigbee.openNetwork(dur);
    esp_zb_lock_release();
    DEBUG_PRINTLN(F("[ZIGBEE-GW] Network open for joining"));
}

void sensor_zigbee_gw_loop() {
    if (!gw_zigbee_initialized) return;
    
    if (pending_report_count > 0) {
        sensor_zigbee_gw_process_reports(0, 0, 0, 0, 0, 0);
    }
    
    static bool last_connected = false;
    bool connected = Zigbee.started() && Zigbee.connected();
    if (connected != last_connected) {
        if (connected) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Coordinator network FORMED"));
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Coordinator network LOST"));
        }
        last_connected = connected;
        gw_zigbee_connected = connected;
    }

    // NOTE: No idle timeout - Arduino Zigbee library does NOT support
    // Zigbee.stop() + Zigbee.begin() restart (causes Load access fault).
    // Once started, Zigbee stays running until reboot.

    // Restore LOW PTI after join window expires
    if (gw_join_window_end != 0 && millis() > gw_join_window_end) {
        gw_set_zigbee_pti_low();
        gw_join_window_end = 0;
    }

    static unsigned long last_status_print = 0;
    if (millis() - last_status_print > 60000) {
        last_status_print = millis();
        DEBUG_PRINTF("[ZIGBEE-GW] Status: started=%d connected=%d devices=%d pending_reports=%d\n",
                    Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0,
                    (int)gw_discovered_devices.size(), (int)pending_report_count);
        
        // Log registered sensors and their config for debugging
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        int zb_count = 0;
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (sensor && sensor->type == SENSOR_ZIGBEE) {
                ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
                DEBUG_PRINTF("[ZIGBEE-GW]   Sensor '%s': ieee=%08lX%08lX ep=%d cluster=0x%04X attr=0x%04X data_ok=%d last=%.2f\n",
                            sensor->name,
                            (unsigned long)(zb->device_ieee >> 32), (unsigned long)(zb->device_ieee & 0xFFFFFFFF),
                            zb->endpoint, zb->cluster_id, zb->attribute_id,
                            zb->flags.data_ok ? 1 : 0, zb->last_data);
                zb_count++;
            }
        }
        if (zb_count == 0) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW]   No Zigbee sensors registered!"));
        }
        
        // Dump pending reports that haven't been consumed
        for (size_t i = 0; i < pending_report_count; i++) {
            ZigbeeAttributeReport& r = pending_reports[i];
            if (!r.consumed) {
                DEBUG_PRINTF("[ZIGBEE-GW]   Pending[%d]: ieee=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld age=%lums\n",
                            (int)i,
                            (unsigned long)(r.ieee_addr >> 32), (unsigned long)(r.ieee_addr & 0xFFFFFFFF),
                            r.cluster_id, r.attr_id & ~TUYA_REPORT_FLAG_PRESCALED, r.value,
                            millis() - r.timestamp);
            }
        }
    }
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
