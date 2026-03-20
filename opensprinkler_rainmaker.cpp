/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * ESP RainMaker Integration — Alexa & Google Home Smart Home
 *
 * This module registers OpenSprinkler irrigation zones as RainMaker
 * "Switch" devices (Alexa/Google Home compatible) and exposes sensor
 * data (rain sensor, flow sensor, temperature, soil moisture) as
 * RainMaker "Temperature-Sensor" / custom devices.
 *
 * All types use esp_rmaker_standard_types.h for strict type compliance.
 */

#include "defines.h"

#if defined(ESP32) && defined(ENABLE_RAINMAKER)

#include "opensprinkler_rainmaker.h"
#include "OpenSprinkler.h"
#include "program.h"
#include "sensors.h"
#include "SensorBase.hpp"

extern "C" {
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_standard_services.h>
#include <esp_rmaker_mqtt.h>
#include <esp_rmaker_user_mapping.h>
}

#include <esp_log.h>

static const char *TAG = "OSRainMaker";

extern OpenSprinkler os;
extern ProgramData pd;
extern ulong flow_count;

// Forward declarations from main.cpp (use correct types)
extern void schedule_all_stations(time_os_t curr_time, unsigned char req_option);
extern void turn_off_station(unsigned char sid, time_os_t curr_time, unsigned char shift);

// ─── Internal state ──────────────────────────────────────────────────────────

// Zone device handles (one per exposed station)
static esp_rmaker_device_t *zone_devices[RMAKER_MAX_ZONES] = {};
static uint8_t               zone_sid_map[RMAKER_MAX_ZONES] = {};  // maps index → station id
static uint8_t               zone_count = 0;

// Controller device (rain delay, enabled, rain sensor)
static esp_rmaker_device_t  *controller_device = nullptr;
static esp_rmaker_param_t   *param_enabled      = nullptr;
static esp_rmaker_param_t   *param_rain_delay   = nullptr;
static esp_rmaker_param_t   *param_rain_sensor  = nullptr;
static esp_rmaker_param_t   *param_water_level  = nullptr;

// Sensor devices
static esp_rmaker_device_t  *sensor_devices[RMAKER_MAX_SENSORS] = {};
static uint8_t               sensor_count_rm = 0;

// Timing for periodic sensor updates
static unsigned long last_sensor_update_ms = 0;
static const unsigned long SENSOR_UPDATE_INTERVAL_MS = 30000; // 30 seconds

// ─── Write callback: zone on/off from Alexa / Google Home ────────────────────

static esp_err_t zone_write_cb(const esp_rmaker_device_t *device,
                               const esp_rmaker_param_t *param,
                               const esp_rmaker_param_val_t val,
                               void *priv_data,
                               esp_rmaker_write_ctx_t *ctx)
{
  const char *param_type = esp_rmaker_param_get_type(const_cast<esp_rmaker_param_t*>(param));
  if (!param_type) return ESP_FAIL;

  // Only handle Power param (esp.param.power)
  if (strcmp(param_type, ESP_RMAKER_PARAM_POWER) != 0) return ESP_OK;

  uint8_t sid = (uint8_t)(uintptr_t)priv_data;
  if (sid >= os.nstations) return ESP_ERR_INVALID_ARG;

  bool power_on = val.val.b;
  unsigned long curr_time = os.now_tz();

  if (power_on) {
    // Turn on for default 10 minutes (600 seconds) — manual run
    // Refuse master stations
    if ((os.status.mas == (unsigned char)(sid + 1)) ||
        (os.status.mas2 == (unsigned char)(sid + 1))) {
      ESP_LOGW(TAG, "Refusing to turn on master station %d", sid);
      return ESP_ERR_NOT_ALLOWED;
    }

    RuntimeQueueStruct *q = nullptr;
    unsigned char sqi = pd.station_qid[sid];
    if (sqi == 0xFF) {
      q = pd.enqueue();
    }
    if (!q) {
      ESP_LOGW(TAG, "Queue full, cannot turn on station %d", sid);
      return ESP_ERR_NO_MEM;
    }

    q->st  = 0;
    q->dur = 600; // 10 minutes default
    q->sid = sid;
    q->pid = 99;  // manual
    schedule_all_stations(curr_time, 0);
    ESP_LOGI(TAG, "Zone %d turned ON via %s (10 min)", sid,
             esp_rmaker_device_cb_src_to_str(ctx ? ctx->src : ESP_RMAKER_REQ_SRC_MAX));
  } else {
    // Turn off
    if (pd.station_qid[sid] == 255) {
      // Not running — still acknowledge
      esp_rmaker_param_update_and_report(const_cast<esp_rmaker_param_t*>(param), esp_rmaker_bool(false));
      return ESP_OK;
    }
    RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
    q->deque_time = curr_time;
    turn_off_station(sid, curr_time, 0);
    ESP_LOGI(TAG, "Zone %d turned OFF via %s", sid,
             esp_rmaker_device_cb_src_to_str(ctx ? ctx->src : ESP_RMAKER_REQ_SRC_MAX));
  }

  esp_rmaker_param_update_and_report(const_cast<esp_rmaker_param_t*>(param), val);
  return ESP_OK;
}

