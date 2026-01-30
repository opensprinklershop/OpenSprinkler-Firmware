/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * Matter Protocol Integration - Simplified Stateless Design
 * Supports HomeKit, Google Home, Alexa via Matter standard
 */

#ifdef ENABLE_MATTER

#include "OpenSprinkler.h"
#include "main.h"
#include "sensors.h"
#include "SensorBase.hpp"
#include "sensor_scheduler.h"
#include "opensprinkler_matter.h"

#if defined(ESP32)
#include <Matter.h>
#ifdef OS_ENABLE_BLE
#include "sensor_ble.h"
#endif
#include <unordered_map>
#include <MatterEndpoints/MatterOnOffPlugin.h>
#include <MatterEndpoints/MatterTemperatureSensor.h>
#include <MatterEndpoints/MatterHumiditySensor.h>
#include <MatterEndpoints/MatterPressureSensor.h>
#include <esp_heap_caps.h>

// Matter event defines
#define MATTER_EVENT_COMMISSIONED 0x01
#define MATTER_EVENT_DECOMMISSIONED 0x02

// ====== PSRAM Allocator for STL Containers ======
template<typename T>
struct PSRAMAllocator {
  using value_type = T;
  
  PSRAMAllocator() = default;
  template<typename U> PSRAMAllocator(const PSRAMAllocator<U>&) {}
  
  T* allocate(std::size_t n) {
    if(auto p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM))) {
      return p;
    }
    throw std::bad_alloc();
  }
  
  void deallocate(T* p, std::size_t) { heap_caps_free(p); }
};

template<typename T, typename U>
bool operator==(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return true; }

template<typename T, typename U>
bool operator!=(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return false; }

extern OpenSprinkler os;

// ====== State Storage (Module-level with PSRAM) ======
namespace {
  // Type aliases for PSRAM-backed maps
  using StationMap = std::unordered_map<uint8_t, std::unique_ptr<MatterOnOffPlugin>,
    std::hash<uint8_t>, std::equal_to<uint8_t>,
    PSRAMAllocator<std::pair<const uint8_t, std::unique_ptr<MatterOnOffPlugin>>>>;
  
  using TempSensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterTemperatureSensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterTemperatureSensor>>>>;
  
  using HumiditySensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterHumiditySensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterHumiditySensor>>>>;
  
  using PressureSensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterPressureSensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterPressureSensor>>>>;
  
  EXT_RAM_BSS_ATTR StationMap stations;
  EXT_RAM_BSS_ATTR TempSensorMap temp_sensors;
  EXT_RAM_BSS_ATTR HumiditySensorMap humidity_sensors;
  EXT_RAM_BSS_ATTR PressureSensorMap pressure_sensors;
  
  EXT_RAM_BSS_ATTR bool matter_started = false;
  EXT_RAM_BSS_ATTR bool commissioned = false;
  EXT_RAM_BSS_ATTR bool matter_ble_lock_held = false;
  EXT_RAM_BSS_ATTR bool ble_init_pending = false;
  EXT_RAM_BSS_ATTR uint32_t ble_init_at = 0;
  
  // Matter QR code and pairing information
  EXT_RAM_BSS_ATTR String qr_code_url = "";
  EXT_RAM_BSS_ATTR String manual_pairing_code = "";
  EXT_RAM_BSS_ATTR uint32_t config_signature = 0;
}

// ====== Helper Functions ======
inline int16_t celsius_to_matter(float c) { return (int16_t)(c * 100.0f); }
inline uint16_t percent_to_matter(float p) { return (uint16_t)(p * 100.0f); }
inline int16_t pressure_to_matter(float hPa) { return (int16_t)(hPa * 10.0f); }

uint32_t compute_config_signature() {
  uint32_t sig = os.nstations;
  for(uint8_t i = 0; i < os.nstations; i++) {
    uint8_t bid = i >> 3, sbit = i & 0x07;
    sig = sig * 31 + ((os.attrib_dis[bid] >> sbit) & 1);
  }
  const SensorScheduleMap& sensors = sensor_get_all_metadata();
  for(const auto& entry : sensors) {
    const SensorMetadata& m = entry.second;
    if(m.isEnabled()) sig = sig * 31 + ((uint32_t)m.type << 16) + m.nr;
  }
  return sig;
}

uint16_t sensor_key(uint8_t type, uint8_t nr) {
  return ((uint16_t)type << 8) | nr;
}

