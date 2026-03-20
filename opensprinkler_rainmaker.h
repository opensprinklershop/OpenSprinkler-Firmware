/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * ESP RainMaker Integration — Alexa & Google Home Smart Home
 *
 * Exposes irrigation zones as RainMaker Switch devices and
 * sensor data (rain, flow, temperature, soil moisture) as
 * RainMaker Temperature-Sensor / custom devices.
 *
 * Uses esp_rmaker_standard_types.h for strict type compliance.
 */

#ifndef _OPENSPRINKLER_RAINMAKER_H
#define _OPENSPRINKLER_RAINMAKER_H

#include "defines.h"

#if defined(ESP32) && defined(ENABLE_RAINMAKER)

// RainMaker supports a limited number of devices per node.
// We expose up to 16 irrigation zones + a controller device + sensor devices.
#define RMAKER_MAX_ZONES       16
#define RMAKER_MAX_SENSORS      8

/**
 * @brief Singleton managing the ESP RainMaker node, zone devices, and sensor devices.
 *
 * Lifecycle:
 *   1. Call OSRainMaker::instance().init() after WiFi is connected and
 *      esp_rmaker_node_init() has been called externally (or let init() do it).
 *   2. Call OSRainMaker::instance().loop() from do_loop() for periodic sensor updates.
 *   3. Call update_station() whenever a zone turns on/off.
 *   4. Call update_sensors() periodically (done automatically in loop()).
 */
class OSRainMaker {
public:
  static OSRainMaker& instance();

  /** Initialize RainMaker node, create zone devices and sensor devices. */
  void init();

  /** Periodic loop — reports sensor value changes to the cloud. */
  void loop();

  /** Report a zone on/off change to RainMaker cloud (called from turn_on/off_station). */
  void update_station(uint8_t sid, bool is_on);

  /** Force-report all sensor values to cloud. */
  void update_sensors();

  /** Report rain sensor state change. */
  void update_rain_sensor(bool rain_detected);

  /** Report rain delay state change. */
  void update_rain_delay(bool delayed);

  /** Report controller enable/disable. */
  void update_controller_enabled(bool enabled);

  /** True after init() completed successfully. */
  bool is_initialized() const { return initialized_; }

private:
  OSRainMaker() = default;
  bool initialized_ = false;
};

#endif // ESP32 && ENABLE_RAINMAKER
#endif // _OPENSPRINKLER_RAINMAKER_H