// ─── Write callback: controller params (enable, rain delay) ──────────────────

static esp_err_t controller_write_cb(const esp_rmaker_device_t *device,
                                     const esp_rmaker_param_t *param,
                                     const esp_rmaker_param_val_t val,
                                     void *priv_data,
                                     esp_rmaker_write_ctx_t *ctx)
{
  const char *param_name = esp_rmaker_param_get_name(const_cast<esp_rmaker_param_t*>(param));
  if (!param_name) return ESP_FAIL;

  if (strcmp(param_name, "Enabled") == 0) {
    os.status.enabled = val.val.b ? 1 : 0;
    os.iopts[IOPT_DEVICE_ENABLE] = os.status.enabled;
    os.iopts_save();
    ESP_LOGI(TAG, "Controller %s via RainMaker", val.val.b ? "enabled" : "disabled");
  } else if (strcmp(param_name, "Rain Delay") == 0) {
    int hours = val.val.i;
    if (hours > 0) {
      os.nvdata.rd_stop_time = os.now_tz() + (unsigned long)hours * 3600UL;
      os.raindelay_start();
    } else {
      os.nvdata.rd_stop_time = 0;
      os.raindelay_stop();
    }
    ESP_LOGI(TAG, "Rain delay set to %d hours via RainMaker", hours);
  }

  esp_rmaker_param_update_and_report(const_cast<esp_rmaker_param_t*>(param), val);
  return ESP_OK;
}

// ─── Create zone devices ─────────────────────────────────────────────────────

static void create_zone_devices(esp_rmaker_node_t *node) {
  char name_buf[40];
  char stn_name[32];

  uint8_t count = 0;
  for (uint8_t sid = 0; sid < os.nstations && count < RMAKER_MAX_ZONES; sid++) {
    // Skip disabled stations
    uint8_t bid = sid >> 3;
    uint8_t s   = sid & 0x07;
    if (os.attrib_dis[bid] & (1 << s)) continue;

    // Skip master stations
    if (os.is_master_station(sid)) continue;

    os.get_station_name(sid, stn_name);
    if (stn_name[0] == '\0') {
      snprintf(name_buf, sizeof(name_buf), "Zone %d", sid + 1);
    } else {
      snprintf(name_buf, sizeof(name_buf), "%.31s", stn_name);
    }

    // Create as Switch device (Alexa "smart plug" / Google Home "switch")
    // This uses ESP_RMAKER_DEVICE_SWITCH type + ESP_RMAKER_PARAM_POWER
    esp_rmaker_device_t *dev = esp_rmaker_switch_device_create(
        name_buf, (void*)(uintptr_t)sid, os.is_running(sid) ? true : false);
    if (!dev) {
      ESP_LOGE(TAG, "Failed to create zone device for sid=%d", sid);
      continue;
    }

    // Add write callback for Alexa/Google Home control
    esp_rmaker_device_add_cb(dev, zone_write_cb, nullptr);

    // Add subtype for better identification in apps
    esp_rmaker_device_add_subtype(dev, "esp.subtype.irrigation-valve");

    // Add station ID as attribute
    char sid_str[8];
    snprintf(sid_str, sizeof(sid_str), "%d", sid);
    esp_rmaker_device_add_attribute(dev, "station_id", sid_str);

    esp_rmaker_node_add_device(node, dev);

    zone_devices[count] = dev;
    zone_sid_map[count]  = sid;
    count++;
  }
  zone_count = count;
  ESP_LOGI(TAG, "Created %d zone devices", count);
}

