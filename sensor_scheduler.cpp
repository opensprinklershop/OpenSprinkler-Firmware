/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Sensor Lazy-Loading by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Sensor Scheduler Implementation - Time-based lazy-loading sensor management
 * Jan 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// IMPORTANT: Include order matters to avoid circular dependencies
#include "sensor_metadata.h"      // Must be first - defines SensorMetadata, SensorScheduleMap
#include "sensor_scheduler.h"
#include "OpenSprinkler.h"
#include "utils.h"

// Include sensors.h AFTER metadata types are defined
#include "sensors.h"
#include "SensorBase.hpp"
#include "sensors_util.h"   // For FileReader

// ArduinoJson with namespace
#include "ArduinoJson.hpp"
using namespace ArduinoJson;

#if defined(ESP32)
#include "esp_heap_caps.h"
#endif

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
#include "sensor_zigbee.h"
#endif

#if defined(ESP32) && defined(OS_ENABLE_BLE)
#include "sensor_ble.h"
#endif

extern OpenSprinkler os;

// =====================================================
// Global State (minimal memory footprint)
// =====================================================
static SensorScheduleMap sensorSchedule;
static bool schedulerReady = false;
static uint32_t lastSchedulerRun = 0;

// Forward declarations from sensors.cpp
extern void detect_asb_board();
extern SensorBase* sensor_make_obj(uint type, boolean ip_based);

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
extern void sensor_mqtt_init();
extern void fyta_check_opts();
#endif

// Forward declaration for logging (defined later in this file)
void sensor_log_value(uint nr, double value, uint32_t timestamp);

// =====================================================
// Helper: PSRAM-aware allocation for sensor objects
// =====================================================
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
static void* psram_alloc(size_t size) {
  void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ptr) {
    // Fallback to internal heap
    ptr = malloc(size);
  }
  return ptr;
}
#else
static void* psram_alloc(size_t size) {
  return malloc(size);
}
#endif

// =====================================================
// Metadata Loading from Flash
// =====================================================

/**
 * @brief Load only metadata from sensors.json (lightweight)
 * Does NOT instantiate full SensorBase objects
 */
static void load_metadata_from_flash() {
  sensorSchedule.clear();
  
  if (!file_exists(SENSOR_FILENAME_JSON)) {
    DEBUG_PRINTLN(F("[SCHEDULER] No sensors.json found"));
    return;
  }
  
  ulong size = file_size(SENSOR_FILENAME_JSON);
  if (size == 0) return;
  
  // Use streaming JSON parser for minimal memory
  FileReader reader(SENSOR_FILENAME_JSON);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reader);
  
  if (err) {
    DEBUG_PRINT(F("[SCHEDULER] JSON parse error: "));
    DEBUG_PRINTLN(err.c_str());
    return;
  }
  
  JsonArray arr;
  if (doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
  } else if (doc.containsKey("sensors")) {
    arr = doc["sensors"].as<JsonArray>();
  } else {
    DEBUG_PRINTLN(F("[SCHEDULER] Invalid sensors.json format"));
    return;
  }
  
  uint32_t now = os.now_tz();
  uint count = 0;
  
  for (JsonVariant v : arr) {
    if (!v.containsKey("nr") || !v.containsKey("type")) continue;
    
    SensorMetadata meta;
    meta.nr = v["nr"].as<uint>();
    meta.type = v["type"].as<uint>();
    
    if (meta.nr == 0 || meta.type == 0) continue;  // Skip deleted sensors
    
    // Read interval (default 60 seconds, minimum 10)
    meta.read_interval = v["ri"] | 60;
    if (meta.read_interval < SCHEDULER_MIN_INTERVAL) {
      meta.read_interval = SCHEDULER_MIN_INTERVAL;
    }
    
    // Stagger initial reads to avoid burst
    meta.next_read_time = now + (count * 2);  // 2 second offset per sensor
    
    // Flags
    uint8_t flags = 0;
    if (v["enable"] | true) flags |= SENSOR_META_FLAG_ENABLE;
    if (v["log"] | false) flags |= SENSOR_META_FLAG_LOG;
    if (v["show"] | true) flags |= SENSOR_META_FLAG_SHOW;
    meta.flags_cache = flags;
    
    // Name (truncated to 19 chars + null)
    const char* name = v["name"] | "";
    strncpy(meta.name, name, sizeof(meta.name) - 1);
    meta.name[sizeof(meta.name) - 1] = '\0';
    
    // Initialize cache as invalid
    meta.cached_value = 0.0;
    meta.cached_native_value = 0;
    meta.last_read_time = 0;
    meta.consecutive_failures = 0;
    
    sensorSchedule[meta.nr] = meta;
    count++;
  }
  
  DEBUG_PRINTF("[SCHEDULER] Loaded %u sensor metadata entries\n", count);
}

