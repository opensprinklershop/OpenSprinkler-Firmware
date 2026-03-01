#ifndef _SENSOR_BASE_HPP
#define _SENSOR_BASE_HPP

#include "sensors.h"
#include <string.h>
#include <math.h>
#include "ArduinoJson.hpp"

// Forward declarations
class BufferFiller;

// SensorBase now contains the persistent attributes previously stored in the old Sensor_t structure
class SensorBase {
public:
  enum TrendState : int8_t {
    TREND_UNAVAILABLE = 0,
    TREND_STRONG_DOWN = -3,
    TREND_SLIGHT_DOWN = -2,
    TREND_NO_CHANGE = -1,
    TREND_SLIGHT_UP = 1,
    TREND_STRONG_UP = 2
  };

  static const uint8_t TREND_HISTORY_SIZE = 24;
  static const uint32_t TREND_MIN_SPAN_SEC = 3600; // require at least 1h span
  static constexpr double TREND_NO_CHANGE_REL = 0.02; // 2% change over window
  static constexpr double TREND_STRONG_REL = 0.10;    // 10% change over window

  // Persistent fields
  uint nr = 0;                    // 1..n sensor-nr, 0=deleted
  char name[30] = {0};            // name
  uint type = 0;                  // sensor type
  uint group = 0;                 // group assignment
  uint32_t ip = 0;                // tcp-ip
  uint port = 0;                  // tcp-port or address
  uint id = 0;                    // modbus id or channel
  uint read_interval = 0;         // seconds
  uint32_t last_native_data = 0;  // last native sensor data
  double last_data = 0.0;         // last converted sensor data
  SensorFlags_t flags = {};       // enable/log/show/data_ok etc.
  int16_t factor = 1;             // factor
  int16_t divider = 1;            // divider
  char userdef_unit[8] = {0};     // unit for custom sensors
  int16_t offset_mv = 0;          // offset in millivolt
  int16_t offset2 = 0;            // offset unit (1/100)
  unsigned char assigned_unitid = 0;  // unitid for userdef and mqtt sensors
  
  /* runtime-only fields not persisted */
  unsigned char unitid = 0;
  uint32_t repeat_read = 0;
  double repeat_data = 0.0;
  uint64_t repeat_native = 0;
  ulong last_read = 0;  // timestamp
  double last_logged_data = 0.0;
  ulong last_logged_time = 0;
  ulong last = 0;

  // runtime-only trend history (not persisted)
  double trend_history[TREND_HISTORY_SIZE] = {0};
  uint32_t trend_time[TREND_HISTORY_SIZE] = {0};
  uint8_t trend_count = 0;
  uint8_t trend_head = 0;  // next write index
  int8_t trend_state = TREND_UNAVAILABLE;

  SensorBase() {}
  explicit SensorBase(uint type) {this->type = type; } // for derived classes compatibility
  virtual ~SensorBase() {}

  /** Initialize sensor hardware/connection */
  virtual bool init() { return true; }
  virtual bool isGeneric() const { return false; }
  
  /** Cleanup sensor resources */
  virtual void deinit() {}

  /**
   * @brief Read sensor value and update last_data/last_native_data
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on success, HTTP_RQT_NOT_RECEIVED on error
   */
  virtual int read(unsigned long time) = 0;

  /**
   * @brief Set device address (for RS485/Modbus sensors)
   * @param newAddress New device address
   * @return HTTP_RQT_SUCCESS on success, HTTP_RQT_NOT_RECEIVED if not supported
   */
  virtual int setAddress(uint8_t newAddress) { (void)newAddress; return HTTP_RQT_NOT_RECEIVED; }

  /**
   * @brief Emit JSON representation to BufferFiller (for HTTP response)
   * @param bfill Output buffer
   * @note Derived classes override to include type-specific fields
   */
  virtual void emitJson(BufferFiller& bfill) const;

  /**
   * @brief Get unit name string for this sensor
   * @return Unit string (e.g., "%", "°C", "V")
   */
  virtual const char* getUnit() const;
  
