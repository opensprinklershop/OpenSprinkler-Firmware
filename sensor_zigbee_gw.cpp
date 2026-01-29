/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Zigbee sensor implementation - Gateway/Coordinator mode
 * Compile-time selectable via ZIGBEE_MODE_ZCZR
 *
 * This implementation starts the Zigbee stack in coordinator/gateway mode
 * and receives attribute reports from bound end devices. It is intended
 * for read-only access to Zigbee sensor data.
 */

#include "sensor_zigbee.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

extern OpenSprinkler os;

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE) && defined(ZIGBEE_MODE_ZCZR)

#include <esp_partition.h>
#include <vector>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include "esp_zigbee_core.h"
#include "Zigbee.h"
#include "ZigbeeEP.h"

// Zigbee Gateway state
static bool zigbee_initialized = false;
static bool zigbee_connected = false;
static bool zigbee_needs_nvram_reset = false;
static unsigned long zigbee_last_use = 0;
static const unsigned long ZIGBEE_IDLE_TIMEOUT_MS = 15000;

// Active Zigbee sensor (prevents concurrent access)
static int active_zigbee_sensor = 0;

// Discovered devices storage
static std::vector<ZigbeeDeviceInfo> discovered_devices;

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

// Lazy-loading report cache: stores incoming attribute reports until sensor reads them
struct ZigbeeAttributeReport {
    uint64_t ieee_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    int32_t value;
    uint8_t lqi;
    unsigned long timestamp;
};

static constexpr size_t MAX_PENDING_REPORTS = 16;
static ZigbeeAttributeReport pending_reports[MAX_PENDING_REPORTS];
static size_t pending_report_count = 0;
static constexpr unsigned long REPORT_VALIDITY_MS = 60000;  // Reports valid for 60 seconds

class ZigbeeReportReceiver;
static ZigbeeReportReceiver* reportReceiver = nullptr;

class ZigbeeReportReceiver : public ZigbeeEP {
public:
    ZigbeeReportReceiver(uint8_t endpoint) : ZigbeeEP(endpoint) {
        // Minimal cluster setup - only what's needed for receiving reports
        _cluster_list = esp_zb_zcl_cluster_list_create();
        
        if (_cluster_list) {
            // Basic cluster (mandatory)
            esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
            if (basic_cluster) {
                esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
            }
            
            // Identify cluster (mandatory)
            esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
            if (identify_cluster) {
                esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
            }
        }
        
        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
        
        DEBUG_PRINTLN(F("[ZIGBEE] Report receiver endpoint created (minimal config)"));
    }
    
    virtual ~ZigbeeReportReceiver() = default;

    /**
     * @brief Callback when attribute report received from Zigbee device
     * IMPORTANT: This callback just CACHES the report. Sensors are loaded on-demand.
     * This prevents sensor structures from being accessed during stack callbacks.
     */
    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute,
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) return;
        
        // Find IEEE address from short address
        uint64_t ieee_addr = 0;
        for (const auto& dev : discovered_devices) {
            if (dev.short_addr == src_address.u.short_addr) {
                ieee_addr = dev.ieee_addr;
                break;
            }
        }
        
        // Store report in cache for lazy-loading by sensor
        if (pending_report_count < MAX_PENDING_REPORTS) {
            ZigbeeAttributeReport& report = pending_reports[pending_report_count++];
            report.ieee_addr = ieee_addr;
            report.endpoint = src_endpoint;
            report.cluster_id = cluster_id;
            report.attr_id = attribute->id;
            report.value = extractAttributeValue(attribute);
            report.lqi = 0;
            report.timestamp = millis();
            
            DEBUG_PRINTF(F("[ZIGBEE] Report cached: cluster=0x%04X attr=0x%04X value=%ld\n"),
                        cluster_id, attribute->id, report.value);
        } else {
            DEBUG_PRINTLN(F("[ZIGBEE] Report cache full - dropping report"));
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
                DEBUG_PRINTF(F("[ZIGBEE] Unknown attribute type: 0x%02X\n"), attr->data.type);
                return 0;
        }
    }
};

// Forward declaration
static void updateSensorFromReport(ZigbeeSensor* zb_sensor, const ZigbeeAttributeReport& report);

static bool erase_zigbee_nvram() {
    const esp_partition_t* zb_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        "zb_storage");

    if (!zb_partition) {
        return false;
    }

    esp_err_t err = esp_partition_erase_range(zb_partition, 0, zb_partition->size);
    return err == ESP_OK;
}

void sensor_zigbee_factory_reset() {
    zigbee_needs_nvram_reset = true;
}

void sensor_zigbee_stop() {
    if (!zigbee_initialized) return;
    zigbee_initialized = false;
    zigbee_connected = false;
    if (reportReceiver) {
        delete reportReceiver;
        reportReceiver = nullptr;
    }
    Zigbee.stop();
}

