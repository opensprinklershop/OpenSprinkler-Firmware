/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Utility functions
 * Sep 2022 @ OpenSprinklerShop
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

#include "sensors.h"
#include "Sensor.hpp"
#include "sensors_util.h"
#include "main.h"
#include <stdlib.h>

#include "OpenSprinkler.h"
#if defined(ESP8266) || defined(ESP32)
#include "Wire.h"
#elif defined(OSPI)
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <modbus/modbus.h>
#include <modbus/modbus-rtu.h>
#include <errno.h>
#else
#include <stdio.h>
#include <iostream>
#include <fstream>
#endif
#include "defines.h"
#include "opensprinkler_server.h"
#include "program.h"
#include "Sensor.hpp"
#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_mqtt.h"
#endif

#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR)
  #include "sensor_zigbee.h"
#endif

#if defined(ESP32)
  #include "sensor_ble.h"
#endif

#if defined(OSPI)
  #include "sensor_ospi_ble.h"
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_fyta.h"
#endif

#include "sensor_group.h"
#if defined(ESP8266) || defined(ESP32)
  #include "sensor_rs485_i2c.h"
  #include "sensor_truebner_rs485.h"
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_modbus_rtu.h"
#endif

#if defined(ESP8266) || defined(ESP32)
  #include "sensor_asb.h"
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_remote.h"
#endif

#include "sensor_internal.h"
#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_weather.h"
#endif

#if defined(OSPI)
  #include "sensor_usbrs485.h"
#endif
#include "utils.h"
#include "weather.h"
#include "osinfluxdb.h"
#include "notifier.h"

#include <map>
#ifdef ADS1115
#include "sensor_ospi_ads1115.h"
#endif
#ifdef PCF8591
#include "sensor_ospi_pcf8591.h"
#endif
#ifdef ESP32
#include <fstream>
#endif

unsigned char findKeyVal(const char *str, char *strbuf, uint16_t maxlen, const char *key,
                bool key_in_pgm = false, uint8_t *keyfound = NULL);

// All sensors (map nr -> SensorBase*)
static std::map<uint, SensorBase*> sensorsMap;
static time_t last_save_time = 0;
static boolean apiInit = false;
static SensorBase *current_sensor = NULL;
static std::map<uint, SensorBase*>::iterator current_sensor_it;

// Factory forward declaration
SensorBase* sensor_make_obj(uint type, boolean ip_based);

// Boards:
static uint16_t asb_detected_boards = 0;  // bit 1=0x48+0x49 bit 2=0x4A+0x4B usw

// Program sensor data (HashMap for efficient lookup by nr)
static std::map<uint, ProgSensorAdjust*> progSensorAdjustsMap;

// Monitor data (HashMap for efficient lookup by nr)
static std::map<uint, Monitor*> monitorsMap;

const char *sensor_unitNames[]{
    "",  "%", "°C", "°F", "V", "%", "in", "mm", "mph", "kmh", "%", "DK", "LM", "LX"
    //0   1     2     3    4    5    6     7      8      9     10,  11,   12,   13
    //   0=Nothing
    //   1=Soil moisture
    //   2=degree celsius temperature
    //   3=degree fahrenheit temperature
    //   4=Volt V
    //   5=Humidity %
    //   6=Rain inch
    //   7=Rain mm
    //   8=Wind mph
    //   9=Wind kmh
    //  10=Level %
    //  11=DK (Permitivität)
    //  12=LM (Lumen)
    //  13=LX (LUX)
};
uint8_t logFileSwitch[3] = {0, 0, 0};  // 0=use smaller File, 1=LOG1, 2=LOG2



uint16_t CRC16(unsigned char buf[], int len) {
  uint16_t crc = 0xFFFF;

  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];      // XOR byte into least sig. byte of crc
    for (int i = 8; i != 0; i--) {  // Loop over each bit
      if ((crc & 0x0001) != 0) {    // If the LSB is set
        crc >>= 1;                  // Shift right and XOR 0xA001
        crc ^= 0xA001;
      } else        // Else LSB is not set
        crc >>= 1;  // Just shift right
    }
  }
  // Note, this number has low and high bytes swapped, so use it accordingly (or
  // swap bytes)
  return crc;
}  // End: CRC16

/**
 * @brief detect connected boards
 *
 */
void detect_asb_board() {
  // detect analog sensor board, 0x48+0x49=Board1, 0x4A+0x4B=Board2
#if defined(ESP8266) || defined(ESP32)
  if (detect_i2c(ASB_BOARD_ADDR1a) && detect_i2c(ASB_BOARD_ADDR1b))
    asb_detected_boards |= ASB_BOARD1;
  if (detect_i2c(ASB_BOARD_ADDR2a) && detect_i2c(ASB_BOARD_ADDR2b))
    asb_detected_boards |= ASB_BOARD2;

  sensor_truebner_rs485_init();
  sensor_rs485_i2c_init();
#endif

// Old, pre OSPi 1.43 analog inputs:
#if defined(PCF8591)
  asb_detected_boards |= OSPI_PCF8591;
#endif

// New OSPi 1.6 analog inputs:
#if defined(ADS1115)
  asb_detected_boards |= OSPI_ADS1115;
#endif
  DEBUG_PRINT("ASB DETECT=");
  DEBUG_PRINTLN(asb_detected_boards);

  for (int log = 0; log <= 2; log++) {
    checkLogSwitch(log);
#if defined(ENABLE_DEBUG)
    DEBUG_PRINT("log=");
    DEBUG_PRINTLN(log);
    const char *f1 = getlogfile(log);
    DEBUG_PRINT("logfile1=");
    DEBUG_PRINTLN(f1);
    DEBUG_PRINT("size1=");
    DEBUG_PRINTLN(file_size(f1));
    const char *f2 = getlogfile2(log);
    DEBUG_PRINT("logfile2=");
    DEBUG_PRINTLN(f2);
    DEBUG_PRINT("size2=");
    DEBUG_PRINTLN(file_size(f2));
#endif
  }
}

uint16_t get_asb_detected_boards() { return asb_detected_boards; }
void add_asb_detected_boards(uint16_t board) { asb_detected_boards |= board; }

boolean sensor_type_supported(int type) {

  if ((type >= ASB_SENSORS_START && type <= ASB_SENSORS_END) &&
    ((asb_detected_boards & ASB_BOARD1) != 0 || (asb_detected_boards & ASB_BOARD2) != 0))
      return true;

  if ((type >= OSPI_SENSORS_START && type <= OSPI_SENSORS_END) &&
    ((asb_detected_boards & OSPI_PCF8591) != 0 || (asb_detected_boards & OSPI_ADS1115) != 0))
      return true;

  if (type >= INDEPENDENT_SENSORS_START)
      return true;

  if (type >= RS485_SENSORS_START && type <= RS485_SENSORS_END) //Always available because of modbus_rtu IP
      return true;
  return false;
}
/*
 * init sensor api and load data
 */
void sensor_api_init(boolean detect_boards) {
  apiInit = true;
  if (detect_boards)
    detect_asb_board();
  sensor_load();
  prog_adjust_load();
  #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  sensor_mqtt_init();
  #endif
  monitor_load();
  #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  fyta_check_opts();
  #endif
#if defined(OSPI)
  //Read rs485 file. Details see below
  std::ifstream file;
  file.open("rs485", std::ifstream::in);
  if (!file.fail()) {
    std::string tty;
    int idx = 0;
    int n = 0;
    DEBUG_PRINTLN(F("Opening USB RS485 Adapters:"));
    while (std::getline(file, tty)) {
      modbus_t * ctx;
      if (tty.find(".") > 0 || tty.find(":") > 0) {
        if (tty.find(":") > 0) {
          // IP:port
          std::string host = tty.substr(0, tty.find(':'));
          std::string port = tty.substr(tty.find(':') + 1);
          ctx = modbus_new_tcp(host.c_str(), atoi(port.c_str()));
        } else {
          // IP only
          ctx = modbus_new_tcp(tty.c_str(), 502);
        }
      }
      else
        ctx = modbus_new_rtu(tty.c_str(), 9600, 'E', 8, 1);
      DEBUG_PRINT(idx);
      DEBUG_PRINT(": ");
      DEBUG_PRINTLN(tty.c_str());

      //unavailable on Raspi? modbus_enable_quirks(ctx, MODBUS_QUIRK_MAX_SLAVE);
      modbus_rtu_set_serial_mode(ctx, MODBUS_RTU_RS485);
      modbus_rtu_set_rts(ctx, MODBUS_RTU_RTS_NONE); // we use auto RTS function by the HAT
      modbus_set_response_timeout(ctx, 1, 500000); // 1.5s
      if (modbus_connect(ctx) == -1) {
        DEBUG_PRINT(F("Connection failed: "));
        DEBUG_PRINTLN(modbus_strerror(errno));        
        modbus_free(ctx);
      } else {
        n++;
        modbusDevs[idx] = ctx;
        asb_detected_boards |= OSPI_USB_RS485;
        #ifdef ENABLE_DEBUG
        modbus_set_debug(ctx, TRUE);
        DEBUG_PRINTLN(F("DEBUG ENABLED"));
        #endif
      }
      idx++;
      if (idx >= MAX_RS485_DEVICES)
        break;
    }
    DEBUG_PRINT(F("Found "));
    DEBUG_PRINT(n);
    DEBUG_PRINTLN(F(" RS485 Adapters"));
  }
#endif
}

void sensor_save_all() {
  sensor_save();
  prog_adjust_save();
  monitor_save();
#if defined(OSPI)
  for (int i = 0; i < MAX_RS485_DEVICES; i++) {
    if (modbusDevs[i]) {
      modbus_close(modbusDevs[i]);
      modbus_free(modbusDevs[i]);
    }
    modbusDevs[i] = NULL;
  }
#endif
}

/**
 * @brief Unload sensorapi from memory, free everything. Be sure that you have save all before
 * 
 */
void sensor_api_free() {
  DEBUG_PRINTLN("sensor_api_free1");
  apiInit = false;
  current_sensor = NULL;
  os.mqtt.setCallback(2, NULL);

  for (auto &kv : progSensorAdjustsMap) {
    delete kv.second;
  }
  progSensorAdjustsMap.clear();

  DEBUG_PRINTLN("sensor_api_free2");

  DEBUG_PRINTLN("sensor_api_free3");

  for (auto &kv : monitorsMap) {
    delete kv.second;
  }
  monitorsMap.clear();

  DEBUG_PRINTLN("sensor_api_free4");

  for (auto &kv : sensorsMap) {
    delete kv.second;
  }
  sensorsMap.clear();

  #if defined(ESP8266) || defined(ESP32)
  sensor_truebner_rs485_free();
  #endif
  #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  sensor_modbus_rtu_free();
  #endif
  DEBUG_PRINTLN("sensor_api_free5");
}

