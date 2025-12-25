/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Sensor utility functions
 * Dec 2025 @ OpenSprinklerShop
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "sensors_util.h"
#include "sensors.h"
#include "Sensor.hpp"
#include "ArduinoJson.hpp"
#include <string.h>

using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;

// Legacy binary sensor file names
#if !defined(ESP32)
#define SENSOR_FILENAME "sensor.dat"          // legacy binary sensor filename
#define SENSOR_FILENAME_BAK "sensor.bak"      // legacy sensor backup
#define SENSORURL_FILENAME "sensorurl.dat"    // legacy sensor URL filename
#else
#define SENSOR_FILENAME "/sensor.dat"         // legacy binary sensor filename
#define SENSOR_FILENAME_BAK "/sensor.bak"     // legacy sensor backup
#define SENSORURL_FILENAME "/sensorurl.dat"   // legacy sensor URL filename
#endif

// External factory function
extern SensorBase* sensor_make_obj(uint type, boolean ip_based);

/**
 * @brief Import legacy binary sensor.dat format and convert to sensor map
 * @param sensorsMap Reference to sensor map to populate
 * @return Number of sensors imported, or 0 if no legacy file found
 */
bool sensor_load_legacy(std::map<uint, SensorBase*>& sensorsMap) {
  if (!file_exists(SENSOR_FILENAME)) return false;
  
  DEBUG_PRINTLN(F("sensor_load_legacy: importing binary format"));
  
  // Legacy Sensor_t structure (old binary format)
  struct Sensor_t {
    uint nr;                      // 4 bytes
    char name[30];                // 30 bytes
    uint type;                    // 4 bytes
    uint group;                   // 4 bytes
    uint32_t ip;                  // 4 bytes
    uint port;                    // 4 bytes
    uint id;                      // 4 bytes
    uint read_interval;           // 4 bytes
    uint32_t last_native_data;    // 4 bytes
    double last_data;             // 8 bytes
    uint32_t flags_raw;           // 4 bytes (SensorFlags_t)
    int16_t factor;               // 2 bytes
    int16_t divider;              // 2 bytes
    char userdef_unit[8];         // 8 bytes
    int16_t offset_mv;            // 2 bytes
    int16_t offset2;              // 2 bytes
    unsigned char assigned_unitid;// 1 byte
    unsigned char undef[15];        // for later
    // Total: 111 bytes
    
    // Type-specific extensions follow after base structure:
    // MQTT: char url[200], char topic[200], char filter[200] = 600 bytes
    // RS485: uint16_t rs485_flags, uint8_t rs485_code, uint16_t rs485_reg = 5 bytes
  };
  
  // Legacy SensorUrl structure (variable length URLs/topics/filters)
  struct SensorUrl_t {
    uint nr;      // 4 bytes - Sensor number
    uint type;    // 4 bytes - SENSORURL_TYPE_URL(0) / TOPIC(1) / FILTER(2)
    uint length;  // 4 bytes - String length
    // char urlstr[length] follows in file (not part of struct)
  };
  
  const size_t BASE_SIZE = 111;
  const size_t SENSORURL_STORE_SIZE = 12;  // nr + type + length (without pointers)
  const size_t RS485_EXT_SIZE = 5;   // flags + code + reg
  
  ulong pos = 0;
  uint imported_count = 0;
  
  while (true) {
    Sensor_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    file_read_block(SENSOR_FILENAME, &legacy, pos, BASE_SIZE);
    
    // Check if valid sensor (nr > 0)
    if (legacy.nr == 0) break;
    
    // Build JSON for this sensor
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    
    obj["nr"] = legacy.nr;
    obj["name"] = legacy.name;
    obj["type"] = legacy.type;
    obj["group"] = legacy.group;
    obj["ip"] = legacy.ip;
    obj["port"] = legacy.port;
    obj["id"] = legacy.id;
    obj["ri"] = legacy.read_interval;
    obj["nativedata"] = legacy.last_native_data;
    obj["data"] = legacy.last_data;
    obj["fac"] = legacy.factor;
    obj["div"] = legacy.divider;
    obj["unit"] = legacy.userdef_unit;
    obj["unitid"] = legacy.assigned_unitid;
    obj["offset"] = legacy.offset_mv;
    obj["offset2"] = legacy.offset2;
    
    // Extract flags from raw uint32_t
    obj["enable"] = (legacy.flags_raw & 0x01) ? 1 : 0;
    obj["log"] = (legacy.flags_raw & 0x02) ? 1 : 0;
    obj["data_ok"] = (legacy.flags_raw & 0x04) ? 1 : 0;
    obj["show"] = (legacy.flags_raw & 0x08) ? 1 : 0;
    
    pos += BASE_SIZE;
    
    // Create sensor object and load data
    boolean ip_based = (legacy.ip != 0);
    SensorBase *sensor = sensor_make_obj(legacy.type, ip_based);
    
    if (!sensor) {
      sensor = new GenericSensor(legacy.type);
    }
    
    sensor->fromJson(obj);
    sensorsMap[sensor->nr] = sensor;
    sensor->flags.data_ok = false;
    
    imported_count++;
    
    // Safety limit
    if (pos > 500000 || imported_count > 200) break;
  }
  
  DEBUG_PRINT(F("sensor_load_legacy: imported "));
  DEBUG_PRINT(imported_count);
  DEBUG_PRINTLN(F(" sensors"));
  
  // Load MQTT sensor URLs from separate file (sensorurl.dat)
  if (file_exists(SENSORURL_FILENAME)) {
    DEBUG_PRINTLN(F("sensor_load_legacy: loading MQTT URLs"));
    ulong url_pos = 0;
    
    while (true) {
      SensorUrl_t urlEntry;
      memset(&urlEntry, 0, sizeof(SensorUrl_t));
      
      // Read header (nr, type, length)
      if (file_read_block(SENSORURL_FILENAME, &urlEntry, url_pos, SENSORURL_STORE_SIZE) < SENSORURL_STORE_SIZE) {
        break;
      }
      
      // Check if valid entry (nr > 0)
      if (urlEntry.nr == 0) break;
      
      url_pos += SENSORURL_STORE_SIZE;
      
      // Read the string data
      if (urlEntry.length > 0 && urlEntry.length < 1024) {
        char *urlstr = (char *)malloc(urlEntry.length + 1);
        
        ulong result = file_read_block(SENSORURL_FILENAME, urlstr, url_pos, urlEntry.length);
        if (result != urlEntry.length) {
          free(urlstr);
          break;
        }
        
        urlstr[urlEntry.length] = 0;
        url_pos += urlEntry.length;
        
        DEBUG_PRINT(urlEntry.nr);
        DEBUG_PRINT(F("/"));
        DEBUG_PRINT(urlEntry.type);
        DEBUG_PRINT(F(": "));
        DEBUG_PRINTLN(urlstr);
        
        // Find the sensor and update its JSON data
        auto it = sensorsMap.find(urlEntry.nr);
        if (it != sensorsMap.end()) {
          SensorBase *sensor = it->second;
          JsonDocument doc;
          sensor->toJson(doc.to<JsonObject>());
          
          // Store based on type
          if (urlEntry.type == 0) {       // SENSORURL_TYPE_URL
            doc["url"] = urlstr;
          } else if (urlEntry.type == 1) { // SENSORURL_TYPE_TOPIC
            doc["topic"] = urlstr;
          } else if (urlEntry.type == 2) { // SENSORURL_TYPE_FILTER
            doc["filter"] = urlstr;
          }
          
          // Reload sensor from updated JSON
          sensor->fromJson(doc.as<JsonObject>());
        }
        
        free(urlstr);
      }
    }
  }
  
  // Initialize sensor drivers
  for (auto &kv : sensorsMap) {
    SensorBase *s = kv.second;
    s->init();
  }
  
  // Save in new JSON format
  sensor_save();
  
  // Delete old binary files to prevent re-import
  remove_file(SENSOR_FILENAME);
  remove_file(SENSORURL_FILENAME);
  
  DEBUG_PRINTLN(F("sensor_load_legacy: migration complete, legacy files deleted"));
  return true;
}