void matter_event_handler(matterEvent_t event, const chip::DeviceLayer::ChipDeviceEvent* data) {
  (void)data;
  switch(event) {
    case MATTER_EVENT_COMMISSIONED:
      commissioned = true;
      DEBUG_PRINTLN("[Matter] Device commissioned");
      break;
    case MATTER_COMMISSIONING_SESSION_STARTED:
    case MATTER_CHIPOBLE_CONNECTION_ESTABLISHED:
      DEBUG_PRINTLN("[Matter] CHIPoBLE active - acquiring BLE semaphore");
      #ifdef OS_ENABLE_BLE
      {
        // Stop any ongoing sensor BLE activity first
        sensor_ble_stop();

        // Acquire BLE controller for Matter (CHIPoBLE needs exclusive access)
        if (sensor_ble_acquire(200)) {
          matter_ble_lock_held = true;
          DEBUG_PRINTLN("[Matter] BLE semaphore acquired for CHIPoBLE");
        } else {
          DEBUG_PRINTLN("[Matter] TIMEOUT: Could not acquire BLE semaphore!");
        }
      }
      #endif
      break;
    case MATTER_COMMISSIONING_COMPLETE:
      DEBUG_PRINTLN("[Matter] Commissioning complete");
      #ifdef OS_ENABLE_BLE
      {
        // Release BLE semaphore after commissioning completes
        if (matter_ble_lock_held) {
          sensor_ble_release();
          matter_ble_lock_held = false;
          DEBUG_PRINTLN("[Matter] BLE semaphore released after commissioning");
        }
      }
      #endif
      break;
    case MATTER_COMMISSIONING_SESSION_STOPPED:
    case MATTER_COMMISSIONING_WINDOW_CLOSED:
      DEBUG_PRINTLN("[Matter] Commissioning session/window closed");
      #ifdef OS_ENABLE_BLE
      {
        // Release BLE semaphore
        if (matter_ble_lock_held) {
          sensor_ble_release();
          matter_ble_lock_held = false;
          DEBUG_PRINTLN("[Matter] BLE semaphore released");
        }
      }
      #endif
      break;
    case MATTER_EVENT_DECOMMISSIONED:
      commissioned = false;
      DEBUG_PRINTLN("[Matter] Device decommissioned");
      break;
    case MATTER_CHIPOBLE_CONNECTION_CLOSED:
      DEBUG_PRINTLN("[Matter] CHIPoBLE connection closed - releasing BLE semaphore");
      // BLE kann jetzt für Sensoren verwendet werden
      #ifdef OS_ENABLE_BLE
      // Release the BLE semaphore so sensors can use it
      if (matter_ble_lock_held) {
        sensor_ble_release();
        matter_ble_lock_held = false;
        DEBUG_PRINTLN("[Matter] BLE semaphore released - sensors can now use BLE");
      }
      #endif
      break;
    default:
      break;
  }
}

// ====== Station Management ======
void create_station_endpoints() {
  DEBUG_PRINTF("[Matter] Creating endpoints for %d stations\n", os.nstations);
  
  for(uint8_t sid = 0; sid < os.nstations; sid++) {
    uint8_t bid = sid >> 3, sbit = sid & 0x07;
    bool disabled = (os.attrib_dis[bid] & (1 << sbit)) != 0;
    if(disabled) continue;
    
    // Allocate endpoint in PSRAM
    void* mem = heap_caps_malloc(sizeof(MatterOnOffPlugin), MALLOC_CAP_SPIRAM);
    if(!mem) {
      DEBUG_PRINTLN("[Matter] PSRAM allocation failed for station");
      continue;
    }
    stations[sid] = std::unique_ptr<MatterOnOffPlugin>(new(mem) MatterOnOffPlugin());
    bool is_on = (os.station_bits[bid] >> sbit) & 1;
    
    if(stations[sid]->begin(is_on)) {
      stations[sid]->onChange([sid](bool value) {
        DEBUG_PRINTF("[Matter] Station %d -> %s\n", sid, value ? "ON" : "OFF");
        if(value) OSMatter::instance().station_on(sid);
        else OSMatter::instance().station_off(sid);
        return true;
      });
    }
  }
  DEBUG_PRINTF("[Matter] %zu station endpoints created\n", stations.size());
}

