/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Matter (CHIP) protocol implementation
 * Jan 2026 @ OpenSprinkler.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef ENABLE_MATTER

#include "OpenSprinkler.h"
#include "program.h"
#include "main.h"
#include "sensors.h"
#include "SensorBase.hpp"
#include "opensprinkler_matter.h"
#include <vector>
#include <unordered_map>

// BT header detection - ESP32-C5 uses NimBLE, not Bluedroid
#if defined(ESP32)
  #if __has_include("esp_bt.h")
    #define HAS_ESP_BT 1
    #include "esp_bt.h"
    #if __has_include("esp_bt_main.h")
      #include "esp_bt_main.h"
      #define HAS_BLUEDROID 1
    #else
      #define HAS_BLUEDROID 0
    #endif
  #elif __has_include("nimble/nimble_port.h")
    // ESP32-C5 with NimBLE stack
    #define HAS_ESP_BT 1
    #define HAS_NIMBLE 1
    #define HAS_BLUEDROID 0
    #include "nimble/nimble_port.h"
    #include "host/ble_hs.h"
  #else
    #define HAS_ESP_BT 0
    #define HAS_BLUEDROID 0
  #endif
#else
  #define HAS_ESP_BT 0
  #define HAS_BLUEDROID 0
#endif

// Arduino ESP32 Matter SDK - Matter.h includes all endpoint types
#include <Matter.h>

extern OpenSprinkler os;
extern ProgramData pd;

// Encapsulated Matter integration
class OSMatterImpl {
public:
  // endpoint storage
  std::unordered_map<unsigned char, std::unique_ptr<MatterOnOffPlugin>> stations;
  std::vector<MatterTemperatureSensor> temp_sensors;
  std::vector<MatterHumiditySensor> hum_sensors;

  bool initialized = false;
  bool commissioned = false;
  uint32_t sensor_signature = 0;

  struct SensorProfile { uint8_t temp_count=0; uint8_t hum_count=0; uint32_t signature=0; };

  static OSMatterImpl& instance() {
    static OSMatterImpl impl;
    return impl;
  }

  static void event_callback(matterEvent_t event, const chip::DeviceLayer::ChipDeviceEvent *eventData) {
    (void)eventData;
    DEBUG_PRINTF("Matter Event: 0x%04X\n", event);
    auto &self = instance();
    switch(event) {
      case MATTER_EVENT_COMMISSIONED:
        self.commissioned = true;
        DEBUG_PRINTLN("Matter: Device commissioned");
        break;
      case MATTER_EVENT_DECOMMISSIONED:
        self.commissioned = false;
        DEBUG_PRINTLN("Matter: Device decommissioned");
        break;
      case MATTER_EVENT_FABRIC_ADDED:
        DEBUG_PRINTLN("Matter: Fabric added");
        break;
      case MATTER_EVENT_FABRIC_REMOVED:
        DEBUG_PRINTLN("Matter: Fabric removed");
        break;
      case MATTER_EVENT_WIFI_CONNECTIVITY_CHANGE:
        DEBUG_PRINTLN("Matter: WiFi connectivity changed");
        break;
      default:
        break;
    }
  }

