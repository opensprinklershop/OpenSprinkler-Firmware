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
#include "opensprinkler_matter.h"
#include "ieee802154_config.h"

#if defined(ESP32)
#include <Matter.h>
#include <app/server/Server.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <new>
#include <WiFi.h>
#include <esp_wifi.h>
#ifdef OS_ENABLE_BLE
#include "sensor_ble.h"
#endif
#include <unordered_map>
#include <MatterEndpoints/MatterOnOffLight.h>
#include <MatterEndpoints/MatterTemperatureSensor.h>
#include <MatterEndpoints/MatterHumiditySensor.h>
#include <MatterEndpoints/MatterPressureSensor.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>

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
extern ProgramData pd;

// ====== State Storage (Module-level with PSRAM) ======
namespace {
#if !defined(CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT)
#define CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT 16
#endif

  // The esp-matter dynamic endpoint limit is reached one endpoint earlier in this
  // composition because the framework also consumes internal endpoint capacity.
  constexpr int kMaxDynamicEndpoints = CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT - 1;

  // Type aliases for PSRAM-backed maps
  using StationMap = std::unordered_map<uint8_t, std::unique_ptr<MatterOnOffLight>,
    std::hash<uint8_t>, std::equal_to<uint8_t>,
    PSRAMAllocator<std::pair<const uint8_t, std::unique_ptr<MatterOnOffLight>>>>;
  
  using ProgramMap = std::unordered_map<uint8_t, std::unique_ptr<MatterOnOffLight>,
    std::hash<uint8_t>, std::equal_to<uint8_t>,
    PSRAMAllocator<std::pair<const uint8_t, std::unique_ptr<MatterOnOffLight>>>>;

  using TempSensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterTemperatureSensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterTemperatureSensor>>>>;
  
  using HumiditySensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterHumiditySensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterHumiditySensor>>>>;
  
  using PressureSensorMap = std::unordered_map<uint16_t, std::unique_ptr<MatterPressureSensor>,
    std::hash<uint16_t>, std::equal_to<uint16_t>,
    PSRAMAllocator<std::pair<const uint16_t, std::unique_ptr<MatterPressureSensor>>>>;
  
  // NOTE: std::unordered_map cannot use EXT_RAM_BSS_ATTR - has internal hash table pointers
  // The PSRAMAllocator handles dynamic allocations in PSRAM
  StationMap stations;
  ProgramMap programs;
  TempSensorMap temp_sensors;
  HumiditySensorMap humidity_sensors;
  PressureSensorMap pressure_sensors;

  bool wifi_sync_pending = false;
  uint32_t matter_init_time_ms = 0;