// =====================================================
// Core Scheduler Functions
// =====================================================

void sensor_scheduler_init(bool detect_boards) {
  DEBUG_PRINTLN(F("[SCHEDULER] sensor_scheduler_init() started"));
  
  schedulerReady = false;
  sensorSchedule.clear();
  
  if (detect_boards) {
    DEBUG_PRINTLN(F("[SCHEDULER] Detecting boards..."));
    detect_asb_board();
  }
  
  // Load metadata only (not full sensor objects!)
  DEBUG_PRINTLN(F("[SCHEDULER] Loading sensor metadata..."));
  load_metadata_from_flash();
  
  // Initialize subsystems
#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  DEBUG_PRINTLN(F("[SCHEDULER] Initializing MQTT..."));
  sensor_mqtt_init();
  DEBUG_PRINTLN(F("[SCHEDULER] Checking FYTA options..."));
  fyta_check_opts();
#endif

  schedulerReady = true;
  lastSchedulerRun = 0;
  
  DEBUG_PRINTLN(F("[SCHEDULER] sensor_scheduler_init() completed"));
  DEBUG_PRINTF("[SCHEDULER] Memory: %u sensors using ~%u bytes metadata\n", 
               sensorSchedule.size(), sensorSchedule.size() * sizeof(SensorMetadata));
}

void sensor_scheduler_loop() {
  if (!schedulerReady) return;
  
  uint32_t now = os.now_tz();
  
  // Rate limit: don't run more than once per second
  if (now == lastSchedulerRun) return;
  lastSchedulerRun = now;
  
  // Process sensors whose time has come
  for (auto &kv : sensorSchedule) {
    SensorMetadata &meta = kv.second;
    
    // Skip if not time yet
    if (now < meta.next_read_time) continue;
    
    // Skip disabled sensors
    if (!meta.isEnabled()) {
      meta.next_read_time = now + meta.read_interval;
      continue;
    }
    
    // Load sensor from Flash ON-DEMAND
    SensorBase* sensor = sensor_load_single(meta.nr);
    
    if (sensor) {
      // Perform read
      int result = sensor->read(now);
      
      if (result == HTTP_RQT_SUCCESS && sensor->flags.data_ok) {
        // Update cache with new values
        meta.cached_value = sensor->last_data;
        meta.cached_native_value = sensor->last_native_data;
        meta.last_read_time = now;
        meta.setDataValid(true);
        meta.consecutive_failures = 0;
        
        // Log if enabled
        if (meta.shouldLog()) {
          sensor_log_value(meta.nr, sensor->last_data, now);
        }
        
        DEBUG_PRINTF("[SCHEDULER] Sensor %u read: %.2f\n", meta.nr, meta.cached_value);
      } else {
        // Read failed
        meta.consecutive_failures++;
        meta.setDataValid(false);
        
        DEBUG_PRINTF("[SCHEDULER] Sensor %u read failed (attempt %u)\n", 
                     meta.nr, meta.consecutive_failures);
      }
      
      // FREE SENSOR OBJECT IMMEDIATELY
      delete sensor;
      sensor = nullptr;
    } else {
      meta.consecutive_failures++;
      DEBUG_PRINTF("[SCHEDULER] Failed to load sensor %u from Flash\n", meta.nr);
    }
    
    // Calculate next read time with backoff on failures
    uint32_t interval = meta.read_interval;
    if (meta.consecutive_failures >= SCHEDULER_FAILURE_THRESHOLD) {
      // Exponential backoff: 2^failures * interval, capped at max
      uint32_t backoff = interval * (1 << (meta.consecutive_failures - SCHEDULER_FAILURE_THRESHOLD + 1));
      if (backoff > SCHEDULER_MAX_BACKOFF) backoff = SCHEDULER_MAX_BACKOFF;
      interval = backoff;
    }
    meta.next_read_time = now + interval;
  }
  
  // BLE/Zigbee maintenance loops (unchanged)
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (sensor_zigbee_is_active()) {
    sensor_zigbee_loop();
  }
#endif

#if defined(ESP32) && defined(OS_ENABLE_BLE)
  if (sensor_ble_is_active()) {
    sensor_ble_loop();
  }
#endif
}

void sensor_scheduler_reload() {
  DEBUG_PRINTLN(F("[SCHEDULER] Reloading metadata..."));
  load_metadata_from_flash();
}

void sensor_scheduler_free() {
  DEBUG_PRINTLN(F("[SCHEDULER] Freeing resources..."));
  schedulerReady = false;
  sensorSchedule.clear();
}

// =====================================================
// Sensor Access Functions (Cached Values)
// =====================================================

double sensor_get_cached_value(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return 0.0;
  if (!it->second.hasValidData()) return 0.0;
  return it->second.cached_value;
}

uint32_t sensor_get_cached_native(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return 0;
  return it->second.cached_native_value;
}