/*
 * get list of all configured sensors
 */
// returns first sensor (deprecated - prefer sensor_map access)
SensorBase *getSensors() {
  if (sensorsMap.empty()) return NULL;
  return sensorsMap.begin()->second;
}

// Helper functions for iterating through sensors
SensorIterator sensors_iterate_begin() {
  return sensorsMap.begin();
}

SensorBase* sensors_iterate_next(SensorIterator& it) {
  if (it != sensorsMap.end()) {
    SensorBase *sensor = it->second;
    ++it;
    return sensor;
  }
  return NULL;
}

// Helper functions for iterating through program adjustments
ProgAdjustIterator prog_adjust_iterate_begin() {
  return progSensorAdjustsMap.begin();
}

ProgSensorAdjust* prog_adjust_iterate_next(ProgAdjustIterator& it) {
  if (it != progSensorAdjustsMap.end()) {
    ProgSensorAdjust *pa = it->second;
    ++it;
    return pa;
  }
  return NULL;
}

// Helper functions for iterating through monitors
MonitorIterator monitor_iterate_begin() {
  return monitorsMap.begin();
}

Monitor* monitor_iterate_next(MonitorIterator& it) {
  if (it != monitorsMap.end()) {
    Monitor *mon = it->second;
    ++it;
    return mon;
  }
  return NULL;
}

/**
 * @brief delete a sensor
 *
 * @param nr
 */
int sensor_delete(uint nr) {
  auto it = sensorsMap.find(nr);
  if (it == sensorsMap.end()) return HTTP_RQT_NOT_RECEIVED;
  // Do not create a new driver object here; just remove the sensor
  delete it->second;
  sensorsMap.erase(it);
  sensor_save();
  return HTTP_RQT_SUCCESS;
}

/**
 * @brief define or insert a sensor from JSON configuration
 *
 * @param json JsonDocument containing sensor configuration
 * @param save if true, save to file after update (default: false)
 */
int sensor_define(ArduinoJson::JsonVariantConst json, bool save) {
  if (!json.containsKey("nr")) {
    return HTTP_RQT_NOT_RECEIVED;
  }
  
  uint nr = json["nr"];
  if (nr == 0) return HTTP_RQT_NOT_RECEIVED;
  
  DEBUG_PRINTLN(F("sensor_define"));
  
  // Check if this is a partial update (no type field) or full definition
  bool is_partial_update = !json.containsKey("type");
  
  auto it = sensorsMap.find(nr);
  
  if (is_partial_update) {
    // Partial update - sensor must exist
    if (it == sensorsMap.end()) {
      return HTTP_RQT_NOT_RECEIVED;
    }
    
    SensorBase *sensor = it->second;
    sensor->fromJson(json);
    
    if (save) sensor_save();
    else last_save_time = os.now_tz() - 3600 + 5; // force save next time
    
    return HTTP_RQT_SUCCESS;
  }
  
  // Full definition with type
  uint type = json["type"];
  if (type == 0) return HTTP_RQT_NOT_RECEIVED;
  
  if (it != sensorsMap.end()) {
    // Sensor exists - check if type changed
    SensorBase *old_sensor = it->second;
    if (old_sensor->type != type) {
      DEBUG_PRINTLN(F("sensor_define: type changed, recreating"));
      delete old_sensor;
      sensorsMap.erase(it);
      // Fall through to create new sensor
    } else {
      // Same type, update from JSON
      old_sensor->fromJson(json);
      
      if (save) sensor_save();
      return HTTP_RQT_SUCCESS;
    }
  }
  
  // Create new sensor
  boolean ip_based = json.containsKey("ip") && (json["ip"].as<uint32_t>() > 0);
  SensorBase *new_sensor = sensor_make_obj(type, ip_based);
  
  if (!new_sensor) {
    return HTTP_RQT_NOT_RECEIVED;
  }
  
  // Load from JSON
  new_sensor->fromJson(json);
  
  sensorsMap[nr] = new_sensor;
  if (save) sensor_save();
  
  return HTTP_RQT_SUCCESS;
}

int sensor_define_userdef(uint nr, int16_t factor, int16_t divider,
                          const char *userdef_unit, int16_t offset_mv,
                          int16_t offset2, int16_t assigned_unitid) {
  // Wrapper: build JSON and call sensor_define
  JsonDocument doc;
  JsonObject config = doc.to<JsonObject>();
  
  config["nr"] = nr;
  config["fac"] = factor;
  config["div"] = divider;
  config["unit"] = userdef_unit;
  config["offset"] = offset_mv;
  config["offset2"] = offset2;
  config["unitid"] = assigned_unitid;
  
  return sensor_define(config);
}

/**
 * @brief initial load stored sensor definitions
 * Supports both new JSON format and legacy binary format migration
 */
#include "ArduinoJson.hpp"

void sensor_load() {
  // DEBUG_PRINTLN(F("sensor_load"));
  sensorsMap.clear();
  current_sensor = NULL;

  // First try legacy binary format migration (sensor.dat)
  // If successful, sensors are already initialized and saved
  sensor_load_legacy(sensorsMap);
  
  if (!sensorsMap.empty()) {
    last_save_time = os.now_tz();
    return;
  }

  // Try JSON format (current format)
  if (file_exists(SENSOR_FILENAME_JSON)) {
    ulong size = file_size(SENSOR_FILENAME_JSON);
    if (size == 0) return;

    FileReader reader(SENSOR_FILENAME_JSON);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, reader);
    if (err) {
      DEBUG_PRINTLN(F("sensor_load: JSON parse error"));
      return;
    }

    JsonArray arr;
    if (doc.is<JsonArray>()) arr = doc.as<JsonArray>();
    else if (doc.containsKey("sensors")) arr = doc["sensors"].as<JsonArray>();
    else return;

    for (JsonVariant v : arr) {
      uint sensorType = v["type"] | 0;
      boolean ip_based = (v["ip"] | 0) != 0;
      
      SensorBase *sensor = sensor_make_obj(sensorType, ip_based);
      if (!sensor) {
        sensor = new GenericSensor(sensorType);
      }
      
      sensor->fromJson(v);
      sensorsMap[sensor->nr] = sensor;
      sensor->flags.data_ok = false;
    }

    // Initialize sensor drivers
    for (auto &kv : sensorsMap) {
      SensorBase *s = kv.second;
      s->init();
    }

    last_save_time = os.now_tz();
    return;
  }
  
  last_save_time = os.now_tz();
}

/**
 * @brief Store sensor data
 *
 */
void sensor_save() {
  if (!apiInit) return;
  DEBUG_PRINTLN(F("sensor_save (json)"));

  // Remove old json file if present (write fresh)
  if (file_exists(SENSOR_FILENAME_JSON)) remove_file(SENSOR_FILENAME_JSON);

  // Build JSON
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (auto &kv : sensorsMap) {
    SensorBase *sensor = kv.second;
    JsonObject obj = arr.add<JsonObject>();
    sensor->toJson(obj);
  }

  // Serialize directly to file
  FileWriter writer(SENSOR_FILENAME_JSON);
  serializeJson(doc, writer);

  last_save_time = os.now_tz();
  DEBUG_PRINTLN(F("sensor_save2"));
  current_sensor = NULL;
}

uint sensor_count() {
  return (uint)sensorsMap.size();
}

SensorBase *sensor_by_nr(uint nr) {
  auto it = sensorsMap.find(nr);
  if (it == sensorsMap.end()) return NULL;
  return it->second;
}

SensorBase *sensor_by_idx(uint idx) {
  if (idx >= sensorsMap.size()) return NULL;
  uint count = 0;
  for (auto &kv : sensorsMap) {
    if (count == idx) return kv.second;
    count++;
  }
  return NULL;
}

// LOGGING METHODS:

/**
 * @brief getlogfile name
 *
 * @param log
 * @return const char*
 */
const char *getlogfile(uint8_t log) {
  uint8_t sw = logFileSwitch[log];
  switch (log) {
    case LOG_STD:
      return sw < 2 ? SENSORLOG_FILENAME1 : SENSORLOG_FILENAME2;
    case LOG_WEEK:
      return sw < 2 ? SENSORLOG_FILENAME_WEEK1 : SENSORLOG_FILENAME_WEEK2;
    case LOG_MONTH:
      return sw < 2 ? SENSORLOG_FILENAME_MONTH1 : SENSORLOG_FILENAME_MONTH2;
  }
  return "";
}

/**
 * @brief getlogfile name2 (opposite file)
 *
 * @param log
 * @return const char*
 */
const char *getlogfile2(uint8_t log) {
  uint8_t sw = logFileSwitch[log];
  switch (log) {
    case 0:
      return sw < 2 ? SENSORLOG_FILENAME2 : SENSORLOG_FILENAME1;
    case 1:
      return sw < 2 ? SENSORLOG_FILENAME_WEEK2 : SENSORLOG_FILENAME_WEEK1;
    case 2:
      return sw < 2 ? SENSORLOG_FILENAME_MONTH2 : SENSORLOG_FILENAME_MONTH1;
  }
  return "";
}

void checkLogSwitch(uint8_t log) {
  if (logFileSwitch[log] == 0) {  // Check file size, use smallest
    ulong logFileSize1 = file_size(getlogfile(log));
    ulong logFileSize2 = file_size(getlogfile2(log));
    if (logFileSize1 < logFileSize2)
      logFileSwitch[log] = 1;
    else
      logFileSwitch[log] = 2;
  }
}

void checkLogSwitchAfterWrite(uint8_t log) {
  ulong size = file_size(getlogfile(log));
  if ((size / SENSORLOG_STORE_SIZE) >= MAX_LOG_SIZE) {  // switch logs if max reached
    if (logFileSwitch[log] == 1)
      logFileSwitch[log] = 2;
    else
      logFileSwitch[log] = 1;
    remove_file(getlogfile(log));
  }
}

bool sensorlog_add(uint8_t log, SensorLog_t *sensorlog) {
#if defined(ESP8266) || defined(ESP32)
  if (checkDiskFree()) {
#endif
    DEBUG_PRINT(F("sensorlog_add "));
    DEBUG_PRINT(log);
    checkLogSwitch(log);
    file_append_block(getlogfile(log), sensorlog, SENSORLOG_STORE_SIZE);
    checkLogSwitchAfterWrite(log);
    DEBUG_PRINT(F("="));
    DEBUG_PRINTLN(sensorlog_filesize(log));
    return true;
#if defined(ESP8266) || defined(ESP32)
  }
  return false;
#endif
}