  bool matter_sync_wifi_config() {
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }

    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
      return false;
    }

    const char* ssid = reinterpret_cast<const char*>(conf.sta.ssid);
    const char* pass = reinterpret_cast<const char*>(conf.sta.password);
    if (!ssid || !ssid[0]) {
      return false;
    }

    os.wifi_ssid = ssid;
    os.wifi_pass = pass ? pass : "";

    os.sopt_save(SOPT_STA_SSID, os.wifi_ssid.c_str());
    os.sopt_save(SOPT_STA_PASS, os.wifi_pass.c_str());

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      char bssid_chl[MAX_SOPTS_SIZE];
      snprintf(bssid_chl, sizeof(bssid_chl),
               "%02x:%02x:%02x:%02x:%02x:%02x@%u",
               ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
               ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5],
               ap_info.primary);
      os.sopt_save(SOPT_STA_BSSID_CHL, bssid_chl);
      memcpy(os.wifi_bssid, ap_info.bssid, 6);
      os.wifi_channel = ap_info.primary;
    }

    if (os.iopts[IOPT_WIFI_MODE] != WIFI_MODE_STA) {
      os.iopts[IOPT_WIFI_MODE] = WIFI_MODE_STA;
      os.iopts_save();
    }

    DEBUG_PRINTLN("[Matter] WiFi credentials stored to OpenSprinkler config");
    return true;
  }
  
  // ── Deferred command queue (Matter callbacks → main loop) ──────────────
  // onChange callbacks run in the Matter/CHIP stack task. pd/os state must
  // only be mutated from the Arduino main loop. Commands are enqueued here
  // and drained by process_matter_cmds() from OSMatter::loop().
  enum MatterCmdType : uint8_t {
    MATTER_CMD_STATION_ON,
    MATTER_CMD_STATION_OFF,
    MATTER_CMD_PROG_START,
    MATTER_CMD_PROG_STOP,
  };
  struct MatterCmd {
    MatterCmdType type;
    union {
      struct { uint8_t sid; } station;
      struct { uint8_t pid; } prog;
    };
  };
  #define MATTER_CMD_QUEUE_LEN 16
  static QueueHandle_t matter_cmd_queue = nullptr;

  bool matter_started = false;
  bool commissioned = false;
  bool matter_ble_lock_held = false;
  bool ble_init_pending = false;
  uint32_t ble_init_at = 0;
  
  // Matter QR code and pairing information
  // NOTE: Arduino String cannot use EXT_RAM_BSS_ATTR - has internal buffer pointer
  String qr_code_url = "";
  String manual_pairing_code = "";
  uint32_t config_signature = 0;
  String last_node_label = "";
  bool node_label_writable = true;

  bool matter_sync_node_label(bool force = false) {
    if (!matter_started || !node_label_writable) {
      return false;
    }

    String desired = os.sopt_load(SOPT_DEVICE_NAME);
    if (desired.length() == 0) {
      desired = DEFAULT_DEVICE_NAME;
    }

    // Keep within Matter Basic Information NodeLabel max length.
    if (desired.length() > 32) {
      desired = desired.substring(0, 32);
    }

    if (!force && desired == last_node_label) {
      return true;
    }

    esp_matter::lock::status_t lock_status =
      esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
    if (lock_status == esp_matter::lock::FAILED) {
      DEBUG_PRINTLN("[Matter] NodeLabel update skipped: CHIP lock timeout");
      return false;
    }

    chip::Protocols::InteractionModel::Status status =
      chip::app::Clusters::BasicInformation::Attributes::NodeLabel::Set(
        0, chip::CharSpan::fromCharString(desired.c_str()));

    if (lock_status == esp_matter::lock::SUCCESS) {
      esp_matter::lock::chip_stack_unlock();
    }

    if (status == chip::Protocols::InteractionModel::Status::Success) {
      last_node_label = desired;
      DEBUG_PRINTLN("[Matter] NodeLabel set to: " + desired);
      return true;
    }

    node_label_writable = false;
    DEBUG_PRINTF("[Matter] NodeLabel is not writable on this endpoint (status=%d)\n", (int)status);
    return false;
  }
}

uint32_t matter_get_init_time_ms() {
  return matter_init_time_ms;
}

// ====== Helper Functions ======
inline int16_t celsius_to_matter(float c) { return (int16_t)(c * 100.0f); }
inline uint16_t percent_to_matter(float p) { return (uint16_t)(p * 100.0f); }
inline int16_t pressure_to_matter(float hPa) { return (int16_t)(hPa * 10.0f); }

