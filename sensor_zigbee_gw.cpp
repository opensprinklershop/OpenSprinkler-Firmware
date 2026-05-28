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
#include <nvs.h>
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

struct GwPendingSwitchCommand {
    bool valid;
    bool use_tuya;
    uint64_t ieee_addr;
    uint8_t endpoint;
    uint8_t dp_id;
    bool turnon;
    uint8_t attempts;
    unsigned long next_try_ms;
};

static GwPendingSwitchCommand gw_pending_switch = {false, false, 0, 1, 1, false, 0, 0};

struct GwTuyaScheduledCommand {
    bool used;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t command_id;
    uint8_t dp_id;
    uint8_t dp_type;
    uint16_t seq;
    uint8_t payload_len;
    uint8_t payload[20];
};

static GwTuyaScheduledCommand gw_tuya_schedule[8];
static uint32_t gw_tuya_next_due_ms = 0;

static void gw_tuya_schedule_next();

static void gw_tuya_scheduled_send(uint8_t slot) {
    if (slot >= (sizeof(gw_tuya_schedule) / sizeof(gw_tuya_schedule[0]))) return;
    GwTuyaScheduledCommand& cmd = gw_tuya_schedule[slot];
    if (!cmd.used) return;

    esp_zb_zcl_custom_cluster_cmd_req_t req = {};
    req.zcl_basic_cmd.dst_addr_u.addr_short = cmd.short_addr;
    req.zcl_basic_cmd.dst_endpoint = cmd.endpoint;
    req.zcl_basic_cmd.src_endpoint = 10;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.cluster_id = 0xEF00;
    req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    req.dis_default_resp = 1;
    req.custom_cmd_id = cmd.command_id;
    req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size = cmd.payload_len;
    req.data.value = cmd.payload;

    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();
    cmd.used = false;
    DEBUG_PRINTF(F("[ZIGBEE-GW] Tuya DP cmd dispatched, slot=%u tsn=%u dp=%u seq=%u short=0x%04X\n"),
                 (unsigned)slot, (unsigned)tsn, (unsigned)cmd.dp_id, (unsigned)cmd.seq, cmd.short_addr);
}

static void gw_tuya_schedule_next() {
    uint32_t now = millis();
    if (gw_tuya_next_due_ms != 0 && (int32_t)(now - gw_tuya_next_due_ms) < 0) return;

    for (uint8_t slot = 0; slot < (sizeof(gw_tuya_schedule) / sizeof(gw_tuya_schedule[0])); slot++) {
        if (!gw_tuya_schedule[slot].used) continue;
        gw_tuya_scheduled_send(slot);
        gw_tuya_next_due_ms = millis() + 250UL;
        return;
    }

    gw_tuya_next_due_ms = 0;
}

static bool gw_schedule_tuya_dp_cmd(uint16_t short_addr, uint8_t endpoint, uint8_t command_id,
                                    uint8_t dp_id, uint8_t dp_type, const uint8_t* payload,
                                    size_t payload_len, uint16_t seq) {
    if (!payload || payload_len == 0 || payload_len > sizeof(gw_tuya_schedule[0].payload)) return false;

    for (uint8_t slot = 0; slot < (sizeof(gw_tuya_schedule) / sizeof(gw_tuya_schedule[0])); slot++) {
        if (gw_tuya_schedule[slot].used) continue;

        gw_tuya_schedule[slot].used = true;
        gw_tuya_schedule[slot].short_addr = short_addr;
        gw_tuya_schedule[slot].endpoint = endpoint;
        gw_tuya_schedule[slot].command_id = command_id;
        gw_tuya_schedule[slot].dp_id = dp_id;
        gw_tuya_schedule[slot].dp_type = dp_type;
        gw_tuya_schedule[slot].seq = seq;
        gw_tuya_schedule[slot].payload_len = (uint8_t)payload_len;
        memcpy(gw_tuya_schedule[slot].payload, payload, payload_len);

        if (gw_tuya_next_due_ms == 0) gw_tuya_next_due_ms = millis() + 50UL;
        DEBUG_PRINTF(F("[ZIGBEE-GW] Tuya DP cmd queued, slot=%u dp=%u seq=%u short=0x%04X\n"),
                 (unsigned)slot, (unsigned)dp_id, (unsigned)seq, short_addr);
        return true;
    }

    DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya DP write failed: scheduler queue full"));
    return false;
}

static void gw_queue_pending_switch(bool use_tuya, uint64_t ieee_addr, uint8_t endpoint, uint8_t dp_id, bool turnon) {
    if (ieee_addr == 0) return;
    gw_pending_switch.valid = true;
    gw_pending_switch.use_tuya = use_tuya;
    gw_pending_switch.ieee_addr = ieee_addr;
    gw_pending_switch.endpoint = endpoint ? endpoint : 1;
    gw_pending_switch.dp_id = dp_id ? dp_id : 1;
    gw_pending_switch.turnon = turnon;
    if (gw_pending_switch.attempts < 250) gw_pending_switch.attempts++;
    gw_pending_switch.next_try_ms = millis() + 1000UL;
    DEBUG_PRINTF(F("[ZIGBEE-GW] Queued deferred %s command: ieee=%016llX ep=%d state=%d attempt=%d\n"),
                 use_tuya ? "Tuya-DP" : "On/Off",
                 (unsigned long long)ieee_addr,
                 gw_pending_switch.endpoint,
                 turnon ? 1 : 0,
                 gw_pending_switch.attempts);
}

// Try to normalize a possibly stale/truncated station IEEE against runtime
// discovered devices. This handles cases where stored station data contains a
// shifted 64-bit value (e.g. good_ieee >> 8), which would otherwise keep
// short-address resolution in a permanent "unknown" state.
static bool gw_try_fix_ieee(uint64_t* ieee_addr) {
    if (!ieee_addr || *ieee_addr == 0) return false;
    uint64_t in = *ieee_addr;
    for (const auto& dev : gw_discovered_devices) {
        if (dev.ieee_addr == 0) continue;
        if (dev.ieee_addr == in) return false; // already exact

        // Common corruption pattern seen in station payloads: one-byte right
        // shift of the IEEE value. Match both directions defensively.
        if ((dev.ieee_addr >> 8) == in || ((in >> 8) == dev.ieee_addr)) {
            *ieee_addr = dev.ieee_addr;
            DEBUG_PRINTF(F("[ZIGBEE-GW] Corrected station IEEE: %016llX -> %016llX\n"),
                         (unsigned long long)in,
                         (unsigned long long)*ieee_addr);
            return true;
        }
    }
    return false;
}

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
#define ZB_ZCL_CLUSTER_ID_METERING                  0x0702