  SensorProfile compute_sensor_profile() {
    SensorProfile profile{};
    SensorIterator it;
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != nullptr) {
      if (!sensor || sensor->type == SENSOR_NONE) continue;
      profile.signature = (profile.signature * 131u) ^ ((uint32_t)sensor->type << 16) ^ sensor->nr;
      switch(sensor->type) {
        case SENSOR_SMT100_TEMP:
        case SENSOR_SMT50_TEMP:
        case SENSOR_SMT100_ANALOG_TEMP:
        case SENSOR_OSPI_ANALOG_SMT50_TEMP:
        case SENSOR_INTERNAL_TEMP:
        case SENSOR_TH100_TEMP:
        case SENSOR_THERM200:
        case SENSOR_FYTA_TEMPERATURE:
        case SENSOR_WEATHER_TEMP_C:
        case SENSOR_WEATHER_TEMP_F:
          profile.temp_count++;
          break;
        case SENSOR_TH100_MOIS:
        case SENSOR_WEATHER_HUM:
          profile.hum_count++;
          break;
        default:
          break;
      }
    }
    return profile;
  }

  void init() {
    DEBUG_PRINTLN("Matter: Initializing...");
    if (initialized) { DEBUG_PRINTLN("Matter: Already initialized"); return; }

    stations.clear(); temp_sensors.clear(); hum_sensors.clear(); sensor_signature = 0;

    // ====== Check available heap BEFORE creating endpoints ======
    uint32_t heap_before_endpoints = ESP.getFreeHeap();
    DEBUG_PRINTF("Matter: Free heap before endpoint creation: %d bytes (%d KB)\n", 
                 heap_before_endpoints, heap_before_endpoints/1024);
    
    // MEMORY-SAFE: Require minimum heap before proceeding
    // Matter.begin() needs ~50-80KB for mbedTLS, CHIP stack, etc.
    // On ESP32-C5 with precompiled libs, mbedTLS uses INTERNAL RAM only!
    const uint32_t MIN_HEAP_FOR_MATTER = 80000; // 80KB minimum
    const uint32_t MAX_STATIONS_LOW_MEM = 4;    // Limit stations if low memory
    
    if (heap_before_endpoints < MIN_HEAP_FOR_MATTER) {
      DEBUG_PRINTF("Matter: INSUFFICIENT HEAP (%d KB < %d KB minimum)\n", 
                   heap_before_endpoints/1024, MIN_HEAP_FOR_MATTER/1024);
      DEBUG_PRINTLN("Matter: The precompiled Arduino framework uses internal RAM for mbedTLS");
      DEBUG_PRINTLN("Matter: WiFi/BLE/Zigbee already consumed most internal heap");
      DEBUG_PRINTLN("Matter: Cannot start Matter with current memory constraints");
      DEBUG_PRINTLN("Matter: Consider disabling WiFi-heavy features or using fewer stations");
      return;
    }
    
    uint8_t nstations = os.nstations;
    uint8_t max_matter_stations = nstations;
    
    // Limit stations if heap is tight (between 80-120KB)
    if (heap_before_endpoints < 120000 && nstations > MAX_STATIONS_LOW_MEM) {
      max_matter_stations = MAX_STATIONS_LOW_MEM;
      DEBUG_PRINTF("Matter: LOW MEMORY - limiting to %d stations (of %d total)\n", 
                   max_matter_stations, nstations);
    }
    
    DEBUG_PRINTF("Matter: Creating valve endpoints for enabled stations (max: %d of %d total)...\n", 
                 max_matter_stations, nstations);
    uint8_t created_count = 0;
    for (unsigned char sid = 0; sid < nstations && created_count < max_matter_stations; sid++) {
      unsigned char bid = sid >> 3; unsigned char sbit = sid & 0x07;
      bool is_disabled = (os.attrib_dis[bid] & (1 << sbit)) != 0;
      if (is_disabled) { DEBUG_PRINTF("Matter: Station %d is disabled, skipping\n", sid); continue; }
      stations[sid] = std::make_unique<MatterOnOffPlugin>();
      bool is_on = (os.station_bits[(sid>>3)] >> (sid&0x07)) & 1;
      if (stations[sid]->begin(is_on)) {
        stations[sid]->onChange([sid](bool value) {
          DEBUG_PRINTF("Matter: Station %d OnOff -> %s\n", sid, value ? "ON" : "OFF");
          if (value) OSMatter::instance().station_on(sid); else OSMatter::instance().station_off(sid);
          return true;
        });
        DEBUG_PRINTF("Matter: Created valve endpoint for station %d\n", sid);
        created_count++;
      }
    }
    DEBUG_PRINTF("Matter: %zu enabled station endpoints created, sensors will be discovered on first loop\n", stations.size());

    Matter.onEvent(event_callback);

#if !HAS_ESP_BT
    DEBUG_PRINTLN("Matter: BT/BLE support not available on this build; skipping Matter init");
    return;
#elif HAS_NIMBLE
    // ESP32-C5 uses NimBLE stack - Matter SDK handles BLE initialization
    DEBUG_PRINTLN("Matter: Using NimBLE stack for BLE commissioning");
#elif HAS_BLUEDROID
    // ESP32/ESP32-S3 with Bluedroid stack - manual BT controller init
    esp_bt_controller_status_t bt_status = esp_bt_controller_get_status();
    DEBUG_PRINTF("Matter: BT controller status before init: %d\n", bt_status);
    if (bt_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
      esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
      esp_err_t init_ret = esp_bt_controller_init(&bt_cfg);
      DEBUG_PRINTF("Matter: BT controller init ret=%d\n", init_ret);
      if (init_ret != ESP_OK && init_ret != ESP_ERR_INVALID_STATE) { DEBUG_PRINTF("Matter: BLE controller init failed (%d), skipping Matter init\n", init_ret); return; }
      bt_status = esp_bt_controller_get_status();
    }
    if (bt_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
      esp_err_t en_ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
      DEBUG_PRINTF("Matter: BT controller enable ret=%d\n", en_ret);
      if (en_ret != ESP_OK && en_ret != ESP_ERR_INVALID_STATE) { DEBUG_PRINTF("Matter: BLE controller enable failed (%d), skipping Matter init\n", en_ret); return; }
      bt_status = esp_bt_controller_get_status();
    }
    DEBUG_PRINTF("Matter: BT controller status after init/enable: %d\n", bt_status);
    if (bt_status != ESP_BT_CONTROLLER_STATUS_ENABLED) { DEBUG_PRINTF("Matter: BLE controller not enabled (status=%d), skipping Matter init\n", bt_status); return; }
#endif

    // ====== Pre-Matter memory status ======
    DEBUG_PRINTLN("Matter: ========== PRE-INIT MEMORY STATUS ==========");
    uint32_t heap_before = ESP.getFreeHeap();
    uint32_t heap_min_before = ESP.getMinFreeHeap();
    DEBUG_PRINTF("Matter: Free Heap: %d bytes (%d KB)\n", heap_before, heap_before/1024);
    DEBUG_PRINTF("Matter: Min Free Heap: %d bytes (%d KB)\n", heap_min_before, heap_min_before/1024);
    #if defined(BOARD_HAS_PSRAM)
    uint32_t psram_before = ESP.getFreePsram();
    DEBUG_PRINTF("Matter: Free PSRAM: %d bytes (%.2f MB)\n", psram_before, psram_before/1048576.0);
    #endif
    DEBUG_PRINTLN("Matter: ===============================================");
    
    DEBUG_PRINTLN("Matter: Calling Matter.begin()...");
    Matter.begin();
    
    // Check if Matter actually started - give it a moment
    delay(500);
    
    uint32_t heap_after = ESP.getFreeHeap();
    
    // ====== Post-Matter memory status ======
    DEBUG_PRINTLN("Matter: ========== POST-INIT MEMORY STATUS ==========");
    DEBUG_PRINTF("Matter: Free Heap: %d bytes (%d KB)\n", heap_after, heap_after/1024);
    DEBUG_PRINTF("Matter: Min Free Heap: %d bytes (%d KB)\n", ESP.getMinFreeHeap(), ESP.getMinFreeHeap()/1024);
    #if defined(BOARD_HAS_PSRAM)
    DEBUG_PRINTF("Matter: Free PSRAM: %d bytes (%.2f MB)\n", ESP.getFreePsram(), ESP.getFreePsram()/1048576.0);
    DEBUG_PRINTF("Matter: PSRAM used: %d bytes\n", psram_before - ESP.getFreePsram());
    #endif
    
    // If Matter failed to start, very little heap was consumed (no CHIP stack allocated)
    // A successful Matter init typically uses 50-100KB+ of heap
    uint32_t heap_used = (heap_before > heap_after) ? (heap_before - heap_after) : 0;
    DEBUG_PRINTF("Matter: Heap used by Matter.begin(): %d bytes (%d KB)\n", heap_used, heap_used/1024);
    DEBUG_PRINTLN("Matter: ===============================================");
    
    // If less than 10KB used, Matter likely failed to initialize the CHIP stack
    if (heap_used < 10000) {
      DEBUG_PRINTLN("Matter: CHIP stack initialization likely failed (low memory usage)");
      DEBUG_PRINTLN("Matter: This usually means mbedTLS or entropy init failed");
      DEBUG_PRINTLN("Matter: Disabling Matter integration for this session");
      stations.clear();
      return;
    }
    
    // Verify that at least one station endpoint is functional
    // If Matter.begin() failed internally, the endpoints won't work
    if (stations.empty()) {
      DEBUG_PRINTLN("Matter: No station endpoints available");
      return;
    }
    
    // Try to get QR code - if this returns empty, Matter probably failed
    String qr_url = Matter.getOnboardingQRCodeUrl();
    String manual_code = Matter.getManualPairingCode();
    if (qr_url.length() == 0 || manual_code.length() == 0) {
      DEBUG_PRINTLN("Matter: Failed to initialize - no pairing codes available");
      stations.clear();
      return;
    }
    
    if (!Matter.isDeviceCommissioned()) {
      DEBUG_PRINTLN("Matter: Device not commissioned");
      DEBUG_PRINTF("Matter: QR Code URL: %s\n", qr_url.c_str());
      DEBUG_PRINTF("Matter: Manual Code: %s\n", Matter.getManualPairingCode().c_str());
    } else {
      commissioned = true;
      DEBUG_PRINTLN("Matter: Device already commissioned");
      for (auto& entry : stations) { if (entry.second) entry.second->updateAccessory(); }
    }

    initialized = true;
    DEBUG_PRINTLN("Matter: Init complete");
  }

  void loop() {
    if (!initialized) return;
    
    // Safety check: verify Matter stack is still functional
    // If heap is critically low, skip processing to avoid crashes
    if (ESP.getFreeHeap() < 20000) {
      static bool heap_warning_shown = false;
      if (!heap_warning_shown) {
        DEBUG_PRINTLN("Matter: Low heap warning - skipping sensor processing");
        heap_warning_shown = true;
      }
      return;
    }
    
    static bool sensors_initialized = false;
    if (!sensors_initialized) {
      sensors_initialized = true;
      
      // Extra safety: verify Matter is still initialized and stations are valid
      if (!initialized || stations.empty()) {
        DEBUG_PRINTLN("Matter: Not properly initialized - skipping sensor discovery");
        return;
      }
      
      // Additional safety: check if any station pointer is actually valid
      bool has_valid_station = false;
      for (const auto& entry : stations) {
        if (entry.second && entry.second.get() != nullptr) {
          has_valid_station = true;
          break;
        }
      }
      if (!has_valid_station) {
        DEBUG_PRINTLN("Matter: No valid station endpoints - skipping sensor discovery");
        return;
      }
      
      SensorIterator it; SensorBase* sensor;
      DEBUG_PRINTLN("Matter: Discovering sensors...");
      while ((sensor = sensors_iterate_next(it)) != nullptr) {
        if (!sensor || sensor->type == SENSOR_NONE) continue;
        switch(sensor->type) {
          case SENSOR_SMT100_TEMP: case SENSOR_SMT50_TEMP: case SENSOR_SMT100_ANALOG_TEMP:
          case SENSOR_OSPI_ANALOG_SMT50_TEMP: case SENSOR_INTERNAL_TEMP: case SENSOR_TH100_TEMP:
          case SENSOR_THERM200: case SENSOR_FYTA_TEMPERATURE: case SENSOR_WEATHER_TEMP_C:
          case SENSOR_WEATHER_TEMP_F: {
            MatterTemperatureSensor temp_sensor; if (temp_sensor.begin()) { temp_sensors.push_back(temp_sensor); DEBUG_PRINTF("Matter: Temp sensor %d -> endpoint %zu\n", sensor->nr, temp_sensors.size()-1); }
            break; }
          case SENSOR_TH100_MOIS: case SENSOR_WEATHER_HUM: {
            MatterHumiditySensor hum_sensor; if (hum_sensor.begin()) { hum_sensors.push_back(hum_sensor); DEBUG_PRINTF("Matter: Humidity sensor %d -> endpoint %zu\n", sensor->nr, hum_sensors.size()-1); }
            break; }
          default: break;
        }
      }
      DEBUG_PRINTF("Matter: Found %zu temp, %zu humidity sensors\n", temp_sensors.size(), hum_sensors.size());
    }

    SensorProfile current_profile = compute_sensor_profile();
    uint8_t enabled_station_count = 0;
    for (unsigned char sid = 0; sid < os.nstations; sid++) {
      unsigned char bid = sid >> 3; unsigned char sbit = sid & 0x07;
      if ((os.attrib_dis[bid] & (1 << sbit)) == 0) enabled_station_count++;
    }
    bool stations_changed = (enabled_station_count != stations.size());
    bool sensors_changed = (current_profile.signature != sensor_signature) ||
                           ((uint8_t)current_profile.temp_count != temp_sensors.size()) ||
                           ((uint8_t)current_profile.hum_count != hum_sensors.size());
    if (stations_changed || sensors_changed) {
      DEBUG_PRINTLN("Matter: Topology changed, reinitializing endpoints...");
      sensors_initialized = false;
      shutdown();
      init();
      return;
    }

    static unsigned long last_status_check = 0;
    if (millis() - last_status_check > 60000) {
      if (!commissioned && Matter.isDeviceCommissioned()) { commissioned = true; DEBUG_PRINTLN("Matter: Device commissioned (detected in loop)"); }
      last_status_check = millis();
    }
  }

  void shutdown() {
    if (!initialized) return;
    DEBUG_PRINTLN("Matter: Shutting down...");
    initialized = false; commissioned = false; sensor_signature = 0;
    stations.clear(); temp_sensors.clear(); hum_sensors.clear();
    DEBUG_PRINTLN("Matter: Shutdown complete");
  }

  void station_on(unsigned char sid) {
    DEBUG_PRINTF("Matter: Station %d ON requested\n", sid);
    if (sid >= os.nstations) return;
    if ((os.status.mas == sid+1) || (os.status.mas2 == sid+1)) return;
    unsigned char sqi = pd.station_qid[sid]; if (sqi != 0xFF) return;
    RuntimeQueueStruct *q = pd.enqueue(); if (!q) return;
    q->st = 0; q->dur = 60; q->sid = sid; q->pid = 99; schedule_all_stations(os.now_tz());
  }

  void station_off(unsigned char sid) {
    if (sid >= os.nstations) return;
    unsigned char ssta = 0; RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
    q->deque_time = os.now_tz(); turn_off_station(sid, os.now_tz(), ssta);
  }
};

