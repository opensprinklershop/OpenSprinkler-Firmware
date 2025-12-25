#ifndef _SENSOR_HPP
#define _SENSOR_HPP

#include "sensors.h"
#include <string.h>
#include "ArduinoJson.hpp"

// Forward declarations
class BufferFiller;

// SensorBase now contains the persistent attributes previously stored in the old Sensor_t structure
class SensorBase {
public:
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
  int16_t factor = 0;             // factor
  int16_t divider = 0;            // divider
  char userdef_unit[8] = {0};     // unit for custom sensors
  int16_t offset_mv = 0;          // offset in millivolt
  int16_t offset2 = 0;            // offset unit (1/100)
  unsigned char assigned_unitid = 0;  // unitid for userdef and mqtt sensors
  
  /* runtime-only fields not persisted */
  bool mqtt_init = false;
  bool mqtt_push = false;
  unsigned char unitid = 0;
  uint32_t repeat_read = 0;
  double repeat_data = 0.0;
  uint64_t repeat_native = 0;
  ulong last_read = 0;  // timestamp
  double last_logged_data = 0.0;
  ulong last_logged_time = 0;

  SensorBase() {}
  explicit SensorBase(uint type) {this->type = type; } // for derived classes compatibility
  virtual ~SensorBase() {}

  /** Initialize sensor hardware/connection */
  virtual bool init() { return true; }
  
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
    obj["fac"] = factor;
    obj["div"] = divider;
    obj["offset"] = offset_mv;
    obj["offset2"] = offset2;
    obj["unit"] = userdef_unit;
    obj["unitid"] = assigned_unitid;
    obj["enable"] = (uint)flags.enable;
    obj["log"] = (uint)flags.log;
    obj["show"] = (uint)flags.show;

    // runtime fields
    obj["data_ok"] = (uint)flags.data_ok;
    obj["last"] = last_read;
    obj["nativedata"] = last_native_data;
    obj["data"] = last_data;
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
    if (obj.containsKey("div")) divider = obj["div"];
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
    if (obj.containsKey("last")) last_read = obj["last"];
    if (obj.containsKey("nativedata")) last_native_data = obj["nativedata"];
    if (obj.containsKey("data")) last_data = obj["data"];
  }
};

// Generic sensor for types without specific behavior
class GenericSensor : public SensorBase {
public:
  GenericSensor(uint type) : SensorBase(type) {}
  virtual ~GenericSensor() {}
  virtual int read(unsigned long /*time*/) override { return HTTP_RQT_NOT_RECEIVED; }
};

#endif // _SENSOR_HPP