// Basic Cluster attribute IDs
#define ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID     0x0001
#define ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID           0x0002
#define ZB_ZCL_ATTR_BASIC_HW_VERSION_ID              0x0003
#define ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID       0x0004
#define ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID        0x0005
#define ZB_ZCL_ATTR_BASIC_DATE_CODE_ID               0x0006
#define ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID            0x0007
#define ZB_ZCL_ATTR_BASIC_SW_BUILD_ID                0x4000

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
static uint16_t s_gw_basic_query_attrs[] = {
    ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID,
    ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID,
    ZB_ZCL_ATTR_BASIC_HW_VERSION_ID,
    ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
    ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
    ZB_ZCL_ATTR_BASIC_DATE_CODE_ID,
    ZB_ZCL_ATTR_BASIC_SW_BUILD_ID
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

struct GwBasicQueryRequest {
    uint64_t ieee_addr;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t attempts;
    unsigned long next_query_ms;
};
static std::vector<GwBasicQueryRequest> gw_basic_query_queue;
#define GW_BASIC_QUERY_RETRY_MS 5000UL
#define GW_BASIC_QUERY_MAX_ATTEMPTS 6

// Tuya manufacturer-specific cluster (manuSpecificTuya)
#define ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC             0xEF00

// Tuya DP (DataPoint) command IDs within cluster 0xEF00
// Per Tuya Standard MCU Protocol: https://developer.tuya.com/en/docs/mcu-standard-protocol/mcusdk-zigbee-uart-protocol
#define TUYA_CMD_QUERY_DP_DATA  0x00  // Gateway -> device (legacy query all DPs)
#define TUYA_CMD_RESPOND_DP     0x01  // Gateway -> device (legacy DP response / ACK)
#define TUYA_CMD_DP_REPORT      0x02  // Device -> gateway (DP report / unsolicited status)
#define TUYA_CMD_QUERY_REQ      0x03  // Gateway -> device (query DPs)
#define TUYA_CMD_DP_SEND        0x04  // Gateway -> device (legacy set DP command)
#define TUYA_CMD_REPORT_DP_DATA 0x06  // Gateway -> device (Report DP data - MAIN CONTROL COMMAND) ← Use this!
#define TUYA_CMD_REPORT_NO_LINK 0x2C  // Gateway -> device (Report DP without triggering linkage)
#define TUYA_CMD_MCU_VERSION_REQ  0x10  // Device → gateway (MCU version query)
#define TUYA_CMD_MCU_VERSION_RESP 0x11  // Gateway → device (MCU version answer)
#define TUYA_CMD_TIME_SYNC_REQ  0x24  // Device → gateway (time sync request)

// Legacy aliases for compatibility
#define TUYA_CMD_DATA_REQUEST   TUYA_CMD_QUERY_DP_DATA
#define TUYA_CMD_DATA_RESPONSE  TUYA_CMD_RESPOND_DP
#define TUYA_CMD_DATA_REPORT    TUYA_CMD_DP_REPORT
#define TUYA_CMD_DATA_QUERY     TUYA_CMD_QUERY_REQ
#define TUYA_CMD_DATA_SEND      TUYA_CMD_DP_SEND
#define TUYA_CMD_ACTIVE_STATUS  TUYA_CMD_DP_REPORT

// Tuya DP data types
#define TUYA_TYPE_RAW    0x00
#define TUYA_TYPE_BOOL   0x01
#define TUYA_TYPE_VALUE  0x02  // 4-byte big-endian integer
#define TUYA_TYPE_STRING 0x03
#define TUYA_TYPE_ENUM   0x04
#define TUYA_TYPE_BITMAP 0x05

// Flag to mark reports originating from Tuya DP parsing (value already scaled)
#define TUYA_REPORT_FLAG_PRESCALED  0x8000
#define TUYA_REPORT_TYPE_SHIFT      8
#define TUYA_REPORT_TYPE_MASK       0x0F00
#define TUYA_REPORT_DP_MASK         0x00FF

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

static bool gw_is_ignorable_unmatched_tuya_dp(uint16_t dp) {
    // These DPs are commonly emitted as valve state / meta / unit / battery
    // channels without dedicated user sensors. Treat them as non-actionable
    // for NO MATCH diagnostics to keep logs readable.
    return (dp == 2   ||   // valve switch state
            dp == 9   ||   // unit selector
            dp == 14  ||   // battery enum/value on some Tuya sensors
            dp == 0x65 ||  // vendor meta
            dp == 0x66 ||  // vendor meta
            dp == 0x6B ||  // GX02 status/meta
            dp == 0x6F);   // vendor meta/status
}

static uint8_t gw_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + c - 'A');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + c - 'a');
    return 0;
}

static uint64_t gw_parse_ieee_hex(const char* hex16) {
    uint64_t ieee = 0;
    if (!hex16) return 0;
    for (uint8_t i = 0; i < 16; i++) {
        ieee = (ieee << 4) | gw_hex_nibble(hex16[i]);
    }
    return ieee;
}

static uint8_t gw_parse_hex_u8(const char* hex, uint8_t len) {
    uint8_t value = 0;
    if (!hex) return 0;
    while (len--) {
        value = (uint8_t)((value << 4) | gw_hex_nibble(*hex++));
    }
    return value;
}

static bool gw_ieee_matches_station(uint64_t station_ieee, uint64_t report_ieee) {
    if (station_ieee == 0 || report_ieee == 0) return false;
    return station_ieee == report_ieee ||
           (station_ieee >> 8) == report_ieee ||
           (report_ieee >> 8) == station_ieee;
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

static bool gw_is_giex_water_valve(uint64_t ieee) {
    if (ieee == 0) return false;

    const char* mfr = "";
    const char* mdl = "";
    const char* vnd = "";

    for (const auto& dev : gw_discovered_devices) {
        if (dev.ieee_addr != ieee) continue;
        mfr = dev.manufacturer;
        mdl = dev.model_id;
        vnd = dev.vendor;
        break;
    }

    if (mfr[0] == '\0' && mdl[0] == '\0' && vnd[0] == '\0') {
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
            ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
            if (zb->device_ieee != ieee) continue;
            mfr = zb->zb_manufacturer;
            mdl = zb->zb_model;
            vnd = zb->zb_vendor;
            break;
        }
    }

    bool looks_ts0601 = (mdl[0] == '\0') || strcmp(mdl, "TS0601") == 0;
    bool known_giex_mfr = strstr(mfr, "sh1btabb") != NULL ||
                          strstr(mfr, "a7sghmms") != NULL ||
                          strstr(mfr, "7ytb3h8u") != NULL;
    bool known_giex_vendor = strstr(vnd, "GIEX") != NULL;
    return looks_ts0601 && (known_giex_mfr || known_giex_vendor);
}

static bool gw_is_giex_water_meter_sensor(ZigbeeSensor* sensor) {
    if (!sensor) return false;
    uint8_t unitid = getSensorUnitId(sensor);
    return sensor->cluster_id == ZB_ZCL_CLUSTER_ID_METERING ||
           unitid == UNIT_LITER || unitid == UNIT_GALLON ||
           unitid == UNIT_LITER_CONSUMPTION || unitid == UNIT_GALLON_CONSUMPTION;
}

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

static bool gw_cache_attribute_report(uint64_t ieee_addr, uint8_t endpoint,
                                      uint16_t cluster_id, uint16_t attr_id,
                                      int32_t value, uint8_t lqi) {
    for (size_t i = 0; i < pending_report_count; i++) {
        ZigbeeAttributeReport& r = pending_reports[i];
        if (r.ieee_addr == ieee_addr && r.cluster_id == cluster_id && r.attr_id == attr_id &&
            (r.endpoint == endpoint || r.endpoint == 0 || endpoint == 0)) {
            r.value = value;
            r.lqi = lqi;
            r.endpoint = endpoint;
            r.timestamp = millis();
            r.consumed = false;
            return true;
        }
    }

    if (pending_report_count >= MAX_PENDING_REPORTS) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Report cache FULL [%d/%d] - dropping report! cluster=0x%04X attr=0x%04X\n"),
                    (int)pending_report_count, (int)MAX_PENDING_REPORTS, cluster_id, attr_id);
        return false;
    }

    ZigbeeAttributeReport& report = pending_reports[pending_report_count++];
    report.ieee_addr = ieee_addr;
    report.endpoint = endpoint;
    report.cluster_id = cluster_id;
    report.attr_id = attr_id;
    report.value = value;
    report.lqi = lqi;
    report.timestamp = millis();
    report.consumed = false;
    return true;
}

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
    info.date_code[0] = '\0';
    info.sw_build_id[0] = '\0';
    info.app_version = 0xFF;
    info.stack_version = 0xFF;
    info.hw_version = 0xFF;
    
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
    bool has_string = gw_extractStringAttribute(attribute, str_buf, sizeof(str_buf));
    bool has_u8 = (attribute->data.type == ESP_ZB_ZCL_ATTR_TYPE_U8);
    uint8_t u8_value = has_u8 ? *(uint8_t*)attribute->data.value : 0xFF;
    
    // Find device in discovered list and update
    uint64_t ieee_addr = 0;
    for (auto& dev : gw_discovered_devices) {
        if (dev.short_addr == short_addr) {
            ieee_addr = dev.ieee_addr;
            if (attribute->id == ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID && has_u8) {
                dev.app_version = u8_value;
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID && has_u8) {
                dev.stack_version = u8_value;
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_HW_VERSION_ID && has_u8) {
                dev.hw_version = u8_value;
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID && has_string) {
                strncpy(dev.manufacturer, str_buf, sizeof(dev.manufacturer) - 1);
                dev.manufacturer[sizeof(dev.manufacturer) - 1] = '\0';
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID && has_string) {
                strncpy(dev.model_id, str_buf, sizeof(dev.model_id) - 1);
                dev.model_id[sizeof(dev.model_id) - 1] = '\0';
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_DATE_CODE_ID && has_string) {
                strncpy(dev.date_code, str_buf, sizeof(dev.date_code) - 1);
                dev.date_code[sizeof(dev.date_code) - 1] = '\0';
            } else if (attribute->id == ZB_ZCL_ATTR_BASIC_SW_BUILD_ID && has_string) {
                strncpy(dev.sw_build_id, str_buf, sizeof(dev.sw_build_id) - 1);
                dev.sw_build_id[sizeof(dev.sw_build_id) - 1] = '\0';
            }
            DEBUG_PRINTF(F("[ZIGBEE-GW] Basic attr 0x%04X for 0x%016llX: app=%u stack=%u hw=%u mfr=\"%s\" model=\"%s\" date=\"%s\" sw=\"%s\"\n"),
                         attribute->id,
                         (unsigned long long)dev.ieee_addr,
                         dev.app_version,
                         dev.stack_version,
                         dev.hw_version,
                         dev.manufacturer,
                         dev.model_id,
                         dev.date_code,
                         dev.sw_build_id);
            break;
        }
    }
    
    // Update matching sensor configurations
    if (ieee_addr != 0 && has_string) {
        const char* mfr = (attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) ? str_buf : nullptr;
        const char* mdl = (attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) ? str_buf : nullptr;
        if (mfr || mdl) ZigbeeSensor::updateBasicClusterInfo(ieee_addr, mfr, mdl);
    }
}