bool sensorlog_add(uint8_t log, SensorBase *sensor, ulong time) {

  if (sensor->flags.data_ok && sensor->flags.log && time > 1000) {

    // Write to log file only if necessary
    if (time-sensor->last_logged_time > 86400 || abs(sensor->last_data - sensor->last_logged_data) > 0.00999) {
      SensorLog_t sensorlog;
      memset(&sensorlog, 0, sizeof(SensorLog_t));
      sensorlog.nr = sensor->nr;
      sensorlog.time = time;
      sensorlog.native_data = sensor->last_native_data;
      sensorlog.data = sensor->last_data;
      sensor->last_logged_data = sensor->last_data;
      sensor->last_logged_time = time;

      if (!sensorlog_add(log, &sensorlog)) {
        sensor->flags.log = 0;
        return false;
      }
    }
    return true;
  }
  return false;
}

ulong sensorlog_filesize(uint8_t log) {
  // DEBUG_PRINT(F("sensorlog_filesize "));
  checkLogSwitch(log);
  ulong size = file_size(getlogfile(log)) + file_size(getlogfile2(log));
  // DEBUG_PRINTLN(size);
  return size;
}

ulong sensorlog_size(uint8_t log) {
  // DEBUG_PRINT(F("sensorlog_size "));
  ulong size = sensorlog_filesize(log) / SENSORLOG_STORE_SIZE;
  // DEBUG_PRINTLN(size);
  return size;
}

void sensorlog_clear_all() { sensorlog_clear(true, true, true); }

void sensorlog_clear(bool std, bool week, bool month) {
  DEBUG_PRINTLN(F("sensorlog_clear "));
  DEBUG_PRINT(std);
  DEBUG_PRINT(week);
  DEBUG_PRINT(month);
  if (std) {
    remove_file(SENSORLOG_FILENAME1);
    remove_file(SENSORLOG_FILENAME2);
    logFileSwitch[LOG_STD] = 1;
  }
  if (week) {
    remove_file(SENSORLOG_FILENAME_WEEK1);
    remove_file(SENSORLOG_FILENAME_WEEK2);
    logFileSwitch[LOG_WEEK] = 1;
  }
  if (month) {
    remove_file(SENSORLOG_FILENAME_MONTH1);
    remove_file(SENSORLOG_FILENAME_MONTH2);
    logFileSwitch[LOG_MONTH] = 1;
  }
}

ulong sensorlog_clear_sensor(uint sensorNr, uint8_t log, bool use_under,
                             double under, bool use_over, double over, time_t before, time_t after) {
#define SLOG_BUFSIZE 64
  SensorLog_t * sensorlog = new SensorLog_t[SLOG_BUFSIZE];
  checkLogSwitch(log);
  const char *flast = getlogfile2(log);
  const char *fcur = getlogfile(log);
  ulong size = file_size(flast) / SENSORLOG_STORE_SIZE;
  ulong size2 = size + file_size(fcur) / SENSORLOG_STORE_SIZE;
  const char *f;
  ulong idxr = 0;
  ulong n = 0;
  DEBUG_PRINTLN(F("clearlog1"));
  DEBUG_PRINTF("nr: %d log: %d under:%lf over: %lf before: %lld after: %lld size: %ld size2: %ld\n", sensorNr, log, under, over, before, after, size, size2);
  while (idxr < size2) {
    ulong idx = idxr;
    if (idx >= size) {
      idx -= size;
      f = fcur;
    } else {
      f = flast;
    }

    ulong result = file_read_block(f, sensorlog, idx * SENSORLOG_STORE_SIZE,
                                   SENSORLOG_STORE_SIZE*SLOG_BUFSIZE);
    int entries = result / SENSORLOG_STORE_SIZE;
    for (int i = 0; i < entries; i++, idxr++) {
      SensorLog_t * sl = &sensorlog[i];
      if (sl->nr > 0 && (sl->nr == sensorNr || sensorNr == 0)) {
        DEBUG_PRINTF("clearlog2 idx=%ld idx2=%ld\n", idx, idxr);
        boolean found = false;
        if (use_under && sl->data < under) found = true;
        if (use_over && sl->data > over) found = true;
        if (before && sl->time < before) found = true;
        if (after && sl->time > after) found = true;
        if (sensorNr > 0 && sl->nr != sensorNr) found = false;
        if (sensorNr > 0 && sl->nr == sensorNr && !use_under && !use_over && !before && !after) found = true;
        if (found) {
          sl->nr = 0;
          file_write_block(f, sl, (idx+i) * SENSORLOG_STORE_SIZE, sizeof(sl->nr));
          DEBUG_PRINTF("clearlog3 idx=%ld idxr=%ld\n", idx, idxr);
          n++;
        }
      }
    }
  }
  delete[] sensorlog;
  DEBUG_PRINTF("clearlog4 n=%ld\n", n);
  return n;
}

SensorLog_t *sensorlog_load(uint8_t log, ulong idx) {
  SensorLog_t *sensorlog = new SensorLog_t;
  return sensorlog_load(log, idx, sensorlog);
}

SensorLog_t *sensorlog_load(uint8_t log, ulong idx, SensorLog_t *sensorlog) {
  // DEBUG_PRINTLN(F("sensorlog_load"));

  // Map lower idx to the other log file
  checkLogSwitch(log);
  const char *flast = getlogfile2(log);
  const char *fcur = getlogfile(log);
  ulong size = file_size(flast) / SENSORLOG_STORE_SIZE;
  const char *f;
  if (idx >= size) {
    idx -= size;
    f = fcur;
  } else {
    f = flast;
  }

  file_read_block(f, sensorlog, idx * SENSORLOG_STORE_SIZE,
                  SENSORLOG_STORE_SIZE);
  return sensorlog;
}

int sensorlog_load2(uint8_t log, ulong idx, int count, SensorLog_t *sensorlog) {
  // DEBUG_PRINTLN(F("sensorlog_load"));

  // Map lower idx to the other log file
  checkLogSwitch(log);
  const char *flast = getlogfile2(log);
  const char *fcur = getlogfile(log);
  ulong size = file_size(flast) / SENSORLOG_STORE_SIZE;
  const char *f;
  if (idx >= size) {
    idx -= size;
    f = fcur;
    size = file_size(f) / SENSORLOG_STORE_SIZE;
  } else {
    f = flast;
  }

  if (idx + count > size) count = size - idx;
  if (count <= 0) return 0;
  file_read_block(f, sensorlog, idx * SENSORLOG_STORE_SIZE,
                  count * SENSORLOG_STORE_SIZE);
  return count;
}

ulong findLogPosition(uint8_t log, ulong after) {
  ulong log_size = sensorlog_size(log);
  ulong a = 0;
  ulong b = log_size - 1;
  ulong lastIdx = 0;
  SensorLog_t sensorlog;
  while (true) {
    ulong idx = (b - a) / 2 + a;
    sensorlog_load(log, idx, &sensorlog);
    if (sensorlog.time < after) {
      a = idx;
    } else if (sensorlog.time > after) {
      b = idx;
    }
    if (a >= b || idx == lastIdx) return idx;
    lastIdx = idx;
  }
  return 0;
}

#if !defined(ARDUINO)
/**
 * compatibility functions for OSPi:
 **/
#define timeSet 0
int timeStatus() { return timeSet; }

void dtostrf(float value, int min_width, int precision, char *txt) {
  printf(txt, "%*.*f", min_width, precision, value);
}

void dtostrf(double value, int min_width, int precision, char *txt) {
  printf(txt, "%*.*d", min_width, precision, value);
}
#endif

// 1/4 of a day: 6*60*60
#define BLOCKSIZE 64
#define CALCRANGE_WEEK 21600
#define CALCRANGE_MONTH 172800
static ulong next_week_calc = 0;
static ulong next_month_calc = 0;

/**
Calculate week+month Data
We store only the average value of 6 hours utc
**/
void calc_sensorlogs() {
  if (sensorsMap.empty() || timeStatus() != timeSet) return;

  ulong log_size = sensorlog_size(LOG_STD);
  if (log_size == 0) return;

  SensorLog_t *sensorlog = NULL;

  time_t time = os.now_tz();
  time_t last_day = time;

  if (time >= next_week_calc) {
    DEBUG_PRINTLN(F("calc_sensorlogs WEEK start"));
    sensorlog = (SensorLog_t *)malloc(sizeof(SensorLog_t) * BLOCKSIZE);
    ulong size = sensorlog_size(LOG_WEEK);
    if (size == 0) {
      sensorlog_load(LOG_STD, 0, sensorlog);
      last_day = sensorlog->time;
    } else {
      sensorlog_load(LOG_WEEK, size - 1, sensorlog);  // last record
      last_day = sensorlog->time + CALCRANGE_WEEK;    // Skip last Range
    }
    time_t fromdate = (last_day / CALCRANGE_WEEK) * CALCRANGE_WEEK;
    time_t todate = fromdate + CALCRANGE_WEEK;

    // 4 blocks per day

    while (todate < time) {
      ulong startidx = findLogPosition(LOG_STD, fromdate);
      for (auto &kv : sensorsMap) {
        SensorBase *sensor = kv.second;
        if (sensor->flags.enable && sensor->flags.log) {
          ulong idx = startidx;
          double data = 0;
          ulong n = 0;
          bool done = false;
          while (!done) {
            int sn = sensorlog_load2(LOG_STD, idx, BLOCKSIZE, sensorlog);
            if (sn <= 0) break;
            for (int i = 0; i < sn; i++) {
              idx++;
              if (sensorlog[i].time >= todate) {
                done = true;
                break;
              }
              if (sensorlog[i].nr == sensor->nr) {
                data += sensorlog[i].data;
                n++;
              }
            }
          }
          if (n > 0) {
            sensorlog->nr = sensor->nr;
            sensorlog->time = fromdate;
            sensorlog->data = data / (double)n;
            sensorlog->native_data = 0;
            sensorlog_add(LOG_WEEK, sensorlog);
          }
        }
      }
      fromdate += CALCRANGE_WEEK;
      todate += CALCRANGE_WEEK;
    }
    next_week_calc = todate;
    DEBUG_PRINTLN(F("calc_sensorlogs WEEK end"));
  }

  if (time >= next_month_calc) {
    log_size = sensorlog_size(LOG_WEEK);
    if (log_size <= 0) {
      if (sensorlog) free(sensorlog);
      return;
    }
    if (!sensorlog)
      sensorlog = (SensorLog_t *)malloc(sizeof(SensorLog_t) * BLOCKSIZE);

    DEBUG_PRINTLN(F("calc_sensorlogs MONTH start"));
    ulong size = sensorlog_size(LOG_MONTH);
    if (size == 0) {
      sensorlog_load(LOG_WEEK, 0, sensorlog);
      last_day = sensorlog->time;
    } else {
      sensorlog_load(LOG_MONTH, size - 1, sensorlog);  // last record
      last_day = sensorlog->time + CALCRANGE_MONTH;    // Skip last Range
    }
    time_t fromdate = (last_day / CALCRANGE_MONTH) * CALCRANGE_MONTH;
    time_t todate = fromdate + CALCRANGE_MONTH;
    // 4 blocks per day

    while (todate < time) {
      ulong startidx = findLogPosition(LOG_WEEK, fromdate);
      for (auto &kv : sensorsMap) {
        SensorBase *sensor = kv.second;
        if (sensor->flags.enable && sensor->flags.log) {
          ulong idx = startidx;
          double data = 0;
          ulong n = 0;
          bool done = false;
          while (!done) {
            int sn = sensorlog_load2(LOG_WEEK, idx, BLOCKSIZE, sensorlog);
            if (sn <= 0) break;
            for (int i = 0; i < sn; i++) {
              idx++;
              if (sensorlog[i].time >= todate) {
                done = true;
                break;
              }
              if (sensorlog[i].nr == sensor->nr) {
                data += sensorlog[i].data;
                n++;
              }
            }
          }
          if (n > 0) {
            sensorlog->nr = sensor->nr;
            sensorlog->time = fromdate;
            sensorlog->data = data / (double)n;
            sensorlog->native_data = 0;
            sensorlog_add(LOG_MONTH, sensorlog);
          }
        }
      }
      fromdate += CALCRANGE_MONTH;
      todate += CALCRANGE_MONTH;
    }
    next_month_calc = todate;
    DEBUG_PRINTLN(F("calc_sensorlogs MONTH end"));
  }
  if (sensorlog) free(sensorlog);
}