// ─── Create controller device ────────────────────────────────────────────────

static void create_controller_device(esp_rmaker_node_t *node) {
  // Use "Other" device type with custom params for controller-level controls
  controller_device = esp_rmaker_device_create(
      "Controller", ESP_RMAKER_DEVICE_OTHER, nullptr);
  if (!controller_device) {
    ESP_LOGE(TAG, "Failed to create controller device");
    return;
  }

  esp_rmaker_device_add_cb(controller_device, controller_write_cb, nullptr);
  esp_rmaker_device_add_subtype(controller_device, "esp.subtype.irrigation-controller");

  // Name param (standard, read-only display name)
  esp_rmaker_param_t *p_name = esp_rmaker_name_param_create(
      ESP_RMAKER_DEF_NAME_PARAM, "Controller");
  esp_rmaker_device_add_param(controller_device, p_name);

  // Enabled toggle (esp.param.toggle + esp.ui.toggle)
  param_enabled = esp_rmaker_param_create(
      "Enabled", ESP_RMAKER_PARAM_TOGGLE,
      esp_rmaker_bool(os.status.enabled ? true : false),
      PROP_FLAG_READ | PROP_FLAG_WRITE | PROP_FLAG_PERSIST);
  esp_rmaker_param_add_ui_type(param_enabled, ESP_RMAKER_UI_TOGGLE);
  esp_rmaker_device_add_param(controller_device, param_enabled);

  // Rain Delay (hours, slider 0–96)
  param_rain_delay = esp_rmaker_param_create(
      "Rain Delay", ESP_RMAKER_PARAM_RANGE,
      esp_rmaker_int(0),
      PROP_FLAG_READ | PROP_FLAG_WRITE);
  esp_rmaker_param_add_ui_type(param_rain_delay, ESP_RMAKER_UI_SLIDER);
  esp_rmaker_param_add_bounds(param_rain_delay,
      esp_rmaker_int(0), esp_rmaker_int(96), esp_rmaker_int(1));
  esp_rmaker_device_add_param(controller_device, param_rain_delay);

  // Rain Sensor (read-only)
  param_rain_sensor = esp_rmaker_param_create(
      "Rain Sensor", ESP_RMAKER_PARAM_TOGGLE,
      esp_rmaker_bool(os.status.sensor1_active ? true : false),
      PROP_FLAG_READ);
  esp_rmaker_param_add_ui_type(param_rain_sensor, ESP_RMAKER_UI_TOGGLE);
  esp_rmaker_device_add_param(controller_device, param_rain_sensor);

  // Water Level (percentage, read-only)
  param_water_level = esp_rmaker_param_create(
      "Water Level", ESP_RMAKER_PARAM_RANGE,
      esp_rmaker_int(os.iopts[IOPT_WATER_PERCENTAGE]),
      PROP_FLAG_READ);
  esp_rmaker_param_add_ui_type(param_water_level, ESP_RMAKER_UI_SLIDER);
  esp_rmaker_param_add_bounds(param_water_level,
      esp_rmaker_int(0), esp_rmaker_int(250), esp_rmaker_int(1));
  esp_rmaker_device_add_param(controller_device, param_water_level);

  esp_rmaker_device_assign_primary_param(controller_device, param_enabled);
  esp_rmaker_node_add_device(node, controller_device);

  ESP_LOGI(TAG, "Created controller device");
}

// ─── Create sensor devices from the sensor API ──────────────────────────────