// ========== Zigbee station switch-failure verification ==========
// When a Tuya DP write is sent to a station, the expected state (ON/OFF)
// and a timestamp are recorded.  When the device echoes the DP back (which
// most Tuya valves do within a few seconds), the actual value is compared.
// A mismatch — or no echo within ZB_VERIFY_TIMEOUT_MS — triggers an MQTT
// alert on topic  station/{sid}/alert/switch.

#define ZB_VERIFY_MAX        8
#define ZB_VERIFY_TIMEOUT_MS 10000   // 10 s without echo → failure
#define ZB_VERIFY_RETRY_MS    3500   // one same-command retry before failing
#define ZB_VERIFY_QUERY_GRACE_MS 5000 // wait after an explicit DP query before failing

struct ZbStationVerify {
    uint64_t ieee;
    uint8_t  dp_id;
    bool     expected_on;
    uint32_t sent_ms;
    uint8_t  sid;
    uint8_t  endpoint;
    uint8_t  retries;
    bool     pending;
};

static ZbStationVerify DRAM_ATTR zb_verify_table[ZB_VERIFY_MAX];
static uint8_t DRAM_ATTR zb_station_switch_error[MAX_NUM_STATIONS];
static uint8_t DRAM_ATTR zb_station_switch_waiting[MAX_NUM_STATIONS];
static uint8_t DRAM_ATTR zb_station_physical_on[MAX_NUM_STATIONS];

static void gw_station_switch_error_set(uint8_t sid, bool error) {
    if (sid >= MAX_NUM_STATIONS) return;
    zb_station_switch_error[sid] = error ? 1 : 0;
    if (error) zb_station_switch_waiting[sid] = 0;
}

static void gw_station_switch_waiting_set(uint8_t sid, bool waiting) {
    if (sid >= MAX_NUM_STATIONS) return;
    zb_station_switch_waiting[sid] = waiting ? 1 : 0;
}

static void gw_station_physical_state_set(uint8_t sid, bool actual_on);

static void gw_station_verify_publish_fail(uint8_t sid, bool expected_on, bool timeout) {
    gw_station_switch_error_set(sid, true);
    DEBUG_PRINTF(F("[ZIGBEE-GW] Switch-fail alert: sid=%u expected_on=%d timeout=%d\n"),
                 (unsigned)sid, expected_on ? 1 : 0, timeout ? 1 : 0);
    if (!os.mqtt.enabled()) return;
    if (!os.mqtt.connected()) os.mqtt.reconnect();
    if (!os.mqtt.connected()) return;
    char topic[48];
    char payload[80];
    snprintf_P(topic,   sizeof(topic),   PSTR("station/%u/alert/switch"), (unsigned)sid);
    snprintf_P(payload, sizeof(payload), PSTR("{\"expected_on\":%d,\"timeout\":%d}"),
               expected_on ? 1 : 0, timeout ? 1 : 0);
    os.mqtt.publish(topic, payload);
}

// Called from gw_cache_tuya_dp_report when a Tuya DP is received.
static void gw_station_verify_process(uint64_t ieee, uint8_t dp_id, bool actual_on) {

    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (!zb_verify_table[i].pending) continue;
        if (zb_verify_table[i].ieee  != ieee)  continue;
        if (zb_verify_table[i].dp_id != dp_id) continue;
        bool ok = (zb_verify_table[i].expected_on == actual_on);
        zb_verify_table[i].pending = false;
        if (!ok) {
            gw_station_verify_publish_fail(zb_verify_table[i].sid,
                                           zb_verify_table[i].expected_on, false);
        } else {
            DEBUG_PRINTF(F("[ZIGBEE-GW] Station sid=%u switch confirmed: dp=%u on=%d\n"),
                         (unsigned)zb_verify_table[i].sid, (unsigned)dp_id, actual_on ? 1 : 0);
        }
        return;
    }
}

static void gw_station_verify_process_giex_off_fallback(uint64_t ieee, uint8_t dp_id, int32_t value) {
    if (ieee == 0 || !gw_is_giex_water_valve(ieee)) return;
    bool off_indicator = (dp_id == 111) || (dp_id == 1 && value == 0);
    if (!off_indicator) return;

    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (!zb_verify_table[i].pending) continue;
        if (zb_verify_table[i].ieee != ieee) continue;
        if (zb_verify_table[i].expected_on) continue;

        uint8_t sid = zb_verify_table[i].sid;
        zb_verify_table[i].pending = false;
        DEBUG_PRINTF(F("[ZIGBEE-GW] Station sid=%u switch confirmed by GX02 off fallback DP%u value=%ld\n"),
                     (unsigned)sid, (unsigned)dp_id, (long)value);
        gw_station_physical_state_set(sid, false);
        return;
    }
}

static void gw_station_physical_state_set(uint8_t sid, bool actual_on) {
    if (sid >= os.nstations) return;

    gw_station_switch_error_set(sid, false);
    gw_station_switch_waiting_set(sid, false);

    bool was_on = zb_station_physical_on[sid] != 0;
    if (was_on == actual_on) return;

    zb_station_physical_on[sid] = actual_on ? 1 : 0;

    DEBUG_PRINTF(F("[ZIGBEE-GW] Station sid=%u physical state from Tuya DP: on=%d\n"),
                 (unsigned)sid, actual_on ? 1 : 0);
}