void sensor_remote_http_callback(char *) {
  // unused
}

void push_message(SensorBase *sensor) {
  if (!sensor || !sensor->last_read) return;

  static char topic[TMP_BUFFER_SIZE];
  static char payload[TMP_BUFFER_SIZE];
  char *postval = tmp_buffer;

  if (os.mqtt.enabled()) {
    DEBUG_PRINTLN("push mqtt1");
    strncpy_P(topic, PSTR("analogsensor/"), sizeof(topic) - 1);
    strncat(topic, sensor->name, sizeof(topic) - 1);
    snprintf_P(payload, TMP_BUFFER_SIZE,
              PSTR("{\"nr\":%u,\"type\":%u,\"data_ok\":%u,\"time\":%u,"
                   "\"value\":%d.%02d,\"unit\":\"%s\"}"),
              sensor->nr, sensor->type, sensor->flags.data_ok,
              sensor->last_read, (int)sensor->last_data,
              abs((int)(sensor->last_data * 100) % 100), getSensorUnit(sensor));

    if (!os.mqtt.connected()) os.mqtt.reconnect();
    os.mqtt.publish(topic, payload);
    DEBUG_PRINTLN("push mqtt2");
  }
  
  //ifttt is enabled, when the ifttt key is present!
  os.sopt_load(SOPT_IFTTT_KEY, tmp_buffer);
	bool ifttt_enabled = strlen(tmp_buffer)!=0;
  if (ifttt_enabled) {
    DEBUG_PRINTLN("push ifttt");
    strcpy_P(postval, PSTR("{\"value1\":\"On site ["));
    os.sopt_load(SOPT_DEVICE_NAME, postval + strlen(postval));
    strcat_P(postval, PSTR("], "));

    strcat_P(postval, PSTR("analogsensor "));
    snprintf_P(postval + strlen(postval), TMP_BUFFER_SIZE - strlen(postval),
              PSTR("nr: %u, type: %u, data_ok: %u, time: %u, value: %d.%02d, "
                   "unit: %s"),
              sensor->nr, sensor->type, sensor->flags.data_ok,
              sensor->last_read, (int)sensor->last_data,
              abs((int)(sensor->last_data * 100) % 100), getSensorUnit(sensor));
    strcat_P(postval, PSTR("\"}"));

    // char postBuffer[1500];
    BufferFiller bf = BufferFiller(ether_buffer, ETHER_BUFFER_SIZE_L);
    bf.emit_p(PSTR("POST /trigger/sprinkler/with/key/$O HTTP/1.0\r\n"
                   "Host: $S\r\n"
                   "Accept: */*\r\n"
                   "Content-Length: $D\r\n"
                   "Content-Type: application/json\r\n\r\n$S"),
              SOPT_IFTTT_KEY, DEFAULT_IFTTT_URL, strlen(postval), postval);

    os.send_http_request(DEFAULT_IFTTT_URL, 80, ether_buffer, sensor_remote_http_callback);
    DEBUG_PRINTLN("push ifttt2");
  }

  add_influx_data(sensor);
}

void read_all_sensors(boolean online) {
  if (sensorsMap.empty()) return;
  //DEBUG_PRINTLN(F("read_all_sensors"));

  ulong time = os.now_tz();

#ifdef ENABLE_DEBUG
  if (time < os.powerup_lasttime + 3)
#else
  if (time < os.powerup_lasttime + 30)
#endif
    return;  // wait 30s before first sensor read

  // Initialize iterator if we're starting over
  if (!current_sensor && !sensorsMap.empty()) {
    current_sensor_it = sensorsMap.begin();
    current_sensor = current_sensor_it->second;
  }

  while (current_sensor && current_sensor_it != sensorsMap.end()) {
    if (time >= current_sensor->last_read + current_sensor->read_interval ||
        current_sensor->repeat_read) {
      if (online || (current_sensor->ip == 0 && current_sensor->type != SENSOR_MQTT)) {
        int result = read_sensor(current_sensor, time);
        if (!current_sensor) {
          // Reset iterator if sensor was deleted during save
          current_sensor = NULL;
          return;
        }
        if (result == HTTP_RQT_SUCCESS) {
          current_sensor->last_read = time;
          sensorlog_add(LOG_STD, current_sensor, time);
          push_message(current_sensor);
        } else if (result == HTTP_RQT_TIMEOUT) {
          // delay next read on timeout:
          current_sensor->last_read = time + max((uint)60, current_sensor->read_interval);
          current_sensor->repeat_read = 0;
          DEBUG_PRINTF("Delayed1: %s\n", current_sensor->name);
        } else if (result == HTTP_RQT_CONNECT_ERR) {
          // delay next read on error:
          current_sensor->last_read = time + max((uint)60, current_sensor->read_interval);
          current_sensor->repeat_read = 0;
          DEBUG_PRINTF("Delayed2: %s\n", current_sensor->name);
        }
        ulong passed = os.now_tz() - time;
        if (passed > MAX_SENSOR_READ_TIME) {
          // Move to next sensor
          ++current_sensor_it;
          if (current_sensor_it != sensorsMap.end()) {
            current_sensor = current_sensor_it->second;
          } else {
            current_sensor = NULL;
          }
          break;
        }
      }
    }
    // Move to next sensor
    ++current_sensor_it;
    if (current_sensor_it != sensorsMap.end()) {
      current_sensor = current_sensor_it->second;
    } else {
      current_sensor = NULL;
    }
  }
  sensor_update_groups();
  calc_sensorlogs();
  check_monitors();
  if (time - last_save_time > 3600)  // 1h
    sensor_save();
}

#if defined(ESP8266) || defined(ESP32)
// read_sensor_adc has been moved to AsbSensor::read in sensor_asb.cpp
// extract function has been moved to sensor_remote.cpp
#endif

// read_sensor_http has been moved to RemoteSensor::read in sensor_remote.cpp

#if defined(ESP8266) || defined(ESP32)
boolean send_rs485_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  if (ip)
    return send_modbus_rtu_command(ip, port, address, reg, data, isbit);
  return send_i2c_rs485_command(address, reg, data, isbit);
}
#endif

#if defined(OSPI)
// read_sensor_rs485 and send_rs485_command have been moved to UsbRs485Sensor in sensor_usbrs485.cpp
#endif

// read_internal_raspi has been moved to InternalSensor::read in sensor_internal.cpp

/**
 * read a sensor
 */
#include "Sensor.hpp"

