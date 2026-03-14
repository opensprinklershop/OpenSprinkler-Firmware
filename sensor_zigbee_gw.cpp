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
extern "C" {

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
static unsigned long gw_join_window_end = 0;
static bool gw_zigbee_needs_nvram_reset = false;

// Discovered devices storage
static std::vector<ZigbeeDeviceInfo> gw_discovered_devices;
static constexpr size_t GW_DISCOVERED_MAX = 128;

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

// Basic Cluster attribute IDs
#define ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID       0x0004
#define ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID        0x0005
#define ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID            0x0007

// Active one-shot read request state (Gateway mode)
static uint16_t gw_read_attr_id = 0;
static bool     gw_read_pending = false;
static unsigned long gw_read_time = 0;
static uint64_t gw_read_pending_ieee = 0;     // IEEE of the pending active read
static uint16_t gw_read_pending_cluster = 0;  // cluster of the pending active read
#define GW_READ_TIMEOUT_MS 5000

// Static storage for Basic Cluster read requests.
// attr_field is processed asynchronously by the Zigbee stack and must
// outlive the caller stack frame.
static uint16_t s_gw_basic_query_attrs[2] = {
    ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
    ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID
};

// Set when any sensor's comm_mode changes during report processing;
// sensor_save() is then called from sensor_zigbee_gw_loop() (main loop thread).
static bool gw_comm_mode_changed = false;

// Configure Reporting planning — schedule ZCL "Configure Reporting" commands
// to tell sleeping end devices (e.g. AQARA T&H) to proactively push attribute
// reports at the specified interval instead of relying on active reads.
struct GwConfigReportRequest {
    uint64_t ieee_addr;
    uint8_t  endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint16_t min_interval;  // seconds: device won't report more often than this
    uint16_t max_interval;  // seconds: device reports at least this often
    unsigned long scheduled_time;
};
static std::vector<GwConfigReportRequest> gw_config_report_queue;
static unsigned long gw_last_config_report_ms = 0;
#define GW_CONFIG_REPORT_STAGGER_MS 600  // ms between successive configure-report sends

// Tuya manufacturer-specific cluster (manuSpecificTuya)
#define ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC             0xEF00

// Tuya DP (DataPoint) command IDs within cluster 0xEF00
#define TUYA_CMD_DATA_REQUEST   0x00  // Gateway → device (query all DPs)
#define TUYA_CMD_DATA_RESPONSE  0x01  // Device → gateway (response to query)
#define TUYA_CMD_DATA_REPORT    0x02  // Device → gateway (unsolicited report)
#define TUYA_CMD_DATA_ACTIVE    0x03  // Device → gateway (requests GW to query)
#define TUYA_CMD_MCU_VERSION_REQ  0x10  // Device → gateway (MCU version query)
#define TUYA_CMD_MCU_VERSION_RESP 0x11  // Gateway → device (MCU version answer)
#define TUYA_CMD_TIME_SYNC_REQ  0x24  // Device → gateway (time sync request)

// Tuya DP data types
#define TUYA_DP_TYPE_RAW    0x00
#define TUYA_DP_TYPE_BOOL   0x01
#define TUYA_DP_TYPE_VALUE  0x02  // 4-byte big-endian integer
#define TUYA_DP_TYPE_STRING 0x03
#define TUYA_DP_TYPE_ENUM   0x04
#define TUYA_DP_TYPE_BITMAP 0x05

// Tuya DP numbers for soil moisture sensors (varies by model)
// Reference: GIEX GX04 (TS0601 / _TZE284_nhgdf6qr)
// Official Z2M table (TS0601_soil_3):
//   DP  3 = soil_moisture VALUE raw % (0-100)    — may not be transmitted by all revisions
//   DP  5 = temperature VALUE ÷10 = °C
//   DP  9 = temperature_unit ENUM (0=°C, 1=°F)
//   DP 14 = soil_moisture_state ENUM (0=dry, 1=normal, 2=wet)*
//   DP 15 = battery VALUE raw % (0-100)
//
// * Z2M labels DP 14 "battery_state" in its converter, but on the physical GX04
//   DP 15 already carries precise battery %, and DP 14 ENUM is confirmed as the
//   3-level soil moisture category.  See firmware issue log for details.
//   Mapping: 0(dry)→20%, 1(normal)→50%, 2(wet)→80% (representative midpoints).
#define TUYA_DP_SOIL_MOISTURE      3   // VALUE raw % (0-100)  — GX04 DP3/TS0601_soil variants
#define TUYA_DP_SOIL_MOISTURE_ALT1 2   // VALUE raw % (0-100)  — some variants
#define TUYA_DP_SOIL_MOISTURE_ALT2 7   // VALUE raw % (0-100)  — some variants
// DP 14: GX04 uses ENUM for 3-level soil moisture category (dry/normal/wet).
// Other Tuya sensors may use DP 14 as VALUE raw % — both cases map to soil moisture.
#define TUYA_DP_SOIL_MOISTURE_ALT3 14  // ENUM → soil category; VALUE → soil raw %
#define TUYA_DP_TEMPERATURE        5   // VALUE ÷10 = °C
#define TUYA_DP_TEMPERATURE_UNIT   9   // ENUM 0=Celsius 1=Fahrenheit
#define TUYA_DP_BATTERY           15   // VALUE raw % (0-100)
// Tuya irrigation timers (common TS0601 variants)
#define TUYA_DP_VALVE_1            1
#define TUYA_DP_VALVE_2            2
#define TUYA_DP_COUNTDOWN_1       13
#define TUYA_DP_COUNTDOWN_2       14
#define TUYA_DP_VALVE_1_ALT      104
#define TUYA_DP_VALVE_2_ALT      105

// GX04 DP 14 soil moisture state enum values
#define TUYA_SOIL_STATE_DRY    0  // → ~20 % representative
#define TUYA_SOIL_STATE_NORMAL 1  // → ~50 % representative
#define TUYA_SOIL_STATE_WET    2  // → ~80 % representative

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

// Forward declarations for functions used in class methods and device management
static void gw_schedule_configure_reporting_for_ieee(uint64_t ieee, unsigned long delay_ms);
static void gw_schedule_default_configure_reporting(uint64_t ieee, uint8_t ep, unsigned long delay_ms);
static bool gw_query_basic_cluster(uint16_t short_addr, uint8_t endpoint);

class GwZigbeeReportReceiver;
static GwZigbeeReportReceiver* gw_reportReceiver = nullptr;

// ========== IEEE address resolution & device management ==========
// Resolve IEEE from short address using the Zigbee stack's internal address
// table. Does NOT auto-add devices - only for already confirmed devices.

static uint64_t gw_resolve_ieee(uint16_t short_addr) {
    // Check our confirmed device list
    for (const auto& dev : gw_discovered_devices) {
        if (dev.short_addr == short_addr) {
            return dev.ieee_addr;
        }
    }
    return 0;  // Device not in confirmed list
}

// Add a device that has confirmed its presence via response to query/report
static void gw_add_responsive_device(uint16_t short_addr, uint64_t ieee_addr, uint8_t endpoint) {
    // Check if already in list
    for (auto& dev : gw_discovered_devices) {
        if (dev.ieee_addr == ieee_addr) {
            // Update short addr and mark as responsive
            dev.short_addr = short_addr;
            dev.endpoint = endpoint;
            dev.has_responded = true;
            dev.last_rx_at_ms = millis();     // stamp last reception
            dev.silent_query_count = 0;       // device is alive — reset silence counter
            // Device re-announced (e.g. after power cycle / re-join).
            // Re-schedule Configure Reporting so sleeping end-devices
            // resume sending proactive reports on their fresh network slot.
            gw_schedule_configure_reporting_for_ieee(ieee_addr, 1000);
            return;
        }
    }
    
    // Add new confirmed device
    if (gw_discovered_devices.size() >= GW_DISCOVERED_MAX) {
        gw_discovered_devices.erase(gw_discovered_devices.begin());
    }
    
    ZigbeeDeviceInfo info = {};
    info.ieee_addr = ieee_addr;
    info.short_addr = short_addr;
    info.endpoint = endpoint;
    info.is_new = true;
    info.has_responded = true;
    info.discovered_at = (uint32_t)os.now_tz();
    info.last_rx_at_ms = millis();
    info.silent_query_count = 0;
    info.manufacturer[0] = '\0';
    info.model_id[0] = '\0';
    
    gw_discovered_devices.push_back(info);
    DEBUG_PRINTF(F("[ZIGBEE-GW] Added responsive device: short=0x%04X ieee=0x%016llX ep=%d\n"),
                 short_addr, (unsigned long long)ieee_addr, endpoint);

    // Query Basic Cluster immediately to get manufacturer/model from the device.
    // This ensures already-paired devices (that don't re-announce after a GW reboot)
    // still have their manufacturer/model filled in once they start sending data.
    gw_query_basic_cluster(short_addr, endpoint);
    
    // Schedule Configure Reporting for all sensors matching this device
    uint64_t resolved_ieee = gw_resolve_ieee(short_addr);
    if (resolved_ieee) {
        gw_schedule_configure_reporting_for_ieee(resolved_ieee, 1000);
    }
    
    // If in join mode and no sensor exists yet for this device, send default
    // Configure Reporting (900s) for common measurement clusters.  The device
    // is still awake right now, so the commands arrive immediately.
    if ((gw_join_window_end != 0)) {
        bool has_sensor = false;
        SensorIterator it = sensors_iterate_begin();
        SensorBase* s;
        while ((s = sensors_iterate_next(it)) != NULL) {
            if (s && s->type == SENSOR_ZIGBEE) {
                ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(s);
                if (zb->device_ieee == ieee_addr) { has_sensor = true; break; }
            }
        }
        if (!has_sensor) {
            gw_schedule_default_configure_reporting(ieee_addr, endpoint, 500);
        }
    }
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

/**
 * @brief Extract a ZCL CHAR_STRING attribute into a C string buffer (GW mode)
 */
static bool gw_extractStringAttribute(const esp_zb_zcl_attribute_t *attr, char *buf, size_t buf_size) {
    if (!attr || !attr->data.value || buf_size == 0) return false;
    
    if (attr->data.type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
        uint8_t *raw = (uint8_t*)attr->data.value;
        uint8_t len = raw[0];
        if (len == 0xFF) { buf[0] = '\0'; return false; }
        if (len >= buf_size) len = buf_size - 1;
        memcpy(buf, raw + 1, len);
        buf[len] = '\0';
        return len > 0;
    } else if (attr->data.type == ESP_ZB_ZCL_ATTR_TYPE_LONG_CHAR_STRING) {
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
 * @brief Handle Basic Cluster (0x0000) attribute read response in Gateway mode
 * Updates discovered device info and matching sensor configurations.
 */
static void gw_handleBasicClusterResponse(uint16_t short_addr, const esp_zb_zcl_attribute_t *attribute) {
    if (!attribute || !attribute->data.value) return;
    
    char str_buf[32] = {0};
    if (!gw_extractStringAttribute(attribute, str_buf, sizeof(str_buf))) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW] Basic Cluster attr 0x%04X: not a string (type=0x%02X)\n"),
                     // attribute->id, attribute->data.type);
        return;
    }
    
    // DEBUG_PRINTF(F("[ZIGBEE-GW] Basic Cluster attr 0x%04X = \"%s\" (from short=0x%04X)\n"),
                 // attribute->id, str_buf, short_addr);
    
    // Find device in discovered list and update
    uint64_t ieee_addr = 0;
    for (auto& dev : gw_discovered_devices) {
        if (dev.short_addr == short_addr) {
            ieee_addr = dev.ieee_addr;
            if (attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) {
                strncpy(dev.manufacturer, str_buf, sizeof(dev.manufacturer) - 1);
                dev.manufacturer[sizeof(dev.manufacturer) - 1] = '\0';
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
                strncpy(dev.model_id, str_buf, sizeof(dev.model_id) - 1);
                dev.model_id[sizeof(dev.model_id) - 1] = '\0';
            }
            DEBUG_PRINTF(F("[ZIGBEE-GW] Updated device 0x%016llX: mfr=\"%s\" model=\"%s\"\n"),
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

// ========== Tuya DP protocol handler (APS layer) - continued ==========
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

static void gw_apply_profile_hint(uint64_t ieee_addr,
                                  const char* manufacturer,
                                  const char* model,
                                  const char* vendor,
                                  const char* reason) {
    if (ieee_addr == 0) return;

    bool updated_device = false;
    for (auto& dev : gw_discovered_devices) {
        if (dev.ieee_addr != ieee_addr) continue;

        if (manufacturer && manufacturer[0] && dev.manufacturer[0] == '\0') {
            strncpy(dev.manufacturer, manufacturer, sizeof(dev.manufacturer) - 1);
            dev.manufacturer[sizeof(dev.manufacturer) - 1] = '\0';
            updated_device = true;
        }
        if (model && model[0] && dev.model_id[0] == '\0') {
            strncpy(dev.model_id, model, sizeof(dev.model_id) - 1);
            dev.model_id[sizeof(dev.model_id) - 1] = '\0';
            updated_device = true;
        }
        if (vendor && vendor[0] && dev.vendor[0] == '\0') {
            strncpy(dev.vendor, vendor, sizeof(dev.vendor) - 1);
            dev.vendor[sizeof(dev.vendor) - 1] = '\0';
            updated_device = true;
        }
        break;
    }

    ZigbeeSensor::updateProfileInfo(ieee_addr, manufacturer, model, vendor);

    if (updated_device) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Applied profile hint (%s): ieee=0x%016llX mfr=\"%s\" model=\"%s\" vendor=\"%s\"\n"),
                     reason ? reason : "unknown",
                     (unsigned long long)ieee_addr,
                     manufacturer ? manufacturer : "",
                     model ? model : "",
                     vendor ? vendor : "");
    }
}

// Tuya sequence counter for outgoing commands
static uint16_t gw_tuya_seq = 0;

/**
 * @brief Send a Tuya time sync response (cmd 0x24) to a device.
 *
 * Tuya devices request time synchronization right after joining the network.
 * If the gateway doesn't respond, many Tuya devices will refuse to report
 * data or will leave the network entirely.
 *
 * Response payload (14 bytes):
 *   [0..3]  UTC seconds since 2000-01-01 (big-endian)
 *   [4]     Local time offset (unused, set to 0)
 *   [5..12] Local time in same format (big-endian), same as UTC for simplicity
 */
static void gw_tuya_send_time_sync(uint16_t short_addr, uint8_t dst_ep, uint8_t seq_number) {
    // Tuya epoch starts 2000-01-01 00:00:00 UTC
    static const uint32_t TUYA_EPOCH_OFFSET = 946684800UL;  // Unix timestamp of 2000-01-01

    time_t now_unix = time(nullptr);
    uint32_t tuya_time = 0;
    if (now_unix > (time_t)TUYA_EPOCH_OFFSET) {
        tuya_time = (uint32_t)(now_unix - TUYA_EPOCH_OFFSET);
    }

    // Build response payload: ZCL header (3 bytes) + Tuya seq (2 bytes) + time data (8 bytes)
    uint8_t payload[8];
    // UTC time (big-endian)
    payload[0] = (tuya_time >> 24) & 0xFF;
    payload[1] = (tuya_time >> 16) & 0xFF;
    payload[2] = (tuya_time >> 8)  & 0xFF;
    payload[3] = (tuya_time)       & 0xFF;
    // Local time (same as UTC)
    payload[4] = payload[0];
    payload[5] = payload[1];
    payload[6] = payload[2];
    payload[7] = payload[3];

    esp_zb_zcl_custom_cluster_cmd_req_t req = {};
    req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    req.zcl_basic_cmd.dst_endpoint = dst_ep;
    req.zcl_basic_cmd.src_endpoint = 10;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.cluster_id = ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC;
    req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    req.dis_default_resp = 1;
    req.custom_cmd_id = TUYA_CMD_TIME_SYNC_REQ;  // Response uses same cmd ID
    req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size = sizeof(payload);
    req.data.value = payload;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();

    // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Time sync response sent to 0x%04X (tuya_time=%lu)\n"),
                // short_addr, (unsigned long)tuya_time);
}

/**
 * @brief Send a Tuya MCU version response (cmd 0x11) to a device.
 *
 * Some Tuya devices query the gateway's MCU version during interview.
 * A minimal response prevents the device from timing out or leaving.
 */
static void gw_tuya_send_mcu_version_resp(uint16_t short_addr, uint8_t dst_ep, uint8_t seq_number) {
    // Response payload: version byte (we report 0x40 = 4.0 like Tuya gateways)
    uint8_t payload[1] = { 0x40 };

    esp_zb_zcl_custom_cluster_cmd_req_t req = {};
    req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    req.zcl_basic_cmd.dst_endpoint = dst_ep;
    req.zcl_basic_cmd.src_endpoint = 10;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.cluster_id = ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC;
    req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    req.dis_default_resp = 1;
    req.custom_cmd_id = TUYA_CMD_MCU_VERSION_RESP;
    req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size = sizeof(payload);
    req.data.value = payload;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();

    // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] MCU version response sent to 0x%04X\n"), short_addr);
}

/**
 * @brief Send a Tuya DP Query (cmd 0x00) to a device to request all datapoints.
 *
 * This is the "interview" that triggers Tuya devices to report their initial
 * sensor values.  Many Tuya devices will NOT spontaneously report data until
 * they receive this query from the gateway.
 */
static void gw_tuya_send_dp_query(uint16_t short_addr, uint8_t dst_ep) {
    // DP Query has no payload - the Tuya device responds with all its DPs
    esp_zb_zcl_custom_cluster_cmd_req_t req = {};
    req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    req.zcl_basic_cmd.dst_endpoint = dst_ep;
    req.zcl_basic_cmd.src_endpoint = 10;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.cluster_id = ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC;
    req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    req.dis_default_resp = 1;
    req.custom_cmd_id = TUYA_CMD_DATA_REQUEST;
    req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size = 0;
    req.data.value = nullptr;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();

    // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP query sent to 0x%04X ep=%d\n"),
                // short_addr, dst_ep);
}

bool sensor_zigbee_gw_request_dp_query(uint64_t device_ieee, uint8_t endpoint) {
    if (!gw_zigbee_initialized || !Zigbee.started() || !Zigbee.connected()) return false;
    if (device_ieee == 0) return false;

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    esp_zb_lock_release();

    if (short_addr == 0xFFFF || short_addr == 0xFFFE) {
        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP query failed: short addr unknown for ieee=%016llX\n"),
                     (unsigned long long)device_ieee);
        return false;
    }

    gw_tuya_send_dp_query(short_addr, endpoint);
    return true;
}

static bool gw_tuya_aps_indication_handler(esp_zb_apsde_data_ind_t ind) {
    // Log ALL incoming APS frames for debugging
    // DEBUG_PRINTF(F("[ZIGBEE-GW][APS] Indication: cluster=0x%04X src=0x%04X ep=%d len=%u prof=0x%04X\n"),
                // ind.cluster_id, ind.src_short_addr, ind.src_endpoint, ind.asdu_length, ind.profile_id);
    
    // Only intercept Tuya cluster 0xEF00
    if (ind.cluster_id != ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) {
        return false;  // Let the stack handle non-Tuya clusters normally
    }

    // Minimum ZCL header: 3 bytes (frame_control + seq + command_id)
    if (!ind.asdu || ind.asdu_length < 3) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Frame too short (%u bytes), ignoring\n"), ind.asdu_length);
        return true;  // Consume anyway — it's on cluster 0xEF00
    }

    uint8_t seq_number = ind.asdu[1];
    uint8_t command_id = ind.asdu[2];

    // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] cmd=0x%02X seq=%d len=%u from 0x%04X ep=%d\n"),
                // command_id, seq_number, ind.asdu_length, ind.src_short_addr, ind.src_endpoint);

    // Handle Tuya time sync request (0x24)
    // Many Tuya devices send this right after joining; without a response
    // they refuse to report data or leave the network.
    if (command_id == TUYA_CMD_TIME_SYNC_REQ) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Time sync request from 0x%04X\n"), ind.src_short_addr);
        gw_tuya_send_time_sync(ind.src_short_addr, ind.src_endpoint, seq_number);
        // Resolve IEEE from stack and add as responsive device
        esp_zb_ieee_addr_t raw_ieee;
        if (esp_zb_ieee_address_by_short(ind.src_short_addr, raw_ieee) == ESP_OK) {
            uint64_t ieee = 0;
            for (int i = 7; i >= 0; i--) ieee = (ieee << 8) | raw_ieee[i];
            if (ieee) gw_add_responsive_device(ind.src_short_addr, ieee, ind.src_endpoint);
        }
        return true;
    }

    // Handle Tuya MCU version query (0x10)
    // Some Tuya devices query the gateway MCU version during interview.
    if (command_id == TUYA_CMD_MCU_VERSION_REQ) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] MCU version request from 0x%04X\n"), ind.src_short_addr);
        gw_tuya_send_mcu_version_resp(ind.src_short_addr, ind.src_endpoint, seq_number);
        // Resolve IEEE from stack and add as responsive device
        esp_zb_ieee_addr_t raw_ieee;
        if (esp_zb_ieee_address_by_short(ind.src_short_addr, raw_ieee) == ESP_OK) {
            uint64_t ieee = 0;
            for (int i = 7; i >= 0; i--) ieee = (ieee << 8) | raw_ieee[i];
            if (ieee) gw_add_responsive_device(ind.src_short_addr, ieee, ind.src_endpoint);
        }
        return true;
    }

    // Handle Tuya active status request (0x03)
    // Device is asking the gateway to query its datapoints.
    if (command_id == TUYA_CMD_DATA_ACTIVE) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Active DP request from 0x%04X — sending query now\n"), ind.src_short_addr);
        // Resolve IEEE from stack and add as responsive device
        esp_zb_ieee_addr_t raw_ieee;
        if (esp_zb_ieee_address_by_short(ind.src_short_addr, raw_ieee) == ESP_OK) {
            uint64_t ieee = 0;
            for (int i = 7; i >= 0; i--) ieee = (ieee << 8) | raw_ieee[i];
            if (ieee) gw_add_responsive_device(ind.src_short_addr, ieee, ind.src_endpoint);
        }
        // Send DP query directly
        gw_tuya_send_dp_query(ind.src_short_addr, ind.src_endpoint);
        return true;
    }

    // Only process dataResponse (0x01) and dataReport (0x02) for DP parsing
    if (command_id != TUYA_CMD_DATA_RESPONSE && command_id != TUYA_CMD_DATA_REPORT) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Unhandled command 0x%02X from 0x%04X\n"),
                    // command_id, ind.src_short_addr);
        return true;  // Consume — don't let ZCL stack fail on unknown Tuya commands
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] APS ind: src=0x%04X ep=%u len=%u\n"),
                 ind.src_short_addr, ind.src_endpoint, ind.asdu_length);

    // Need at least 5 bytes for ZCL header (3) + Tuya seq (2) for DP parsing
    if (ind.asdu_length < 5) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP frame too short (%u bytes)\n"), ind.asdu_length);
        return true;
    }

    // Resolve IEEE address from short address via the stack's address table.
    // Add device as responsive since it's reporting datapoints.
    esp_zb_ieee_addr_t raw_ieee;
    uint64_t ieee_addr = 0;
    if (esp_zb_ieee_address_by_short(ind.src_short_addr, raw_ieee) == ESP_OK) {
        for (int i = 7; i >= 0; i--) ieee_addr = (ieee_addr << 8) | raw_ieee[i];
        if (ieee_addr) gw_add_responsive_device(ind.src_short_addr, ieee_addr, ind.src_endpoint);
    }

    if (!ieee_addr) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Could not resolve IEEE for short=0x%04X\n"), ind.src_short_addr);
        return true;
    }

    // Mark this device as a Tuya device (so the loop can send periodic DP queries)
    for (auto& dev : gw_discovered_devices) {
        if (dev.ieee_addr == ieee_addr) {
            dev.is_tuya = true;
            break;
        }
    }

    // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Processing DP frame: cmd=0x%02X len=%u src=0x%04X\n"),
                // command_id, ind.asdu_length, ind.src_short_addr);

    // Parse DP records starting after ZCL header (3 bytes) + Tuya seq (2 bytes) = offset 5
    uint32_t offset = 5;
    bool dp3_soil_received = false;  // DP 3/2/7 (exact %) takes priority over DP 14 (categorical ENUM)
    bool saw_temp_dp = false;
    bool saw_battery_dp = false;
    bool saw_soil_signature_dp = false;
    bool saw_irrigation_valve_dp = false;
    bool saw_irrigation_countdown_dp = false;
    while (offset + 4 <= ind.asdu_length) {  // Minimum DP record: 4 bytes header
        uint8_t dp_number = ind.asdu[offset];
        uint8_t dp_type = ind.asdu[offset + 1];
        uint16_t dp_len = ((uint16_t)ind.asdu[offset + 2] << 8) | ind.asdu[offset + 3];
        offset += 4;

        if (offset + dp_len > ind.asdu_length) {
            // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %d truncated (need %u, have %u)\n"),
                        // dp_number, dp_len, ind.asdu_length - offset);
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

        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %u: type=%u len=%u value=%ld\n"),
                dp_number, dp_type, dp_len, dp_value);

        // Collect generic Tuya irrigation signature DPs even if we don't map all
        // of them to sensor values in this switch.
        if (dp_number == TUYA_DP_VALVE_1 || dp_number == TUYA_DP_VALVE_2 ||
            dp_number == TUYA_DP_VALVE_1_ALT || dp_number == TUYA_DP_VALVE_2_ALT) {
            saw_irrigation_valve_dp = true;
        }
        if (dp_number == TUYA_DP_COUNTDOWN_1 || dp_number == TUYA_DP_COUNTDOWN_2) {
            saw_irrigation_countdown_dp = true;
        }

        // Map Tuya DPs to standard ZCL cluster/attribute pairs
        switch (dp_number) {
            case TUYA_DP_SOIL_MOISTURE:
            case TUYA_DP_SOIL_MOISTURE_ALT1:
            case TUYA_DP_SOIL_MOISTURE_ALT2:
                // Tuya soil_moisture is raw % (0-100), map to soil moisture cluster
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Mapped DP %u -> cluster=0x%04X attr=0x%04X\n"),
                             dp_number, ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE, 0x0000);
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE, 0x0000,
                                     dp_value, ind.lqi);
                dp3_soil_received = true;  // mark: exact % available — DP 14 ENUM is lower priority
                saw_soil_signature_dp = true;
                break;

            case TUYA_DP_SOIL_MOISTURE_ALT3:  // DP 14
                // DP 3/2/7 (exact %) takes priority: skip the ENUM conversion
                // if an exact soil moisture value was already processed in this packet.
                if (dp3_soil_received && dp_type == TUYA_DP_TYPE_ENUM) {
                    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP 14 ENUM skipped (DP 3 already received)\n"));
                    break;
                }
                // VALUE type → soil moisture raw % (some Tuya sensor variants)
                // ENUM type  → GX04 soil_moisture_state: 0=dry, 1=normal, 2=wet
                //               Maps to representative % midpoints so the value
                //               is stored in the soil moisture cluster like any other reading.
                if (dp_type == TUYA_DP_TYPE_VALUE) {
                    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Mapped DP %u -> soil moisture (VALUE)\n"),
                                 dp_number);
                    gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                         ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE, 0x0000,
                                         dp_value, ind.lqi);
                    saw_soil_signature_dp = true;
                } else if (dp_type == TUYA_DP_TYPE_ENUM) {
                    // 3-level soil moisture category → representative % midpoints
                    int32_t soil_pct = (dp_value == TUYA_SOIL_STATE_WET)    ? 80 :
                                       (dp_value == TUYA_SOIL_STATE_NORMAL) ? 50 : 20;
                    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %u soil_state=%ld (%s) → %ld%%\n"),
                                 dp_number, dp_value,
                                 dp_value == TUYA_SOIL_STATE_WET    ? "wet"    :
                                 dp_value == TUYA_SOIL_STATE_NORMAL ? "normal" : "dry",
                                 soil_pct);
                    gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                         ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE, 0x0000,
                                         soil_pct, ind.lqi);
                    saw_soil_signature_dp = true;
                } else {
                    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %u unknown type=%u value=%ld — skipped\n"),
                                 dp_number, dp_type, dp_value);
                }
                break;

            case TUYA_DP_TEMPERATURE:
                // Tuya temperature is in tenths of °C (e.g. 227 = 22.7°C)
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Mapped DP %u -> cluster=0x%04X attr=0x%04X\n"),
                             dp_number, ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, 0x0000);
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, 0x0000,
                                     dp_value, ind.lqi);
                saw_temp_dp = true;
                break;

            case TUYA_DP_BATTERY:
                // Tuya battery is raw % (0-100), map to power config cluster
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Mapped DP %u -> cluster=0x%04X attr=0x%04X\n"),
                             dp_number, ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0021);
                gw_cache_tuya_report(ieee_addr, ind.src_endpoint,
                                     ZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0021,
                                     dp_value, ind.lqi);
                saw_battery_dp = true;
                break;

            case TUYA_DP_TEMPERATURE_UNIT:
                // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Temperature unit: %s\n"),
                            // dp_value == 0 ? "Celsius" : "Fahrenheit");
                // Informational only — not cached as a sensor value
                break;

            default:
                DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Unhandled DP %u (type=%u len=%u) value=%ld\n"),
                             dp_number, dp_type, dp_len, dp_value);
                break;
        }

        offset += dp_len;
    }

    // Generic Tuya profile hinting for soil/irrigation classes.
    // This provides workable lookup keys when Basic Cluster strings are absent,
    // so the UI can still retrieve a useful template from devices_api.
    if (ieee_addr != 0) {
        if (saw_temp_dp && saw_battery_dp && saw_soil_signature_dp) {
            // Soil probe class (e.g. GIEX GX04 / TS0601_soil variants)
            gw_apply_profile_hint(ieee_addr,
                                  "_TZE284_nhgdf6qr",
                                  "TS0601",
                                  "GIEX",
                                  "tuya-soil-signature");
        } else if (saw_irrigation_valve_dp && (saw_irrigation_countdown_dp || saw_battery_dp)) {
            // Irrigation timer class (2-zone TS0601 variants, incl. GIEX GX03)
            gw_apply_profile_hint(ieee_addr,
                                  "_TZE284_8zizsafo",
                                  "TS0601",
                                  "GIEX",
                                  "tuya-irrigation-signature");
        } else if (saw_soil_signature_dp && saw_battery_dp) {
            // Generic Tuya soil class fallback when no temperature DP is seen yet.
            gw_apply_profile_hint(ieee_addr,
                                  "_TZE284_nhgdf6qr",
                                  "TS0601",
                                  "Tuya",
                                  "tuya-soil-generic");
        }
    }

    return true;  // Consumed — do not let ZCL stack process cluster 0xEF00
}