bool sensor_has_valid_data(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return false;
  return it->second.hasValidData();
}

const SensorMetadata* sensor_get_metadata(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return nullptr;
  return &it->second;
}

const SensorScheduleMap& sensor_get_all_metadata() {
  return sensorSchedule;
}

size_t sensor_get_count() {
  return sensorSchedule.size();
}

// =====================================================
// On-Demand Sensor Loading
// =====================================================

SensorBase* sensor_load_single(uint nr) {
  if (!file_exists(SENSOR_FILENAME_JSON)) {
    return nullptr;
  }
  
  // Read and parse sensors.json
  FileReader reader(SENSOR_FILENAME_JSON);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reader);
  
  if (err) {
    DEBUG_PRINTF("[SCHEDULER] JSON error loading sensor %u: %s\n", nr, err.c_str());
    return nullptr;
  }
  
  JsonArray arr;
  if (doc.is<JsonArray>()) {
    arr = doc.as<JsonArray>();
  } else if (doc.containsKey("sensors")) {
    arr = doc["sensors"].as<JsonArray>();
  } else {
    return nullptr;
  }
  
  // Find the sensor with matching nr
  for (JsonVariant v : arr) {
    if (v["nr"].as<uint>() != nr) continue;
    
    uint type = v["type"].as<uint>();
    if (type == 0) continue;
    
    bool ip_based = v.containsKey("ip") && (v["ip"].as<uint32_t>() > 0);
    
    // Create sensor object (factory function)
    SensorBase* sensor = sensor_make_obj(type, ip_based);
    if (!sensor) {
      DEBUG_PRINTF("[SCHEDULER] Factory failed for type %u\n", type);
      return nullptr;
    }
    
    // Populate from JSON
    sensor->fromJson(v);
    
    // Initialize hardware if needed
    sensor->init();
    
    return sensor;
  }
  
  DEBUG_PRINTF("[SCHEDULER] Sensor %u not found in JSON\n", nr);
  return nullptr;
}

int sensor_read_single_now(uint nr) {
  SensorBase* sensor = sensor_load_single(nr);
  if (!sensor) return HTTP_RQT_NOT_RECEIVED;
  
  uint32_t now = os.now_tz();
  int result = sensor->read(now);
  
  // Update cache
  auto it = sensorSchedule.find(nr);
  if (it != sensorSchedule.end()) {
    if (result == HTTP_RQT_SUCCESS && sensor->flags.data_ok) {
      it->second.cached_value = sensor->last_data;
      it->second.cached_native_value = sensor->last_native_data;
      it->second.last_read_time = now;
      it->second.setDataValid(true);
      it->second.consecutive_failures = 0;
    } else {
      it->second.setDataValid(false);
      it->second.consecutive_failures++;
    }
    // Reset next read time
    it->second.next_read_time = now + it->second.read_interval;
  }
  
  delete sensor;
  return result;
}

void sensor_schedule_immediate(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it != sensorSchedule.end()) {
    it->second.next_read_time = 0;  // Will be processed in next loop
  }
}

// =====================================================
// Metadata Update Functions
// =====================================================

void sensor_update_cache(uint nr, double value, uint32_t native_value, uint32_t timestamp) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return;
  
  it->second.cached_value = value;
  it->second.cached_native_value = native_value;
  it->second.last_read_time = timestamp;
  it->second.setDataValid(true);
  it->second.consecutive_failures = 0;
}

void sensor_invalidate_cache(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return;
  
  it->second.setDataValid(false);
}

void sensor_update_flags(uint nr, bool enable, bool log, bool show) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return;
  
  uint8_t flags = 0;
  if (enable) flags |= SENSOR_META_FLAG_ENABLE;
  if (log) flags |= SENSOR_META_FLAG_LOG;
  if (show) flags |= SENSOR_META_FLAG_SHOW;
  it->second.flags_cache = (it->second.flags_cache & SENSOR_META_FLAG_DATA_OK) | flags;
}

// =====================================================
// Compatibility Layer
// =====================================================

bool sensor_scheduler_ready() {
  return schedulerReady;
}

SensorScheduleIterator sensor_metadata_begin() {
  return sensorSchedule.begin();
}

SensorScheduleIterator sensor_metadata_end() {
  return sensorSchedule.end();
}

// =====================================================
// Logging Helper (calls into existing log infrastructure)
// =====================================================
void sensor_log_value(uint nr, double value, uint32_t timestamp) {
  // Load full sensor for logging (includes unit info)
  SensorBase* sensor = sensor_load_single(nr);
  if (!sensor) return;
  
  sensor->last_data = value;
  sensor->flags.data_ok = true;
  
  // Call existing logging function from sensors.cpp
  // sensorlog_add(log_type, sensor, time) - LOG_STD=0
  sensorlog_add(LOG_STD, sensor, timestamp);
  
  delete sensor;
}