SensorBase* sensor_make_obj(uint type, boolean ip_based) {
  switch (type) {
    // Group sensors
    case SENSOR_GROUP_MIN:
    case SENSOR_GROUP_MAX:
    case SENSOR_GROUP_AVG:
    case SENSOR_GROUP_SUM:
      return new GroupSensor(type);

    #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
    case SENSOR_FYTA_MOISTURE:
    case SENSOR_FYTA_TEMPERATURE: {
      FytaSensor *s = new FytaSensor(type);
      return s;
    }
    #endif

    // Analog Sensor Board (ASB) sensors
#if defined(ESP8266) || defined(ESP32)
    case SENSOR_ANALOG_EXTENSION_BOARD:
    case SENSOR_ANALOG_EXTENSION_BOARD_P:
    case SENSOR_SMT50_MOIS:
    case SENSOR_SMT50_TEMP:
    case SENSOR_SMT100_ANALOG_MOIS:
    case SENSOR_SMT100_ANALOG_TEMP:
    case SENSOR_VH400:
    case SENSOR_THERM200:
    case SENSOR_AQUAPLUMB:
    case SENSOR_USERDEF:
      return new AsbSensor(type);
#endif

    // Truebner sensors (SENSOR_SMT100_*, SENSOR_TH100_*)
    case SENSOR_SMT100_MOIS:
    case SENSOR_SMT100_TEMP:
    case SENSOR_SMT100_PMTY:
    case SENSOR_TH100_MOIS:
    case SENSOR_TH100_TEMP:
      if (ip_based)
        {
          #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
          return new ModbusRtuSensor(type);
          #else
          return new GenericSensor(type);
          #endif
        }
#if defined(ESP8266) || defined(ESP32)
      if (get_asb_detected_boards() & ASB_I2C_RS485)
        return new RS485I2CSensor(type);
      if (get_asb_detected_boards() & (RS485_TRUEBNER1 | RS485_TRUEBNER2 | RS485_TRUEBNER3 | RS485_TRUEBNER4))
        return new TruebnerRS485Sensor(type);
#elif defined(OSPI)
      // On OSPI, use USB RS485 if available
      if (get_asb_detected_boards() & OSPI_USB_RS485)
        return new UsbRs485Sensor(type);
#endif
      break;

    // Generic RS485 sensor (prefer I2C RS485 if available)
    case SENSOR_RS485:
      if (get_asb_detected_boards() & ASB_I2C_RS485)
        return new RS485I2CSensor(type);
      return nullptr; // fallback to existing C implementation

#if defined(ADS1115) || defined(PCF8591)
    // OSPI analog sensors
    case SENSOR_OSPI_ANALOG:
    case SENSOR_OSPI_ANALOG_P:
    case SENSOR_OSPI_ANALOG_SMT50_MOIS:
    case SENSOR_OSPI_ANALOG_SMT50_TEMP:
#ifdef ADS1115
      return new OspiAds1115Sensor(type);
#else
      return new OspiPcf8591Sensor(type);
#endif
#endif

    // Remote HTTP sensor
    case SENSOR_REMOTE:
      #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
      return new RemoteSensor(type);
      #else
      return new GenericSensor(type);
      #endif

    // MQTT sensor
    case SENSOR_MQTT:
      #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
      return new MqttSensor(type);
      #else
      return new GenericSensor(type);
      #endif

    // Zigbee sensors
#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR)
    case SENSOR_ZIGBEE:
      return new ZigbeeSensor(type);
#endif
    // BLE sensors
#if defined(OSPI) || defined(ESP32)
    case SENSOR_BLE:
#if defined(OSPI)
      return new OspiBLESensor(type);
#else
      return new BLESensor(type);
#endif
#endif
    // Internal system sensors
#if defined(ESP8266) || defined(ESP32)
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE:
      return new InternalSensor(type);
#endif
#if defined(OSPI)
    case SENSOR_OSPI_INTERNAL_TEMP:
      return new InternalSensor(type);
#endif

    // Weather sensors
    case SENSOR_WEATHER_TEMP_F:
    case SENSOR_WEATHER_TEMP_C:
    case SENSOR_WEATHER_HUM:
    case SENSOR_WEATHER_PRECIP_IN:
    case SENSOR_WEATHER_PRECIP_MM:
    case SENSOR_WEATHER_WIND_MPH:
    case SENSOR_WEATHER_WIND_KMH:
    case SENSOR_WEATHER_ETO:
    case SENSOR_WEATHER_RADIATION:
      #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
      return new WeatherSensor(type);
      #else
      return new GenericSensor(type);
      #endif
  }
  return new GenericSensor(type);
}

int read_sensor(SensorBase *sensor, ulong time) {
  if (!sensor || !sensor->flags.enable) return HTTP_RQT_NOT_RECEIVED;

  return sensor->read(time);
}

/**
 * @brief Update group values
 *
 */
void sensor_update_groups() {
  ulong time = os.now_tz();

  for (auto &kv : sensorsMap) {
    SensorBase *sensor = kv.second;
    if (time >= sensor->last_read + sensor->read_interval) {
      switch (sensor->type) {
        case SENSOR_GROUP_MIN:
        case SENSOR_GROUP_MAX:
        case SENSOR_GROUP_AVG:
        case SENSOR_GROUP_SUM: {
          uint nr = sensor->nr;
          double value = 0;
          int n = 0;
          for (auto &kv2 : sensorsMap) {
            SensorBase *group = kv2.second;
            if (group->nr != nr && group->group == nr &&
                group->flags.enable) {  // && group->flags.data_ok) {
              switch (sensor->type) {
                case SENSOR_GROUP_MIN:
                  if (n++ == 0)
                    value = group->last_data;
                  else if (group->last_data < value)
                    value = group->last_data;
                  break;
                case SENSOR_GROUP_MAX:
                  if (n++ == 0)
                    value = group->last_data;
                  else if (group->last_data > value)
                    value = group->last_data;
                  break;
                case SENSOR_GROUP_AVG:
                case SENSOR_GROUP_SUM:
                  n++;
                  value += group->last_data;
                  break;
              }
            }
          }
          if (sensor->type == SENSOR_GROUP_AVG && n > 0) {
            value = value / (double)n;
          }
          sensor->last_data = value;
          sensor->last_native_data = 0;
          sensor->last_read = time;
          sensor->flags.data_ok = n > 0;
          sensorlog_add(LOG_STD, sensor, time);
          break;
        }
      }
    }
  }
}

/**
 * @brief Set SMT100 Sensor address
 *
 * @param sensor
 * @return int
 */
int set_sensor_address(SensorBase *sensor, uint8_t new_address) {
  if (!sensor) return HTTP_RQT_NOT_RECEIVED;
  return sensor->setAddress(new_address);
}

double calc_linear(ProgSensorAdjust *p, double sensorData) {
  //   min max  factor1 factor2
  //   10..90 -> 5..1 factor1 > factor2
  //    a   b    c  d
  //   (b-sensorData) / (b-a) * (c-d) + d
  //
  //   10..90 -> 1..5 factor1 < factor2
  //    a   b    c  d
  //   (sensorData-a) / (b-a) * (d-c) + c

  // Limit to min/max:
  if (sensorData < p->min) sensorData = p->min;
  if (sensorData > p->max) sensorData = p->max;

  // Calculate:
  double div = (p->max - p->min);
  if (abs(div) < 0.00001) return 0;

  if (p->factor1 > p->factor2) {  // invers scaling factor:
    return (p->max - sensorData) / div * (p->factor1 - p->factor2) + p->factor2;
  } else {  // upscaling factor:
    return (sensorData - p->min) / div * (p->factor2 - p->factor1) + p->factor1;
  }
}

double calc_digital_min(ProgSensorAdjust *p, double sensorData) {
  return sensorData <= p->min ? p->factor1 : p->factor2;
}

double calc_digital_max(ProgSensorAdjust *p, double sensorData) {
  return sensorData >= p->max ? p->factor2 : p->factor1;
}

double calc_digital_minmax(ProgSensorAdjust *p, double sensorData) {
  if (sensorData <= p->min) return p->factor1;
  if (sensorData >= p->max) return p->factor1;
  return p->factor2;
}
/**
 * @brief calculate adjustment
 *
 * @param prog
 * @return double
 */
double calc_sensor_watering(uint prog) {
  double result = 1;

  for (auto &kv : progSensorAdjustsMap) {
    ProgSensorAdjust *p = kv.second;
    if (!p) continue;
    if (p->prog - 1 == prog) {
      SensorBase *sensor = sensor_by_nr(p->sensor);
      if (sensor && sensor->flags.enable && sensor->flags.data_ok) {
        double res = calc_sensor_watering_int(p, sensor->last_data);
        result = result * res;
      }
    }
  }
  if (result < 0.0) result = 0.0;
  if (result > 20.0)  // Factor 20 is a huge value!
    result = 20.0;
  return result;
}

double calc_sensor_watering_int(ProgSensorAdjust *p, double sensorData) {
  double res = 0;
  if (!p) return res;
  switch (p->type) {
    case PROG_NONE:
      res = 1;
      break;
    case PROG_LINEAR:
      res = calc_linear(p, sensorData);
      break;
    case PROG_DIGITAL_MIN:
      res = calc_digital_min(p, sensorData);
      break;
    case PROG_DIGITAL_MAX:
      res = calc_digital_max(p, sensorData);
      break;
    case PROG_DIGITAL_MINMAX:
      res = calc_digital_minmax(p, sensorData);
      break;
    default:
      res = 0;
  }
  return res;
}

// ProgSensorAdjust JSON serialization methods
void ProgSensorAdjust::toJson(ArduinoJson::JsonObject obj) const {
  obj["nr"] = nr;
  obj["type"] = type;
  obj["sensor"] = sensor;
  obj["prog"] = prog;
  obj["factor1"] = factor1;
  obj["factor2"] = factor2;
  obj["min"] = min;
  obj["max"] = max;
  obj["name"] = name;
}