// ========== Configure Reporting helpers ==========

// Return ZCL attribute data type for the MeasuredValue (0x0000) attribute
// of standard measurement clusters. Required by Configure Reporting command.
static uint8_t gw_cluster_attr_type(uint16_t cluster_id) {
    switch (cluster_id) {
        case ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:     return 0x29; // int16 (S16)
        case ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT: return 0x29; // int16 (S16)
        default:                                      return 0x21; // uint16 (U16)
    }
}

// Send a ZCL Configure Reporting command for one attribute.
// Tells the remote device to push reports every [min_interval..max_interval] seconds.
bool sensor_zigbee_gw_configure_reporting(uint64_t device_ieee, uint8_t endpoint,
                                           uint16_t cluster_id, uint16_t attr_id,
                                           uint16_t min_interval, uint16_t max_interval) {
    if (!gw_zigbee_initialized || !Zigbee.started() || !Zigbee.connected()) {
        // DEBUG_PRINTF("[ZIGBEE-GW] ConfigReport SKIP: not ready. ieee=%016llX\n",
                     // (unsigned long long)device_ieee);
        return false;
    }
    if (device_ieee == 0) return false;

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }

    uint16_t delta_val = 0;  // report on any change (threshold = 0 raw units)

    esp_zb_zcl_config_report_record_t record;
    memset(&record, 0, sizeof(record));
    record.direction    = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    record.attributeID  = attr_id;
    record.attrType     = gw_cluster_attr_type(cluster_id);
    record.min_interval = min_interval;
    record.max_interval = max_interval;
    record.reportable_change = &delta_val;

    esp_zb_zcl_config_report_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.zcl_basic_cmd.src_endpoint = 10;
    cmd.zcl_basic_cmd.dst_endpoint = endpoint;
    cmd.clusterID      = cluster_id;
    cmd.dis_default_resp = 1;
    cmd.record_number  = 1;
    cmd.record_field   = &record;

    esp_zb_lock_acquire(portMAX_DELAY);
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    if (short_addr != 0xFFFF && short_addr != 0xFFFE) {
        cmd.address_mode = (esp_zb_zcl_address_mode_t)ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    } else {
        cmd.address_mode = (esp_zb_zcl_address_mode_t)ESP_ZB_APS_ADDR_MODE_64_ENDP_PRESENT;
        memcpy(cmd.zcl_basic_cmd.dst_addr_u.addr_long, ieee_le, sizeof(ieee_le));
    }
    esp_zb_zcl_config_report_cmd_req(&cmd);
    esp_zb_lock_release();

    // DEBUG_PRINTF("[ZIGBEE-GW] \xE2\x9C\x93 Configure Reporting SENT: ieee=%016llX short=0x%04X ep=%d "
                 // "cluster=0x%04X attr=0x%04X min=%ds max=%ds\n",
                 // (unsigned long long)device_ieee, short_addr, endpoint,
                 // cluster_id, attr_id, min_interval, max_interval);
    return true;
}