static void gw_station_status_process_tuya_dp(uint64_t ieee, uint8_t endpoint, uint8_t dp_id, bool actual_on) {
    if (ieee == 0 || dp_id == 0 || os.nstations == 0) return;

    for (uint8_t sid = 0; sid < os.nstations; sid++) {
        if (os.get_station_type(sid) != STN_TYPE_ZIGBEE) continue;

        StationData station = {};
        os.get_station_data(sid, &station);
        ZigbeeStationData* data = (ZigbeeStationData*)station.sped;

        uint64_t station_ieee = gw_parse_ieee_hex(data->device_ieee);
        if (!gw_ieee_matches_station(station_ieee, ieee)) continue;

        uint8_t station_ep = gw_parse_hex_u8(data->endpoint, sizeof(data->endpoint));
        if (station_ep == 0) station_ep = 1;
        if (endpoint != 0 && station_ep != endpoint) continue;

        bool use_tuya = (data->use_tuya[0] == '1');
        uint8_t station_dp = gw_parse_hex_u8(data->tuya_dp, sizeof(data->tuya_dp));
        if (station_dp == 0) station_dp = 1;

        const char* mfr = "";
        const char* mdl = "";
        const char* vnd = "";
        bool dev_is_tuya = false;
        bool matched_device = false;

        for (const auto& dev : gw_discovered_devices) {
            if (dev.ieee_addr != ieee) continue;
            mfr = dev.manufacturer;
            mdl = dev.model_id;
            vnd = dev.vendor;
            dev_is_tuya = dev.is_tuya;
            matched_device = true;
            break;
        }

        if (!matched_device) {
            SensorIterator it = sensors_iterate_begin();
            SensorBase* sensor;
            while ((sensor = sensors_iterate_next(it)) != NULL) {
                if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;
                ZigbeeSensor* zb = static_cast<ZigbeeSensor*>(sensor);
                if (zb->device_ieee != ieee) continue;
                mfr = zb->zb_manufacturer;
                mdl = zb->zb_model;
                vnd = zb->zb_vendor;
                matched_device = true;
                break;
            }
        }

        bool is_ts0601 = (strcmp(mdl, "TS0601") == 0);
        bool gx02_by_mfr = (strstr(mfr, "sh1btabb") != NULL) || (strstr(mfr, "7ytb3h8u") != NULL);
        bool gx02_by_vendor = (strstr(vnd, "GIEX") != NULL);
        bool is_gx02 = is_ts0601 && (gx02_by_mfr || gx02_by_vendor);
        if (!use_tuya && (dev_is_tuya || is_gx02)) use_tuya = true;
        if (is_gx02) station_dp = 2;

        if (!use_tuya || station_dp != dp_id) continue;
        DEBUG_PRINTF(F("[ZIGBEE-GW] Station sid=%u matched Tuya DP report: dp=%u on=%d\n"),
                 (unsigned)sid, (unsigned)dp_id, actual_on ? 1 : 0);
        gw_station_physical_state_set(sid, actual_on);
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
                                  int32_t value, int lqi, uint8_t dp_type) {
    uint16_t flagged_attr = tuya_report_attr((uint8_t)mapped_attr, dp_type);

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

static void gw_cache_tuya_dp_report(uint64_t ieee_addr, uint8_t src_endpoint,
                                    uint8_t dp_number, int32_t value, int lqi, uint8_t dp_type) {
    gw_cache_tuya_report(ieee_addr, src_endpoint,
                         ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC,
                         (uint16_t)dp_number,
                         value, lqi, dp_type);
    // Keep the dashboard's Zigbee physical-state icon in sync when a valve is
    // operated manually on the device and reports its switch DP back.
    gw_station_status_process_tuya_dp(ieee_addr, src_endpoint, dp_number, value != 0);
    // Check if this DP echoes a pending station switch command.
    gw_station_verify_process(ieee_addr, dp_number, value != 0);
    gw_station_verify_process_giex_off_fallback(ieee_addr, dp_number, value);
}

// Tuya sequence counter for outgoing commands. Some Tuya MCU devices reject
// repeated/lower transaction numbers across gateway restarts, so keep it in NVS
// and store the next value before dispatching each command.
static uint16_t gw_tuya_seq = 0;
static bool gw_tuya_seq_loaded = false;

static void gw_save_tuya_seq(uint16_t next_seq) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("zb_gw", NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_set_u16(handle, "tuya_seq", next_seq);
    nvs_commit(handle);
    nvs_close(handle);
}

static void gw_load_tuya_seq() {
    if (gw_tuya_seq_loaded) return;
    gw_tuya_seq_loaded = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("zb_gw", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        uint16_t stored = 0;
        // Per Tuya MCU UART Protocol: SEQ cycles from 0 to 0xfff0 (65520)
        if (nvs_get_u16(handle, "tuya_seq", &stored) == ESP_OK && stored <= 0xfff0) {
            gw_tuya_seq = stored;
        } else {
            // Start at 0 per Tuya spec (0-0xfff0 cycling)
            gw_tuya_seq = 0;
            nvs_set_u16(handle, "tuya_seq", gw_tuya_seq);
            nvs_commit(handle);
        }
        nvs_close(handle);
    } else {
        gw_tuya_seq = 0;
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Sequence start=%u (0x%04X, max=0xfff0=%u)\n"), 
                 (unsigned)gw_tuya_seq, (unsigned)gw_tuya_seq, 0xfff0);
}

static void gw_zigbee_default_response_cb(zb_cmd_type_t resp_to_cmd, esp_zb_zcl_status_t status, uint8_t endpoint, uint16_t cluster) {
    if (cluster != ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) return;
    DEBUG_PRINTF(F("[ZIGBEE-GW][ZCL] Default response: cluster=0x%04X ep=%u resp_to_cmd=0x%02X status=0x%02X\n"),
                 cluster, endpoint, (unsigned)resp_to_cmd, (unsigned)status);
}

static uint16_t gw_next_tuya_seq() {
    gw_load_tuya_seq();
    // Per Tuya MCU UART Protocol: Sequence number cycles from 0 to 0xfff0 (65520)
    if (gw_tuya_seq > 0xfff0) gw_tuya_seq = 0;
    uint16_t seq = gw_tuya_seq;
    gw_tuya_seq = (uint16_t)(gw_tuya_seq + 1);
    // Wrap at 0xfff1 (65521) back to 0
    if (gw_tuya_seq > 0xfff0) gw_tuya_seq = 0;
    gw_save_tuya_seq(gw_tuya_seq);
    return seq;
}

static size_t gw_build_tuya_dp_payload(uint8_t* payload, size_t payload_size,
                                       uint16_t seq, uint8_t dp_id, uint8_t dp_type,
                                       const uint8_t* value, uint16_t value_len) {
    const size_t required = 6U + value_len;
    if (!payload || !value || payload_size < required) return 0;

    payload[0] = (uint8_t)(seq >> 8);
    payload[1] = (uint8_t)(seq & 0xFF);
    payload[2] = dp_id;
    payload[3] = dp_type;
    payload[4] = (uint8_t)(value_len >> 8);
    payload[5] = (uint8_t)(value_len & 0xFF);
    memcpy(payload + 6, value, value_len);
    return required;
}

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
 * @brief Send a Tuya DP Query (cmd 0x03) to a device to request all datapoints.
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
    req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    req.dis_default_resp = 1;
    req.custom_cmd_id = TUYA_CMD_DATA_QUERY;
    req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    req.data.size = 0;
    req.data.value = nullptr;

    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&req);
    esp_zb_lock_release();

    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP query sent -> short_addr=0x%04X ep=%d cmd=0x%02X dir=TO_SRV tsn=%u\n"),
                 short_addr, dst_ep, TUYA_CMD_DATA_QUERY, (unsigned)tsn);
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

    DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] cmd=0x%02X zcl_seq=%u len=%u from 0x%04X ep=%u fc=0x%02X\n"),
                 command_id, seq_number, ind.asdu_length, ind.src_short_addr, ind.src_endpoint, ind.asdu[0]);

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

    // Some Tuya firmwares send MCU version response (0x11) spontaneously
    // during interview/keepalive. Treat it as a known non-DP control frame.
    if (command_id == TUYA_CMD_MCU_VERSION_RESP) {
        esp_zb_ieee_addr_t raw_ieee;
        if (esp_zb_ieee_address_by_short(ind.src_short_addr, raw_ieee) == ESP_OK) {
            uint64_t ieee = 0;
            for (int i = 7; i >= 0; i--) ieee = (ieee << 8) | raw_ieee[i];
            if (ieee) gw_add_responsive_device(ind.src_short_addr, ieee, ind.src_endpoint);
        }
        return true;
    }

    // Handle Tuya data query seen from a device defensively: answer with a
    // gateway query, matching the legacy behavior this firmware used before.
    if (command_id == TUYA_CMD_DATA_QUERY) {
        // DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP query from 0x%04X - sending query now\n"), ind.src_short_addr);
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

    // Process all known DP-bearing response/report variants.
    if (command_id != TUYA_CMD_DATA_RESPONSE &&
        command_id != TUYA_CMD_DATA_REPORT &&
        command_id != TUYA_CMD_DATA_SEND &&
        command_id != TUYA_CMD_ACTIVE_STATUS) {
        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] Unhandled command 0x%02X from 0x%04X\n"),
                command_id, ind.src_short_addr);
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
    int dps_parsed = 0;
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
        if (dp_type == TUYA_TYPE_VALUE && dp_len == 4) {
            dp_value = (int32_t)(((uint32_t)ind.asdu[offset] << 24) |
                                 ((uint32_t)ind.asdu[offset + 1] << 16) |
                                 ((uint32_t)ind.asdu[offset + 2] << 8) |
                                 ((uint32_t)ind.asdu[offset + 3]));
        } else if (dp_type == TUYA_TYPE_ENUM || dp_type == TUYA_TYPE_BOOL) {
            dp_value = (int32_t)ind.asdu[offset];
        } else if (dp_len <= 4) {
            // Generic big-endian extraction for short payloads
            for (uint16_t j = 0; j < dp_len; j++) {
                dp_value = (dp_value << 8) | ind.asdu[offset + j];
            }
        }

        DEBUG_PRINTF(F("[ZIGBEE-GW][TUYA] DP %u: type=%u len=%u value=%ld\n"),
                dp_number, dp_type, dp_len, dp_value);

        // Always cache raw Tuya DP reports (cluster 0xEF00, attr=DP id)
        // so per-sensor custom DP mappings can consume them.
        gw_cache_tuya_dp_report(ieee_addr, ind.src_endpoint, dp_number, dp_value, ind.lqi, dp_type);

        offset += dp_len;
        dps_parsed++;
    }

    // A bare Tuya ACK without DP records only confirms protocol receipt. It is
    // not proof that a valve actually opened or closed, so switch verification
    // remains pending until the expected DP report arrives or times out.

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