void ProgSensorAdjust::fromJson(ArduinoJson::JsonVariantConst obj) {
  nr = obj["nr"] | 0;
  type = obj["type"] | 0;
  sensor = obj["sensor"] | 0;
  prog = obj["prog"] | 0;
  factor1 = obj["factor1"] | 0.0;
  factor2 = obj["factor2"] | 0.0;
  min = obj["min"] | 0.0;
  max = obj["max"] | 0.0;
  
  const char* nameStr = obj["name"] | "";
  strncpy(name, nameStr, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
}

/**
 * @brief calculate adjustment
 *
 * @param nr
 * @return double
 */
double calc_sensor_watering_by_nr(uint nr) {
  double result = 1;
  
  auto it = progSensorAdjustsMap.find(nr);
  if (it != progSensorAdjustsMap.end()) {
    ProgSensorAdjust *p = it->second;
    if (p) {
      SensorBase *sensor = sensor_by_nr(p->sensor);
      if (sensor && sensor->flags.enable && sensor->flags.data_ok) {
        double res = 0;
        switch (p->type) {
          case PROG_NONE:
            res = 1;
            break;
          case PROG_LINEAR:
            res = calc_linear(p, sensor->last_data);
            break;
          case PROG_DIGITAL_MIN:
            res = calc_digital_min(p, sensor->last_data);
            break;
          case PROG_DIGITAL_MAX:
            res = calc_digital_max(p, sensor->last_data);
            break;
          default:
            res = 0;
        }

        result = result * res;
      }
    }
  }

  return result;
}

/**
 * @brief Define or update a program sensor adjustment from JSON configuration
 * @param json JsonDocument containing prog adjustment configuration
 * @param save if true, save to file after update (default: true)
 * @return HTTP_RQT_SUCCESS on success, HTTP_RQT_NOT_RECEIVED on error
 */
int prog_adjust_define(ArduinoJson::JsonVariantConst json, bool save) {
  if (!json.containsKey("nr")) {
    return HTTP_RQT_NOT_RECEIVED;
  }
  
  uint nr = json["nr"];
  if (nr == 0) return HTTP_RQT_NOT_RECEIVED;
  
  DEBUG_PRINTLN(F("prog_adjust_define"));
  
  // Check if type is 0 (delete request)
  if (json.containsKey("type") && json["type"].as<uint>() == 0) {
    return prog_adjust_delete(nr);
  }
  
  // Check if already exists
  auto it = progSensorAdjustsMap.find(nr);
  ProgSensorAdjust *p = nullptr;
  
  if (it != progSensorAdjustsMap.end()) {
    // Update existing
    p = it->second;
    p->fromJson(json);
  } else {
    // Create new
    p = new ProgSensorAdjust;
    p->fromJson(json);
    
    // Validate required fields
    if (p->nr == 0 || p->type == 0) {
      delete p;
      return HTTP_RQT_NOT_RECEIVED;
    }
    
    progSensorAdjustsMap[nr] = p;
  }
  
  if (save) {
    prog_adjust_save();
  }
  return HTTP_RQT_SUCCESS;
}

/**
 * @brief Legacy interface - define a program sensor adjustment with individual parameters
 * @deprecated Use prog_adjust_define(JsonVariantConst, bool) instead
 */
int prog_adjust_define(uint nr, uint type, uint sensor, uint prog,
                       double factor1, double factor2, double min, double max, char * name) {
  // Convert to JSON and call new implementation
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonObject obj = doc.to<ArduinoJson::JsonObject>();
  
  obj["nr"] = nr;
  obj["type"] = type;
  obj["sensor"] = sensor;
  obj["prog"] = prog;
  obj["factor1"] = factor1;
  obj["factor2"] = factor2;
  obj["min"] = min;
  obj["max"] = max;
  if (name && name[0] != 0) {
    obj["name"] = name;
  }
  
  return prog_adjust_define(obj, true);
}

int prog_adjust_delete(uint nr) {
  auto it = progSensorAdjustsMap.find(nr);
  if (it != progSensorAdjustsMap.end()) {
    delete it->second;
    progSensorAdjustsMap.erase(it);
    prog_adjust_save();
    return HTTP_RQT_SUCCESS;
  }
  return HTTP_RQT_NOT_RECEIVED;
}

void prog_adjust_save() {
  if (!apiInit) return;
  
  DEBUG_PRINTLN(F("prog_adjust_save"));
  
  // Delete old file if exists
  if (file_exists(PROG_SENSOR_FILENAME)) {
    remove_file(PROG_SENSOR_FILENAME);
  }
  
  // Create JSON document
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonArray array = doc.to<ArduinoJson::JsonArray>();
  
  // Serialize all prog sensor adjusts
  for (auto &kv : progSensorAdjustsMap) {
    ProgSensorAdjust *pa = kv.second;
    if (pa) {
      ArduinoJson::JsonObject obj = array.add<ArduinoJson::JsonObject>();
      pa->toJson(obj);
    }
  }
  
  // Write to file using FileWriter for buffered writing
  FileWriter writer(PROG_SENSOR_FILENAME);
  ArduinoJson::serializeJson(doc, writer);
}

void prog_adjust_load() {
  DEBUG_PRINTLN(F("prog_adjust_load"));
  
  // Clean up existing map
  for (auto &kv : progSensorAdjustsMap) {
    delete kv.second;
  }
  progSensorAdjustsMap.clear();
  
  // Check if JSON file exists
  if (!file_exists(PROG_SENSOR_FILENAME)) {
    DEBUG_PRINTLN(F("prog_adjust JSON file not found, checking for legacy"));
    // Try to load legacy binary format
    if (prog_adjust_load_legacy(progSensorAdjustsMap)) {
      DEBUG_PRINTLN(F("prog_adjust loaded from legacy binary format"));
      return;
    }
    DEBUG_PRINTLN(F("No prog_adjust data found"));
    return;
  }
  
  // Read JSON from file using FileReader for buffered reading
  FileReader reader(PROG_SENSOR_FILENAME);
  ArduinoJson::JsonDocument doc;
  ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, reader);
  
  if (error) {
    DEBUG_PRINT(F("prog_adjust_load deserializeJson() failed: "));
    DEBUG_PRINTLN(error.c_str());
    return;
  }
  
  // Parse JSON array
  if (!doc.is<ArduinoJson::JsonArray>()) {
    DEBUG_PRINTLN(F("prog_adjust JSON is not an array"));
    return;
  }
  
  ArduinoJson::JsonArray array = doc.as<ArduinoJson::JsonArray>();
  
  for (ArduinoJson::JsonVariantConst v : array) {
    ProgSensorAdjust *pa = new ProgSensorAdjust;
    pa->fromJson(v);
    
    // Skip invalid entries
    if (!pa->nr || !pa->type) {
      delete pa;
      continue;
    }
    
    // Add to map
    progSensorAdjustsMap[pa->nr] = pa;
  }
  
  DEBUG_PRINT(F("Loaded "));
  DEBUG_PRINT(prog_adjust_count());
  DEBUG_PRINTLN(F(" prog adjustments"));
}

uint prog_adjust_count() {
  return progSensorAdjustsMap.size();
}

ProgSensorAdjust *prog_adjust_by_nr(uint nr) {
  auto it = progSensorAdjustsMap.find(nr);
  if (it != progSensorAdjustsMap.end()) {
    return it->second;
  }
  return NULL;
}

ProgSensorAdjust *prog_adjust_by_idx(uint idx) {
  if (idx >= progSensorAdjustsMap.size()) return NULL;
  
  auto it = progSensorAdjustsMap.begin();
  std::advance(it, idx);
  return it->second;
}

#if defined(ESP8266)
ulong diskFree() {
  struct FSInfo fsinfo;
  LittleFS.info(fsinfo);
  return fsinfo.totalBytes - fsinfo.usedBytes;
}
#elif defined(ESP32)
ulong diskFree() {
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}
#endif

bool checkDiskFree() {
  if (diskFree() < MIN_DISK_FREE) {
    DEBUG_PRINT(F("fs has low space!"));
    return false;
  }
  return true;
}

// SensorBase default emitJson implementation
void SensorBase::emitJson(BufferFiller& bfill) const {
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonObject obj = doc.to<ArduinoJson::JsonObject>();
  toJson(obj);
  
  // Serialize to string and output
  String jsonStr;
  ArduinoJson::serializeJson(doc, jsonStr);
  bfill.emit_p(PSTR("$S"), jsonStr.c_str());
}

// SensorBase default implementation for getUnitId()
// Most sensor types override this method with their own implementation
unsigned char SensorBase::getUnitId() const {
  // Default fallback for unknown types
  // Group sensors have their own override in GroupSensor class
  return UNIT_NONE;
}

const char* SensorBase::getUnit() const {
  int unitid = getUnitId();
  if (unitid == UNIT_USERDEF) return userdef_unit;
  if (unitid < 0 || (uint16_t)unitid >= sizeof(sensor_unitNames))
    return sensor_unitNames[0];
  return sensor_unitNames[unitid];
}

// Wrapper functions for backward compatibility
const char *getSensorUnit(int unitid) {
  if (unitid == UNIT_USERDEF) return "?";
  if (unitid < 0 || (uint16_t)unitid >= sizeof(sensor_unitNames))
    return sensor_unitNames[0];
  return sensor_unitNames[unitid];
}

const char *getSensorUnit(SensorBase *sensor) {
  if (!sensor) return sensor_unitNames[0];
  return sensor->getUnit();
}

boolean sensor_isgroup(const SensorBase *sensor) {
  if (!sensor) return false;

  switch (sensor->type) {
    case SENSOR_GROUP_MIN:
    case SENSOR_GROUP_MAX:
    case SENSOR_GROUP_AVG:
    case SENSOR_GROUP_SUM:
      return true;

    default:
      return false;
  }
}

// Wrapper for backward compatibility - delegates to sensor type
unsigned char getSensorUnitId(int type) {
  // Create temporary sensor to get unit ID for type
  GenericSensor temp(type);
  return temp.getUnitId();
}

unsigned char getSensorUnitId(SensorBase *sensor) {
  if (!sensor) return UNIT_NONE;
  return sensor->getUnitId();
}

/**
 * @brief Write data to influx db
 * 
 * @param sensor 
 * @param log 
 */
void add_influx_data(SensorBase *sensor) {
  if (!os.influxdb.isEnabled())
    return;

  #if defined(ESP8266) || defined(ESP32)
  Point sensor_data("analogsensor");
  os.sopt_load(SOPT_DEVICE_NAME, tmp_buffer);
  sensor_data.addTag("devicename", tmp_buffer);
  snprintf(tmp_buffer, 10, "%d", sensor->nr);
  sensor_data.addTag("nr", tmp_buffer);
  sensor_data.addTag("name", sensor->name);
  sensor_data.addTag("unit", getSensorUnit(sensor));

  sensor_data.addField("native_data", sensor->last_native_data);
  sensor_data.addField("data", sensor->last_data);

  os.influxdb.write_influx_data(sensor_data);

  #else

/*
influxdb_cpp::server_info si("127.0.0.1", 8086, "db", "usr", "pwd");
influxdb_cpp::builder()
    .meas("foo")
    .tag("k", "v")
    .tag("x", "y")
    .field("x", 10)
    .field("y", 10.3, 2)
    .field("z", 10.3456)
    .field("b", !!10)
    .timestamp(1512722735522840439)
    .post_http(si);
*/
  influxdb_cpp::server_info * client = os.influxdb.get_client();
  if (!client)
    return;

  os.sopt_load(SOPT_DEVICE_NAME, tmp_buffer);
  char nr_buf[10];
  snprintf(nr_buf, 10, "%d", sensor->nr);
  influxdb_cpp::builder()
    .meas("analogsensor")
    .tag("devicename", tmp_buffer)
    .tag("nr", nr_buf)
    .tag("name", sensor->name)
    .tag("unit", getSensorUnit(sensor))
    .field("native_data", (long)sensor->last_native_data)
    .field("data", sensor->last_data, 2)
    .timestamp(millis())
    .post_http(*client);

  #endif
}


//Value Monitoring

/**
 * @brief Serialize Monitor to JSON
 */