// ====== Sensor Management ======
void create_sensor_endpoints() {
  const SensorScheduleMap& metadata = sensor_get_all_metadata();
  DEBUG_PRINTF("[Matter] Discovering %zu sensors\n", metadata.size());
  
  for(const auto& entry : metadata) {
    const SensorMetadata& m = entry.second;
    if(!m.isEnabled()) continue;
    uint16_t key = sensor_key(m.type, m.nr);
    
    switch(m.type) {
      // Temperature sensors
      case SENSOR_SMT100_TEMP:
      case SENSOR_SMT50_TEMP:
      case SENSOR_SMT100_ANALOG_TEMP:
      case SENSOR_OSPI_ANALOG_SMT50_TEMP:
      case SENSOR_INTERNAL_TEMP:
      case SENSOR_TH100_TEMP:
      case SENSOR_THERM200:
      case SENSOR_FYTA_TEMPERATURE:
      case SENSOR_WEATHER_TEMP_C:
      case SENSOR_WEATHER_TEMP_F: {
        void* mem = heap_caps_malloc(sizeof(MatterTemperatureSensor), MALLOC_CAP_SPIRAM);
        if(!mem) break;
        temp_sensors[key] = std::unique_ptr<MatterTemperatureSensor>(
          new(mem) MatterTemperatureSensor());
        if(temp_sensors[key]->begin()) {
          DEBUG_PRINTF("[Matter] Temp sensor %d.%d\n", m.type, m.nr);
        }
        break;
      }
      
      // Humidity sensors
      case SENSOR_TH100_MOIS:
      case SENSOR_WEATHER_HUM: {
        void* mem = heap_caps_malloc(sizeof(MatterHumiditySensor), MALLOC_CAP_SPIRAM);
        if(!mem) break;
        humidity_sensors[key] = std::unique_ptr<MatterHumiditySensor>(
          new(mem) MatterHumiditySensor());
        if(humidity_sensors[key]->begin()) {
          DEBUG_PRINTF("[Matter] Humidity sensor %d.%d\n", m.type, m.nr);
        }
        break;
      }
      
      // Precipitation (mapped to pressure)
      case SENSOR_WEATHER_PRECIP_MM:
      case SENSOR_WEATHER_PRECIP_IN: {
        void* mem = heap_caps_malloc(sizeof(MatterPressureSensor), MALLOC_CAP_SPIRAM);
        if(!mem) break;
        pressure_sensors[key] = std::unique_ptr<MatterPressureSensor>(
          new(mem) MatterPressureSensor());
        if(pressure_sensors[key]->begin()) {
          DEBUG_PRINTF("[Matter] Precip sensor %d.%d\n", m.type, m.nr);
        }
        break;
      }
    }
  }
  
  DEBUG_PRINTF("[Matter] Created %zu temp, %zu humidity, %zu pressure\n",
               temp_sensors.size(), humidity_sensors.size(), pressure_sensors.size());
}

void update_sensor_values() {
  const SensorScheduleMap& metadata = sensor_get_all_metadata();
  
  for(const auto& entry : metadata) {
    const SensorMetadata& m = entry.second;
    if(!m.isEnabled()) continue;
    
    uint16_t key = sensor_key(m.type, m.nr);
    
    // Use cached sensor value (lazy-loaded by scheduler)
    double value = sensor_get_cached_value(m.nr);
    
    switch(m.type) {
      case SENSOR_SMT100_TEMP:
      case SENSOR_SMT50_TEMP:
      case SENSOR_SMT100_ANALOG_TEMP:
      case SENSOR_OSPI_ANALOG_SMT50_TEMP:
      case SENSOR_INTERNAL_TEMP:
      case SENSOR_TH100_TEMP:
      case SENSOR_THERM200:
      case SENSOR_FYTA_TEMPERATURE:
      case SENSOR_WEATHER_TEMP_C:
      case SENSOR_WEATHER_TEMP_F: {
        auto it = temp_sensors.find(key);
        if(it != temp_sensors.end() && it->second) {
          it->second->setTemperature(value);
        }
        break;
      }
      
      case SENSOR_TH100_MOIS:
      case SENSOR_WEATHER_HUM: {
        auto it = humidity_sensors.find(key);
        if(it != humidity_sensors.end() && it->second) {
          it->second->setHumidity(value);
        }
        break;
      }
      
      case SENSOR_WEATHER_PRECIP_MM:
      case SENSOR_WEATHER_PRECIP_IN: {
        auto it = pressure_sensors.find(key);
        if(it != pressure_sensors.end() && it->second) {
          it->second->setPressure(value);
        }
        break;
      }
    }
  }
}

