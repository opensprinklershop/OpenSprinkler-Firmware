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

class ZigbeeReportReceiver;
static ZigbeeReportReceiver* reportReceiver = nullptr;

class ZigbeeReportReceiver : public ZigbeeEP {
public:
    ZigbeeReportReceiver(uint8_t endpoint) : ZigbeeEP(endpoint) {
        _cluster_list = esp_zb_zcl_cluster_list_create();

        esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
        esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(NULL);
        esp_zb_cluster_list_add_identify_cluster(_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

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

        _ep_config.endpoint = endpoint;
        _ep_config.app_profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        _ep_config.app_device_id = ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID;
        _ep_config.app_device_version = 0;
    }

    virtual void zbAttributeRead(uint16_t cluster_id, const esp_zb_zcl_attribute_t *attribute,
                                  uint8_t src_endpoint, esp_zb_zcl_addr_t src_address) override {
        if (!attribute) return;

        uint64_t ieee_addr = 0;
        for (const auto& dev : discovered_devices) {
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
            0);
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
            default: return 0;
        }
    }
};

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
    
    // Check if Zigbee was already started by another component (e.g., Matter)
    if (Zigbee.started()) {
        DEBUG_PRINTLN(F("[ZIGBEE] Zigbee already running (started by another component)"));
        zigbee_initialized = true;
        return;
    }

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
//    (void)esp_wifi_set_ps(WIFI_PS_NONE);
//    (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
//    (void)esp_coex_wifi_i154_enable();

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
    zigbee_last_use = millis();
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_ZIGBEE) continue;

        ZigbeeSensor* zb_sensor = static_cast<ZigbeeSensor*>(sensor);
        bool matches = (zb_sensor->cluster_id == cluster_id && zb_sensor->attribute_id == attr_id);
        if (zb_sensor->device_ieee != 0 && ieee_addr != 0) {
            matches = matches && (zb_sensor->device_ieee == ieee_addr);
        }
        if (matches) {
            zb_sensor->last_native_data = value;
            double converted_value = (double)value;
            if (cluster_id == ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE && attr_id == 0x0000) {
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 100.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT && attr_id == 0x0000) {
                converted_value = value / 10.0;
            } else if (cluster_id == ZB_ZCL_CLUSTER_ID_POWER_CONFIG && attr_id == 0x0021) {
                converted_value = value / 2.0;
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
            zb_sensor->last_lqi = lqi;
            zb_sensor->flags.data_ok = true;
            zb_sensor->repeat_read = 1;
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