void Monitor::toJson(ArduinoJson::JsonObject obj) const {
  obj["nr"] = nr;
  obj["type"] = type;
  obj["sensor"] = sensor;
  obj["prog"] = prog;
  obj["zone"] = zone;
  obj["active"] = active;
  obj["time"] = time;
  obj["name"] = name;
  obj["maxRuntime"] = maxRuntime;
  obj["prio"] = prio;
  obj["reset_seconds"] = reset_seconds;
  
  // Serialize Monitor_Union based on type
  ArduinoJson::JsonObject mObj = obj["m"].to<ArduinoJson::JsonObject>();
  switch(type) {
    case MONITOR_MIN:
    case MONITOR_MAX:
      mObj["value1"] = m.minmax.value1;
      mObj["value2"] = m.minmax.value2;
      break;
    case MONITOR_SENSOR12:
      mObj["sensor12"] = m.sensor12.sensor12;
      mObj["invers"] = m.sensor12.invers;
      break;
    case MONITOR_SET_SENSOR12:
      mObj["monitor"] = m.set_sensor12.monitor;
      mObj["sensor12"] = m.set_sensor12.sensor12;
      break;
    case MONITOR_AND:
    case MONITOR_OR:
    case MONITOR_XOR:
      mObj["monitor1"] = m.andorxor.monitor1;
      mObj["monitor2"] = m.andorxor.monitor2;
      mObj["monitor3"] = m.andorxor.monitor3;
      mObj["monitor4"] = m.andorxor.monitor4;
      mObj["invers1"] = m.andorxor.invers1;
      mObj["invers2"] = m.andorxor.invers2;
      mObj["invers3"] = m.andorxor.invers3;
      mObj["invers4"] = m.andorxor.invers4;
      break;
    case MONITOR_NOT:
      mObj["monitor"] = m.mnot.monitor;
      break;
    case MONITOR_TIME:
      mObj["time_from"] = m.mtime.time_from;
      mObj["time_to"] = m.mtime.time_to;
      mObj["weekdays"] = m.mtime.weekdays;
      break;
    case MONITOR_REMOTE:
      mObj["rmonitor"] = m.remote.rmonitor;
      mObj["ip"] = m.remote.ip;
      mObj["port"] = m.remote.port;
      break;
  }
}

/**
 * @brief Deserialize Monitor from JSON
 */
void Monitor::fromJson(ArduinoJson::JsonVariantConst obj) {
  nr = obj["nr"] | 0;
  type = obj["type"] | 0;
  sensor = obj["sensor"] | 0;
  prog = obj["prog"] | 0;
  zone = obj["zone"] | 0;
  active = obj["active"] | false;
  time = obj["time"] | 0;
  strncpy(name, obj["name"] | "", sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  maxRuntime = obj["maxRuntime"] | 0;
  prio = obj["prio"] | 0;
  reset_seconds = obj["reset_seconds"] | 0;
  reset_time = 0;
  
  // Deserialize Monitor_Union based on type
  memset(&m, 0, sizeof(Monitor_Union_t));
  ArduinoJson::JsonVariantConst mVar = obj["m"];
  if (!mVar.isNull()) {
    switch(type) {
      case MONITOR_MIN:
      case MONITOR_MAX:
        m.minmax.value1 = mVar["value1"] | 0.0;
        m.minmax.value2 = mVar["value2"] | 0.0;
        break;
      case MONITOR_SENSOR12:
        m.sensor12.sensor12 = mVar["sensor12"] | 0;
        m.sensor12.invers = mVar["invers"] | false;
        break;
      case MONITOR_SET_SENSOR12:
        m.set_sensor12.monitor = mVar["monitor"] | 0;
        m.set_sensor12.sensor12 = mVar["sensor12"] | 0;
        break;
      case MONITOR_AND:
      case MONITOR_OR:
      case MONITOR_XOR:
        m.andorxor.monitor1 = mVar["monitor1"] | 0;
        m.andorxor.monitor2 = mVar["monitor2"] | 0;
        m.andorxor.monitor3 = mVar["monitor3"] | 0;
        m.andorxor.monitor4 = mVar["monitor4"] | 0;
        m.andorxor.invers1 = mVar["invers1"] | false;
        m.andorxor.invers2 = mVar["invers2"] | false;
        m.andorxor.invers3 = mVar["invers3"] | false;
        m.andorxor.invers4 = mVar["invers4"] | false;
        break;
      case MONITOR_NOT:
        m.mnot.monitor = mVar["monitor"] | 0;
        break;
      case MONITOR_TIME:
        m.mtime.time_from = mVar["time_from"] | 0;
        m.mtime.time_to = mVar["time_to"] | 0;
        m.mtime.weekdays = mVar["weekdays"] | 0;
        break;
      case MONITOR_REMOTE:
        m.remote.rmonitor = mVar["rmonitor"] | 0;
        m.remote.ip = mVar["ip"] | 0;
        m.remote.port = mVar["port"] | 0;
        break;
    }
  }
}

void monitor_load() {
  DEBUG_PRINTLN(F("monitor_load"));
  
  // Clean up existing map
  for (auto &kv : monitorsMap) {
    delete kv.second;
  }
  monitorsMap.clear();
  
  // Check if JSON file exists
  if (!file_exists(MONITOR_FILENAME)) {
    DEBUG_PRINTLN(F("monitor JSON file not found, checking for legacy"));
    // Try to load legacy binary format
    if (monitor_load_legacy(monitorsMap)) {
      DEBUG_PRINTLN(F("monitor loaded from legacy binary format"));
      return;
    }
    DEBUG_PRINTLN(F("No monitor data found"));
    return;
  }
  
  // Read JSON from file using FileReader
  FileReader reader(MONITOR_FILENAME);
  ArduinoJson::JsonDocument doc;
  ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, reader);
  
  if (error) {
    DEBUG_PRINT(F("monitor_load deserializeJson() failed: "));
    DEBUG_PRINTLN(error.c_str());
    return;
  }
  
  // Parse JSON array
  if (!doc.is<ArduinoJson::JsonArray>()) {
    DEBUG_PRINTLN(F("monitor JSON is not an array"));
    return;
  }
  
  ArduinoJson::JsonArray array = doc.as<ArduinoJson::JsonArray>();
  
  for (ArduinoJson::JsonVariantConst v : array) {
    Monitor_t *mon = new Monitor_t;
    mon->fromJson(v);
    
    // Skip invalid entries
    if (!mon->nr || !mon->type) {
      delete mon;
      continue;
    }
    
    // Add to map
    monitorsMap[mon->nr] = mon;
  }
  
  DEBUG_PRINT(F("Loaded "));
  DEBUG_PRINT(monitor_count());
  DEBUG_PRINTLN(F(" monitors"));
}

void monitor_save() {
  if (!apiInit) return;
  
  DEBUG_PRINTLN(F("monitor_save"));
  
  // Delete old file if exists
  if (file_exists(MONITOR_FILENAME)) {
    remove_file(MONITOR_FILENAME);
  }
  
  // Create JSON document
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonArray array = doc.to<ArduinoJson::JsonArray>();
  
  // Serialize all monitors
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    if (mon) {
      ArduinoJson::JsonObject obj = array.add<ArduinoJson::JsonObject>();
      mon->toJson(obj);
    }
  }
  
  // Write to file using FileWriter for buffered writing
  FileWriter writer(MONITOR_FILENAME);
  ArduinoJson::serializeJson(doc, writer);
}

int monitor_count() {
  return monitorsMap.size();
}

int monitor_delete(uint nr) {
  auto it = monitorsMap.find(nr);
  if (it != monitorsMap.end()) {
    delete it->second;
    monitorsMap.erase(it);
    monitor_save();
    return HTTP_RQT_SUCCESS;
  }
  return HTTP_RQT_NOT_RECEIVED;
}

bool monitor_define(uint nr, uint type, uint sensor, uint prog, uint zone, const Monitor_Union_t m, char * name, ulong maxRuntime, uint8_t prio, ulong reset_seconds) {
  // Find or create monitor
  auto it = monitorsMap.find(nr);
  Monitor_t *p;
  
  if (it != monitorsMap.end()) {
    // Update existing monitor
    p = it->second;
    p->type = type;
    p->sensor = sensor;
    p->prog = prog;
    p->zone = zone;
    p->m = m;
    //p->active = false;
    p->maxRuntime = maxRuntime;
    p->prio = prio;
    p->reset_time = 0;
    p->reset_seconds = reset_seconds;
    strncpy(p->name, name, sizeof(p->name)-1);
  } else {
    // Create new monitor
    p = new Monitor_t;
    p->nr = nr;
    p->type = type;
    p->sensor = sensor;
    p->prog = prog;
    p->zone = zone;
    p->m = m;
    p->active = false;
    p->maxRuntime = maxRuntime;
    p->prio = prio;
    p->reset_time = 0;
    p->reset_seconds = reset_seconds;
    strncpy(p->name, name, sizeof(p->name)-1);
    
    monitorsMap[nr] = p;
  }

  monitor_save();
  check_monitors();
  return HTTP_RQT_SUCCESS;
}

Monitor_t *monitor_by_nr(uint nr) {
  auto it = monitorsMap.find(nr);
  if (it != monitorsMap.end()) {
    return it->second;
  }
  return NULL;
}

Monitor_t *monitor_by_idx(uint idx) {
  if (idx >= monitorsMap.size()) return NULL;
  
  auto it = monitorsMap.begin();
  std::advance(it, idx);
  return it->second;
}

void start_monitor_action(Monitor_t * mon) {
  mon->time = os.now_tz();
  if (mon->prog > 0)
    manual_start_program(mon->prog, 255, QUEUE_OPTION_APPEND);

  DEBUG_PRINTLN(F("start_monitor_action"));
  DEBUG_PRINT(F("Zone: "));
  DEBUG_PRINTLN(mon->zone);
  DEBUG_PRINT(F("Max Runtime: "));
  DEBUG_PRINTLN(mon->maxRuntime);

  if (mon->zone > 0) {
    uint sid = mon->zone-1;

		// schedule manual station
		// skip if the station is a master station
		// (because master cannot be scheduled independently)
		if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
			return;

    uint16_t timer=mon->maxRuntime;
    RuntimeQueueStruct *q = NULL;
		unsigned char sqi = pd.station_qid[sid];
		// check if the station already has a schedule
		if (sqi!=0xFF) {  // if so, we will overwrite the schedule
			q = pd.queue+sqi;
		} else {  // otherwise create a new queue element
			q = pd.enqueue();
		}
		// if the queue is not full
    DEBUG_PRINTLN(F("start_monitor_action: queue not full"));
		if (q) {
			q->st = 0;
			q->dur = timer;
			q->sid = sid;
			q->pid = 253;
			schedule_all_stations(mon->time);
      DEBUG_PRINTLN(F("start_monitor_action: schedule_all_stations"));
		} 
  }
}

void stop_monitor_action(Monitor_t * mon) {
  mon->time = os.now_tz();
  if (mon->zone > 0) {
    int sid = mon->zone-1;
    RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
    if (q) {
		  q->deque_time = mon->time;
		  turn_off_station(sid, mon->time, 0);
      DEBUG_PRINTLN(F("stop_monitor_action: turn_off_station"));
    }
  }
}