// Schedule Configure Reporting for all sensors whose IEEE address matches `ieee`.
// Used after a device announces itself — deduplicates against existing queue entries.
static void gw_schedule_configure_reporting_for_ieee(uint64_t ieee, unsigned long delay_ms) {
    if (ieee == 0) return;
    SensorIterator it = sensors_iterate_begin();
    SensorBase* s;
    unsigned long now = millis();
    while ((s = sensors_iterate_next(it)) != NULL) {
        if (!s || s->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(s);
        if (zb->device_ieee != ieee) continue;

        uint ri = zb->read_interval ? zb->read_interval : 60;
        uint16_t max_interval = (ri >= 15 && ri <= 3600) ? (uint16_t)ri : 120;

        bool found = false;
        for (const auto& ex : gw_config_report_queue) {
            if (ex.ieee_addr == ieee && ex.cluster_id == zb->cluster_id && ex.attr_id == zb->attribute_id) {
                found = true;
                break;
            }
        }
        if (found) continue;

        GwConfigReportRequest req;
        req.ieee_addr      = ieee;
        req.endpoint       = zb->endpoint;
        req.cluster_id     = zb->cluster_id;
        req.attr_id        = zb->attribute_id;
        req.min_interval   = 10;
        req.max_interval   = max_interval;
        req.scheduled_time = now + delay_ms;
        gw_config_report_queue.push_back(req);
        // Update stored interval; clear prediction anchor if the interval changed
        // so it gets re-established on the next confirmed report.
        if (zb->report_interval_s != 0 && zb->report_interval_s != max_interval) {
            zb->join_anchor_ts = 0;
            DEBUG_PRINTF(F("[ZIGBEE-GW] ConfigReport interval changed %u\u2192%u for '%s' \u2014 anchor cleared\n"),
                         zb->report_interval_s, max_interval, zb->name);
        }
        zb->report_interval_s = max_interval;
        delay_ms += 700;
    }
}

// Schedule Configure Reporting for common measurement clusters on a newly
// discovered device that has no matching sensor yet.  The device is still awake
// during the join window so the commands will be received immediately.
// This ensures sleeping end devices (e.g. Aqara) configure their reporting
// interval BEFORE going to sleep — solving the chicken-and-egg problem where
// sensors only get created after scan finishes.
static constexpr uint16_t GW_DEFAULT_REPORT_INTERVAL = 900;  // 15 min

static void gw_schedule_default_configure_reporting(uint64_t ieee, uint8_t ep, unsigned long delay_ms) {
    if (ieee == 0) return;

    // Common ZCL measurement clusters + attribute 0x0000 (MeasuredValue)
    static const uint16_t common_clusters[] = {
        0x0402,  // Temperature Measurement
        0x0405,  // Relative Humidity
        0x0408,  // Soil Moisture (Leaf Wetness)
        0x0400,  // Illuminance Measurement
        0x0403,  // Pressure Measurement
    };

    unsigned long now = millis();
    for (size_t i = 0; i < sizeof(common_clusters) / sizeof(common_clusters[0]); i++) {
        // Check for duplicate entries
        bool found = false;
        for (const auto& ex : gw_config_report_queue) {
            if (ex.ieee_addr == ieee && ex.cluster_id == common_clusters[i] && ex.attr_id == 0x0000) {
                found = true;
                break;
            }
        }
        if (found) continue;

        GwConfigReportRequest req;
        req.ieee_addr      = ieee;
        req.endpoint       = ep;
        req.cluster_id     = common_clusters[i];
        req.attr_id        = 0x0000;
        req.min_interval   = 10;
        req.max_interval   = GW_DEFAULT_REPORT_INTERVAL;
        req.scheduled_time = now + delay_ms;
        gw_config_report_queue.push_back(req);
        delay_ms += 700;
    }
    DEBUG_PRINTF(F("[ZIGBEE-GW] Queued default ConfigReport (900s) for ieee=%016llX ep=%d (%d clusters)\n"),
                 (unsigned long long)ieee, ep, (int)(sizeof(common_clusters) / sizeof(common_clusters[0])));
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
            
            // CLIENT-side Basic Cluster - needed to receive Read Attributes Responses
            // when we query remote devices' Basic Cluster (ManufacturerName, ModelIdentifier)
            esp_zb_attribute_list_t *basic_client_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_BASIC);
            if (basic_client_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, basic_client_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Basic cluster 0x0000 added (CLIENT for remote queries)"));
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
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Temp cluster 0x0402 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *humidity_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT);
            if (humidity_cluster) {
                esp_zb_cluster_list_add_humidity_meas_cluster(_cluster_list, humidity_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Humidity cluster 0x0405 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *soil_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE);
            if (soil_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, soil_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Soil moisture cluster 0x0408 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *pressure_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT);
            if (pressure_cluster) {
                esp_zb_cluster_list_add_pressure_meas_cluster(_cluster_list, pressure_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Pressure cluster 0x0403 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *light_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT);
            if (light_cluster) {
                esp_zb_cluster_list_add_illuminance_meas_cluster(_cluster_list, light_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Illuminance cluster 0x0400 added (CLIENT)"));
            }

            esp_zb_attribute_list_t *power_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_POWER_CONFIG);
            if (power_cluster) {
                esp_zb_cluster_list_add_power_config_cluster(_cluster_list, power_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Power config cluster 0x0001 added (CLIENT)"));
            }

            // Tuya manufacturer-specific cluster (0xEF00) as CLIENT so we receive
            // incoming DP reports from Tuya devices (GIEX GX-04, etc.)
            esp_zb_attribute_list_t *tuya_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC);
            if (tuya_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, tuya_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya cluster 0xEF00 added (CLIENT)"));
            }

            // Tuya cluster 0xEF00 as SERVER — Tuya devices check that the
            // coordinator advertises this cluster as a server during the
            // match descriptor / interview phase.  Without this, many Tuya
            // devices do not complete joining or refuse to send DP reports.
            esp_zb_attribute_list_t *tuya_srv_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC);
            if (tuya_srv_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, tuya_srv_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya cluster 0xEF00 added (SERVER)"));
            }
        }
        
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
        
        // DEBUG_PRINTLN(F("[ZIGBEE-GW] Report receiver endpoint created (full cluster config)"));

        // Allow multiple device bindings so findEndpoint() is called for every
        // new device announcement, not just the first one.
        _allow_multiple_binding = true;
    }
    
    virtual ~GwZigbeeReportReceiver() = default;

    // Called by ZigbeeCore when a new device announces itself (DEVICE_ANNCE).
    // Query the device for basic information and Tuya DPs if applicable.
    // Device will be added to discovered list only when it responds.
    void findEndpoint(esp_zb_zdo_match_desc_req_param_t *cmd_req) override {
        if (!cmd_req) return;
        uint16_t short_addr = cmd_req->dst_nwk_addr;
        // DEBUG_PRINTF("[ZIGBEE-GW] Device announced: short=0x%04X — querying for info\n", short_addr);
        
        // Send Basic Cluster query to trigger response and add confirmed device
        gw_query_basic_cluster(short_addr, 1);
        
        // Send Tuya DP query in parallel (no-op for non-Tuya devices)
        gw_tuya_send_dp_query(short_addr, 1);
    }

    // Override zbReadBasicCluster to handle Basic Cluster read responses ourselves.
    // The base class implementation calls xSemaphoreGive(lock), but 'lock' may be
    // uninitialised due to an Arduino Zigbee library bug (ZigbeeEP constructor
    // checks 'if (!lock)' before ever assigning it, so heap-poisoned memory
    // 0xfefefefe is treated as valid → crash in xQueueGenericSend).
    // Note: This alternative callback path may not include address  info,
    // so device addition happens primarily via zbAttributeRead.
    void zbReadBasicCluster(const esp_zb_zcl_attribute_t *attribute) override {
        if (!attribute) return;
        // DEBUG_PRINTF(F("[ZIGBEE-GW] Basic Cluster attr 0x%04X received via zbReadBasicCluster\n"),
                     // attribute->id);
        // If the pending active read was on the Basic cluster, clear the flag now.
        if (gw_read_pending && gw_read_pending_cluster == ZB_ZCL_CLUSTER_ID_BASIC) {
            gw_read_pending = false;
        }
    }

    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute,
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) {
            // DEBUG_PRINTLN(F("[ZIGBEE-GW] zbAttributeRead called with NULL attribute!"));
            return;
        }

        gw_read_pending = false;
        
        // DEBUG_PRINTF(F("[ZIGBEE-GW] >>> zbAttributeRead: cluster=0x%04X attr=0x%04X type=0x%02X src_ep=%d src_short=0x%04X\n"),
                    // cluster_id, attribute->id, attribute->data.type, src_endpoint, src_address.u.short_addr);
        
        // Resolve IEEE from short address and add as responsive device
        uint64_t ieee_addr = 0;
        esp_zb_ieee_addr_t raw_ieee;
        if (esp_zb_ieee_address_by_short(src_address.u.short_addr, raw_ieee) == ESP_OK) {
            for (int i = 7; i >= 0; i--) ieee_addr = (ieee_addr << 8) | raw_ieee[i];
            if (ieee_addr) gw_add_responsive_device(src_address.u.short_addr, ieee_addr, src_endpoint);
        }
        
        int32_t value = extractAttributeValue(attribute);
        
        // DEBUG_PRINTF(F("[ZIGBEE-GW] >>> resolved ieee=%08lX%08lX value=%ld\n"),
                    // (unsigned long)(ieee_addr >> 32), (unsigned long)(ieee_addr & 0xFFFFFFFF), value);
        
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
            
            DEBUG_PRINTF(F("[ZIGBEE-GW] ✓ Report CACHED [%d/%d]: IEEE=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld ep=%d\n"),
                        (int)pending_report_count, (int)MAX_PENDING_REPORTS,
                        (unsigned long)(ieee_addr >> 32), (unsigned long)(ieee_addr & 0xFFFFFFFF),
                        cluster_id, attribute->id, value, src_endpoint);
        } else {
            DEBUG_PRINTF(F("[ZIGBEE-GW] ✗ Report cache FULL [%d/%d] - dropping report! cluster=0x%04X attr=0x%04X\n"),
                        (int)pending_report_count, (int)MAX_PENDING_REPORTS, cluster_id, attribute->id);
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
                // DEBUG_PRINTF(F("[ZIGBEE-GW] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// ========== Helper: update sensor from cached report ==========

static void gw_updateSensorFromReport(ZigbeeSensor* zb_sensor, const ZigbeeAttributeReport& report, bool solicited) {
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
        } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE) {
            // Tuya soil moisture is already raw % (0-100)
            converted_value = report.value;
        } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG) {
            // Tuya battery is raw % (0-100), keep as-is
            zb_sensor->last_battery = (uint32_t)report.value;
        }
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
    zb_sensor->last_read = os.now_tz();
    zb_sensor->last = zb_sensor->last_read;
    zb_sensor->last_report_at_ms = millis();

    // Update communication mode based on whether this report was solicited
    // (response to our ZCL Read Attributes) or unsolicited (device-pushed report).
    // Battery/power config reports are excluded — they don't indicate data channel.
    uint16_t report_attr_raw = report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED;
    bool is_battery = (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                       report_attr_raw == 0x0021);
    if (!is_battery) {
        ZbCommMode new_mode = solicited ? ZB_COMM_ACTIVE : ZB_COMM_REPORT;
        if (zb_sensor->comm_mode == ZB_COMM_UNKNOWN ||
            (new_mode == ZB_COMM_REPORT && zb_sensor->comm_mode == ZB_COMM_ACTIVE)) {
            if (zb_sensor->comm_mode != new_mode) {
                zb_sensor->comm_mode = new_mode;
                gw_comm_mode_changed = true;
                DEBUG_PRINTF(F("[ZIGBEE-GW] '%s' comm_mode → %s\n"), zb_sensor->name,
                             new_mode == ZB_COMM_REPORT ? "REPORT" : "ACTIVE");
            }
        }
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW] Sensor updated: cluster=0x%04X raw=%ld conv=%.2f factor=%d div=%d offset=%d\n"),
                report.cluster_id, report.value, converted_value, zb_sensor->factor, zb_sensor->divider, zb_sensor->offset_mv);
}