static void create_sensor_devices(esp_rmaker_node_t *node) {
  uint8_t count = 0;
  SensorIterator it = sensors_iterate_begin();

  while (count < RMAKER_MAX_SENSORS) {
    SensorBase *s = sensors_iterate_next(it);
    if (!s) break;
    if (!s->flags.enable) continue;
    if (s->nr == 0) continue;  // deleted sensor

    char dev_name[40];
    if (s->name[0]) {
      snprintf(dev_name, sizeof(dev_name), "%.31s", s->name);
    } else {
      snprintf(dev_name, sizeof(dev_name), "Sensor %u", s->nr);
    }

    uint8_t uid = getSensorUnitId(s);
    esp_rmaker_device_t *dev = nullptr;

    // Map sensor unit to appropriate RainMaker device type
    switch (uid) {
      case UNIT_DEGREE:
      case UNIT_FAHRENHEIT: {
        // Use standard Temperature Sensor device
        dev = esp_rmaker_temp_sensor_device_create(
            dev_name, nullptr, (float)s->last_data);
        break;
      }
      case UNIT_PERCENT:
      case UNIT_HUM_PERCENT: {
        // Soil moisture / humidity — use custom device with range param
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              (uid == UNIT_HUM_PERCENT) ? "Humidity" : "Moisture",
              ESP_RMAKER_PARAM_RANGE,
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_SLIDER);
          esp_rmaker_param_add_bounds(pv,
              esp_rmaker_float(0.0f), esp_rmaker_float(100.0f), esp_rmaker_float(0.1f));
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
          esp_rmaker_device_add_subtype(dev, "esp.subtype.soil-sensor");
        }
        break;
      }
      case UNIT_LX:
      case UNIT_LM: {
        // Light sensor
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              "Illuminance", ESP_RMAKER_PARAM_TEMPERATURE, // closest standard param
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_TEXT);
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
        }
        break;
      }
      default: {
        // Generic sensor — expose value as text
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          const char *unit = getSensorUnit(s);
          char val_str[32];
          snprintf(val_str, sizeof(val_str), "%.2f %s", s->last_data, unit ? unit : "");

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              "Value", ESP_RMAKER_PARAM_TEMPERATURE,
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_TEXT);
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
        }
        break;
      }
    }

    if (dev) {
      // Add sensor nr as attribute
      char nr_str[8];
      snprintf(nr_str, sizeof(nr_str), "%u", s->nr);
      esp_rmaker_device_add_attribute(dev, "sensor_nr", nr_str);

      // Add sensor type as attribute
      char type_str[8];
      snprintf(type_str, sizeof(type_str), "%u", s->type);
      esp_rmaker_device_add_attribute(dev, "sensor_type", type_str);

      esp_rmaker_node_add_device(node, dev);
      sensor_devices[count] = dev;
      count++;
    }
  }

  sensor_count_rm = count;
  ESP_LOGI(TAG, "Created %d sensor devices from Analog Sensor API", count);
}

// ─── Periodic sensor value updates ──────────────────────────────────────────

static void report_sensor_values() {
  // Update controller params
  if (controller_device) {
    if (param_rain_sensor) {
      bool rain_active = (os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN)
                         ? (os.status.sensor1_active ? true : false) : false;
      esp_rmaker_param_update_and_report(param_rain_sensor, esp_rmaker_bool(rain_active));
    }
    if (param_water_level) {
      esp_rmaker_param_update_and_report(param_water_level,
          esp_rmaker_int(os.iopts[IOPT_WATER_PERCENTAGE]));
    }
    if (param_rain_delay) {
      int rd_hours = 0;
      if (os.nvdata.rd_stop_time > os.now_tz()) {
        rd_hours = (int)((os.nvdata.rd_stop_time - os.now_tz()) / 3600UL);
        if (rd_hours < 1) rd_hours = 1; // still active
      }
      esp_rmaker_param_update_and_report(param_rain_delay, esp_rmaker_int(rd_hours));
    }
  }

  // Update sensor device values
  uint8_t idx = 0;
  SensorIterator it = sensors_iterate_begin();

  while (idx < sensor_count_rm) {
    SensorBase *s = sensors_iterate_next(it);
    if (!s) break;
    if (!s->flags.enable || s->nr == 0) continue;

    esp_rmaker_device_t *dev = sensor_devices[idx];
    if (!dev) { idx++; continue; }

    // Get the primary parameter and update it
    // For temp sensors, the primary param is "Temperature"
    // For others, it's the first custom param added
    uint8_t uid = getSensorUnitId(s);
    const char *param_name;
    switch (uid) {
      case UNIT_DEGREE:
      case UNIT_FAHRENHEIT:
        param_name = ESP_RMAKER_DEF_TEMPERATURE_NAME;
        break;
      case UNIT_PERCENT:
        param_name = "Moisture";
        break;
      case UNIT_HUM_PERCENT:
        param_name = "Humidity";
        break;
      case UNIT_LX:
      case UNIT_LM:
        param_name = "Illuminance";
        break;
      default:
        param_name = "Value";
        break;
    }

    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, param_name);
    if (p && s->flags.data_ok) {
      esp_rmaker_param_update_and_report(p, esp_rmaker_float((float)s->last_data));
    }
    idx++;
  }
}