static bool gw_cluster_supports_config_reporting(uint16_t cluster_id, uint16_t attr_id) {
    // Simple Metering CurrentSummationDelivered is uint48. The current report
    // helper is sized for the common measurement attributes, so water meters
    // use active reads or their own unsolicited reports instead.
    if (cluster_id == ZB_ZCL_CLUSTER_ID_METERING && attr_id == 0x0000) return false;
    return true;
}

// Send a ZCL Configure Reporting command for one attribute.
// Tells the remote device to push reports every [min_interval..max_interval] seconds.
bool sensor_zigbee_gw_configure_reporting(uint64_t device_ieee, uint8_t endpoint,
                                           uint16_t cluster_id, uint16_t attr_id,
                                           uint16_t min_interval, uint16_t max_interval) {
    if (!gw_cluster_supports_config_reporting(cluster_id, attr_id)) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] ConfigReport skipped: unsupported attr width c=0x%04X a=0x%04X\n"),
                     cluster_id, attr_id);
        return false;
    }
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

            esp_zb_attribute_list_t *meter_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_METERING);
            if (meter_cluster) {
                esp_zb_cluster_list_add_custom_cluster(_cluster_list, meter_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
                // DEBUG_PRINTLN(F("[ZIGBEE-GW] Metering cluster 0x0702 added (CLIENT)"));
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

        // Handle Basic Cluster responses (version, manufacturer, model strings)
        // so that newly-discovered devices get a real label instead of "unknown".
        // Must run BEFORE the generic sensor cache path which only handles
        // numeric attribute reports.
        if (cluster_id == ZB_ZCL_CLUSTER_ID_BASIC &&
            (attribute->id == ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_HW_VERSION_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_DATE_CODE_ID ||
             attribute->id == ZB_ZCL_ATTR_BASIC_SW_BUILD_ID)) {
            // Ensure the device is present in the discovered list before we try
            // to fill its manufacturer/model fields.
            uint64_t basic_ieee = 0;
            esp_zb_ieee_addr_t basic_raw;
            if (esp_zb_ieee_address_by_short(src_address.u.short_addr, basic_raw) == ESP_OK) {
                for (int i = 7; i >= 0; i--) basic_ieee = (basic_ieee << 8) | basic_raw[i];
                if (basic_ieee) gw_add_responsive_device(src_address.u.short_addr, basic_ieee, src_endpoint);
            }
            gw_handleBasicClusterResponse(src_address.u.short_addr, attribute);
            return; // Don't treat the string as a sensor value
        }

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
        
        if (gw_cache_attribute_report(ieee_addr, src_endpoint, cluster_id, attribute->id, value, 0)) {
            DEBUG_PRINTF(F("[ZIGBEE-GW] ✓ Report CACHED [%d/%d]: IEEE=%08lX%08lX cluster=0x%04X attr=0x%04X value=%ld ep=%d\n"),
                        (int)pending_report_count, (int)MAX_PENDING_REPORTS,
                        (unsigned long)(ieee_addr >> 32), (unsigned long)(ieee_addr & 0xFFFFFFFF),
                        cluster_id, attribute->id, value, src_endpoint);
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
                // DEBUG_PRINTF(F("[ZIGBEE-GW] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// ========== Helper: update sensor from cached report ==========

static void gw_updateSensorFromReport(ZigbeeSensor* zb_sensor, const ZigbeeAttributeReport& report, bool solicited) {
    if (!zb_sensor) return;

    // ── Battery report short-circuit ─────────────────────────────────────
    // Battery reports (cluster 0x0001/attr 0x0021 or Tuya DP 15) carry a
    // percentage that must ONLY update `last_battery` — never the sensor's
    // `last_data`, otherwise the battery percentage (50/100/…) is logged as
    // the soil-moisture / temperature / etc. value.  This guard is
    // defense-in-depth: even when the dispatch loop or auto-correct routes
    // a battery report to a non-battery sensor by mistake, the measurement
    // payload is preserved.
    {
        bool is_tuya_report = (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0;
        uint16_t raw_attr = zigbee_report_attr_id(report.attr_id);
        bool is_battery_report =
            (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && raw_attr == 0x0021) ||
            (is_tuya_report && report.cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC &&
             (zb_sensor->tuya_dp_battery >= 0 && raw_attr == (uint16_t)zb_sensor->tuya_dp_battery));
        if (is_battery_report) {
            uint32_t batt_pct = zigbee_battery_percent_from_report(is_tuya_report, raw_attr, tuya_report_type(report.attr_id), zb_sensor->tuya_dp_battery, report.value);
            zb_sensor->last_battery = batt_pct;
            zb_sensor->last_lqi     = report.lqi;
            // Intentionally do NOT touch last_data / last_native_data /
            // flags.data_ok / last_read / last / comm_mode — those belong
            // to the actual measurement channel, not the battery channel.
            DEBUG_PRINTF(F("[ZIGBEE-GW] Sensor '%s' battery update: %u%% (no data overwrite)\n"),
                         zb_sensor->name, (unsigned)batt_pct);
            return;
        }
    }

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
        } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) {
            uint16_t dp = zigbee_report_attr_id(report.attr_id);
            if (zb_sensor->tuya_dp_value >= 0 && dp == (uint16_t)zb_sensor->tuya_dp_value) {
                converted_value = report.value;
            }
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
    }
    // NOTE: POWER_CONFIG/0x0021 (battery) is handled by the early-return
    // guard at the top of this function — it must never reach the
    // measurement-update path below.

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
    // Battery reports already returned above, so every report reaching here
    // belongs to the measurement channel.
    {
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
    read_req.attr_number = sizeof(s_gw_basic_query_attrs) / sizeof(s_gw_basic_query_attrs[0]);
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

bool sensor_zigbee_gw_query_basic_cluster_by_ieee(uint64_t device_ieee, uint8_t endpoint) {
    if (!gw_zigbee_initialized || !gw_reportReceiver) return false;
    if (!Zigbee.started() || !Zigbee.connected()) return false;
    if (device_ieee == 0) return false;

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }

    esp_zb_zcl_read_attr_cmd_t read_req;
    memset(&read_req, 0, sizeof(read_req));

    esp_zb_lock_acquire(portMAX_DELAY);
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    esp_zb_lock_release();
    if (short_addr != 0xFFFF && short_addr != 0xFFFE) {
        read_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        read_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
        gw_add_responsive_device(short_addr, device_ieee, endpoint);
    } else {
        read_req.address_mode = ESP_ZB_APS_ADDR_MODE_64_ENDP_PRESENT;
        memcpy(read_req.zcl_basic_cmd.dst_addr_u.addr_long, ieee_le, sizeof(ieee_le));
    }

    read_req.zcl_basic_cmd.dst_endpoint = endpoint;
    read_req.zcl_basic_cmd.src_endpoint = 10;
    read_req.clusterID = ZB_ZCL_CLUSTER_ID_BASIC;
    read_req.attr_number = sizeof(s_gw_basic_query_attrs) / sizeof(s_gw_basic_query_attrs[0]);
    read_req.attr_field = s_gw_basic_query_attrs;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_read_attr_cmd_req(&read_req);
    gw_read_pending = true;
    gw_read_time = millis();
    gw_read_pending_ieee = device_ieee;
    gw_read_pending_cluster = ZB_ZCL_CLUSTER_ID_BASIC;
    esp_zb_lock_release();

    DEBUG_PRINTF(F("[ZIGBEE-GW] Basic query sent: ieee=%016llX short=0x%04X ep=%d\n"),
                 (unsigned long long)device_ieee, short_addr, endpoint);
    return true;
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
        if (!gw_cluster_supports_config_reporting(zb->cluster_id, zb->attribute_id)) continue;

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
    Zigbee.onGlobalDefaultResponse(gw_zigbee_default_response_cb);
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

    if (cluster_id != 0 || attr_id != 0) {
        gw_cache_attribute_report(ieee_addr, endpoint, cluster_id, attr_id, value, lqi);
    }
    
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
            
            uint16_t report_attr_unmasked = zigbee_report_attr_id(report.attr_id);
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

            // Optional Tuya DP override per sensor: allow matching raw EF00/DP
            // reports directly, independent of the configured cluster/attribute.
            bool is_tuya_dp_report = (report.cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC) &&
                                     ((report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0);
            uint16_t report_dp = zigbee_report_attr_id(report.attr_id);
            if (!matches && is_tuya_dp_report && zb_sensor->tuya_dp_value >= 0 &&
                ieee_match && ep_match && report_dp == (uint16_t)zb_sensor->tuya_dp_value) {
                matches = true;
            }

            if (!matches && is_tuya_dp_report && ieee_match && ep_match &&
                report_dp == 111 && gw_is_giex_water_valve(report.ieee_addr) &&
                gw_is_giex_water_meter_sensor(zb_sensor)) {
                if (zb_sensor->tuya_dp_value != 111) {
                    zb_sensor->tuya_dp_value = 111;
                    if (zb_sensor->tuya_dp_battery < 0) zb_sensor->tuya_dp_battery = 108;
                    sensor_save();
                    DEBUG_PRINTF(F("[ZIGBEE-GW] Auto-mapped GIEX/GX02 water meter '%s': value DP=111 battery DP=%d\n"),
                                 zb_sensor->name, (int)zb_sensor->tuya_dp_battery);
                }
                matches = true;
            }
            
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
            bool suppress_nomatch = false;
            if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC &&
                ((report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0)) {
                suppress_nomatch = gw_is_ignorable_unmatched_tuya_dp(zigbee_report_attr_id(report.attr_id));
            }

            if (!suppress_nomatch) {
                DEBUG_PRINTF(F("[ZIGBEE-GW] ✗ NO MATCH (checked %d ZigBee sensor%s)\n"), checked_count, checked_count == 1 ? "" : "s");
                DEBUG_PRINTF(F("[ZIGBEE-GW]   Report detail: ieee=0x%016llX ep=%u cluster=0x%04X attr=0x%04X value=%ld%s solicited=%d\n"),
                             (unsigned long long)report.ieee_addr,
                             report.endpoint,
                             report.cluster_id,
                             zigbee_report_attr_id(report.attr_id),
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
        }
        
        bool handled_custom_tuya_batt = false;
        bool handled_custom_tuya_unit = false;
        if (!found) {

            // Custom Tuya battery DP mapping per sensor
            if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TUYA_SPECIFIC && report.ieee_addr != 0 &&
                ((report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0)) {
                uint16_t report_dp = zigbee_report_attr_id(report.attr_id);
                SensorIterator batt_it = sensors_iterate_begin();
                SensorBase* batt_sensor;
                while ((batt_sensor = sensors_iterate_next(batt_it)) != NULL) {
                    if (!batt_sensor || batt_sensor->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb_batt = static_cast<ZigbeeSensor*>(batt_sensor);
                    if (zb_batt->device_ieee != report.ieee_addr) continue;
                    bool explicit_battery_dp = (zb_batt->tuya_dp_battery >= 0 && (uint16_t)zb_batt->tuya_dp_battery == report_dp);
                    bool giex_battery_dp = (report_dp == 108 && gw_is_giex_water_valve(report.ieee_addr));
                    if (!explicit_battery_dp && !giex_battery_dp) continue;

                    if (giex_battery_dp && zb_batt->tuya_dp_battery < 0) {
                        zb_batt->tuya_dp_battery = 108;
                        sensor_save();
                        DEBUG_PRINTF(F("[ZIGBEE-GW] Auto-mapped GIEX/GX02 battery '%s': battery DP=108\n"),
                                     zb_batt->name);
                    }

                    uint32_t battery_pct = zigbee_battery_percent_from_report(true, report_dp, tuya_report_type(report.attr_id), zb_batt->tuya_dp_battery, report.value);
                    zb_batt->last_battery = battery_pct;
                    zb_batt->last_lqi = report.lqi;
                    handled_custom_tuya_batt = true;
                }
                if (handled_custom_tuya_batt) {
                    DEBUG_PRINTF(F("[ZIGBEE-GW] Custom Tuya battery DP %u applied: ieee=%08lX%08lX value=%ld\n"),
                                (unsigned)report_dp,
                                (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                                report.value);
                }

                SensorIterator unit_it = sensors_iterate_begin();
                SensorBase* unit_sensor;
                while ((unit_sensor = sensors_iterate_next(unit_it)) != NULL) {
                    if (!unit_sensor || unit_sensor->type != SENSOR_ZIGBEE) continue;
                    ZigbeeSensor* zb_unit = static_cast<ZigbeeSensor*>(unit_sensor);
                    if (zb_unit->device_ieee != report.ieee_addr) continue;
                    if (zb_unit->tuya_dp_unit < 0 || (uint16_t)zb_unit->tuya_dp_unit != report_dp) continue;

                    zb_unit->tuya_unit = (report.value < 0) ? 0xFF : (uint8_t)report.value;
                    zb_unit->last_lqi = report.lqi;
                    handled_custom_tuya_unit = true;
                }
                if (handled_custom_tuya_unit) {
                    DEBUG_PRINTF(F("[ZIGBEE-GW] Custom Tuya unit DP %u applied: ieee=%08lX%08lX unit=%ld\n"),
                                (unsigned)report_dp,
                                (unsigned long)(report.ieee_addr >> 32), (unsigned long)(report.ieee_addr & 0xFFFFFFFF),
                                report.value);
                }
            }
            
            // Auto-correct misconfigured sensors: if a known standard ZCL cluster report comes in
            // for a sensor that has the correct IEEE address but wrong cluster/attribute configured,
            // correct the stored cluster_id/attribute_id and re-save.
            // This handles cases where sensors were saved with wrong values (e.g. 0x0401 instead of 0x0405).
            uint16_t report_attr_raw = zigbee_report_attr_id(report.attr_id);
            bool is_known_standard_cluster = (
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT         && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE            && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT     && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT  && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT         && report_attr_raw == 0x0000) ||
                (report.cluster_id == ZB_ZCL_CLUSTER_ID_METERING                 && report_attr_raw == 0x0000)
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

            // If custom Tuya battery/unit handlers consumed this report, skip
            // generic battery/unmatched handling.
            if (handled_custom_tuya_batt || handled_custom_tuya_unit) {
                // handled
            }
            // If this is a battery report (cluster 0x0001, attr 0x0021), update
            // battery on any sensor matching the IEEE address
            else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
                zigbee_report_attr_id(report.attr_id) == 0x0021 && report.ieee_addr != 0) {
                bool is_tuya_battery = (report.attr_id & TUYA_REPORT_FLAG_PRESCALED) != 0;
                uint32_t battery_pct = zigbee_battery_percent_from_report(is_tuya_battery, zigbee_report_attr_id(report.attr_id), tuya_report_type(report.attr_id), -1, report.value);
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
                    zigbee_report_attr_id(report.attr_id) != last_unmatched_attr ||
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
                    last_unmatched_attr = zigbee_report_attr_id(report.attr_id);
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

    if (dur == 0) {
        gw_join_window_end = 0;
        esp_zb_lock_acquire(portMAX_DELAY);
        Zigbee.openNetwork(0);
        esp_zb_lock_release();
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Join window CLOSED by request"));
        return;
    }
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

uint16_t sensor_zigbee_gw_get_join_window_remaining() {
    if (gw_join_window_end == 0) return 0;
    unsigned long now = millis();
    if ((long)(gw_join_window_end - now) <= 0) return 0;
    unsigned long remaining_ms = gw_join_window_end - now;
    return (uint16_t)((remaining_ms + 999UL) / 1000UL);
}

void sensor_zigbee_gw_loop() {
    if (!gw_zigbee_initialized) return;

    // Check for timed-out station switch verifications.
    sensor_zigbee_station_verify_tick();

    // Continue serialized Tuya writes from the normal gateway loop. Scheduling
    // the next alarm from inside the Zigbee scheduler callback can be dropped
    // by ZBOSS on ESP32-C5, so the callback only dispatches one command.
    gw_tuya_schedule_next();

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

    // Retry one deferred station command if it was queued while Zigbee was not
    // connected (or short address was not yet known during startup/rejoin).
    if (connected && gw_pending_switch.valid && millis() >= gw_pending_switch.next_try_ms) {
        bool ok = false;
        if (gw_pending_switch.use_tuya) {
            ok = sensor_zigbee_send_tuya_dp_write(gw_pending_switch.ieee_addr,
                                                  gw_pending_switch.endpoint,
                                                  gw_pending_switch.dp_id,
                                                  gw_pending_switch.turnon);
        } else {
            ok = sensor_zigbee_send_on_off(gw_pending_switch.ieee_addr,
                                           gw_pending_switch.endpoint,
                                           gw_pending_switch.turnon);
        }
        if (ok) {
            DEBUG_PRINTF(F("[ZIGBEE-GW] Deferred command delivered: ieee=%016llX ep=%d state=%d\n"),
                         (unsigned long long)gw_pending_switch.ieee_addr,
                         gw_pending_switch.endpoint,
                         gw_pending_switch.turnon ? 1 : 0);
            gw_pending_switch.valid = false;
            gw_pending_switch.attempts = 0;
        } else {
            if (gw_pending_switch.attempts >= 30) {
                DEBUG_PRINTF(F("[ZIGBEE-GW] Deferred command dropped after %d attempts: ieee=%016llX\n"),
                             gw_pending_switch.attempts,
                             (unsigned long long)gw_pending_switch.ieee_addr);
                gw_pending_switch.valid = false;
                gw_pending_switch.attempts = 0;
            } else {
                gw_pending_switch.next_try_ms = millis() + 1000UL;
            }
        }
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

bool sensor_zigbee_send_on_off(uint64_t device_ieee, uint8_t endpoint, bool turnon) {
    if (!gw_zigbee_initialized || !Zigbee.started() || !Zigbee.connected()) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Standard On/Off command failed: Zigbee not initialized or not connected"));
        gw_queue_pending_switch(false, device_ieee, endpoint, 0, turnon);
        return false;
    }
    if (device_ieee == 0) return false;

    // Correct stale/truncated station IEEE against discovered devices before
    // attempting short-address lookup.
    gw_try_fix_ieee(&device_ieee);

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
    esp_zb_lock_release();

    if (short_addr == 0xFFFF || short_addr == 0xFFFE) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] Standard On/Off command failed: short addr unknown for ieee=%016llX\n"),
                    (unsigned long long)device_ieee);
        gw_queue_pending_switch(false, device_ieee, endpoint, 0, turnon);
        return false;
    }

    esp_zb_zcl_on_off_cmd_t req = {};
    req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    req.zcl_basic_cmd.dst_endpoint = endpoint;
    req.zcl_basic_cmd.src_endpoint = 10;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.on_off_cmd_id = turnon ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

    DEBUG_PRINTF(F("[ZIGBEE-GW] Sending On/Off command -> short_addr=0x%04X ep=%d state=%d\n"),
                 short_addr, endpoint, turnon);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&req);
    esp_zb_lock_release();

    return true;
}

static bool gw_resolve_tuya_short_addr(uint64_t* device_ieee, uint16_t* short_addr, const char* context,
                                       bool queue_on_unavailable, uint8_t endpoint, uint8_t dp_id, bool turnon) {
    if (!gw_zigbee_initialized || !Zigbee.started() || !Zigbee.connected()) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] %s failed: Zigbee not initialized or not connected\n"), context);
        if (queue_on_unavailable && device_ieee) gw_queue_pending_switch(true, *device_ieee, endpoint, dp_id, turnon);
        return false;
    }
    if (!device_ieee || *device_ieee == 0 || !short_addr) return false;

    // Correct stale/truncated station IEEE against discovered devices before
    // attempting short-address lookup.
    gw_try_fix_ieee(device_ieee);

    esp_zb_ieee_addr_t ieee_le = {0};
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(*device_ieee >> (i * 8));
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    *short_addr = esp_zb_address_short_by_ieee(ieee_le);
    esp_zb_lock_release();

    if (*short_addr == 0xFFFF || *short_addr == 0xFFFE) {
        DEBUG_PRINTF(F("[ZIGBEE-GW] %s failed: short addr unknown for ieee=%016llX\n"),
                    context, (unsigned long long)*device_ieee);
        if (queue_on_unavailable) gw_queue_pending_switch(true, *device_ieee, endpoint, dp_id, turnon);
        return false;
    }

    return true;
}

static bool gw_send_tuya_dp_raw_cmd(uint64_t device_ieee, uint8_t endpoint, uint8_t dp_id, uint8_t dp_type,
                                    const uint8_t* value, uint16_t value_len, uint8_t command_id,
                                    bool queue_on_unavailable, bool queued_turnon) {
    uint16_t short_addr = 0;
    if (!gw_resolve_tuya_short_addr(&device_ieee, &short_addr, "Tuya DP write",
                                    queue_on_unavailable, endpoint, dp_id, queued_turnon)) {
        return false;
    }

    // Prepare a generic Tuya dataRequest payload. This is the same logical
    // shape used by Zigbee2MQTT's sendDataPoint* helpers and works for valves,
    // switches, lights, and other Tuya MCU devices once their DP map is known.
    uint8_t payload[20];
    uint16_t tuya_seq = gw_next_tuya_seq();
    size_t payload_len = gw_build_tuya_dp_payload(payload, sizeof(payload),
                                                  tuya_seq, dp_id, dp_type,
                                                  value, value_len);
    if (payload_len == 0) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW] Tuya DP write failed: payload build error"));
        return false;
    }

    DEBUG_PRINTF(F("[ZIGBEE-GW] Queueing Tuya DP set -> short_addr=0x%04X ep=%d cmd=0x%02X dp=%d type=%d len=%u seq=%u dir=TO_SRV defresp=off\n"),
                 short_addr, endpoint, command_id, dp_id, dp_type, (unsigned)value_len, (unsigned)tuya_seq);
    DEBUG_PRINTF(F("[ZIGBEE-GW] Tuya payload (%u bytes): %02X %02X %02X %02X %02X %02X %02X%s\n"),
                 (unsigned)payload_len,
                 payload[0], payload[1], payload[2], payload[3],
                 payload[4], payload[5], payload[6], payload_len > 7 ? " ..." : "");

    return gw_schedule_tuya_dp_cmd(short_addr, endpoint, command_id, dp_id, dp_type,
                                   payload, payload_len, tuya_seq);
}

static bool gw_send_tuya_dp_bool_write_cmd(uint64_t device_ieee, uint8_t endpoint, uint8_t dp_id, bool turnon,
                                           uint8_t command_id, bool queue_on_unavailable) {
    uint8_t bool_value[1] = { (uint8_t)(turnon ? 0x01 : 0x00) };
    return gw_send_tuya_dp_raw_cmd(device_ieee, endpoint, dp_id, TUYA_TYPE_BOOL,
                                   bool_value, sizeof(bool_value), command_id,
                                   queue_on_unavailable, turnon);
}

static bool gw_send_tuya_dp_value_write_cmd(uint64_t device_ieee, uint8_t endpoint, uint8_t dp_id, uint32_t value,
                                            uint8_t command_id, bool queue_on_unavailable) {
    uint8_t value_bytes[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF)
    };
    return gw_send_tuya_dp_raw_cmd(device_ieee, endpoint, dp_id, TUYA_TYPE_VALUE,
                                   value_bytes, sizeof(value_bytes), command_id,
                                   queue_on_unavailable, value != 0);
}

static bool gw_send_giex_water_valve_state_cmd(uint64_t device_ieee, uint8_t endpoint, bool turnon,
                                               bool queue_on_unavailable) {
    if (!turnon) {
        DEBUG_PRINTLN(F("[ZIGBEE-GW][GX02] Sending OFF via DP2 using standard Report DP (cmd 0x06)"));
        return gw_send_tuya_dp_bool_write_cmd(device_ieee, endpoint, 2, false,
                                              TUYA_CMD_REPORT_DP_DATA, queue_on_unavailable);
    }

    DEBUG_PRINTLN(F("[ZIGBEE-GW][GX02] Preparing ON: mode=duration (DP1=0), target=0 (DP104), then state=ON (DP2) using standard Report DP (cmd 0x06)"));
    gw_send_tuya_dp_bool_write_cmd(device_ieee, endpoint, 1, false,
                                   TUYA_CMD_REPORT_DP_DATA, false);
    gw_send_tuya_dp_value_write_cmd(device_ieee, endpoint, 104, 0,
                                    TUYA_CMD_REPORT_DP_DATA, false);
    return gw_send_tuya_dp_bool_write_cmd(device_ieee, endpoint, 2, true,
                                          TUYA_CMD_REPORT_DP_DATA, queue_on_unavailable);
}

bool sensor_zigbee_send_tuya_dp_write(uint64_t device_ieee, uint8_t endpoint, uint8_t dp_id, bool turnon) {
    if (dp_id == 2 && gw_is_giex_water_valve(device_ieee)) {
        return gw_send_giex_water_valve_state_cmd(device_ieee, endpoint, turnon, true);
    }
    return gw_send_tuya_dp_bool_write_cmd(device_ieee, endpoint, dp_id, turnon, TUYA_CMD_DATA_REQUEST, true);
}

bool sensor_zigbee_send_giex_water_valve_state(uint64_t device_ieee, uint8_t endpoint, bool turnon) {
    return gw_send_giex_water_valve_state_cmd(device_ieee, endpoint, turnon, true);
}

void sensor_zigbee_station_verify_register(uint8_t sid, uint64_t ieee, uint8_t endpoint, uint8_t dp_id, bool expected_on) {
    gw_station_switch_error_set(sid, false);
    gw_station_switch_waiting_set(sid, true);

    // Update existing entry for same ieee + dp_id.
    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (zb_verify_table[i].pending &&
            zb_verify_table[i].ieee  == ieee &&
            zb_verify_table[i].dp_id == dp_id) {
            zb_verify_table[i].expected_on = expected_on;
            zb_verify_table[i].sent_ms     = millis();
            zb_verify_table[i].sid         = sid;
            zb_verify_table[i].endpoint    = endpoint;
            zb_verify_table[i].retries     = 0;
            return;
        }
    }
    // Find a free slot.
    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (!zb_verify_table[i].pending) {
            zb_verify_table[i] = {ieee, dp_id, expected_on, millis(), sid, endpoint, 0, true};
            return;
        }
    }
    // All slots full — silently skip (worst case: no alert for this switch).
}