// ========== NVRAM erase ==========

/**
 * @brief Send a ZCL Read Attributes request for Basic Cluster to a remote device (GW mode)
 * Reads ManufacturerName (0x0004) and ModelIdentifier (0x0005) in one request.
 */
static bool gw_query_basic_cluster_internal(uint16_t short_addr, uint8_t endpoint) {
    if (!gw_zigbee_initialized || !gw_reportReceiver) return false;
    if (!Zigbee.started() || !Zigbee.connected()) return false;
    if (short_addr == 0xFFFF || short_addr == 0xFFFE) return false;
    
    // DEBUG_PRINTF(F("[ZIGBEE-GW] Querying Basic Cluster: short=0x%04X ep=%d\n"),
                 // short_addr, endpoint);
    
    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));
    read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;  // our endpoint
    read_req.clusterID = ZB_ZCL_CLUSTER_ID_BASIC;
    read_req.attr_number = 2;
    read_req.attr_field = s_gw_basic_query_attrs;
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_read_attr_cmd_req(&read_req);
    esp_zb_lock_release();
    
    DEBUG_PRINTF(F("[ZIGBEE-GW] Basic Cluster read request sent: short=0x%04X ep=%d\n"), short_addr, endpoint);
    return true;
}

// Alias for direct Basic Cluster query (no queueing—direct send)
static bool gw_query_basic_cluster(uint16_t short_addr, uint8_t endpoint) {
    return gw_query_basic_cluster_internal(short_addr, endpoint);
}