/**
 * @brief Import legacy binary progsensor.dat format and convert to JSON
 * @param progSensorAdjustsMap Reference to map to populate
 * @return true if legacy file was found and imported, false otherwise
 */
bool prog_adjust_load_legacy(std::map<uint, ProgSensorAdjust*>& progSensorAdjustsMap) {
  // Legacy binary filename
  #if !defined(ESP32)
  #define PROG_SENSOR_FILENAME_LEGACY "progsensor.dat"
  #else
  #define PROG_SENSOR_FILENAME_LEGACY "/progsensor.dat"
  #endif
  
  if (!file_exists(PROG_SENSOR_FILENAME_LEGACY)) {
    return false;
  }
  
  DEBUG_PRINTLN(F("prog_adjust_load_legacy: importing binary format"));
  
  // Legacy ProgSensorAdjust structure (old binary format)
  struct ProgSensorAdjustLegacy_t {
    uint nr;      // adjust-nr 1..x
    uint type;    // PROG_XYZ type=0 -->delete
    uint sensor;  // sensor-nr
    uint prog;    // program-nr=pid
    double factor1;
    double factor2;
    double min;
    double max;
    char name[30];
    unsigned char undef[2];  // for later (was in old struct)
    // Note: 'next' pointer is not stored in file
  };
  
  // Size calculation: structure size without the pointer
  // This matches the old PROGSENSOR_STORE_SIZE calculation
  const size_t LEGACY_STORE_SIZE = sizeof(ProgSensorAdjustLegacy_t);
  
  ulong pos = 0;
  uint imported_count = 0;
  
  while (true) {
    ProgSensorAdjustLegacy_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    
    ulong result = file_read_block(PROG_SENSOR_FILENAME_LEGACY, &legacy, pos, LEGACY_STORE_SIZE);
    if (result < LEGACY_STORE_SIZE) {
      break;
    }
    
    // Check if valid entry (nr > 0 and type > 0)
    if (legacy.nr == 0 || legacy.type == 0) {
      break;
    }
    
    // Create new ProgSensorAdjust object
    ProgSensorAdjust* pa = new ProgSensorAdjust();
    pa->nr = legacy.nr;
    pa->type = legacy.type;
    pa->sensor = legacy.sensor;
    pa->prog = legacy.prog;
    pa->factor1 = legacy.factor1;
    pa->factor2 = legacy.factor2;
    pa->min = legacy.min;
    pa->max = legacy.max;
    strncpy(pa->name, legacy.name, sizeof(pa->name) - 1);
    pa->name[sizeof(pa->name) - 1] = '\0';
    
    // Add to map
    progSensorAdjustsMap[pa->nr] = pa;
    imported_count++;
    pos += LEGACY_STORE_SIZE;
    
    // Safety limit
    if (pos > 100000 || imported_count > 500) break;
  }
  
  DEBUG_PRINT(F("prog_adjust_load_legacy: imported "));
  DEBUG_PRINT(imported_count);
  DEBUG_PRINTLN(F(" program adjustments"));
  
  if (imported_count > 0) {
    // Save in new JSON format
    prog_adjust_save();
    
    // Delete old binary file to prevent re-import
    remove_file(PROG_SENSOR_FILENAME_LEGACY);
    
    DEBUG_PRINTLN(F("prog_adjust_load_legacy: migration complete, legacy file deleted"));
    return true;
  }
  
  return false;
}