// ─── Public API ─────────────────────────────────────────────────────────────

OSRainMaker& OSRainMaker::instance() {
  static OSRainMaker inst;
  return inst;
}

void OSRainMaker::init() {
  if (initialized_) return;

  // ── Bootstrap the RainMaker node ──────────────────────────────────────────
  // esp_rmaker_node_init() must be called once before devices can be added.
  // If a node already exists (e.g. from an earlier init path), reuse it.
  esp_rmaker_node_t *node = (esp_rmaker_node_t *)esp_rmaker_get_node();
  if (!node) {
    esp_rmaker_config_t rmaker_cfg = {
      .enable_time_sync = true,
    };
    node = esp_rmaker_node_init(&rmaker_cfg, "OpenSprinkler", "Irrigation Controller");
    if (!node) {
      ESP_LOGE(TAG, "esp_rmaker_node_init() failed — RainMaker not available");
      return;
    }
    ESP_LOGI(TAG, "RainMaker node created");
  }

  ESP_LOGI(TAG, "Initializing OpenSprinkler RainMaker integration...");

  // Create zone devices (irrigation valves as switches)
  create_zone_devices(node);

  // Create controller device (enable, rain delay, rain sensor, water level)
  create_controller_device(node);

  // Create sensor devices from the Analog Sensor API
  create_sensor_devices(node);

  // ── Start the RainMaker agent (connects to cloud via MQTT) ────────────────
  esp_err_t err = esp_rmaker_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_rmaker_start() failed: %s", esp_err_to_name(err));
    return;
  }

  last_sensor_update_ms = millis();
  initialized_ = true;

  ESP_LOGI(TAG, "RainMaker integration initialized: %d zones, 1 controller, %d sensors",
           zone_count, sensor_count_rm);
}

void OSRainMaker::loop() {
  if (!initialized_) return;
  if (!esp_rmaker_is_mqtt_connected()) return;

  unsigned long now = millis();
  if ((long)(now - last_sensor_update_ms) >= (long)SENSOR_UPDATE_INTERVAL_MS) {
    last_sensor_update_ms = now;
    report_sensor_values();
  }
}

void OSRainMaker::update_station(uint8_t sid, bool is_on) {
  if (!initialized_) return;
  if (!esp_rmaker_is_mqtt_connected()) return;

  // Find the zone device for this station
  for (uint8_t i = 0; i < zone_count; i++) {
    if (zone_sid_map[i] == sid && zone_devices[i]) {
      esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_type(
          zone_devices[i], ESP_RMAKER_PARAM_POWER);
      if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_bool(is_on));
      }
      return;
    }
  }
}

void OSRainMaker::update_sensors() {
  if (!initialized_) return;
  report_sensor_values();
}

void OSRainMaker::update_rain_sensor(bool rain_detected) {
  if (!initialized_ || !param_rain_sensor) return;
  if (!esp_rmaker_is_mqtt_connected()) return;
  esp_rmaker_param_update_and_report(param_rain_sensor, esp_rmaker_bool(rain_detected));
}

void OSRainMaker::update_rain_delay(bool delayed) {
  if (!initialized_ || !param_rain_delay) return;
  if (!esp_rmaker_is_mqtt_connected()) return;
  int hours = delayed ? (int)((os.nvdata.rd_stop_time - os.now_tz()) / 3600UL) : 0;
  if (hours < 0) hours = 0;
  esp_rmaker_param_update_and_report(param_rain_delay, esp_rmaker_int(hours));
}

void OSRainMaker::update_controller_enabled(bool enabled) {
  if (!initialized_ || !param_enabled) return;
  if (!esp_rmaker_is_mqtt_connected()) return;
  esp_rmaker_param_update_and_report(param_enabled, esp_rmaker_bool(enabled));
}

#endif // ESP32 && ENABLE_RAINMAKER