void sensor_zigbee_gw_query_basic_cluster(uint16_t short_addr, uint8_t endpoint) {
    // Directly query the device (no queueing)
    // DEBUG_PRINTF(F("[ZIGBEE-GW] API: Querying Basic Cluster for device 0x%04X\n"), short_addr);
    gw_query_basic_cluster(short_addr, endpoint);
}

bool sensor_zigbee_gw_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                     uint16_t cluster_id, uint16_t attribute_id) {
    if (!gw_zigbee_initialized || !gw_reportReceiver) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Read FAILED: GW not initialized"));
        return false;
    }
    if (!Zigbee.started() || !Zigbee.connected()) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Read FAILED: ZB not started=%d or connected=%d\n"),
                    Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0);
        return false;
    }
    if (device_ieee == 0) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Read FAILED: device_ieee is 0"));
        return false;
    }
    // Block only if the exact same (IEEE + cluster) is still pending — allows concurrent reads on different clusters of the same device
    if (gw_read_pending && gw_read_pending_ieee == device_ieee && gw_read_pending_cluster == cluster_id) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Read BLOCKED: same cluster already pending (%.0fs ago). ieee=%016llX cluster=0x%04X attr=0x%04X\n"),
                    (millis() - gw_read_time) / 1000.0, (unsigned long long)device_ieee, cluster_id, attribute_id);
        return false;
    }

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }

    gw_read_attr_id = attribute_id;

    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));

    esp_zb_lock_acquire(portMAX_DELAY);
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);

    if (short_addr != 0xFFFF && short_addr != 0xFFFE) {
        // Short address known: use 16-bit unicast (lower overhead)
        read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    } else {
        // Short address not in coordinator table (e.g. after reboot).
        // Use 64-bit IEEE address mode — the ZigBee stack resolves
        // the route automatically if the device is still on the network.
        // DEBUG_PRINTF(F("[ZIGBEE-GW] Short addr unknown for ieee=%016llX (table may have been cleared after reboot). Using 64-bit IEEE mode.\n"),
                     // (unsigned long long)device_ieee);
        read_req.address_mode = ESP_ZB_APS_ADDR_MODE_64_ENDP_PRESENT;
        memcpy(read_req.zcl_basic_cmd.dst_addr_u.addr_long, ieee_le, sizeof(ieee_le));
    }

    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;
    read_req.clusterID = cluster_id;
    read_req.attr_number = 1;
    read_req.attr_field = &gw_read_attr_id;

    esp_zb_zcl_read_attr_cmd_req(&read_req);
    gw_read_pending = true;
    gw_read_time = millis();
    gw_read_pending_ieee = device_ieee;
    gw_read_pending_cluster = cluster_id;
    esp_zb_lock_release();

    DEBUG_PRINTF(F("[ZIGBEE-GW] ✓ Active read SENT: ieee=%016llX short=0x%04X ep=%d cluster=0x%04X attr=0x%04X\n"),
                 (unsigned long long)device_ieee, short_addr, endpoint, cluster_id, attribute_id);
    return true;
}

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