// Public OSMatter interface implementation
OSMatter& OSMatter::instance() {
  static OSMatter inst;
  return inst;
}

void OSMatter::init() { OSMatterImpl::instance().init(); }
void OSMatter::loop() { OSMatterImpl::instance().loop(); }
void OSMatter::shutdown() { OSMatterImpl::instance().shutdown(); }
void OSMatter::station_on(unsigned char sid) { OSMatterImpl::instance().station_on(sid); }
void OSMatter::station_off(unsigned char sid) { OSMatterImpl::instance().station_off(sid); }
void OSMatter::update_station_status(unsigned char sid, bool on) { (void)sid; (void)on; }
void OSMatter::update_flow_rate(float gpm) { (void)gpm; }
void OSMatter::update_sensor_value(unsigned char sensor_id, float value) { (void)sensor_id; (void)value; }
bool OSMatter::is_commissioned() const { return OSMatterImpl::instance().commissioned; }
uint8_t OSMatter::get_fabric_count() const { return 0; }
void OSMatter::factory_reset() {}

// Backward-compatible free-function wrappers
void matter_init() { OSMatter::instance().init(); }
void matter_loop() { OSMatter::instance().loop(); }
void matter_shutdown() { OSMatter::instance().shutdown(); }
void matter_station_on(unsigned char sid) { OSMatter::instance().station_on(sid); }
void matter_station_off(unsigned char sid) { OSMatter::instance().station_off(sid); }
void matter_update_station_status(unsigned char sid, bool on) { OSMatter::instance().update_station_status(sid, on); }
void matter_update_flow_rate(float gpm) { OSMatter::instance().update_flow_rate(gpm); }
void matter_update_sensor_value(unsigned char sensor_id, float value) { OSMatter::instance().update_sensor_value(sensor_id, value); }
bool matter_is_commissioned() { return OSMatter::instance().is_commissioned(); }
uint8_t matter_get_fabric_count() { return OSMatter::instance().get_fabric_count(); }
void matter_factory_reset() { OSMatter::instance().factory_reset(); }

#endif // ENABLE_MATTER