  /**
   * @brief Get unit ID for this sensor type
   * @return Unit ID (UNIT_PERCENT, UNIT_DEGREE, UNIT_VOLT, etc.)
   */
  virtual unsigned char getUnitId() const;

  void trend_reset() {
    trend_count = 0;
    trend_head = 0;
    trend_state = TREND_UNAVAILABLE;
    memset(trend_history, 0, sizeof(trend_history));
    memset(trend_time, 0, sizeof(trend_time));
  }

  void trend_add_sample(double value, uint32_t sample_time) {
    if (!isfinite(value) || sample_time == 0) {
      trend_state = TREND_UNAVAILABLE;
      return;
    }

    trend_history[trend_head] = value;
    trend_time[trend_head] = sample_time;
    trend_head = (uint8_t)((trend_head + 1) % TREND_HISTORY_SIZE);
    if (trend_count < TREND_HISTORY_SIZE) {
      trend_count++;
    }

    if (trend_count < 3) {
      trend_state = TREND_UNAVAILABLE;
      return;
    }

    uint8_t oldest_idx = (uint8_t)((trend_head + TREND_HISTORY_SIZE - trend_count) % TREND_HISTORY_SIZE);
    uint8_t newest_idx = (uint8_t)((trend_head + TREND_HISTORY_SIZE - 1) % TREND_HISTORY_SIZE);
    uint32_t t_oldest = trend_time[oldest_idx];
    uint32_t t_newest = trend_time[newest_idx];
    if (t_newest <= t_oldest + TREND_MIN_SPAN_SEC) {
      trend_state = TREND_UNAVAILABLE;
      return;
    }

    // Linear regression slope over time window (literature: standard least-squares trend)
    double mean_t = 0.0;
    double mean_y = 0.0;
    for (uint8_t i = 0; i < trend_count; i++) {
      uint8_t idx = (uint8_t)((oldest_idx + i) % TREND_HISTORY_SIZE);
      mean_t += (double)trend_time[idx];
      mean_y += trend_history[idx];
    }
    mean_t /= (double)trend_count;
    mean_y /= (double)trend_count;

    double num = 0.0;
    double den = 0.0;
    for (uint8_t i = 0; i < trend_count; i++) {
      uint8_t idx = (uint8_t)((oldest_idx + i) % TREND_HISTORY_SIZE);
      double dt = (double)trend_time[idx] - mean_t;
      double dy = trend_history[idx] - mean_y;
      num += dt * dy;
      den += dt * dt;
    }
    if (den <= 0.0) {
      trend_state = TREND_UNAVAILABLE;
      return;
    }

    double slope = num / den; // units per second
    double window = (double)(t_newest - t_oldest);
    double change = slope * window;
    double baseline = fabs(mean_y);
    if (baseline < 1.0) baseline = 1.0;
    double rel = fabs(change) / baseline;

    if (fabs(change) < 0.02 || rel < TREND_NO_CHANGE_REL) {
      trend_state = TREND_NO_CHANGE;
    } else if (rel >= TREND_STRONG_REL) {
      trend_state = (change > 0) ? TREND_STRONG_UP : TREND_STRONG_DOWN;
    } else {
      trend_state = (change > 0) ? TREND_SLIGHT_UP : TREND_SLIGHT_DOWN;
    }
  }

  int8_t get_trend_state() const {
    return trend_state;
  }

  /**
   * @brief Serialize sensor configuration to JSON object
   * @param obj JSON object to populate
   */
  virtual void toJson(ArduinoJson::JsonObject obj) const {
    if (!obj) return;
    obj["nr"] = nr;
    obj["type"] = type;
    obj["group"] = group;
    obj["name"] = name;
    obj["ip"] = ip;
    obj["port"] = port;
    obj["id"] = id;
    obj["ri"] = read_interval;
    obj["factor"] = factor;
    obj["divider"] = divider;
    obj["fac"] = factor;
    obj["div"] = divider;
    obj["offset"] = offset_mv;
    obj["offset2"] = offset2;
    obj["unit"] = getUnit();  // Use virtual method to get correct unit label
    obj["unitid"] = getUnitId();  // Use virtual method for consistency
    obj["enable"] = (uint)flags.enable;
    obj["log"] = (uint)flags.log;
    obj["show"] = (uint)flags.show;

    // runtime fields
    obj["data_ok"] = (uint)flags.data_ok;
    obj["last"] = last;
    obj["nativedata"] = last_native_data;
    obj["data"] = last_data;
    obj["trend"] = trend_state;
  }

