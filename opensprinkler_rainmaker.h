/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * ESP RainMaker Integration — Alexa & Google Home Smart Home
 *
 * Exposes irrigation zones as RainMaker Switch devices and
 * sensor data as RainMaker Temperature-Sensor / custom devices.
 * Uses "On Network" provisioning via esp_local_ctrl + mDNS.
 * Users register in the ESP RainMaker app and add the device
 * using the PoP PIN displayed in the OpenSprinkler web UI.
 *
 * All mutable state lives in a PSRAM-allocated data block
 * (OSRainMakerData) to free scarce internal SRAM.  Zone and
 * sensor arrays are sized dynamically at init time instead of
 * using a fixed RMAKER_MAX_ZONES / RMAKER_MAX_SENSORS limit.
 */

#ifndef _OPENSPRINKLER_RAINMAKER_H
#define _OPENSPRINKLER_RAINMAKER_H

#include "defines.h"

#if defined(ESP32) && defined(ENABLE_RAINMAKER)

// Forward declaration — full definition lives in opensprinkler_rainmaker.cpp.
// All RainMaker state (zones, sensors, local-ctrl config) is allocated in
// PSRAM at runtime to free internal SRAM.
struct OSRainMakerData;

class OSRainMaker {
public:
  /**
   * Returns a pointer to the (heap-allocated) singleton, or nullptr when
   * RainMaker is disabled (IOPT_RAINMAKER_ENABLE == 0).  All callers that
   * only want to do something when RainMaker is active should use this:
   *
   *   if (auto *rm = OSRainMaker::get()) rm->update_station(sid, true);
   */
  static OSRainMaker* get();

  /**
   * Heap-allocates the singleton on first call; subsequent calls return the
   * same object.  Only call when IOPT_RAINMAKER_ENABLE is set — i.e. from the
   * network-up path in main.cpp AFTER checking the option.  All update/loop
   * callsites should go through get() instead.
   */
  static OSRainMaker& instance();

  void init();
  void loop();
  void process_pending_cmds();         // drain RainMaker command queue in main loop context
  void update_station(uint8_t sid, bool is_on);
  void update_sensors();
  void update_rain_sensor(bool rain_detected);
  void update_rain_delay(bool delayed);
  void update_controller_enabled(bool enabled);
  /** Report whether program pid is currently running (call when a program starts/stops). */
  void update_program(uint8_t pid, bool running);

  /**
   * Re-sync all program devices to the current pd.nprograms list.
   * Call after any structural change (add, delete, moveup) or after a
   * program is renamed / its enabled flag toggled.
   * Removes stale devices from the node, allocates fresh device objects
   * and adds them — esp_rmaker_start() must already have been called.
   */
  void sync_programs();

  /**
   * Re-sync all zone devices to the current station configuration.
   * Call after a station is renamed or its disable attribute changes.
   */
  void sync_zones();

  /**
   * Update just the Name param of one program device (cheaper than a full
   * sync when only a rename occurred and the program count did not change).
   */
  void update_program_name(uint8_t pid);

  /**
   * Update just the Name param of one zone device (cheaper than a full
   * sync when only a rename occurred and the zone set did not change).
   */
  void update_zone_name(uint8_t sid);

  /** Trigger RainMaker unlink: stops agent, clears NVS, schedules reboot. */
  bool unlink();

  /** Reset user-node mapping only (preserves TLS certs). */
  int reset_mapping();

  /** Full factory reset: erase certs + mapping, reboot after delay_sec. */
  int factory_reset(int delay_sec = 2);

  // ── Stubs for BLE/assisted-claiming API (always inactive in self-claiming mode) ──

  /** Self-claiming never needs BLE provisioning — always returns false. */
  static bool needs_provisioning() { return false; }

  /// No-ops: BLE claiming lifecycle hooks (unused in self-claiming mode)
  void on_wifi_connected() {}
  void on_claim_successful() {}
  void notify_claim_successful() {}
  void notify_claim_failed() {}

  /// Always false: no BLE provisioning in self-claiming mode
  bool is_provisioning() const { return false; }
  bool is_ble_claiming() const { return false; }

  /// Self-claiming never defers sensors (no BLE claiming active)
  bool sensors_deferred() const { return false; }

  bool is_initialized() const;

  /** True after unlink() was called (device will reboot shortly). */
  bool is_unlinking() const;

  /** True if device is connected via Ethernet. */
  bool is_ethernet() const;

  /** True when the RainMaker local-control service is advertised on the LAN. */
  bool is_local_ctrl_started() const;

  /** Current RainMaker MQTT connection state. */
  bool is_mqtt_connected() const;

  /** Current RainMaker user-node mapping state. */
  int get_user_mapping_state() const;

  /** PoP (Proof of Possession) derived from eFuse unique ID — always available. */
  const char* get_pop() const;

  /** Service name derived from MAC (e.g. "PROV_aabbcc"). */
  const char* get_prov_service_name() const;

  // Backward compat alias used by server code
  const char* get_local_ctrl_pop() const;

  void refresh_runtime_state();

private:
  OSRainMaker() = default;
  OSRainMakerData *d_ = nullptr;   // PSRAM-allocated data block
  bool ensure_data();               // lazy PSRAM allocation
};

#endif // ESP32 && ENABLE_RAINMAKER
#endif // _OPENSPRINKLER_RAINMAKER_H