// ====== Public API ======
void OSMatter::init() {
  if(matter_started) {
    DEBUG_PRINTLN("[Matter] Already initialized");
    return;
  }
  
  DEBUG_PRINTLN("[Matter] Initializing...");
  
  #if defined(BOARD_HAS_PSRAM)
  DEBUG_PRINTF("[Matter] Pre-init: Heap %d KB, PSRAM %.2f MB\n",
               ESP.getFreeHeap()/1024, ESP.getFreePsram()/1048576.0);
  
  // Pre-allocate internal RAM for crypto operations
  // Hardware AES requires DMA-capable internal memory
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  DEBUG_PRINTF("[Matter] Internal heap before crypto reserve: %d bytes\n", internal_free);
  
  // If internal heap is below threshold, try to free up space
  if(internal_free < 50000) {
    DEBUG_PRINTLN("[Matter] WARNING: Low internal heap - crypto may fail!");
    // Try to allocate and immediately free to defragment
    void* defrag_block = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(defrag_block) {
      heap_caps_free(defrag_block);
    }
  }
  #endif
  
  create_station_endpoints();
  create_sensor_endpoints();
  Matter.onEvent(matter_event_handler);
  
  DEBUG_PRINTLN("[Matter] Starting Matter.begin()...");
  Matter.begin();
  delay(100);
  
  #if defined(BOARD_HAS_PSRAM)
  DEBUG_PRINTF("[Matter] Post-init: Heap %d KB, PSRAM %.2f MB\n",
               ESP.getFreeHeap()/1024, ESP.getFreePsram()/1048576.0);
  #endif
  
  if(!Matter.isDeviceCommissioned()) {
    qr_code_url = Matter.getOnboardingQRCodeUrl();
    manual_pairing_code = Matter.getManualPairingCode();
    DEBUG_PRINTLN("[Matter] QR: " + qr_code_url);
    DEBUG_PRINTLN("[Matter] Code: " + manual_pairing_code);
  } else {
    commissioned = true;
    DEBUG_PRINTLN("[Matter] Already commissioned");
    #ifdef OS_ENABLE_BLE
    // Schedule BLE init after Matter settles (1 second delay)
    ble_init_pending = true;
    ble_init_at = millis() + 1000;
    DEBUG_PRINTLN("[Matter] BLE init scheduled (commissioned device)");
    #endif
  }

  #ifdef OS_ENABLE_BLE
  // If BLE commissioning is not enabled, schedule BLE init
  if (!Matter.isBLECommissioningEnabled()) {
    ble_init_pending = true;
    ble_init_at = millis() + 1000;
    DEBUG_PRINTLN("[Matter] BLE commissioning disabled - BLE init scheduled");
  }
  #endif

  #ifdef OS_ENABLE_BLE
  // Default: BLE will be managed via event system
  // Do NOT init here - Matter may still be using BLE controller
  DEBUG_PRINTLN("[Matter] BLE managed via event system");
  #endif
  
  config_signature = compute_config_signature();
  matter_started = true;
  DEBUG_PRINTLN("[Matter] Init complete");
}

void OSMatter::loop() {
  if(!matter_started) return;
  
  #ifdef OS_ENABLE_BLE
  // Process deferred BLE init
  if (ble_init_pending && millis() >= ble_init_at) {
    ble_init_pending = false;
    DEBUG_PRINTLN("[Matter] Initializing BLE (deferred)");
    sensor_ble_init();
  }
  #endif
  
  // Check config changes
  uint32_t current_sig = compute_config_signature();
  if(current_sig != config_signature) {
    DEBUG_PRINTLN("[Matter] Config changed - reinitializing");
    stations.clear();
    temp_sensors.clear();
    humidity_sensors.clear();
    pressure_sensors.clear();
    create_station_endpoints();
    create_sensor_endpoints();
    config_signature = current_sig;
  }
  
  // Update sensors every 10s
  static ulong last_update = 0;
  if(millis() - last_update > 10000) {
    update_sensor_values();
    last_update = millis();
  }
}

void OSMatter::update_station(uint8_t sid, bool is_on) {
  auto it = stations.find(sid);
  if(it != stations.end() && it->second) {
    it->second->setOnOff(is_on);
  }
}

bool OSMatter::is_commissioned() {
  return commissioned;
}

String OSMatter::get_qr_code_url() {
  return qr_code_url;
}

String OSMatter::get_manual_pairing_code() {
  return manual_pairing_code;
}

void OSMatter::station_on(unsigned char sid) {
  if(sid >= os.nstations) return;
  os.set_station_bit(sid, 1);
}

void OSMatter::station_off(unsigned char sid) {
  if(sid >= os.nstations) return;
  os.set_station_bit(sid, 0);
}

OSMatter& OSMatter::instance() {
  static OSMatter inst;
  return inst;
}

#endif // ESP32
#endif // ENABLE_MATTER