  /**
   * @brief Load sensor configuration from JSON object
   * @param obj JSON object with configuration data
   */
  virtual void fromJson(ArduinoJson::JsonVariantConst obj) {
    if (obj.containsKey("nr")) nr = obj["nr"];
    if (obj.containsKey("type")) type = obj["type"];
    if (obj.containsKey("group")) group = obj["group"];
    if (obj.containsKey("name")) {
      const char *sname = obj["name"].as<const char*>();
      if (sname) strncpy(name, sname, sizeof(name)-1);
    }
    if (obj.containsKey("ip")) ip = obj["ip"];
    if (obj.containsKey("port")) port = obj["port"];
    if (obj.containsKey("id")) id = obj["id"];
    if (obj.containsKey("ri")) read_interval = obj["ri"];
    if (obj.containsKey("fac")) factor = obj["fac"];
    else if (obj.containsKey("factor")) factor = obj["factor"];
    if (obj.containsKey("div")) divider = obj["div"];
    else if (obj.containsKey("divider")) divider = obj["divider"];
    if (obj.containsKey("offset")) offset_mv = obj["offset"];
    if (obj.containsKey("offset2")) offset2 = obj["offset2"];
    if (obj.containsKey("unit")) {
      const char *unit = obj["unit"].as<const char*>();
      if (unit) strncpy(userdef_unit, unit, sizeof(userdef_unit)-1);
    }
    if (obj.containsKey("unitid")) assigned_unitid = obj["unitid"];
    if (obj.containsKey("enable")) flags.enable = obj["enable"];
    if (obj.containsKey("log")) flags.log = obj["log"];
    if (obj.containsKey("show")) flags.show = obj["show"];

    if (obj.containsKey("data_ok")) flags.data_ok = obj["data_ok"];
    if (obj.containsKey("last")) last = obj["last"];
    if (obj.containsKey("nativedata")) last_native_data = obj["nativedata"];
    if (obj.containsKey("data")) last_data = obj["data"];
  }
};

// Generic sensor for types without specific behaviour.
// Used as a fallback when a sensor type is not compiled into the current
// firmware variant (e.g. a ZigBee sensor saved on a Matter build).
//
// The full raw JSON is captured at load-time by sensor_load() via setRawJson()
// and replayed at save-time by sensor_save(), so all variant-specific fields
// (e.g. ZigBee device_ieee, cluster_id, endpoint) survive a firmware-switch
// roundtrip without any data loss.
//
// All ArduinoJson serialization/deserialization is kept in sensors.cpp (where
// the ArduinoJson namespace is fully in scope) rather than here, to avoid
// template-disambiguation and namespace-qualification issues in a shared header.
class GenericSensor : public SensorBase {
public:
  // Raw JSON snapshot stored by sensor_load() — preserves variant-specific
  // fields across firmware switches.  Heap-allocated so it works on both
  // ESP32 (Arduino) and Linux (OSPI) without string-type dependencies.
  char* _raw_json = nullptr;

  explicit GenericSensor(uint type) : SensorBase(type), _raw_json(nullptr) {}
  virtual ~GenericSensor() {
    if (_raw_json) { free(_raw_json); _raw_json = nullptr; }
  }

  // Called from sensors.cpp to store the serialised JSON for later replay on save.
  void setRawJson(const char* json, size_t len) {
    if (_raw_json) { free(_raw_json); _raw_json = nullptr; }
    _raw_json = (char*)malloc(len + 1);
    if (_raw_json) { memcpy(_raw_json, json, len); _raw_json[len] = '\0'; }
  }

  virtual bool isGeneric() const override { return true; }
  virtual int read(unsigned long /*time*/) override { return HTTP_RQT_NOT_RECEIVED; }
};

#endif // _SENSOR_HPP