uint8_t sensor_zigbee_station_status_code(uint8_t sid) {
    if (sid >= MAX_NUM_STATIONS) return 0;
    if (zb_station_switch_error[sid]) return 2;
    if (zb_station_switch_waiting[sid]) return zb_station_physical_on[sid] ? 4 : 1;

    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (zb_verify_table[i].pending && zb_verify_table[i].sid == sid) {
            return zb_station_physical_on[sid] ? 4 : 1;
        }
    }
    return zb_station_physical_on[sid] ? 3 : 0;
}

void sensor_zigbee_station_clear_error(uint8_t sid) {
    gw_station_switch_error_set(sid, false);
    gw_station_switch_waiting_set(sid, false);
}

void sensor_zigbee_station_verify_tick() {
    uint32_t now = millis();
    for (int i = 0; i < ZB_VERIFY_MAX; i++) {
        if (!zb_verify_table[i].pending) continue;
        if (zb_verify_table[i].retries == 0 && now - zb_verify_table[i].sent_ms >= ZB_VERIFY_RETRY_MS) {
            bool is_state_dp = (zb_verify_table[i].dp_id == 2);
            // Use standard Report DP (0x06) for GX02, fallback to DATA_QUERY (0x03) for others
            uint8_t retry_cmd = (is_state_dp && gw_is_giex_water_valve(zb_verify_table[i].ieee))
                                  ? TUYA_CMD_REPORT_DP_DATA
                                  : TUYA_CMD_QUERY_REQ;
            DEBUG_PRINTF(F("[ZIGBEE-GW] Switch confirm pending: sid=%u dp=%u expected_on=%d, retrying with cmd=0x%02X\n"),
                         (unsigned)zb_verify_table[i].sid,
                         (unsigned)zb_verify_table[i].dp_id,
                         zb_verify_table[i].expected_on ? 1 : 0,
                         (unsigned)retry_cmd);
            bool retry_sent = false;
            if (is_state_dp && gw_is_giex_water_valve(zb_verify_table[i].ieee)) {
                retry_sent = gw_send_tuya_dp_bool_write_cmd(zb_verify_table[i].ieee,
                                                            zb_verify_table[i].endpoint ? zb_verify_table[i].endpoint : 1,
                                                            2,
                                                            zb_verify_table[i].expected_on,
                                                            retry_cmd,
                                                            false);
                if (!retry_sent) {
                    // Try legacy DATA_SEND (0x04) as compatibility fallback.
                    uint8_t alt_cmd = TUYA_CMD_DATA_SEND;
                    bool retry_sent_alt = gw_send_tuya_dp_bool_write_cmd(zb_verify_table[i].ieee,
                                                                          zb_verify_table[i].endpoint ? zb_verify_table[i].endpoint : 1,
                                                                          2,
                                                                          zb_verify_table[i].expected_on,
                                                                          alt_cmd,
                                                                          false);
                    retry_sent = retry_sent || retry_sent_alt;
                }
            } else {
                retry_sent = gw_send_tuya_dp_bool_write_cmd(zb_verify_table[i].ieee,
                                                            zb_verify_table[i].endpoint ? zb_verify_table[i].endpoint : 1,
                                                            zb_verify_table[i].dp_id,
                                                            zb_verify_table[i].expected_on,
                                                            retry_cmd,
                                                            false);
            }
            if (retry_sent) {
                zb_verify_table[i].retries = 1;
                zb_verify_table[i].sent_ms = now;
            }
            continue;
        }
        if (zb_verify_table[i].retries == 1 && now - zb_verify_table[i].sent_ms >= ZB_VERIFY_TIMEOUT_MS) {
            DEBUG_PRINTF(F("[ZIGBEE-GW] Switch confirm still pending: sid=%u dp=%u expected_on=%d, querying Tuya DPs before fail\n"),
                         (unsigned)zb_verify_table[i].sid,
                         (unsigned)zb_verify_table[i].dp_id,
                         zb_verify_table[i].expected_on ? 1 : 0);
            if (sensor_zigbee_gw_request_dp_query(zb_verify_table[i].ieee,
                                                  zb_verify_table[i].endpoint ? zb_verify_table[i].endpoint : 1)) {
                zb_verify_table[i].retries = 2;
                zb_verify_table[i].sent_ms = now;
                continue;
            }
        }
        if (zb_verify_table[i].retries >= 2 && now - zb_verify_table[i].sent_ms >= ZB_VERIFY_QUERY_GRACE_MS) {
            DEBUG_PRINTF(F("[ZIGBEE-GW] Switch confirm still pending after DP query grace: sid=%u dp=%u expected_on=%d, marking failed\n"),
                         (unsigned)zb_verify_table[i].sid,
                         (unsigned)zb_verify_table[i].dp_id,
                         zb_verify_table[i].expected_on ? 1 : 0);
            gw_station_verify_publish_fail(zb_verify_table[i].sid,
                                           zb_verify_table[i].expected_on, true);
            zb_verify_table[i].pending = false;
        }
    }
}

void sensor_zigbee_gw_force_off_all_stations() {
    if (os.nstations == 0) return;

    for (uint8_t sid = 0; sid < os.nstations; sid++) {
        uint8_t bid = sid >> 3;
        uint8_t s = sid & 0x07;
        if (!(os.attrib_spe[bid] & (1 << s))) continue;
        if (os.get_station_type(sid) != STN_TYPE_ZIGBEE) continue;

        StationData* pdata = (StationData*)tmp_buffer;
        os.get_station_data(sid, pdata);
        os.switch_zigbeestation((ZigbeeStationData*)pdata->sped, false, sid);
    }
}

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