void sensor_zigbee_start() {
    // When Matter is enabled, completely disable Zigbee sensor gateway
    // Matter uses the same IEEE 802.15.4 radio and ZBOSS stack
    #ifdef ENABLE_MATTER
    static bool matter_warning_shown = false;
    if (!matter_warning_shown) {
        DEBUG_PRINTLN(F("[ZIGBEE] Matter enabled - Zigbee sensor gateway disabled"));
        matter_warning_shown = true;
    }
    return;
    #endif

    if (zigbee_initialized) return;
    
    // Ensure clean state: stop any previous Zigbee instance
    if (Zigbee.started()) {
        DEBUG_PRINTLN(F("[ZIGBEE] Stopping previous Zigbee instance..."));
        Zigbee.stop();
        if (reportReceiver) {
            delete reportReceiver;
            reportReceiver = nullptr;
        }
    }
    
    // Small delay to ensure Zigbee stack cleanup
    vTaskDelay(pdMS_TO_TICKS(100));

    zigbee_last_use = millis();;

    if (zigbee_needs_nvram_reset) {
        zigbee_needs_nvram_reset = false;
        erase_zigbee_nvram();
    }

    esp_zb_radio_config_t radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE };
    esp_zb_host_config_t host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE };

    Zigbee.setHostConfig(host_config);
    Zigbee.setRadioConfig(radio_config);

    WiFi.setSleep(false);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    (void)esp_coex_wifi_i154_enable();

    reportReceiver = new ZigbeeReportReceiver(10);
    Zigbee.addEndpoint(reportReceiver);
    reportReceiver->setManufacturerAndModel("OpenSprinkler", "ZigbeeGateway");

#ifdef ZIGBEE_COORDINATOR
    const bool use_coordinator = true;
#else
    const bool use_coordinator = true;
#endif

    if (!Zigbee.begin(use_coordinator ? ZIGBEE_COORDINATOR : ZIGBEE_END_DEVICE, true)) {
        delete reportReceiver;
        reportReceiver = nullptr;
        return;
    }

    zigbee_initialized = true;
}

bool sensor_zigbee_is_active() {
    return zigbee_initialized;
}

bool sensor_zigbee_ensure_started() {
    if (zigbee_initialized) return true;
    sensor_zigbee_start();
    return zigbee_initialized;
}

void ZigbeeSensor::zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint,
                                            uint16_t cluster_id, uint16_t attr_id,
                                            int32_t value, uint8_t lqi) {
    // This callback is called by the Zigbee stack through the report receiver
    // It now processes any pending reports from the cache
    zigbee_last_use = millis();
    
    // Process pending reports in cache (lazy-loading of sensors)
    for (size_t i = 0; i < pending_report_count; i++) {
        const ZigbeeAttributeReport& report = pending_reports[i];
        
        // Skip expired reports
        if (millis() - report.timestamp > REPORT_VALIDITY_MS) {
            continue;
        }
        
        // Find matching sensor and update it (lazy-load if needed)
        SensorIterator it = sensors_iterate_begin();
        SensorBase* sensor;
        bool found = false;
        
        while ((sensor = sensors_iterate_next(it)) != NULL) {
            if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;

            ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
            
            // Check if sensor matches this report
            bool matches = (zb_sensor->cluster_id == report.cluster_id && 
                           zb_sensor->attribute_id == report.attr_id);
            if (zb_sensor->device_ieee != 0 && report.ieee_addr != 0) {
                matches = matches && (zb_sensor->device_ieee == report.ieee_addr);
            }
            
            if (matches) {
                // Update sensor with cached report value
                updateSensorFromReport(zb_sensor, report);
                found = true;
                break;
            }
        }
        
        // Also check sensors that might not be loaded yet (lazy-load potential)
        // by storing report value for next read attempt
        if (!found) {
            DEBUG_PRINTF(F("[ZIGBEE] Report cached for lazy-load: cluster=0x%04X attr=0x%04X\n"),
                        report.cluster_id, report.attr_id);
        }
    }
    
    // Clear old reports from cache
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < pending_report_count; read_idx++) {
        if (millis() - pending_reports[read_idx].timestamp <= REPORT_VALIDITY_MS) {
            pending_reports[write_idx++] = pending_reports[read_idx];
        }
    }
    pending_report_count = write_idx;
}

/**
 * @brief Helper function to update sensor from cached report
 * Applies cluster-specific conversions and calibrations
 */