uint32_t compute_config_signature() {
  uint32_t sig = os.nstations;
  sig = sig * 31 + pd.nprograms;
  for(uint8_t i = 0; i < os.nstations; i++) {
    uint8_t bid = i >> 3, sbit = i & 0x07;
    sig = sig * 31 + ((os.attrib_dis[bid] >> sbit) & 1);
  }
  SensorIterator it = sensors_iterate_begin();
  SensorBase* sensor = sensors_iterate_next(it);
  while (sensor) {
    if (sensor->flags.enable) {
      sig = sig * 31 + ((uint32_t)sensor->type << 16) + sensor->nr;
    }
    sensor = sensors_iterate_next(it);
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
      wifi_sync_pending = true;
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
      wifi_sync_pending = true;
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
void create_station_endpoints(int &budget) {
  DEBUG_PRINTF("[Matter] Creating endpoints for %d stations\n", os.nstations);
  
  for(uint8_t sid = 0; sid < os.nstations; sid++) {
    uint8_t bid = sid >> 3, sbit = sid & 0x07;
    bool disabled = (os.attrib_dis[bid] & (1 << sbit)) != 0;
    if(disabled) continue;
    
    if (budget <= 0) {
      DEBUG_PRINTLN("[Matter] Dynamic endpoint budget exhausted (stations)");
      break;
    }

    // Allocate endpoint in PSRAM
    void* mem = heap_caps_malloc(sizeof(MatterOnOffLight), MALLOC_CAP_SPIRAM);
    if(!mem) {
      DEBUG_PRINTLN("[Matter] PSRAM allocation failed for station");
      continue;
    }
    stations[sid] = std::unique_ptr<MatterOnOffLight>(new(mem) MatterOnOffLight());
    bool is_on = (os.station_bits[bid] >> sbit) & 1;
    
    if(stations[sid]->begin(is_on)) {
      budget--;
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

// ====== Program Management ======
void create_program_endpoints(int &budget) {
  DEBUG_PRINTF("[Matter] Creating program endpoints for %d programs\n", pd.nprograms);

  for (uint8_t pid = 0; pid < pd.nprograms; pid++) {
    if (budget <= 0) {
      DEBUG_PRINTLN("[Matter] Dynamic endpoint budget exhausted (programs)");
      break;
    }

    void *mem = heap_caps_malloc(sizeof(MatterOnOffLight), MALLOC_CAP_SPIRAM);
    if (!mem) {
      DEBUG_PRINTLN("[Matter] PSRAM allocation failed for program");
      continue;
    }
    programs[pid] = std::unique_ptr<MatterOnOffLight>(new(mem) MatterOnOffLight());
    if (programs[pid]->begin(false)) {
      budget--;
      programs[pid]->onChange([pid](bool value) {
        DEBUG_PRINTF("[Matter] Program %d -> %s (queued)\n", pid, value ? "START" : "STOP");
        MatterCmd cmd{};
        cmd.type = value ? MATTER_CMD_PROG_START : MATTER_CMD_PROG_STOP;
        cmd.prog.pid = pid;
        xQueueSend(matter_cmd_queue, &cmd, 0);
        return true;
      });
    }
  }
  DEBUG_PRINTF("[Matter] %zu program endpoints created\n", programs.size());
}

// ====== Sensor Management ======
void create_sensor_endpoints(int &budget) {
  size_t sensor_total = sensor_count();
  DEBUG_PRINTF("[Matter] Discovering %zu sensors\n", sensor_total);

  SensorIterator it = sensors_iterate_begin();
  SensorBase* sensor = sensors_iterate_next(it);
  while (sensor) {
    if (!sensor->flags.enable) {
      sensor = sensors_iterate_next(it);
      continue;
    }
    uint16_t key = sensor_key(sensor->type, sensor->nr);
    
    uint8_t unit = sensor->getUnitId();

    if (budget <= 0) {
      DEBUG_PRINTLN("[Matter] Dynamic endpoint budget exhausted (sensors)");
      break;
    }

    switch(sensor->type) {
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
          budget--;
          DEBUG_PRINTF("[Matter] Temp sensor %d.%d\n", sensor->type, sensor->nr);
        }
        break;
      }
      
      // Humidity sensors
      case SENSOR_SMT100_MOIS:
      case SENSOR_SMT50_MOIS:
      case SENSOR_SMT100_ANALOG_MOIS:
      case SENSOR_VH400:
      case SENSOR_FYTA_MOISTURE:
      case SENSOR_TH100_MOIS:
      case SENSOR_WEATHER_HUM: {
        void* mem = heap_caps_malloc(sizeof(MatterHumiditySensor), MALLOC_CAP_SPIRAM);
        if(!mem) break;
        humidity_sensors[key] = std::unique_ptr<MatterHumiditySensor>(
          new(mem) MatterHumiditySensor());
        if(humidity_sensors[key]->begin()) {
          budget--;
          DEBUG_PRINTF("[Matter] Humidity sensor %d.%d\n", sensor->type, sensor->nr);
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
          budget--;
          DEBUG_PRINTF("[Matter] Precip sensor %d.%d\n", sensor->type, sensor->nr);
        }
        break;
      }

      // Generic sensors (MQTT/Zigbee/BLE/Remote/Groups): map by unit where possible.
      case SENSOR_MQTT:
      case SENSOR_ZIGBEE:
      case SENSOR_BLE:
      case SENSOR_REMOTE:
      case SENSOR_GROUP_MIN:
      case SENSOR_GROUP_MAX:
      case SENSOR_GROUP_AVG:
      case SENSOR_GROUP_SUM: {
        if (unit == UNIT_DEGREE || unit == UNIT_FAHRENHEIT) {
          void* mem = heap_caps_malloc(sizeof(MatterTemperatureSensor), MALLOC_CAP_SPIRAM);
          if(!mem) break;
          temp_sensors[key] = std::unique_ptr<MatterTemperatureSensor>(
            new(mem) MatterTemperatureSensor());
          if(temp_sensors[key]->begin()) {
            budget--;
            DEBUG_PRINTF("[Matter] Generic temp sensor %d.%d\n", sensor->type, sensor->nr);
          }
        } else if (unit == UNIT_HUM_PERCENT || unit == UNIT_PERCENT) {
          void* mem = heap_caps_malloc(sizeof(MatterHumiditySensor), MALLOC_CAP_SPIRAM);
          if(!mem) break;
          humidity_sensors[key] = std::unique_ptr<MatterHumiditySensor>(
            new(mem) MatterHumiditySensor());
          if(humidity_sensors[key]->begin()) {
            budget--;
            DEBUG_PRINTF("[Matter] Generic humidity sensor %d.%d\n", sensor->type, sensor->nr);
          }
        } else if (unit == UNIT_MM || unit == UNIT_INCH) {
          void* mem = heap_caps_malloc(sizeof(MatterPressureSensor), MALLOC_CAP_SPIRAM);
          if(!mem) break;
          pressure_sensors[key] = std::unique_ptr<MatterPressureSensor>(
            new(mem) MatterPressureSensor());
          if(pressure_sensors[key]->begin()) {
            budget--;
            DEBUG_PRINTF("[Matter] Generic precip sensor %d.%d\n", sensor->type, sensor->nr);
          }
        }
        break;
      }
    }
    sensor = sensors_iterate_next(it);
  }
  
  DEBUG_PRINTF("[Matter] Created %zu temp, %zu humidity, %zu pressure\n",
               temp_sensors.size(), humidity_sensors.size(), pressure_sensors.size());
}

void update_sensor_values() {
  esp_matter::lock::status_t lock_status =
    esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
  if (lock_status == esp_matter::lock::FAILED) {
    DEBUG_PRINTLN("[Matter] Sensor update skipped: CHIP lock timeout");
    return;
  }

  SensorIterator it = sensors_iterate_begin();
  SensorBase* sensor = sensors_iterate_next(it);
  while (sensor) {
    if (!sensor->flags.enable || !sensor->flags.data_ok) {
      sensor = sensors_iterate_next(it);
      continue;
    }
    uint16_t key = sensor_key(sensor->type, sensor->nr);
    double value = sensor->last_data;

    // Generic sensors are routed by engineering unit.
    if (sensor->type == SENSOR_MQTT || sensor->type == SENSOR_ZIGBEE ||
        sensor->type == SENSOR_BLE || sensor->type == SENSOR_REMOTE ||
        sensor->type == SENSOR_GROUP_MIN || sensor->type == SENSOR_GROUP_MAX ||
        sensor->type == SENSOR_GROUP_AVG || sensor->type == SENSOR_GROUP_SUM) {
      uint8_t unit = sensor->getUnitId();
      if (unit == UNIT_DEGREE || unit == UNIT_FAHRENHEIT) {
        auto ts = temp_sensors.find(key);
        if (ts != temp_sensors.end() && ts->second) {
          ts->second->setTemperature(value);
        }
      } else if (unit == UNIT_HUM_PERCENT || unit == UNIT_PERCENT) {
        auto hs = humidity_sensors.find(key);
        if (hs != humidity_sensors.end() && hs->second) {
          hs->second->setHumidity(value);
        }
      } else if (unit == UNIT_MM || unit == UNIT_INCH) {
        auto ps = pressure_sensors.find(key);
        if (ps != pressure_sensors.end() && ps->second) {
          ps->second->setPressure(value);
        }
      }
      sensor = sensors_iterate_next(it);
      continue;
    }

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
      case SENSOR_WEATHER_TEMP_F: {
        auto it = temp_sensors.find(key);
        if(it != temp_sensors.end() && it->second) {
          it->second->setTemperature(value);
        }
        break;
      }
      
      case SENSOR_SMT100_MOIS:
      case SENSOR_SMT50_MOIS:
      case SENSOR_SMT100_ANALOG_MOIS:
      case SENSOR_VH400:
      case SENSOR_FYTA_MOISTURE:
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
    sensor = sensors_iterate_next(it);
  }

  if (lock_status == esp_matter::lock::SUCCESS) {
    esp_matter::lock::chip_stack_unlock();
  }
}

// ====== Public API ======
void OSMatter::init() {
  if(matter_started) {
    DEBUG_PRINTLN("[Matter] Already initialized");
    return;
  }

  // Create deferred command queue (if not already created)
  if (!matter_cmd_queue) {
    matter_cmd_queue = xQueueCreate(MATTER_CMD_QUEUE_LEN, sizeof(MatterCmd));
  }

  // Runtime check: only start Matter when IEEE 802.15.4 mode is MATTER
  if (!ieee802154_is_matter()) {
    DEBUG_PRINTLN("[Matter] Not in MATTER mode - Matter disabled");
    return;
  }

  if (matter_init_time_ms == 0) {
    matter_init_time_ms = millis();
  }
  
  DEBUG_PRINTLN("[Matter] Initializing...");
  
  #if defined(BOARD_HAS_PSRAM)
  DEBUG_PRINTF("[Matter] Pre-init: Heap %d KB, PSRAM %.2f MB\n",
               ESP.getFreeHeap()/1024, ESP.getFreePsram()/1048576.0);
  
  // Pre-allocate internal RAM for crypto operations
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  
  // If internal heap is below threshold, try to defragment
  if(internal_free < 50000) {
    DEBUG_PRINTLN("[Matter] WARNING: Low internal heap!");
    // Try to allocate and immediately free to defragment
    void* defrag_block = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(defrag_block) {
      heap_caps_free(defrag_block);
    }
  }
  #endif
  
  int endpoint_budget = kMaxDynamicEndpoints;
  create_station_endpoints(endpoint_budget);
  create_program_endpoints(endpoint_budget);
  create_sensor_endpoints(endpoint_budget);
  DEBUG_PRINTF("[Matter] Endpoint budget remaining: %d\n", endpoint_budget);
  Matter.onEvent(matter_event_handler);
  
  DEBUG_PRINTLN("[Matter] Starting Matter.begin()...");
  Matter.begin();
  
  // Give CHIP stack time to process initialization events and set up mDNS.
  // Multiple short yields are better than one long delay, as each yield()
  // allows the CHIP FreeRTOS task to process queued platform events.
  for (int i = 0; i < 5; i++) {
    delay(100);
    yield();
  }
  
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
  matter_sync_node_label(true);
  DEBUG_PRINTLN("[Matter] Init complete");
}

static void process_matter_cmds() {
  if (!matter_cmd_queue) return;
  MatterCmd cmd;
  while (xQueueReceive(matter_cmd_queue, &cmd, 0) == pdTRUE) {
    switch (cmd.type) {
      case MATTER_CMD_STATION_ON:
        if (cmd.station.sid < os.nstations)
          os.set_station_bit(cmd.station.sid, 1);
        break;
      case MATTER_CMD_STATION_OFF:
        if (cmd.station.sid < os.nstations)
          os.set_station_bit(cmd.station.sid, 0);
        break;
      case MATTER_CMD_PROG_START:
        manual_start_program(cmd.prog.pid + 1, 255, QUEUE_OPTION_INSERT_FRONT);
        break;
      case MATTER_CMD_PROG_STOP:
        for (unsigned char sid = 0; sid < os.nstations; sid++) {
          unsigned char qid = pd.station_qid[sid];
          if (qid < pd.nqueue && qpid_decode(pd.queue[qid].pid) == (uint8_t)(cmd.prog.pid + 1)) {
            turn_off_station(sid, os.now_tz(), 1);
          }
        }
        break;
    }
  }
}

void OSMatter::loop() {
  process_matter_cmds();
  if(!matter_started) return;

  // Prevent re-initialization during WiFi connection/scan (avoids PSRAM access conflicts)
  if(os.state == OS_STATE_CONNECTING) return;
  
  #ifdef OS_ENABLE_BLE
  // Process deferred BLE init
  if (ble_init_pending && millis() >= ble_init_at) {
    ble_init_pending = false;
    DEBUG_PRINTLN("[Matter] Initializing BLE (deferred)");
    sensor_ble_init();
  }
  #endif

  if (wifi_sync_pending) {
    if (matter_sync_wifi_config()) {
      wifi_sync_pending = false;
    }
  }
  
  // Check config changes
  uint32_t current_sig = compute_config_signature();
  if(current_sig != config_signature) {
    DEBUG_PRINTLN("[Matter] Config changed - reinitializing");
    stations.clear();
    programs.clear();
    temp_sensors.clear();
    humidity_sensors.clear();
    pressure_sensors.clear();
    int endpoint_budget = kMaxDynamicEndpoints;
    create_station_endpoints(endpoint_budget);
    create_program_endpoints(endpoint_budget);
    create_sensor_endpoints(endpoint_budget);
    DEBUG_PRINTF("[Matter] Endpoint budget remaining: %d\n", endpoint_budget);
    config_signature = current_sig;
  }
  
  // Periodic sync/update every 10s
  static ulong last_update = 0;
  if(millis() - last_update > 10000) {
    matter_sync_node_label();
    if (commissioned) {
      update_sensor_values();
    }
    last_update = millis();
  }
}

void OSMatter::update_station(uint8_t sid, bool is_on) {
  auto it = stations.find(sid);
  if(it != stations.end() && it->second) {
    esp_matter::lock::status_t lock_status =
      esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
    if (lock_status == esp_matter::lock::FAILED) {
      DEBUG_PRINTLN("[Matter] Station update skipped: CHIP lock timeout");
      return;
    }
    it->second->setOnOff(is_on);
    if (lock_status == esp_matter::lock::SUCCESS) {
      esp_matter::lock::chip_stack_unlock();
    }
  }
}

void OSMatter::update_program(uint8_t pid, bool running) {
  auto it = programs.find(pid);
  if(it != programs.end() && it->second) {
    esp_matter::lock::status_t lock_status =
      esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
    if (lock_status == esp_matter::lock::FAILED) {
      DEBUG_PRINTLN("[Matter] Program update skipped: CHIP lock timeout");
      return;
    }
    it->second->setOnOff(running);
    if (lock_status == esp_matter::lock::SUCCESS) {
      esp_matter::lock::chip_stack_unlock();
    }
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

bool OSMatter::open_commissioning_window(uint16_t timeout_seconds) {
  if (!matter_started) {
    DEBUG_PRINTLN("[Matter] Cannot open commissioning window: Matter not started");
    return false;
  }

  esp_matter::lock::status_t lock_status =
    esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
  if (lock_status == esp_matter::lock::FAILED) {
    DEBUG_PRINTLN("[Matter] Cannot open commissioning window: CHIP lock timeout");
    return false;
  }

  chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
  if (mgr.IsCommissioningWindowOpen()) {
    DEBUG_PRINTLN("[Matter] Commissioning window already open");
    if (lock_status == esp_matter::lock::SUCCESS) {
      esp_matter::lock::chip_stack_unlock();
    }
    return true;
  }
  constexpr auto kAdv = chip::CommissioningWindowAdvertisement::kDnssdOnly;
  CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(
      chip::System::Clock::Seconds16(timeout_seconds), kAdv);
  if (lock_status == esp_matter::lock::SUCCESS) {
    esp_matter::lock::chip_stack_unlock();
  }
  if (err != CHIP_NO_ERROR) {
    DEBUG_PRINTF("[Matter] OpenBasicCommissioningWindow failed: %s\n", err.AsString());
    return false;
  }
  DEBUG_PRINTF("[Matter] Commissioning window opened (%u s)\n", timeout_seconds);
  return true;
}

bool OSMatter::remove_commissioning() {
  if (!matter_started) {
    DEBUG_PRINTLN("[Matter] Cannot remove commissioning: Matter not started");
    return false;
  }

  esp_matter::lock::status_t lock_status =
    esp_matter::lock::chip_stack_lock(pdMS_TO_TICKS(100));
  if (lock_status == esp_matter::lock::FAILED) {
    DEBUG_PRINTLN("[Matter] Cannot remove commissioning: CHIP lock timeout");
    return false;
  }

  chip::FabricTable &fabrics = chip::Server::GetInstance().GetFabricTable();
  size_t fabric_count = fabrics.FabricCount();
  if (fabric_count > 0) {
    fabrics.DeleteAllFabrics();
  }
  commissioned = (fabrics.FabricCount() > 0);

  if (lock_status == esp_matter::lock::SUCCESS) {
    esp_matter::lock::chip_stack_unlock();
  }

  DEBUG_PRINTF("[Matter] Removed %u fabric(s)\n", (unsigned)fabric_count);
  return true;
}

void OSMatter::station_on(unsigned char sid) {
  if(sid >= os.nstations) return;
  MatterCmd cmd{};
  cmd.type = MATTER_CMD_STATION_ON;
  cmd.station.sid = sid;
  xQueueSend(matter_cmd_queue, &cmd, 0);
}

void OSMatter::station_off(unsigned char sid) {
  if(sid >= os.nstations) return;
  MatterCmd cmd{};
  cmd.type = MATTER_CMD_STATION_OFF;
  cmd.station.sid = sid;
  xQueueSend(matter_cmd_queue, &cmd, 0);
}

OSMatter& OSMatter::instance() {
  static OSMatter inst;
  return inst;
}

#endif // ESP32
#endif // ENABLE_MATTER