// Schedule Configure Reporting for every SENSOR_ZIGBEE that has a known IEEE address.
// Called once when the Zigbee coordinator network forms (or re-forms after reboot).
static void gw_schedule_configure_reporting_all(unsigned long initial_delay_ms = 5000) {
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    int count = 0;
    unsigned long now = millis();
    unsigned long delay_ms = initial_delay_ms;

    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
        ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
        if (zb->device_ieee == 0) continue;

        uint ri = zb->read_interval ? zb->read_interval : 60;
        uint16_t max_interval = (ri >= 15 && ri <= 3600) ? (uint16_t)ri : 120;

        bool found = false;
        for (const auto& ex : gw_config_report_queue) {
            if (ex.ieee_addr == zb->device_ieee &&
                ex.cluster_id == zb->cluster_id &&
                ex.attr_id == zb->attribute_id) {
                found = true;
                break;
            }
        }
        if (found) continue;

        GwConfigReportRequest req;
        req.ieee_addr      = zb->device_ieee;
        req.endpoint       = zb->endpoint;
        req.cluster_id     = zb->cluster_id;
        req.attr_id        = zb->attribute_id;
        req.min_interval   = 10;
        req.max_interval   = max_interval;
        req.scheduled_time = now + delay_ms;
        gw_config_report_queue.push_back(req);
        // Store configured interval in sensor for predictive boost
        zb->report_interval_s = max_interval;
        delay_ms += 1000;
        count++;
    }
    if (count > 0) {
        // DEBUG_PRINTF("[ZIGBEE-GW] Scheduled configure reporting for %d sensor(s)\n", count);
    }
}

void sensor_zigbee_gw_factory_reset() {
    gw_zigbee_needs_nvram_reset = true;
}

void sensor_zigbee_gw_stop() {
    // Zigbee Coordinator stays running permanently (non-Matter mode).
    // The Arduino Zigbee library does NOT support stop+restart anyway.
    // This function is now a no-op. Coexistence priorities are
    // managed dynamically via the radio arbiter.
    if (!gw_zigbee_initialized) return;
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] stop() called — Zigbee stays active (permanent mode)"));
}

void sensor_zigbee_gw_start() {
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] sensor_zigbee_gw_start() called"));
    // DEBUG_PRINTF("[ZIGBEE-GW] ieee802154 mode: %d (%s)\n",
                // (int)ieee802154_get_mode(), ieee802154_mode_name(ieee802154_get_mode()));

    if (!ieee802154_is_zigbee_gw()) {
        static bool mode_warning_shown = false;
        if (!mode_warning_shown) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Not in ZIGBEE_GATEWAY mode - Zigbee GW disabled"));
            mode_warning_shown = true;
        }
        return;
    }

    if (gw_zigbee_initialized) {
        // DEBUG_PRINTLN(F("[ZIGBEE-GW] Already initialized, skipping"));
        return;
    }

    // Zigbee stays active once started (no stop/restart toggling).

    if (gw_zigbee_needs_nvram_reset) {
        gw_zigbee_needs_nvram_reset = false;
        gw_erase_zigbee_nvram();
    }

    esp_zb_radio_config_t radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE };
    esp_zb_host_config_t host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE };
    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);

    if (WiFi.getMode() != WIFI_MODE_NULL) {
        // DEBUG_PRINTLN(F("[ZIGBEE-GW] WiFi active - coexistence base already configured"));
        // NOTE: Per-packet PTI must be set AFTER Zigbee.begin() — ieee802154_mac_init()
        // inside esp_zb_start() resets all PTI values to defaults.
    } else {
        // DEBUG_PRINTLN(F("[ZIGBEE-GW] No WiFi - Zigbee has full radio access (Ethernet mode)"));
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
    // DEBUG_PRINTF("[ZIGBEE-GW] Primary channel mask set to 0x%08X\n",
                 // (unsigned)ZIGBEE_COEX_CHANNEL_MASK);
#else
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] Using default channel mask (all channels 11-26)"));
#endif

    // Log heap state before Zigbee init for diagnostics
    // DEBUG_PRINTF(F("[ZIGBEE-GW] Heap before init: internal free=%u, largest=%u, PSRAM free=%u, largest=%u\n"),
                 // heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 // heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 // heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 // heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

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
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] Zigbee Coordinator started successfully!"));
    // DEBUG_PRINTF("[ZIGBEE-GW] Zigbee.started()=%d, Zigbee.connected()=%d\n",
                // Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0);

    // Register APS-layer indication handler for Tuya DP protocol (cluster 0xEF00).
    // This intercepts raw Tuya frames before ZCL processing and converts them
    // into standard report-cache entries for sensors like GIEX GX-04.
    esp_zb_aps_data_indication_handler_register(gw_tuya_aps_indication_handler);
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya APS indication handler registered"));

    // DEBUG_PRINTF(F("[ZIGBEE-GW] Heap AFTER init: internal free=%u, largest=%u, PSRAM free=%u, largest=%u\n"),
                 // heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 // heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 // heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 // heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Dynamic coex: re-initialise after Zigbee.begin() which resets PTI defaults

    // Log channel and PAN ID for diagnostics
    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t zb_channel = esp_zb_get_current_channel();
    uint16_t zb_pan_id = esp_zb_get_pan_id();
    esp_zb_ieee_addr_t ext_pan_raw;
    esp_zb_get_extended_pan_id(ext_pan_raw);
    esp_zb_lock_release();
    uint64_t zb_ext_pan = 0;
    for (int i = 7; i >= 0; i--) zb_ext_pan = (zb_ext_pan << 8) | ext_pan_raw[i];
    // DEBUG_PRINTF(F("[ZIGBEE-GW] Network: channel=%d PAN=0x%04X extPAN=%08lX%08lX\n"),
                 // zb_channel, zb_pan_id,
                 // (unsigned long)(zb_ext_pan >> 32), (unsigned long)(zb_ext_pan & 0xFFFFFFFF));

    // Network stays closed after init. Use the HTTP API ("zj" command)
    // to open the network for joining when pairing new devices.
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] Network closed — use API to open for joining"));
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

    // Allow Zigbee startup even while WiFi/Ethernet is reconnecting.
    // Discovery/pairing should not be blocked by temporary network reconnects.
    bool is_ethernet = (wmode == WIFI_MODE_NULL);
    if (!is_ethernet && WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] WiFi not connected yet - starting Zigbee anyway"));
    } else if (is_ethernet && !os.network_connected()) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Ethernet not connected yet - starting Zigbee anyway"));
    }

    DEBUG_PRINTLN(F("[ZIGBEE-GW] ensure_started: network ready, starting Zigbee GW..."));
    sensor_zigbee_gw_start();
    DEBUG_PRINTF("[ZIGBEE-GW] ensure_started result: initialized=%d\n", gw_zigbee_initialized ? 1 : 0);
    return gw_zigbee_initialized;
}