static void updateSensorFromReport(ZigbeeSensor* zb_sensor, const ZigbeeAttributeReport& report) {
    if (!zb_sensor) return;
    
    zb_sensor->last_native_data = report.value;
    double converted_value = (double)report.value;
    
    // Apply cluster-specific conversions
    if (report.cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 100.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && report.attr_id == 0x0000) {
        converted_value = report.value / 10.0;
    } else if (report.cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && report.attr_id == 0x0021) {
        converted_value = report.value / 2.0;
        zb_sensor->last_battery = (uint32_t)converted_value;
    }
    
    // Apply sensor-specific calibrations
    converted_value -= (double)zb_sensor->offset_mv / 1000.0;
    if (zb_sensor->factor && zb_sensor->divider)
        converted_value *= (double)zb_sensor->factor / (double)zb_sensor->divider;
    else if (zb_sensor->divider)
        converted_value /= (double)zb_sensor->divider;
    else if (zb_sensor->factor)
        converted_value *= (double)zb_sensor->factor;
    converted_value += zb_sensor->offset2 / 100.0;
    
    // Update sensor state
    zb_sensor->last_data = converted_value;
    zb_sensor->last_lqi = report.lqi;
    zb_sensor->flags.data_ok = true;
    zb_sensor->repeat_read = 1;
    
    DEBUG_PRINTF(F("[ZIGBEE] Sensor updated: cluster=0x%04X value=%.2f\n"),
                report.cluster_id, converted_value);
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
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else return 0;
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
        if (ieee_str) device_ieee = parseIeeeAddress(ieee_str);
    }
    if (obj.containsKey("endpoint")) endpoint = obj["endpoint"].as<uint8_t>();
    if (obj.containsKey("cluster_id")) cluster_id = obj["cluster_id"].as<uint16_t>();
    if (obj.containsKey("attribute_id")) attribute_id = obj["attribute_id"].as<uint16_t>();
    if (obj.containsKey("poll_interval")) poll_interval = obj["poll_interval"].as<uint32_t>();
    if (obj.containsKey("factor")) factor = obj["factor"].as<int32_t>();
    if (obj.containsKey("divider")) divider = obj["divider"].as<int32_t>();
    if (obj.containsKey("offset_mv")) offset_mv = obj["offset_mv"].as<int32_t>();
    if (obj.containsKey("offset2")) offset2 = obj["offset2"].as<int32_t>();
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
    return true;
}

void ZigbeeSensor::deinit() {
    device_bound = false;
    flags.data_ok = false;
}

int ZigbeeSensor::read(unsigned long time) {
    if (!zigbee_initialized && !sensor_zigbee_ensure_started()) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    zigbee_last_use = millis();

    if (active_zigbee_sensor > 0 && active_zigbee_sensor != (int)nr) {
        repeat_read = 1;
        SensorBase *t = sensor_by_nr(active_zigbee_sensor);
        if (!t || !t->flags.enable) active_zigbee_sensor = 0;
        return HTTP_RQT_NOT_RECEIVED;
    }

    if (repeat_read == 0) {
        if (active_zigbee_sensor != (int)nr) {
            active_zigbee_sensor = nr;
        }
        if (!Zigbee.started()) {
            flags.data_ok = false;
            active_zigbee_sensor = 0;
            return HTTP_RQT_NOT_RECEIVED;
        }
        repeat_read = 1;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    } else {
        repeat_read = 0;
        active_zigbee_sensor = 0;
        last_read = time;
        return flags.data_ok ? HTTP_RQT_SUCCESS : HTTP_RQT_NOT_RECEIVED;
    }
}

void sensor_zigbee_bind_device(uint nr, const char *device_ieee_str) {
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
    (void)duration;
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
    
    // Process pending reports from cache (lazy-loading of sensors)
    if (pending_report_count > 0) {
        ZigbeeSensor::zigbee_attribute_callback(0, 0, 0, 0, 0, 0);  // Trigger cache processing
    }
    
    static bool last_connected = false;
    bool connected = Zigbee.started() && Zigbee.connected();
    if (connected != last_connected) {
        last_connected = connected;
        zigbee_connected = connected;
    }

    if ((millis() - zigbee_last_use) > ZIGBEE_IDLE_TIMEOUT_MS && active_zigbee_sensor == 0) {
        sensor_zigbee_stop();
    }
}

bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                   uint16_t cluster_id, uint16_t attribute_id) {
    (void)device_ieee;
    (void)endpoint;
    (void)cluster_id;
    (void)attribute_id;
    return false;
}

void ZigbeeSensor::emitJson(BufferFiller& bfill) const {
    SensorBase::emitJson(bfill);
}

unsigned char ZigbeeSensor::getUnitId() const {
    if (assigned_unitid > 0) return assigned_unitid;
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

#endif // ESP32C5 && OS_ENABLE_ZIGBEE && ZIGBEE_MODE_ZCZR