/**
 * @brief Import legacy binary monitors.dat format and convert to JSON
 */
bool monitor_load_legacy(std::map<uint, Monitor*>& monitorsMap) {
  // Define legacy filename (old .dat format)
  #if !defined(ESP32)
    const char* MONITOR_FILENAME_LEGACY = "monitors.dat";
  #else
    const char* MONITOR_FILENAME_LEGACY = "/monitors.dat";
  #endif
  
  // Check if legacy file exists
  if (!file_exists(MONITOR_FILENAME_LEGACY)) {
    return false;
  }
  
  ulong legacy_file_size = file_size(MONITOR_FILENAME_LEGACY);
  
  DEBUG_PRINT(F("monitor_load_legacy: found legacy file, size="));
  DEBUG_PRINTLN(legacy_file_size);
  DEBUG_PRINT(F("monitor_load_legacy: MONITOR_STORE_SIZE="));
  DEBUG_PRINTLN(MONITOR_STORE_SIZE);
  
  // Validate file size
  if (legacy_file_size % MONITOR_STORE_SIZE != 0) {
    DEBUG_PRINTLN(F("monitor_load_legacy: invalid file size"));
    return false;
  }
  
  // Read legacy binary records
  ulong pos = 0;
  int imported_count = 0;
  
  while (pos < legacy_file_size) {
    Monitor *mon = new Monitor;
    
    // Read binary record
    file_read_block(MONITOR_FILENAME_LEGACY, mon, pos, MONITOR_STORE_SIZE);
    
    // Skip invalid entries
    if (!mon->nr || !mon->type) {
      delete mon;
      break;
    }
    
    // Add to map
    monitorsMap[mon->nr] = mon;
    imported_count++;
    pos += MONITOR_STORE_SIZE;
    
    // Safety limit
    if (pos > 100000 || imported_count > 500) break;
  }
  
  DEBUG_PRINT(F("monitor_load_legacy: imported "));
  DEBUG_PRINT(imported_count);
  DEBUG_PRINTLN(F(" monitors"));
  
  if (imported_count > 0) {
    // Save in new JSON format
    DEBUG_PRINTLN(F("monitor_load_legacy: saving in JSON format"));
    
    // Create JSON document
    ArduinoJson::JsonDocument doc;
    ArduinoJson::JsonArray array = doc.to<ArduinoJson::JsonArray>();
    
    for (auto &kv : monitorsMap) {
      Monitor *mon = kv.second;
      if (mon) {
        ArduinoJson::JsonObject obj = array.add<ArduinoJson::JsonObject>();
        mon->toJson(obj);
      }
    }
    
    // Write to JSON file
    FileWriter writer(MONITOR_FILENAME);
    ArduinoJson::serializeJson(doc, writer);
    
    // Delete old binary file to prevent re-import
    remove_file(MONITOR_FILENAME_LEGACY);
    
    DEBUG_PRINTLN(F("monitor_load_legacy: migration complete, legacy file deleted"));
    return true;
  }
  
  return false;
}