void push_message(Monitor_t * mon, float value, int monidx) {
  uint16_t type; 
  switch(mon->prio) {
    case 0: type = NOTIFY_MONITOR_LOW; break;
    case 1: type = NOTIFY_MONITOR_MID; break;
    case 2: type = NOTIFY_MONITOR_HIGH; break;
    default: return;
  }
  char name[30];
  strncpy(name, mon->name, sizeof(name)-1);
  DEBUG_PRINT("monitoring: activated ");
  DEBUG_PRINT(name);
  DEBUG_PRINT(" - ");
  DEBUG_PRINTLN(type);
  notif.add(type, (uint32_t)mon->prio, value, (uint8_t)monidx);
}

bool get_monitor(uint nr, bool inv, bool defaultBool) {
  Monitor_t *mon = monitor_by_nr(nr);
  if (!mon) return defaultBool;
  return inv ? !mon->active : mon->active;
}

bool get_remote_monitor(Monitor_t *mon, bool defaultBool) {
#if defined(ESP8266) || defined(ESP32)
  IPAddress _ip(mon->m.remote.ip);
  unsigned char ip[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
#else
  unsigned char ip[4];
  ip[3] = (unsigned char)((mon->m.remote.ip >> 24) & 0xFF);
  ip[2] = (unsigned char)((mon->m.remote.ip >> 16) & 0xFF);
  ip[1] = (unsigned char)((mon->m.remote.ip >> 8) & 0xFF);
  ip[0] = (unsigned char)((mon->m.remote.ip & 0xFF));
#endif

  DEBUG_PRINTLN(F("read_monitor_http"));

  char *p = tmp_buffer;
  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);

  bf.emit_p(PSTR("GET /ml?pw=$O&nr=$D"), SOPT_PASSWORD, mon->m.remote.rmonitor);
  bf.emit_p(PSTR(" HTTP/1.0\r\nHOST: $D.$D.$D.$D\r\n\r\n"), ip[0], ip[1], ip[2], ip[3]);

  DEBUG_PRINTLN(p);

  char server[20];
  sprintf(server, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  int res = os.send_http_request(server, mon->m.remote.port, p, NULL, false, 500);
  if (res == HTTP_RQT_SUCCESS) {
    DEBUG_PRINTLN("Send Ok");
    p = ether_buffer;
    DEBUG_PRINTLN(p);

    char buf[20];
    char *s = strstr(p, "\"time\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      ulong time = strtoul(buf, NULL, 0);
      if (time == 0 || time == mon->time) {
        return defaultBool;
      } else {
        mon->time = time;
      }
    }

    s = strstr(p, "\"active\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      return strtoul(buf, NULL, 0);
    }

    return HTTP_RQT_SUCCESS;
  }
  return defaultBool;
}

void check_monitors() {
  //DEBUG_PRINTLN(F("check_monitors"));
  time_os_t timeNow = os.now_tz();

  int monidx = 0;
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    uint nr = mon->nr;

    bool wasActive = mon->active;
    double value = 0;

    switch(mon->type) {
      case MONITOR_MIN: 
      case MONITOR_MAX: {
       SensorBase * sensor = sensor_by_nr(mon->sensor);
        if (sensor && sensor->flags.data_ok) {
          value = sensor->last_data;

          if (!mon->active) {
            if ((mon->type == MONITOR_MIN && value <= mon->m.minmax.value1) || 
              (mon->type == MONITOR_MAX && value >= mon->m.minmax.value1)) {
              mon->active = true;
            }
          } else {
            if ((mon->type == MONITOR_MIN && value >= mon->m.minmax.value2) || 
              (mon->type == MONITOR_MAX && value <= mon->m.minmax.value2)) {
              mon->active = false;
            }
          }
        }
        break; }

      case MONITOR_SENSOR12:
        if (mon->m.sensor12.sensor12 == 1)        
      	  if (os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_SOIL)
            mon->active = mon->m.sensor12.invers? !os.status.sensor1_active : os.status.sensor1_active;
        if (mon->m.sensor12.sensor12 == 2)
          if (os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_SOIL)
            mon->active = mon->m.sensor12.invers? !os.status.sensor2_active : os.status.sensor2_active;
        break;

      case MONITOR_SET_SENSOR12:
        mon->active = get_monitor(mon->m.set_sensor12.monitor, false, false);
        if (mon->m.set_sensor12.sensor12 == 1) {
          os.status.forced_sensor1 = mon->active;
        }
        if (mon->m.set_sensor12.sensor12 == 2) {
          os.status.forced_sensor2 = mon->active;
        }
        break;
      case MONITOR_AND:
        mon->active = get_monitor(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, true) &&
          get_monitor(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, true) &&
          get_monitor(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, true) &&
          get_monitor(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, true);          
        break;
      case MONITOR_OR:
        mon->active = get_monitor(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, false) ||
          get_monitor(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, false) ||
          get_monitor(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, false) ||
          get_monitor(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, false);
        break;
      case MONITOR_XOR:
        mon->active = get_monitor(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, false) ^
          get_monitor(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, false) ^
          get_monitor(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, false) ^
          get_monitor(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, false);
        break;
      case MONITOR_NOT:
        mon->active = get_monitor(mon->m.mnot.monitor, true, false);
        break;
      case MONITOR_TIME: {
        uint16_t time = hour(timeNow) * 100 + minute(timeNow); //HHMM
#if defined(ARDUINO)       
        uint8_t wday = (weekday(timeNow)+5)%7; //Monday = 0
#else
        time_os_t ct = timeNow;
	struct tm *ti = gmtime(&ct);
	uint8_t wday = (ti->tm_wday+1)%7; 
#endif
        mon->active  = (mon->m.mtime.weekdays >> wday) & 0x01;
        if (mon->m.mtime.time_from > mon->m.mtime.time_to) // FROM > TO ? Over night value
          mon->active &= time >= mon->m.mtime.time_from || time <= mon->m.mtime.time_to;
        else
          mon->active &= time >= mon->m.mtime.time_from && time <= mon->m.mtime.time_to;
        break;
      }
      case MONITOR_REMOTE:
        mon->active = get_remote_monitor(mon, wasActive);
        break;
    }

    if (mon->active != wasActive) {
      DEBUG_PRINT(F("Monitor "));
      DEBUG_PRINT(mon->nr);
      DEBUG_PRINT(F(" changed from "));
      DEBUG_PRINT(wasActive ? "active" : "inactive");
      DEBUG_PRINT(F(" to "));
      DEBUG_PRINTLN(mon->active ? "active" : "inactive");
      if (mon->active) {
        if (mon->reset_seconds > 0) {
          mon->reset_time = timeNow + mon->reset_seconds; 
        } else {
          mon->reset_time = 0;
        }
        start_monitor_action(mon);
        push_message(mon, value, monidx);
        mon = monitor_by_nr(nr); //restart because if send by mail we unloaded+reloaded the monitors
      } else {
        stop_monitor_action(mon);
      }
    } else if (mon->active) {
      if (mon->reset_time > 0 && mon->reset_time < timeNow) { //time is over
        mon->active = false;
        DEBUG_PRINT(F("Monitor "));
        DEBUG_PRINT(mon->nr);
        DEBUG_PRINT(F(" time is over at "));
        DEBUG_PRINTLN(timeNow);
        stop_monitor_action(mon);
        mon->reset_time = timeNow + mon->reset_seconds; 
      } else if (mon->reset_time == 0 && mon->reset_seconds > 0) { //reset time not set, but reset seconds is set
        mon->reset_time = timeNow + mon->reset_seconds; 
      }
    }

    monidx++;
  }
}

void replace_pid(uint old_pid, uint new_pid) {
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    if (mon->prog == old_pid) {
      DEBUG_PRINT(F("replace_pid: "));
      DEBUG_PRINT(old_pid);
      DEBUG_PRINT(F(" with "));
      DEBUG_PRINTLN(new_pid);
      mon->prog = new_pid;
    }
  }
  for (auto &kv : progSensorAdjustsMap) {
    ProgSensorAdjust *psa = kv.second;
    if (psa && psa->prog == old_pid) {
      DEBUG_PRINT(F("replace_pid psa: "));
      DEBUG_PRINT(old_pid);
      DEBUG_PRINT(F(" with "));
      DEBUG_PRINTLN(new_pid);
      psa->prog = new_pid;
    }
  }
  sensor_save_all();
}

char *strnlstr(const char *haystack, const char *needle, size_t needle_len, size_t len)
{
  int i;
  for (i=0; i<=(int)(len-needle_len); i++)
  {
		if (haystack[0] == 0)
			break;
    if ((haystack[0] == needle[0]) &&
        (strncmp(haystack, needle, needle_len) == 0))
            return (char *)haystack;
    haystack++;
  }
  return NULL;
}

int findValue(const char *payload, unsigned int length, const char *jsonFilter, double& value) {
	char *p = (char*)payload;				
	char *f = (char*)jsonFilter;
	bool emptyFilter = !jsonFilter||!jsonFilter[0];

	while (!emptyFilter && f && p) {
		f = strstr((char*)jsonFilter, "|");
		if (f) {
			p = strnlstr(p, jsonFilter, f-jsonFilter, (char*)payload-p+length);
			jsonFilter = f+1;
		} else {
			p = strstr(p, jsonFilter);
		}
	}
	if (p) {
		p += emptyFilter?0:strlen(jsonFilter);
		char buf[30];
		p = strpbrk(p, "0123456789.-+nullNULL");
		uint i = 0;
		while (p && i < sizeof(buf) && p < (char*)payload+length) {
			char ch = *p++;
			if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') {
				buf[i++] = ch;
			} else break;
		}
		buf[i] = 0;
		DEBUG_PRINT("result: ");
		DEBUG_PRINTLN(buf);	

		value = -9999;
		return sscanf(buf, "%lf", &value);
	}
	return 0;
}

int findString(const char *payload, unsigned int length, const char *jsonFilter, String& value) {
	char *p = (char *)payload;				
	char *f = (char *)jsonFilter;
	bool emptyFilter = !jsonFilter||!jsonFilter[0];

	while (!emptyFilter && f && p) {
		f = strstr((char*)jsonFilter, "|");
		if (f) {
			p = strnlstr(p, jsonFilter, f-jsonFilter, (char*)payload-p+length);
			jsonFilter = f+1;
		} else {
			p = strstr(p, jsonFilter);
		}
	}
  value = "";
	if (p) {
		p += emptyFilter?0:strlen(jsonFilter)+1;

    p = strchr(p, '\"');
    if (p) {
      p++;
      char *q = strchr(p, '\"');
      if (q) {
#if defined(ESP8266) || defined(ESP32)
        value.concat(p, q-p);
#else
        value.assign(p, q-p);
#endif
        return 1;
      }
    }
	}
	return 0;
}