void sensor_zigbee_gw_process_reports(uint64_t ieee_addr, uint8_t endpoint,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       int32_t value, uint8_t lqi) {
    
    // Log processing status if there are pending reports
    static unsigned long last_report_debug = 0;
    if (pending_report_count > 0 || (millis() - last_report_debug > 30000)) {
        last_report_debug = millis();
        // DEBUG_PRINTF(F("[ZIGBEE-GW] PROCESS: %d pending, now checking sensors...\n"), pending_report_count);
    }
    
    for (size_t i = 0; i < pending_report_count; i++) {
        ZigbeeAttributeReport& report = pending_reports[i];
        
        // Skip already-consumed or expired reports
        if (report.consumed || millis() - report.timestamp > REPORT_VALIDITY_MS) {
            continue;
        }
        
        // DEBUG_PRINTF(F("[ZIGBEE-GW] Report[%d]: ieee=%08lX%08lX ep=%d cluster=0x%04X attr=0x%04X val=%ld lqi=%d\n"),
                    // (int)i,
                    // (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                    // report.endpoint, report.cluster_id, report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED,
                    // report.value, report.lqi);
        
        // Determine if this report is a response to our own ZCL Read Attributes request
        // (solicited) or a device-initiated unsolicited attribute report.
        bool report_solicited = gw_read_pending &&
                                gw_read_pending_ieee    == report.ieee_addr &&
                                gw_read_pending_cluster == report.cluster_id;

        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        bool found = false;
        int checked_count = 0;
        
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
            checked_count++;
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
            
            if (matches) {
                DEBUG_PRINTF(F("[ZIGBEE-GW]   ✓ Matched '%s': c=0x%04X a=0x%04X ieee=%08lX%08lX → raw=%ld\n"),
                            zb_sensor->name, report.cluster_id, report_attr_unmasked,
                            (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                            report.value);
                gw_updateSensorFromReport(zb_sensor, report, report_solicited);
                found = true;
                // Don't break — multiple logical sensors may reference the
                // same physical device (same IEEE/cluster/attr).  Continue
                // iterating so every matching sensor is updated.
            }
        }
        
        // Mark consumed after iterating ALL sensors so every match is serviced
        report.consumed = true;
        
        if (checked_count == 0) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] ✗ WARNING: No ZigBee sensors configured! (checked empty list)"));
        } else if (!found) {
            DEBUG_PRINTF(F("[ZIGBEE-GW] ✗ NO MATCH (checked %d ZigBee sensor%s)\n"), checked_count, checked_count == 1 ? "" : "s");
            DEBUG_PRINTF(F("[ZIGBEE-GW]   Report detail: ieee=0x%016llX ep=%u cluster=0x%04X attr=0x%04X value=%ld%s solicited=%d\n"),
                         (unsigned long long)report.ieee_addr,
                         report.endpoint,
                         report.cluster_id,
                         report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED,
                         report.value,
                         (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) ? " (Tuya)" : "",
                         report_solicited ? 1 : 0);

            SensorIterator dbg_it = sensors_iterate_begin();
            SensorBase* dbg_sensor;
            bool same_ieee_found = false;
            while ((dbg_sensor = sensors_iterate_next(dbg_it)) != NULL) {
                if (!dbg_sensor || dbg_sensor->type != SENSOR_ZIGBEE) continue;
                ZigbeeSensor* dbg_zb = static_cast<ZigbeeSensor*>(dbg_sensor);
                if (report.ieee_addr == 0 || dbg_zb->device_ieee != report.ieee_addr) continue;
                same_ieee_found = true;
                DEBUG_PRINTF(F("[ZIGBEE-GW]   Candidate '%s': ep=%u cluster=0x%04X attr=0x%04X mfr=\"%s\" model=\"%s\" vendor=\"%s\" data_ok=%d\n"),
                             dbg_sensor->name,
                             dbg_zb->endpoint,
                             dbg_zb->cluster_id,
                             dbg_zb->attribute_id,
                             dbg_zb->zb_manufacturer,
                             dbg_zb->zb_model,
                             dbg_zb->zb_vendor,
                             dbg_zb->flags.data_ok ? 1 : 0);
            }
            if (!same_ieee_found && report.ieee_addr != 0) {
                DEBUG_PRINTF(F("[ZIGBEE-GW]   No configured ZigBee sensor bound to ieee=0x%016llX\n"),
                             (unsigned long long)report.ieee_addr);
            }
        }
        
        if (!found) {
            
            // Auto-correct misconfigured sensors: if a known standard ZCL cluster report comes in
            // for a sensor that has the correct IEEE address but wrong cluster/attribute configured,
            // correct the stored cluster_id/attribute_id and re-save.
            // This handles cases where sensors were saved with wrong values (e.g. 0x0401 instead of 0x0405).
            uint16_t report_attr_raw = report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED;
            bool is_known_standard_cluster = (
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT         && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE            && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT     && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT  && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT         && report_attr_raw == 0x0000)
            );
            if (is_known_standard_cluster && report.ieee_addr != 0) {
                SensorIterator ac_it = sensors_iterate_begin();
                SensorBase* ac_sensor;
                while ((ac_sensor = sensors_iterate_next(ac_it)) != NULL) {
                    if (!ac_sensor || ac_sensor->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb_ac = static_cast<ZigbeeSensor*>(ac_sensor);
                    if (zb_ac->device_ieee != report.ieee_addr) continue;
                    if (zb_ac->flags.data_ok) continue;  // already receiving data correctly
                    if (zb_ac->cluster_id == report.cluster_id && zb_ac->attribute_id == report_attr_raw) continue;  // already correct
                    // Correct the misconfigured cluster/attribute
                    // DEBUG_PRINTF(F("[ZIGBEE-GW] Auto-correcting sensor '%s': cluster 0x%04X→0x%04X attr 0x%04X→0x%04X\n"),
                                // zb_ac->name, zb_ac->cluster_id, report.cluster_id, zb_ac->attribute_id, report_attr_raw);
                    zb_ac->cluster_id = report.cluster_id;
                    zb_ac->attribute_id = report_attr_raw;
                    sensor_save();
                    // Now apply the report to this newly-corrected sensor
                    // Auto-correct reports are always unsolicited (we didn't ask for them)
                    gw_updateSensorFromReport(zb_ac, report, false);
                }
            }

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
                        // DEBUG_PRINTF(F("[ZIGBEE-GW]   (%d identical unmatched reports suppressed)\n"), unmatched_count - 1);
                    }
                    // DEBUG_PRINTF(F("[ZIGBEE-GW] Unmatched report: ieee=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld%s\n"),
                                // (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                                // report.cluster_id, report.attr_id & ~TUYA_REPORT_FLAG_PRESCALED, report.value,
                                // (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) ? " (Tuya)" : "");
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

// Track when the join window closes so radio lock can be released

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
    // DEBUG_PRINTF("[ZIGBEE-GW] Opening network for %d seconds (permit join), started=%d connected=%d\n",
                 // dur, Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0);

    // Acquire exclusive radio ownership for the scan window
    unsigned long window_ms = (unsigned long)dur * 1000UL;
    unsigned long now_ms = millis();
    unsigned long requested_end = now_ms + window_ms;
    if (gw_join_window_end != 0 && (long)(gw_join_window_end - now_ms) > 0) {
        if (requested_end > gw_join_window_end) {
            // DEBUG_PRINTF("[ZIGBEE-GW] Join window extended: remaining=%lu ms -> %lu ms\n",
                         // (unsigned long)(gw_join_window_end - now_ms),
                         // (unsigned long)(requested_end - now_ms));
            gw_join_window_end = requested_end;
        } else {
            // DEBUG_PRINTF("[ZIGBEE-GW] Join window already open: remaining=%lu ms\n",
                         // (unsigned long)(gw_join_window_end - now_ms));
        }
    } else {
        gw_join_window_end = requested_end;
    }


    // Enter join mode: applies maximum radio priority (txrx=HIGH) for the full
    // join window duration.  On 2.4 GHz this also disconnects WiFi for up to 10s.
    // The coex manager auto-exits join mode when duration_ms elapses, and

    esp_zb_lock_acquire(portMAX_DELAY);
    Zigbee.openNetwork(dur);
    esp_zb_lock_release();
    // DEBUG_PRINTLN(F("[ZIGBEE-GW] Network open for joining"));
}

void sensor_zigbee_gw_loop() {
    if (!gw_zigbee_initialized) return;

    // Persist comm_mode changes that were set during report processing.
    if (gw_comm_mode_changed) {
        gw_comm_mode_changed = false;
        sensor_save();
    }

    // Timeout stale active-read requests
    if (gw_read_pending && millis() - gw_read_time > GW_READ_TIMEOUT_MS) {
        gw_read_pending = false;
        DEBUG_PRINTF(F("[ZIGBEE-GW] ⚠ Read TIMEOUT: attr_id=0x%04X waited %lu ms (GW_READ_TIMEOUT_MS=%lu). Clearing flag.\n"),
                    gw_read_attr_id, millis() - gw_read_time, GW_READ_TIMEOUT_MS);
    }

    // Always process pending reports regardless of web priority.
    // Zigbee ZCL reports are lightweight and must not be starved.
    // During join mode the radio is already dedicated to ZigBee — skip the
    // lock acquire/release cycle to avoid noisy strategy reapply calls.
    if (pending_report_count > 0) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW] LOOP: %d reports waiting → processing now...\n"), pending_report_count);
        
        sensor_zigbee_gw_process_reports(0, 0, 0, 0, 0, 0);
        
        // DEBUG_PRINTF(F("[ZIGBEE-GW] LOOP: processing done, %d reports remaining\n"), pending_report_count);
    }
    
    static bool last_connected = false;
    bool connected = Zigbee.started() && Zigbee.connected();
    if (connected != last_connected) {
        if (connected) {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Coordinator network FORMED"));
            // Schedule Configure Reporting for all sensors with known IEEE addresses.
            // Sleeping end devices (AQARA, etc.) will receive this on their next wake
            // and start pushing reports proactively.
            gw_schedule_configure_reporting_all(5000);

            // Auto-open network once on startup so previously-paired devices can
            // rejoin after a coordinator restart (firmware update, power cycle, etc.)
            // without the user sending the "zj" command manually.
            static bool gw_startup_open_done = false;
            if (!gw_startup_open_done) {
                gw_startup_open_done = true;
                DEBUG_PRINTLN(F("[ZIGBEE-GW] Auto-opening network 180s for device rejoin after restart"));
                sensor_zigbee_gw_open_network(180);
            }

            // Proactively query known Tuya sensors via ZBOSS address table.
            // If ZBOSS NVRAM is intact after a restart, esp_zb_address_short_by_ieee()
            // resolves the short address and we send a DP query immediately, before
            // the device announces itself, so data starts flowing right away.
            {
                SensorIterator it_sq = sensors_iterate_begin();
                SensorBase* s_sq;
                while ((s_sq = sensors_iterate_next(it_sq)) != NULL) {
                    if (!s_sq || s_sq->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb_sq = static_cast<ZigbeeSensor*>(s_sq);
                    if (zb_sq->device_ieee == 0) continue;
                    // Tuya devices have manufacturer names starting with "_TZ"
                    if (zb_sq->zb_manufacturer[0] != '_' || zb_sq->zb_manufacturer[1] != 'T') continue;
                    bool sent = sensor_zigbee_gw_request_dp_query(
                        zb_sq->device_ieee, zb_sq->endpoint ? zb_sq->endpoint : 1);
                    if (sent) {
                        DEBUG_PRINTF(F("[ZIGBEE-GW] Startup DP query → '%s' (ieee=%016llX)\n"),
                                     s_sq->name, (unsigned long long)zb_sq->device_ieee);
                    }
                }
            }
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE-GW] Coordinator network LOST"));
        }
        last_connected = connected;
        gw_zigbee_connected = connected;
    }

    // NOTE: No idle timeout - Arduino Zigbee library does NOT support
    // Zigbee.stop() + Zigbee.begin() restart (causes Load access fault).
    // Once started, Zigbee stays running until reboot.

    // Release radio lock and restore WiFi after join window expires.
    if (gw_join_window_end != 0 && millis() > gw_join_window_end) {
        gw_join_window_end = 0;
        // DEBUG_PRINTLN(F("[ZIGBEE-GW] Join window closed, radio released"));
    }

    // Boot-settle guard: wait 90s after first ZigBee connection before
    // sending Tuya DP refresh queries (avoids hammering devices during startup).
    static unsigned long gw_first_connected_ms = 0;
    if (connected && gw_first_connected_ms == 0) gw_first_connected_ms = millis();
    bool boot_settled = gw_first_connected_ms > 0 && (millis() - gw_first_connected_ms) > 90000;

    // Periodic Tuya DP refresh: send a "get all datapoints" query (cmd 0x00) to
    // every confirmed Tuya device that has at least one stale REPORT-mode sensor.
    // This mirrors the Z2M behaviour (dp: true in tuyaBase) and ensures sensors
    // that only send DP 3 (soil moisture) on significant changes still get polled.
    // Rate-limited to once per 90 s globally; this is also the earliest we will
    // fire after boot-settle, so the first query arrives very shortly after WiFi
    // has stabilised (boot_settled = 90 s after first connection).
    static unsigned long gw_tuya_refresh_ms = 0;
    if (connected && boot_settled && !(gw_join_window_end != 0)) {
        unsigned long now_tr = millis();
        if (now_tr - gw_tuya_refresh_ms >= 90000UL) {
            gw_tuya_refresh_ms = now_tr;
            for (auto& dev : gw_discovered_devices) {  // non-const: we update silent_query_count
                if (!dev.is_tuya || !dev.has_responded) continue;
                // Check if any REPORT-mode sensor for this device is stale
                bool needs_refresh = false;
                SensorIterator it_tr = sensors_iterate_begin();
                SensorBase* s_tr;
                while ((s_tr = sensors_iterate_next(it_tr)) != NULL) {
                    if (!s_tr || s_tr->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb_tr = static_cast<ZigbeeSensor*>(s_tr);
                    if (zb_tr->device_ieee != dev.ieee_addr) continue;
                    if (zb_tr->comm_mode == ZB_COMM_REPORT) {
                        uint32_t intv_tr = zb_tr->read_interval ? zb_tr->read_interval : 60;
                        // Stale = no report this boot, or last report > 2× read_interval ago
                        if (zb_tr->last_report_at_ms == 0 ||
                            (now_tr - zb_tr->last_report_at_ms) > (unsigned long)intv_tr * 2000UL) {
                            needs_refresh = true;
                            break;
                        }
                    }
                }
                if (needs_refresh) {
                    bool sent = sensor_zigbee_gw_request_dp_query(dev.ieee_addr, dev.endpoint);
                    if (sent) {
                        dev.silent_query_count++;
                        unsigned long silent_s = dev.last_rx_at_ms > 0
                            ? (now_tr - dev.last_rx_at_ms) / 1000UL : 9999UL;
                        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Refresh query → ieee=%016llX ep=%d (silent=%lus, count=%d)\n"),
                                     (unsigned long long)dev.ieee_addr, dev.endpoint,
                                     silent_s, dev.silent_query_count);

                        // After 5 unanswered queries (~7.5 min), open network so the device
                        // can rejoin if it lost its network slot after a coordinator restart.
                        // Reset counter after opening to wait another 5 cycles before retrying.
                        if (dev.silent_query_count >= 5 && !(gw_join_window_end != 0)) {
                            DEBUG_PRINTF(F("[ZIGBEE-GW] ⚠ Device %016llX silent %lus — auto-opening network for rejoin (60s)\n"),
                                         (unsigned long long)dev.ieee_addr, silent_s);
                            sensor_zigbee_gw_open_network(60);
                            dev.silent_query_count = 0;  // avoid re-opening every cycle
                        }
                    }
                }
            }
        }
    }

    // Process pending Configure Reporting requests (one per stagger window)
    if (connected && !gw_config_report_queue.empty()) {
        unsigned long now_ms = millis();
        if (now_ms - gw_last_config_report_ms >= GW_CONFIG_REPORT_STAGGER_MS) {
            for (auto it_cr = gw_config_report_queue.begin(); it_cr != gw_config_report_queue.end(); ++it_cr) {
                if (now_ms >= it_cr->scheduled_time) {
                    sensor_zigbee_gw_configure_reporting(
                        it_cr->ieee_addr, it_cr->endpoint,
                        it_cr->cluster_id, it_cr->attr_id,
                        it_cr->min_interval, it_cr->max_interval);
                    gw_last_config_report_ms = now_ms;
                    gw_config_report_queue.erase(it_cr);
                    break;  // send one at a time; restart next loop
                }
            }
        }
    }

    static unsigned long last_status_print = 0;
    if (millis() - last_status_print > 60000) {
        last_status_print = millis();
        // DEBUG_PRINTF("[ZIGBEE-GW] Status: started=%d connected=%d devices=%d pending_reports=%d\n",
                    // Zigbee.started() ? 1 : 0, Zigbee.connected() ? 1 : 0,
                    // (int)gw_discovered_devices.size(), (int)pending_report_count);
        
        // Log registered sensors and their config for debugging
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        int zb_count = 0;
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (sensor && sensor->type == SENSOR_ZIGBEE) {
                ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
                // DEBUG_PRINTF("[ZIGBEE-GW]   Sensor '%s': ieee=%08lX%08lX ep=%d cluster=0x%04X attr=0x%04X data_ok=%d last=%.2f\n",
                            // sensor->name,
                            // (unsigned long)(zb->device_ieee >> 32), (unsigned long)(zb->device_ieee & 0xFFFFFFFF),
                            // zb->endpoint, zb->cluster_id, zb->attribute_id,
                            // zb->flags.data_ok ? 1 : 0, zb->last_data);
                zb_count++;
            }
        }
        if (zb_count == 0) {
            // DEBUG_PRINTLN(F("[ZIGBEE-GW]   No Zigbee sensors registered!"));
        }
        
        // Dump pending reports that haven't been consumed
        for (size_t i = 0; i < pending_report_count; i++) {
            ZigbeeAttributeReport& r = pending_reports[i];
            if (!r.consumed) {
                // DEBUG_PRINTF("[ZIGBEE-GW]   Pending[%d]: ieee=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld age=%lums\n",
                            // (int)i,
                            // (unsigned long)(r.ieee_addr >> 32), (unsigned long)(r.ieee_addr & 0xFFFFFFFF),
                            // r.cluster_id, r.attr_id & ~TUYA_REPORT_FLAG_PRESCALED, r.value,
                            // millis() - r.timestamp);
            }
        }
    }
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
