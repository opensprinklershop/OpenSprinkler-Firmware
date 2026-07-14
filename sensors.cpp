/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
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
#include "SensorBase.hpp"
#include "sensors_util.h"
#include "main.h"
#include "TimeLib.h"
#include <new>
#include <stdlib.h>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include "OpenSprinkler.h"
#if defined(ESP8266) || defined(ESP32)
#include "Wire.h"
#if defined(ESP32)
#include <WiFi.h>
#endif
#elif defined(OSPI)
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <modbus/modbus.h>
#include <modbus/modbus-rtu.h>
#include <errno.h>
#include <sys/statvfs.h>
#else
#include <stdio.h>
#include <iostream>
#include <fstream>
#endif
#include "defines.h"
#include "opensprinkler_server.h"
#include "program.h"
#include "SensorBase.hpp"
#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_mqtt.h"
#endif

#if defined(ESP32C5)
  #include "ieee802154_config.h"
  #if defined(OS_ENABLE_ZIGBEE)
    #include "sensor_zigbee.h"
  #endif
#endif

#if defined(ESP32)
  #include "sensor_ble.h"
#endif

#if defined(OSPI)
  #include "sensor_ospi_ble.h"
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  #include "sensor_fyta.h"
#if defined(ESP32) || defined(OSPI)
  #include "sensor_gardena.h"
#endif
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
  #include "sensor_remote_json.h"
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

#ifdef ESP32
#include "esp_ieee802154.h"
#endif


unsigned char findKeyVal(const char *str, char *strbuf, uint16_t maxlen, const char *key,
                bool key_in_pgm = false, uint8_t *keyfound = NULL);

// All sensors (map nr -> SensorBase*)
// NOTE: std::map cannot use EXT_RAM_BSS_ATTR - has internal tree pointers that need constructor
static std::map<uint, SensorBase*> sensorsMap;
static time_t last_save_time = 0;
static boolean apiInit = false;
static SensorBase * current_sensor = NULL;
// NOTE: std::map::iterator cannot use EXT_RAM_BSS_ATTR - has internal pointers
static std::map<uint, SensorBase*>::iterator current_sensor_it;

// Factory forward declaration
SensorBase* sensor_make_obj(uint type, boolean ip_based);

// Boards:
static uint16_t asb_detected_boards = 0;  // bit 1=0x48+0x49 bit 2=0x4A+0x4B usw

// Program sensor data (HashMap for efficient lookup by nr)
// NOTE: std::map cannot use EXT_RAM_BSS_ATTR - has internal tree pointers
static std::map<uint, ProgSensorAdjust*> progSensorAdjustsMap;

// Monitor data (HashMap for efficient lookup by nr)
// NOTE: std::map cannot use EXT_RAM_BSS_ATTR - has internal tree pointers
static std::map<uint, Monitor*> monitorsMap;

static const unsigned char MAX_SENSOR_UNITNAMES = 18;
const char *sensor_unitNames[]{
  "",  "%", "°C", "°F", "V", "%", "in", "mm", "mph", "kmh", "%", "DK", "LM", "LX", "L", "gal", "L Verbrauch", "gal Verbrauch"
  //0   1     2     3    4    5    6     7      8      9     10,  11,   12,   13,  14,  15,    16,            17
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
    //  14=L  (Liter, absolute meter counter)
    //  15=gal (Gallon, absolute meter counter)
    //  16=L Verbrauch (relative consumption)
    //  17=gal Verbrauch (relative consumption)
};
uint8_t logFileSwitch[3] = {0, 0, 0};  // 0=use smaller File, 1=LOG1, 2=LOG2

extern volatile ulong flow_count;

static bool sensor_unit_is_water_absolute(uint8_t unitid) {
  return unitid == UNIT_LITER || unitid == UNIT_GALLON;
}

static bool sensor_unit_is_water_consumption(uint8_t unitid) {
  return unitid == UNIT_LITER_CONSUMPTION || unitid == UNIT_GALLON_CONSUMPTION;
}

static bool sensor_unit_is_water_volume(uint8_t unitid) {
  return sensor_unit_is_water_absolute(unitid) || sensor_unit_is_water_consumption(unitid);
}

static uint8_t sensor_consumption_unit_for(uint8_t unitid) {
  if (unitid == UNIT_GALLON || unitid == UNIT_GALLON_CONSUMPTION) return UNIT_GALLON_CONSUMPTION;
  return UNIT_LITER_CONSUMPTION;
}

static double flow_pulse_volume() {
  return (double)os.get_flow_volume_per_pulse();
}

class FlowPulseSensor : public SensorBase {
public:
  explicit FlowPulseSensor(uint type) : SensorBase(type) {}

  virtual int read(unsigned long time) override {
    (void)time;
    uint32_t current_count = (uint32_t)flow_count;
    if (last_read == 0 || current_count < last_native_data) {
      last_native_data = current_count;
      last_data = 0;
      flags.data_ok = 1;
      return HTTP_RQT_SUCCESS;
    }

    uint32_t delta = current_count - last_native_data;
    last_native_data = current_count;
    last_data = (double)delta * flow_pulse_volume();
    flags.data_ok = 1;
    return HTTP_RQT_SUCCESS;
  }

  virtual unsigned char getUnitId() const override {
    if (assigned_unitid == UNIT_GALLON_CONSUMPTION || assigned_unitid == UNIT_GALLON) return UNIT_GALLON_CONSUMPTION;
    return UNIT_LITER_CONSUMPTION;
  }
};

static void sensor_standard_waterlog_add(SensorBase *sensor, ulong time) {
  if (!sensor || !sensor->stdlog || !sensor->flags.data_ok) return;

  uint8_t unitid = getSensorUnitId(sensor);
  if (!sensor_unit_is_water_volume(unitid)) return;

  double volume = sensor->last_data;
  uint8_t log_unitid = sensor_consumption_unit_for(unitid);

  if (sensor_unit_is_water_absolute(unitid)) {
    if (sensor->last_stdlog_time == 0) {
      sensor->last_stdlog_data = sensor->last_data;
      sensor->last_stdlog_time = time;
      return;
    }
    volume = sensor->last_data - sensor->last_stdlog_data;
    sensor->last_stdlog_data = sensor->last_data;
  }

  ulong duration = sensor->last_stdlog_time > 0 && time > sensor->last_stdlog_time ? time - sensor->last_stdlog_time : sensor->read_interval;
  sensor->last_stdlog_time = time;
  if (volume <= 0) return;
  write_flow_log(volume, log_unitid, duration, time);
}



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

// ADS1115 Schreib/Rücklese-Test auf dem Lo_thresh-Register (0x02).
// Lo_thresh ist ein reines 16-bit R/W-Register ohne Hardware-Seiteneffekte.
// Gibt true zurück, wenn der Wert korrekt zurückgelesen wird.
#if defined(ESP8266) || defined(ESP32)
static bool ads1115_scratch_test(int addr) {
  const uint16_t TEST_VAL = 0x5A5A;
  Wire.beginTransmission(addr);
  Wire.write(0x02);  // ADS1115 Lo_thresh-Register
  Wire.write((uint8_t)(TEST_VAL >> 8));
  Wire.write((uint8_t)(TEST_VAL & 0xFF));
  if (Wire.endTransmission() != 0) return false;

#if defined(ESP8266)
  // Software I2C on ESP8266 can need a short settle time before the register
  // pointer can be read back reliably from ADS1115-based boards.
  delay(3);
#endif

  Wire.beginTransmission(addr);
  Wire.write(0x02);
  Wire.endTransmission(false);

#if defined(ESP8266)
  delay(1);
#endif

  Wire.requestFrom(addr, 2);
  if (Wire.available() < 2) return false;
  uint16_t val = ((uint16_t)Wire.read() << 8) | Wire.read();
  return (val == TEST_VAL);
}

// SC16IS752 Scratch-Test über das SPR-Register (0x07) mit dem SC16IS752-typischen
// Befehlsbyte-Format (reg << 3). Wird als Negativ-Test genutzt: Schlägt er fehl,
// ist kein SC16IS752 auf der Adresse – was für einen ADS1115 spricht.
#if defined(ESP32)
static bool sc16is752_scratch_test_at(int addr) {
  const uint8_t TEST_VAL = 0xA5;
  Wire.beginTransmission(addr);
  Wire.write((0x07 << 3) | 0x00);  // SPR, Schreibzugriff
  Wire.write(TEST_VAL);
  if (Wire.endTransmission() != 0) return false;

  Wire.beginTransmission(addr);
  Wire.write((0x07 << 3) | 0x80);  // SPR, Lesezugriff
  Wire.endTransmission(false);
  Wire.requestFrom(addr, 1);
  if (!Wire.available()) return false;
  return (Wire.read() == TEST_VAL);
}
#endif

// Prüft, ob auf addr ein ADS1115 sitzt:
// Primär: positiver ADS1115-Scratch-Test.
// Fallback: negativer SC16IS752-Scratch-Test (kein SC16IS752 → kein Fehlalarm).
static bool is_ads1115(int addr) {
  if (ads1115_scratch_test(addr)) return true;
#if defined(ESP8266)
  // On ESP8266, the negative SC16 scratch probe can alias on some ADS1115 clones.
  // Fall back to simple ACK detection to avoid false negatives.
  return detect_i2c(addr);
#else
  return !sc16is752_scratch_test_at(addr);
#endif
}

#if defined(ESP8266)
static void reset_asb_i2c_bus() {
  Wire.begin(SDA, SCL);
  Wire.setClock(100000);
  delay(10);
}

static bool detect_asb_pair(int addr_a, int addr_b) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) {
      reset_asb_i2c_bus();
      delay(5 * attempt);
    }

    if (detect_i2c(addr_a) && is_ads1115(addr_a) &&
        detect_i2c(addr_b) && is_ads1115(addr_b)) {
      return true;
    }
  }

  return false;
}
#else
static bool detect_asb_pair(int addr_a, int addr_b) {
  return detect_i2c(addr_a) && is_ads1115(addr_a) &&
         detect_i2c(addr_b) && is_ads1115(addr_b);
}
#endif
#endif  // defined(ESP8266) || defined(ESP32)

/**
 * @brief detect connected boards
 *
 */
void detect_asb_board() {
  // detect analog sensor board, 0x48+0x49=Board1, 0x4A+0x4B=Board2
#if defined(ESP8266) || defined(ESP32)
  asb_detected_boards &= ~(ASB_BOARD1 | ASB_BOARD2);

#if defined(ESP8266)
  reset_asb_i2c_bus();
#endif

  if (detect_asb_pair(ASB_BOARD_ADDR1a, ASB_BOARD_ADDR1b))
    asb_detected_boards |= ASB_BOARD1;
  if (detect_asb_pair(ASB_BOARD_ADDR2a, ASB_BOARD_ADDR2b))
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
  // DEBUG_PRINT(F("ASB DETECT="));
  // DEBUG_PRINTLN(asb_detected_boards);

  for (int log = 0; log <= 2; log++) {
    checkLogSwitch(log);
/*#if defined(ENABLE_DEBUG)
    // DEBUG_PRINT(F("log="));
    // DEBUG_PRINTLN(log);
    const char *f1 = getlogfile(log);
    // DEBUG_PRINT(F("logfile1="));
    // DEBUG_PRINTLN(f1);
    // DEBUG_PRINT(F("size1="));
    // DEBUG_PRINTLN(file_size(f1));
    const char *f2 = getlogfile2(log);
    // DEBUG_PRINT(F("logfile2="));
    // DEBUG_PRINTLN(f2);
    // DEBUG_PRINT(F("size2="));
    // DEBUG_PRINTLN(file_size(f2));
#endif*/
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

  // Internal temperature sensor: available on OSPI (Linux) and ESP32 (built-in temp sensor)
#if defined(OSPI) || defined(ESP32)
  if (type == SENSOR_INTERNAL_TEMP)
      return true;
#endif

  // ZigBee sensors require Ethernet (WiFi shares the 2.4GHz radio)
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (type == SENSOR_ZIGBEE && !useEth)
      return false;
#endif

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
  // DEBUG_PRINTLN(F("[SENSOR_API] sensor_api_init() started"));
  apiInit = true;
  if (detect_boards) {
    // DEBUG_PRINTLN(F("[SENSOR_API] Detecting ASB boards..."));
    detect_asb_board();
  }
  // DEBUG_PRINTLN(F("[SENSOR_API] Loading sensors..."));
  sensor_load();
  // DEBUG_PRINTLN(F("[SENSOR_API] Loading prog_adjust..."));
  prog_adjust_load();
  // DEBUG_PRINTLN(F("[SENSOR_API] Loading monitors..."));
  monitor_load();

#if defined(OSPI)
  //Read rs485 file. Details see below
  std::ifstream file;
  file.open("rs485", std::ifstream::in);
  if (!file.fail()) {
    std::string tty;
    int idx = 0;
    int n = 0;
    // DEBUG_PRINTLN(F("Opening USB RS485 Adapters:"));
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
      // DEBUG_PRINT(idx);
      // DEBUG_PRINT(F(": "));
      // DEBUG_PRINTLN(tty.c_str());

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
        // DEBUG_PRINTLN(F("DEBUG ENABLED"));
        #endif
      }
      idx++;
      if (idx >= MAX_RS485_DEVICES)
        break;
    }
    // DEBUG_PRINT(F("Found "));
    // DEBUG_PRINT(n);
    // DEBUG_PRINTLN(F(" RS485 Adapters"));
  }
#endif
  // DEBUG_PRINTLN(F("[SENSOR_API] sensor_api_init() completed"));
}

bool is_api_init() {
  return apiInit;
}

static bool apiConnected = false;

bool is_sensor_api_connected() {
  return apiConnected;
}

static bool radioEarlyInitDone = false;

bool is_radio_early_init_done() {
  return radioEarlyInitDone;
}

void sensor_radio_early_init() {
  if (radioEarlyInitDone) return;

#if defined(ESP32C5)
  // Only for non-Matter modes: init BLE + Zigbee immediately at boot
  if (ieee802154_is_matter()) {
    // DEBUG_PRINTLN(F("[RADIO] Matter mode — skipping early radio init (Matter manages BLE)"));
    return;
  }
#endif

  // DEBUG_PRINTLN(F("[RADIO] Early radio init: BLE + Zigbee"));

#if defined(ESP32) && defined(OS_ENABLE_BLE)
  DEBUG_PRINTLN(F("[RADIO] Initializing BLE..."));
  if (sensor_ble_init()) {
    DEBUG_PRINTLN(F("[RADIO] BLE initialized successfully"));
  } else {
    DEBUG_PRINTLN(F("[RADIO] BLE init failed"));
  }
#endif

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (ieee802154_is_zigbee()) {
    // DEBUG_PRINTF("[RADIO] Starting Zigbee (%s mode)...\n",
                 // ieee802154_is_zigbee_gw() ? "gateway" : "client");
    sensor_zigbee_start();
  }
#endif

// Initialise dynamic coexistence manager (replaces static PTI config).
  // This MUST run AFTER Zigbee.begin() because ieee802154_mac_init()
  // inside esp_zb_start() resets all PTI values to defaults.
  

  // Apply actual Ethernet state AFTER coex_init() so the correct strategy
  // (ZIGBEE_HIGH) is used in Ethernet mode.  coex_init() no longer resets
  // s_ethernet, so this call is authoritative for the current boot session.
#if defined(ESP32C5)
  
#endif

  radioEarlyInitDone = true;
  // Also mark apiConnected so Zigbee ensure_started() is unblocked
  apiConnected = true;
  // DEBUG_PRINTLN(F("[RADIO] Early radio init complete"));
}

void sensor_api_connect() {
  #if defined(ESP8266) || defined(ESP32)
  // Allow sensor_api_connect in both WiFi STA and Ethernet mode
  #if defined(ESP32C5)
  // ESP32-C5: Ethernet mode has WiFi.getMode()==WIFI_MODE_NULL
  wifi_mode_t wmode = WiFi.getMode();
  if (wmode != WIFI_MODE_STA && wmode != WIFI_MODE_NULL) {
    // DEBUG_PRINTF("[SENSOR_API] sensor_api_connect skipped (wifi_mode=%d, not STA/ETH)\n", wmode);
    return;
  }
  #else
  if (os.get_wifi_mode() != WIFI_MODE_STA) {
    // DEBUG_PRINTLN(F("[SENSOR_API] sensor_api_connect skipped (not STA mode)"));
    return;
  }
  #endif
  #endif

  if (!os.network_connected()) {
    DEBUG_PRINTLN(F("[SENSOR_API] sensor_api_connect skipped (network not connected)"));
    return;
  }

  #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
  // DEBUG_PRINTLN(F("[SENSOR_API] Initializing MQTT..."));
  sensor_mqtt_init();
  // DEBUG_PRINTLN(F("[SENSOR_API] Checking FYTA options..."));
  fyta_check_opts();
  #if defined(ESP32)
  gardena_check_opts();
  #endif
  #endif

  // BLE + Zigbee: handled by sensor_radio_early_init() at boot (non-Matter)
  // or lazily here for Matter mode.
  if (!radioEarlyInitDone) {
    sensor_radio_early_init();
  }

  apiConnected = true;
}
// sensor_api_connect() initializes all network-dependent sensor subsystems
// (MQTT, FYTA, Gardena, Zigbee, BLE) once the device is connected in WiFi STA mode
// and the required delay after Matter init has elapsed.

/**
 * @brief Sensor maintenance loop
 * @note Calls subsystem-specific loop functions (BLE, Zigbee auto-stop timers, etc.)
 */
void sensor_api_loop() {
#if defined(ESP32C5)
  // Dynamic coexistence: periodically re-evaluate radio priorities
  
#endif

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (ieee802154_is_zigbee() && sensor_zigbee_is_active()) {
    sensor_zigbee_loop();
  }
#endif

#if defined(ESP32) && defined(OS_ENABLE_BLE)
  if (sensor_ble_is_active()) {
    sensor_ble_loop();
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
  // DEBUG_PRINTLN(F("sensor_api_free1"));
  apiInit = false;
  current_sensor = NULL;
  os.mqtt.setCallback(2, NULL);

  for (auto &kv : progSensorAdjustsMap) {
    delete kv.second;
  }
  progSensorAdjustsMap.clear();

  // DEBUG_PRINTLN(F("sensor_api_free2"));

  // DEBUG_PRINTLN(F("sensor_api_free3"));

  for (auto &kv : monitorsMap) {
    delete kv.second;
  }
  monitorsMap.clear();

  // DEBUG_PRINTLN(F("sensor_api_free4"));

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
  // DEBUG_PRINTLN(F("sensor_api_free5"));
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
  // Clear this sensor's log entries so a newly created sensor that later
  // re-uses the same nr does not inherit stale logs of the deleted sensor.
  sensorlog_clear_sensor(nr, LOG_STD, false, 0, false, 0, 0, 0);
  sensorlog_clear_sensor(nr, LOG_WEEK, false, 0, false, 0, 0, 0);
  sensorlog_clear_sensor(nr, LOG_MONTH, false, 0, false, 0, 0, 0);
  sensor_save();
  return HTTP_RQT_SUCCESS;
}

/** Trigger Zigbee re-configuration after a sensor is created or updated. */
static inline void sensor_notify_zigbee(SensorBase *s) {
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (s && s->type == SENSOR_ZIGBEE) {
    sensor_zigbee_request_configure_reporting(s->nr);
    sensor_zigbee_request_dp_query(s->nr);
    sensor_zigbee_request_active_read(s->nr);
  }
#else
  (void)s;
#endif
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
  
  // DEBUG_PRINTLN(F("sensor_define"));
  
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
    sensor_notify_zigbee(sensor);
    if (sensor->type != SENSOR_ZIGBEE) last_save_time = os.now_tz() - 3600 + 5; // force save next time
    
    return HTTP_RQT_SUCCESS;
  }
  
  // Full definition with type
  uint type = json["type"];
  if (type == 0) return HTTP_RQT_NOT_RECEIVED;
  
  if (it != sensorsMap.end()) {
    // Sensor exists - check if type changed
    SensorBase *old_sensor = it->second;
    if (old_sensor->type != type) {
      // DEBUG_PRINTLN(F("sensor_define: type changed, recreating"));
      delete old_sensor;
      sensorsMap.erase(it);
      // Fall through to create new sensor
    } else {
      // Same type, update from JSON
      old_sensor->fromJson(json);
      
      if (save) sensor_save();
      sensor_notify_zigbee(old_sensor);
      return HTTP_RQT_SUCCESS;
    }
  }
  
  // Gateway-managed Zigbee devices should not be duplicated as bare placeholder
  // sensors. Keep real logical channel definitions, because station control uses
  // those sensor rows for endpoint/control-mode/Tuya-DP mapping.
  if (type == SENSOR_ZIGBEE) {
    bool has_logical_config = json.containsKey("cluster_id") ||
                              json.containsKey("attribute_id") ||
                              json.containsKey("control_mode") ||
                              json.containsKey("use_tuya_control") ||
                              json.containsKey("tuya_dp") ||
                              json.containsKey("tuya_dp_value") ||
                              json.containsKey("tuya_dp_status") ||
                              json.containsKey("tuya_dp_consumption") ||
                              json.containsKey("tuya_dp_unit") ||
                              json.containsKey("tuya_dp_batt") ||
                              json.containsKey("zb_type") ||
                              json.containsKey("zigbee_type") ||
                              json.containsKey("dp") ||
                              json.containsKey("dp_value") ||
                              json.containsKey("dp_status") ||
                              json.containsKey("dp_consumption") ||
                              json.containsKey("dp_unit") ||
                              json.containsKey("dp_battery");

    if (!has_logical_config) {
      uint64_t ieee_addr = 0;
      const char *ieee_str = nullptr;
      if (json.containsKey("device_ieee") && json["device_ieee"].is<const char*>()) {
        ieee_str = json["device_ieee"];
      } else if (json.containsKey("ieee") && json["ieee"].is<const char*>()) {
        ieee_str = json["ieee"];
      } else if (json.containsKey("ieee_addr") && json["ieee_addr"].is<const char*>()) {
        ieee_str = json["ieee_addr"];
      }
      if (ieee_str && ieee_str[0]) {
        if (strlen(ieee_str) == 16) {
          for (int i = 0; i < 16; i++) {
            char c = ieee_str[i];
            ieee_addr = (ieee_addr << 4) | (uint64_t)((c >= '0' && c <= '9') ? (c - '0') : (c >= 'A' && c <= 'F') ? (10 + c - 'A') : (c >= 'a' && c <= 'f') ? (10 + c - 'a') : 0);
          }
        } else {
          ieee_addr = strtoull(ieee_str, nullptr, 0);
        }
      } else if (json.containsKey("device_ieee")) {
        ieee_addr = json["device_ieee"].as<uint64_t>();
      } else if (json.containsKey("ieee_addr")) {
        ieee_addr = json["ieee_addr"].as<uint64_t>();
      }

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
      if (ieee_addr != 0) {
        extern int sensor_zigbee_gw_get_discovered_devices(ZigbeeDeviceInfo* out, int max_devices);
        extern int sensor_zigbee_get_discovered_devices(ZigbeeDeviceInfo* out, int max_devices);
        const int max_devices = 128;
        ZigbeeDeviceInfo *devs = new (std::nothrow) ZigbeeDeviceInfo[max_devices];
        if (devs) {
          bool found = false;
          int n_gw = sensor_zigbee_gw_get_discovered_devices(devs, max_devices);
          for (int i = 0; i < n_gw; i++) {
            if (devs[i].ieee_addr == ieee_addr) { found = true; break; }
          }
          if (!found) {
            int n_cli = sensor_zigbee_get_discovered_devices(devs, max_devices);
            for (int i = 0; i < n_cli; i++) {
              if (devs[i].ieee_addr == ieee_addr) { found = true; break; }
            }
          }
          delete[] devs;
          if (found) {
            DEBUG_PRINTLN(F("[SENSOR] Zigbee gateway device already exists; skipping placeholder sensor."));
            return HTTP_RQT_SUCCESS;
          }
        }
      }
#endif
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
  sensor_notify_zigbee(new_sensor);
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

#if !defined(ESP8266)
static void sensor_trend_init_from_log(SensorBase *sensor) {
  if (!sensor) return;

  const uint32_t TREND_INIT_LOOKBACK_SEC = 24UL * 3600UL; // 24h window
  const uint16_t TREND_INIT_BLOCK = 64;
  const uint16_t TREND_INIT_MAX_ENTRIES = 512;

  ulong total = sensorlog_size(LOG_STD);
  if (total == 0) return;

  ulong now = os.now_tz();
  ulong cutoff = (now > TREND_INIT_LOOKBACK_SEC) ? (now - TREND_INIT_LOOKBACK_SEC) : 0;
  ulong start = findLogPosition(LOG_STD, cutoff);

  // Clamp to last N entries to avoid long scans on large logs
  if (total > TREND_INIT_MAX_ENTRIES && (total - start) > TREND_INIT_MAX_ENTRIES) {
    start = total - TREND_INIT_MAX_ENTRIES;
  }

  SensorLog_t *buffer = new SensorLog_t[TREND_INIT_BLOCK];
  for (ulong idx = start; idx < total; idx += TREND_INIT_BLOCK) {
    int count = sensorlog_load2(LOG_STD, idx, TREND_INIT_BLOCK, buffer);
    if (count <= 0) break;
    for (int i = 0; i < count; i++) {
      if (buffer[i].nr == sensor->nr) {
        sensor->trend_add_sample(buffer[i].data, buffer[i].time);
      }
    }
  }
  delete[] buffer;
}
#endif // !defined(ESP8266)

// Parse a sensor JSON config file into sensorsMap. Returns false if the file is
// missing, empty or contains invalid/corrupt JSON. Does not clear the map or
// initialize drivers — that is the caller's responsibility so it can fall back
// to a backup file before committing.
static bool sensor_parse_file(const char *fn) {
  if (!file_exists(fn)) return false;
  ulong size = file_size(fn);
  if (size == 0) return false;

  FileReader reader(fn);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reader);
  if (err) {
    DEBUG_PRINTF("sensor_load: JSON parse error (%s) in %s, file size=%lu\n",
                 err.c_str(), fn, size);
    return false;
  }

  JsonArray arr;
  if (doc.is<JsonArray>()) arr = doc.as<JsonArray>();
  else if (doc.containsKey("sensors")) arr = doc["sensors"].as<JsonArray>();
  else return false;

  for (JsonVariant v : arr) {
    uint sensorType = v["type"] | 0;
    uint sensorNr = v["nr"] | 0;
    if (sensorNr == 0 || sensorType == 0) {
      continue; // Skip invalid sensor entries (type=0 or nr=0)
    }
    boolean ip_based = (v["ip"] | 0) != 0;

    SensorBase *sensor = sensor_make_obj(sensorType, ip_based);
    if (!sensor) {
      sensor = new GenericSensor(sensorType);
    }

    sensor->fromJson(v);

    // If the type is not supported in this firmware variant (e.g. a ZigBee
    // sensor loaded on a Matter build), store the full raw JSON so all
    // variant-specific fields (device_ieee, cluster_id, …) survive the
    // roundtrip back to the original firmware.
    if (!sensor_type_supported(sensorType)) {
      DEBUG_PRINTF("sensor_load: type %u not supported in this firmware variant "
                   "\u2014 loaded as generic sensor (all data preserved)\n", sensorType);
      if (sensor->isGeneric()) {
        GenericSensor* gs = static_cast<GenericSensor*>(sensor);
        String raw;
        serializeJson(v, raw);
        gs->setRawJson(raw.c_str(), raw.length());
      }
    }

    sensorsMap[sensor->nr] = sensor;
    sensor->flags.data_ok = false;
  }
  return true;
}

void sensor_load() {
  // DEBUG_PRINTLN(F("sensor_load"));

  // Clean up existing map to avoid memory / heap leaks on reload
  for (auto &kv : sensorsMap) {
    delete kv.second;
  }
  sensorsMap.clear();
  current_sensor = NULL;

  bool loaded = sensor_parse_file(SENSOR_FILENAME_JSON);
  bool from_backup = false;

  if (!loaded) {
    // Primary missing/corrupt: try to recover from the backup written by
    // sensor_save() before discarding anything (same protection as #263).
    for (auto &kv : sensorsMap) delete kv.second;
    sensorsMap.clear();

    if (sensor_parse_file(SENSOR_FILENAME_JSON ".bak")) {
      DEBUG_PRINTLN(F("sensor_load: recovered sensors from backup"));
      loaded = true;
      from_backup = true;
    } else {
      // Both primary and backup are unusable. If a non-empty but corrupt
      // primary exists, set it aside for diagnostics instead of looping on it.
      for (auto &kv : sensorsMap) delete kv.second;
      sensorsMap.clear();
      if (file_exists(SENSOR_FILENAME_JSON) && file_size(SENSOR_FILENAME_JSON) > 0) {
        if (file_exists("/sensors_bad.json")) remove_file("/sensors_bad.json");
        if (!rename_file(SENSOR_FILENAME_JSON, "/sensors_bad.json"))
          remove_file(SENSOR_FILENAME_JSON);
      }
    }
  }

  if (loaded) {
    // Initialize sensor drivers
    for (auto &kv : sensorsMap) {
      SensorBase *s = kv.second;
#if !defined(ESP8266)
      s->trend_reset();
#endif
      s->init();
    }

#if !defined(ESP8266)
    // Initialize trend history from logs (longer time window)
    for (auto &kv : sensorsMap) {
      SensorBase *s = kv.second;
      if (s) {
        sensor_trend_init_from_log(s);
      }
    }
#endif

    // If we had to fall back to the backup, re-materialize a valid primary file.
    if (from_backup) sensor_save();
  }

  last_save_time = os.now_tz();
}


/**
 * @brief Store sensor data
 *
 */
void sensor_request_save() {
  // Schedule a deferred save (~5 seconds from now) instead of saving immediately.
  // The periodic sensor loop checks (time - last_save_time > 3600) so setting
  // last_save_time to now-3595 triggers a save after ~5 seconds.
  last_save_time = os.now_tz() - 3600 + 5;
}

void sensor_save() {
  if (!apiInit) return;
  // DEBUG_PRINTLN(F("sensor_save (json)"));

  const char *tmpfile = SENSOR_FILENAME_JSON ".tmp";
  const char *bakfile = SENSOR_FILENAME_JSON ".bak";

  // Stream-serialize sensors one at a time to minimize peak heap usage.
  // Building the full array in a single JsonDocument can OOM on ESP8266.
  // Write to a temp file first so an interrupted/partial write never clobbers
  // the existing good configuration (same data-loss class as #263).
  if (file_exists(tmpfile)) remove_file(tmpfile);
  bool ok = true;
  {
    FileWriter writer(tmpfile);
    writer.write('[');

  bool first = true;
  for (auto &kv : sensorsMap) {
    SensorBase *sensor = kv.second;

    if (!first) writer.write(',');
    first = false;

    // Build a temporary per-sensor JsonDocument (freed each iteration)
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();

    // For GenericSensors that preserve a raw JSON snapshot (unsupported type
    // in this firmware variant), first restore ALL original fields so that
    // variant-specific data (e.g. ZigBee device_ieee, cluster_id) is not lost.
    // Base-class fields written below will overwrite with current values.
    GenericSensor* gs = sensor->isGeneric() ? static_cast<GenericSensor*>(sensor) : nullptr;
    if (gs && gs->_raw_json) {
      JsonDocument tmp;
      if (deserializeJson(tmp, gs->_raw_json) == DeserializationError::Ok) {
        JsonObject tmpObj = tmp.as<JsonObject>();
        for (auto kv_pair : tmpObj) {
          obj[kv_pair.key()] = kv_pair.value();
        }
      }
    }

    // Always write current base-class values (name, enable, log, etc.).
    sensor->toJson(obj);
    if (doc.overflowed()) ok = false;
    serializeJson(doc, writer);
  }

    writer.write(']');
  }  // writer flushes its buffer on destruction here

  // Validate the temp file: must be non-empty, terminate with ']' (the stream
  // completed), and the build must not have overflowed (OOM). Otherwise keep
  // the previous good file untouched.
  ulong tsize = file_size(tmpfile);
  unsigned char lastc = 0;
  if (tsize > 0) file_read_block(tmpfile, &lastc, tsize - 1, 1);
  if (!ok || tsize < 2 || lastc != ']') {
    DEBUG_PRINTLN(F("sensor_save: serialization failed, keeping previous file"));
    remove_file(tmpfile);
    last_save_time = os.now_tz();
    current_sensor = NULL;
    return;
  }

  // Rotate the current good file to a backup, then atomically swap in the new
  // validated file.
  if (file_exists(SENSOR_FILENAME_JSON)) {
    if (file_exists(bakfile)) remove_file(bakfile);
    rename_file(SENSOR_FILENAME_JSON, bakfile);
  }
  if (!rename_file(tmpfile, SENSOR_FILENAME_JSON)) {
    if (file_exists(SENSOR_FILENAME_JSON)) remove_file(SENSOR_FILENAME_JSON);
    if (!rename_file(tmpfile, SENSOR_FILENAME_JSON)) {
      DEBUG_PRINTLN(F("sensor_save: rename failed, restoring backup"));
      if (file_exists(bakfile)) rename_file(bakfile, SENSOR_FILENAME_JSON);
      remove_file(tmpfile);
    }
  }

  last_save_time = os.now_tz();
  // DEBUG_PRINTLN(F("sensor_save2"));
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
  static uint32_t last_error_time = 0;
  static uint32_t error_count = 0;
  
  // If we've had too many errors recently, stop trying
  if (error_count > 5 && (millis() - last_error_time) < 60000) {
    return false; // Skip logging for 60 seconds after 5 failures
  }
  
  if (!checkDiskFree()) {
    error_count++;
    last_error_time = millis();
    return false;
  }
  
  // Reset error counter on success
  error_count = 0;
#endif
  
  // DEBUG_PRINT(F("sensorlog_add "));
  // DEBUG_PRINT(log);
  checkLogSwitch(log);
  file_append_block(getlogfile(log), sensorlog, SENSORLOG_STORE_SIZE);
  checkLogSwitchAfterWrite(log);
  // DEBUG_PRINT(F("="));
  // DEBUG_PRINTLN(sensorlog_filesize(log));
  
  return true;
}

bool sensorlog_add(uint8_t log, SensorBase *sensor, ulong time) {
  if (sensor->flags.data_ok && time > 1000) {
    sensor->last = time;

    if (!sensor->flags.log) {
      return false;
    }

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
  // DEBUG_PRINTLN(F("sensorlog_clear "));
  // DEBUG_PRINT(std);
  // DEBUG_PRINT(week);
  // DEBUG_PRINT(month);
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
  // DEBUG_PRINTLN(F("clearlog1"));
  // DEBUG_PRINTF("nr: %d log: %d under:%lf over: %lf before: %lld after: %lld size: %ld size2: %ld\n", sensorNr, log, under, over, before, after, size, size2);
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
        // DEBUG_PRINTF("clearlog2 idx=%ld idx2=%ld\n", idx, idxr);
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
          // DEBUG_PRINTF("clearlog3 idx=%ld idxr=%ld\n", idx, idxr);
          n++;
        }
      }
    }
  }
  delete[] sensorlog;
  // DEBUG_PRINTF("clearlog4 n=%ld\n", n);
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

void dtostrf(float value, int min_width, int precision, char *txt) {
  sprintf(txt, "%*.*f", min_width, precision, value);
}

void dtostrf(double value, int min_width, int precision, char *txt) {
  sprintf(txt, "%*.*f", min_width, precision, value);
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
    // DEBUG_PRINTLN(F("calc_sensorlogs WEEK start"));
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
    sensorlog = (SensorLog_t *)heap_caps_malloc(sizeof(SensorLog_t) * BLOCKSIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    sensorlog = (SensorLog_t *)malloc(sizeof(SensorLog_t) * BLOCKSIZE);
#endif
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
          double last_water_data = 0;
          ulong n = 0;
          uint8_t unitid = getSensorUnitId(sensor);
          bool water_meter = sensor_unit_is_water_volume(unitid);
          bool water_absolute = sensor_unit_is_water_absolute(unitid);
          bool have_water_data = false;
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
                if (water_absolute) {
                  if (!have_water_data) {
                    last_water_data = sensorlog[i].data;
                    have_water_data = true;
                    continue;
                  }
                  double delta = sensorlog[i].data - last_water_data;
                  last_water_data = sensorlog[i].data;
                  if (delta < 0) continue;
                  data += delta;
                } else if (water_meter) {
                  data += sensorlog[i].data;
                } else {
                  data += sensorlog[i].data;
                }
                n++;
              }
            }
          }
          if (n > 0) {
            sensorlog->nr = sensor->nr;
            sensorlog->time = fromdate;
            sensorlog->data = water_meter ? data : data / (double)n;
            sensorlog->native_data = 0;
            sensorlog_add(LOG_WEEK, sensorlog);
          }
        }
      }
      fromdate += CALCRANGE_WEEK;
      todate += CALCRANGE_WEEK;
    }
    next_week_calc = todate;
    // DEBUG_PRINTLN(F("calc_sensorlogs WEEK end"));
  }

  if (time >= next_month_calc) {
    log_size = sensorlog_size(LOG_WEEK);
    if (log_size <= 0) {
      if (sensorlog) free(sensorlog);
      return;
    }
    if (!sensorlog)
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
      sensorlog = (SensorLog_t *)heap_caps_malloc(sizeof(SensorLog_t) * BLOCKSIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      sensorlog = (SensorLog_t *)malloc(sizeof(SensorLog_t) * BLOCKSIZE);
#endif

    // DEBUG_PRINTLN(F("calc_sensorlogs MONTH start"));
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
          bool water_meter = sensor_unit_is_water_volume(getSensorUnitId(sensor));
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
            sensorlog->data = water_meter ? data : data / (double)n;
            sensorlog->native_data = 0;
            sensorlog_add(LOG_MONTH, sensorlog);
          }
        }
      }
      fromdate += CALCRANGE_MONTH;
      todate += CALCRANGE_MONTH;
    }
    next_month_calc = todate;
    // DEBUG_PRINTLN(F("calc_sensorlogs MONTH end"));
  }
  if (sensorlog) free(sensorlog);
}

void sensor_remote_http_callback(char *) {
  // unused
}

// ---- Deferred MQTT push queue ----
// When ZigBee data arrives during predictive boost, WiFi may be down.
// Queue the sensor nr and flush when WiFi/MQTT reconnects.
static constexpr int MAX_MQTT_DEFERRED = 8;
static uint mqtt_deferred_nrs[MAX_MQTT_DEFERRED];
static int mqtt_deferred_count = 0;

static void mqtt_defer_push(uint nr) {
  for (int i = 0; i < mqtt_deferred_count; i++) {
    if (mqtt_deferred_nrs[i] == nr) return;
  }
  if (mqtt_deferred_count < MAX_MQTT_DEFERRED) {
    mqtt_deferred_nrs[mqtt_deferred_count++] = nr;
    DEBUG_PRINTF("[MQTT] Deferred push for sensor %u (WiFi down, queue=%d)\n", nr, mqtt_deferred_count);
  }
}

static void flush_deferred_mqtt() {
  if (mqtt_deferred_count == 0) return;
  if (!os.mqtt.enabled() || !os.mqtt.connected()) return;

  // Copy and clear queue — push_message may re-add on failure
  uint nrs[MAX_MQTT_DEFERRED];
  int count = mqtt_deferred_count;
  memcpy(nrs, mqtt_deferred_nrs, sizeof(uint) * count);
  mqtt_deferred_count = 0;

  DEBUG_PRINTF("[MQTT] Flushing %d deferred push(es)\n", count);
  for (int i = 0; i < count; i++) {
    SensorBase* sensor = sensor_by_nr(nrs[i]);
    if (sensor && sensor->last_read) {
      push_message(sensor);
    }
  }
}

void push_message(SensorBase *sensor) {
  if (!sensor || !sensor->last_read) return;

  static char EXT_RAM_BSS_ATTR topic[TMP_BUFFER_SIZE];
  static char EXT_RAM_BSS_ATTR payload[TMP_BUFFER_SIZE];
  char *postval = tmp_buffer;

  if (os.mqtt.enabled()) {
    // DEBUG_PRINTLN(F("push mqtt1"));
    strncpy_P(topic, PSTR("analogsensor/"), sizeof(topic) - 1);
    strncat(topic, sensor->name, sizeof(topic) - 1);
    snprintf_P(payload, TMP_BUFFER_SIZE,
              PSTR("{\"nr\":%u,\"type\":%u,\"data_ok\":%u,\"time\":%u,"
                   "\"value\":%d.%02d,\"unit\":\"%s\"}"),
              sensor->nr, sensor->type, sensor->flags.data_ok,
              sensor->last_read, (int)sensor->last_data,
              abs((int)(sensor->last_data * 100) % 100), getSensorUnit(sensor));

    if (os.mqtt.connected()) {
      os.mqtt.publish(topic, payload);
    } else {
      // Keep loopTask stack shallow: MQTT reconnect can trigger a TLS connect
      // path with high stack usage. Reconnect is handled centrally in os.mqtt.loop().
      mqtt_defer_push(sensor->nr);
    }
    // DEBUG_PRINTLN(F("push mqtt2"));
  }
  
  //ifttt is enabled, when the ifttt key is present!
  os.sopt_load(SOPT_IFTTT_KEY, tmp_buffer);
	bool ifttt_enabled = strlen(tmp_buffer)!=0;
  if (ifttt_enabled) {
    DEBUG_PRINTLN(F("push ifttt"));
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

    os.send_http_request_async(DEFAULT_IFTTT_URL, 80, ether_buffer, sensor_remote_http_callback, false, 12000);
    // DEBUG_PRINTLN(F("push ifttt2"));
  }

  add_influx_data(sensor);
}

void read_all_sensors(boolean online) {
  // Flush deferred MQTT pushes when network is back
  if (online) flush_deferred_mqtt();

  // Do not read or log sensors if NTP is enabled but system time is not yet synced
  #if defined(ARDUINO)
  if (os.iopts[IOPT_USE_NTP] && (time(NULL) < 1704067200UL)) {
    return;
  }
  #endif

  if (sensorsMap.empty()) {
    static unsigned long last_warn = 0;
    if (millis() - last_warn > 120000) {
      last_warn = millis();
      // DEBUG_PRINTLN(F("[SENSOR] sensorsMap is empty - no sensors configured"));
    }
    return;
  }
  
  static boolean first_read = true;
  if (first_read) {
    first_read = false;
    // DEBUG_PRINTF(F("[SENSOR] read_all_sensors() started, %d sensors configured\n"), sensorsMap.size());
  }

  ulong time = os.now_tz();

#ifdef ENABLE_DEBUG
  if (time < os.powerup_lasttime + 3)
#else
  if (time < os.powerup_lasttime + 30)
#endif
  {
    static unsigned long last_boot_msg = 0;
    if (millis() - last_boot_msg > 5000) {
      last_boot_msg = millis();
      // DEBUG_PRINTF(F("[SENSOR] Waiting for boot delay: %lu/%u seconds\n"), time - os.powerup_lasttime, 
// #ifdef ENABLE_DEBUG
                   // 3
// #else
                   // 30
// #endif
      // );
    }
    return;  // wait 30s before first sensor read
  }

  // Initialize iterator if we're starting over
  if (!current_sensor && !sensorsMap.empty()) {
    current_sensor_it = sensorsMap.begin();
    current_sensor = current_sensor_it->second;
  }

  uint8_t sensors_read_this_pass = 0;
  unsigned long pass_start_ms = millis();
  while (current_sensor && current_sensor_it != sensorsMap.end()) {
    //ulong time_since_last = (current_sensor->last_read == 0) ? 99999 : (time - current_sensor->last_read);
    boolean should_read = (time >= current_sensor->last_read + current_sensor->read_interval || current_sensor->repeat_read);
    
    if (should_read) {
      if (!current_sensor->flags.enable || current_sensor->type == SENSOR_TYPE_NONE) {
        current_sensor->last_read = time;
        current_sensor->repeat_read = 0;
      } else if (online || (current_sensor->ip == 0 && current_sensor->type != SENSOR_MQTT)) {
        //boolean was_repeat = current_sensor->repeat_read;
        DEBUG_PRINTF(F("[SENSOR] read begin #%d type=%d name='%s' repeat=%d\n"),
                     current_sensor->nr, current_sensor->type, current_sensor->name, current_sensor->repeat_read);
        
        unsigned long read_start_ms = millis();
        int result = read_sensor(current_sensor, time);
        unsigned long read_ms = millis() - read_start_ms;
        if (!current_sensor) {
          // Reset iterator if sensor was deleted during save
          current_sensor = NULL;
          return;
        }
        DEBUG_PRINTF(F("[SENSOR] read end #%d result=%d duration=%lums\n"),
                     current_sensor->nr, result, read_ms);
        if (result == HTTP_RQT_SUCCESS) {
          current_sensor->last_read = time;
#if !defined(ESP8266)
          current_sensor->trend_add_sample(current_sensor->last_data, time);
#endif
          unsigned long log_start_ms = millis();
          sensorlog_add(LOG_STD, current_sensor, time);
          sensor_standard_waterlog_add(current_sensor, time);
          DEBUG_PRINTF(F("[SENSOR] log done #%d duration=%lums\n"), current_sensor->nr, millis() - log_start_ms);
          unsigned long push_start_ms = millis();
          push_message(current_sensor);
          DEBUG_PRINTF(F("[SENSOR] push done #%d duration=%lums\n"), current_sensor->nr, millis() - push_start_ms);
        } else if (result == HTTP_RQT_TIMEOUT) {
          // delay next read on timeout:
          current_sensor->last_read = time + max((uint)60, current_sensor->read_interval);
          current_sensor->repeat_read = 0;
          // DEBUG_PRINTF("Delayed1: %s\n", current_sensor->name);
        } else if (result == HTTP_RQT_CONNECT_ERR) {
          // delay next read on error:
          current_sensor->last_read = time + max((uint)60, current_sensor->read_interval);
          current_sensor->repeat_read = 0;
          // DEBUG_PRINTF("Delayed2: %s\n", current_sensor->name);
        } else if (result == HTTP_RQT_NOT_RECEIVED) {
          // ZigBee sensors manage their own read timing via last_read:
          // the sensor sets last_read when it sends an active read request,
          // and uses (time >= last_read + poll_interval) to decide the next
          // active read. Overwriting last_read here would reset that timer
          // every second, making should_read inside ZigbeeSensor::read()
          // permanently false.
          // Sensors that use sample-averaging (PCF8591, ADS1115, ASB etc.) store
          // the read-start timestamp in last_read and compare it to
          // (last_read + read_interval) each call to decide when to finish
          // averaging. Overwriting last_read while repeat_read > 0 resets that
          // window every second, so the averaging interval can never expire and
          // no averaged value is ever published.
          // Fix: only advance last_read when the sensor is not mid-averaging.
          if (current_sensor->type != SENSOR_ZIGBEE && current_sensor->repeat_read == 0) {
            if (current_sensor->last_read < time) {
              current_sensor->last_read = time;
            }
          }
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
        sensors_read_this_pass++;
        if (sensors_read_this_pass >= 3 || (millis() - pass_start_ms) > 500UL) {
          ++current_sensor_it;
          if (current_sensor_it != sensorsMap.end()) {
            current_sensor = current_sensor_it->second;
          } else {
            current_sensor = NULL;
          }
          return;
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
#include "SensorBase.hpp"

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
#if defined(ESP32) || defined(OSPI)
    case SENSOR_GARDENA_MOISTURE:
    case SENSOR_GARDENA_TEMPERATURE: {
      GardenaSensor *s = new GardenaSensor(type);
      return s;
    }
#endif
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
    case SENSOR_MODBUS_RTU:
#if defined(ESP8266) || defined(ESP32)
      if (!ip_based && (get_asb_detected_boards() & ASB_I2C_RS485))
        return new RS485I2CSensor(type);
#endif
      return new ModbusRtuSensor(type); // fallback to existing C implementation
      break;


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

    // Remote JSON sensor
    case SENSOR_REMOTE_JSON:
      #if defined(ESP8266) || defined(ESP32) || defined(OSPI)
      return new RemoteJsonSensor(type);
      #else
      return new GenericSensor(type);
      #endif

    // Zigbee sensors
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
    case SENSOR_ZIGBEE:
      return new ZigbeeSensor(type);
#endif
    // BLE sensors
#if (defined(OSPI) || defined(ESP32))
    case SENSOR_BLE:
#if defined(OSPI)
      return new OspiBLESensor(type);
#endif
#if defined(ESP32) && defined(OS_ENABLE_BLE)
      return new BLESensor(type);
#endif
#endif
    case SENSOR_FLOW_PULSE:
  return new FlowPulseSensor(type);

    // Internal system sensors
#if defined(ESP8266) || defined(ESP32)
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE:
      return new InternalSensor(type);
#endif
#if defined(ESP32) || defined(OSPI)
    case SENSOR_INTERNAL_TEMP:
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
  if (!sensor) {
    DEBUG_PRINTLN(F("[SENSOR] read_sensor: sensor is NULL!"));
    return HTTP_RQT_NOT_RECEIVED;
  }
  if (!sensor->flags.enable) {
    // DEBUG_PRINTF(F("[SENSOR] read_sensor #%d: disabled\n"), sensor->nr);
    return HTTP_RQT_NOT_RECEIVED;
  }

  int result = sensor->read(time);
  /*
  const char *result_str = "?";
  switch(result) {
    case HTTP_RQT_SUCCESS: result_str = "SUCCESS"; break;
    case HTTP_RQT_NOT_RECEIVED: result_str = "NOT_RECEIVED"; break;
    case HTTP_RQT_TIMEOUT: result_str = "TIMEOUT"; break;
    case HTTP_RQT_CONNECT_ERR: result_str = "CONNECT_ERR"; break;
    default: result_str = "UNKNOWN"; break;
  }
  // Suppress routine NOT_RECEIVED noise for ZigBee sensors: they return
  // NOT_RECEIVED while passively waiting for a device report or rejoining,
  // which is entirely normal and not an error worth logging every interval.
  if (result != HTTP_RQT_NOT_RECEIVED || sensor->type != SENSOR_ZIGBEE) {
    DEBUG_PRINTF(F("[SENSOR] read #%d result=%s (=%d)\n"), sensor->nr, result_str, result);
  }*/
  return result;
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
          // If the group sensor itself has a group number assigned, aggregate
          // all sensors sharing that group number (allows multiple group
          // sensors to reference the same members). Otherwise fall back to the
          // legacy behavior where members point to this group sensor's nr.
          boolean shared = (sensor->group != 0);
          uint target = shared ? sensor->group : nr;
          double value = 0;
          int n = 0;
          for (auto &kv2 : sensorsMap) {
            SensorBase *member = kv2.second;
            // In shared mode, skip other group sensors so groups don't
            // aggregate each other when they share the same group number.
            if (shared && sensor_isgroup(member)) continue;
            if (member->nr != nr && member->group == target && member->flags.enable) {
              switch (sensor->type) {
                case SENSOR_GROUP_MIN:
                  if (n++ == 0) value = member->last_data;
                  else if (member->last_data < value) value = member->last_data;
                  break;
                case SENSOR_GROUP_MAX:
                  if (n++ == 0) value = member->last_data;
                  else if (member->last_data > value) value = member->last_data;
                  break;
                case SENSOR_GROUP_AVG:
                case SENSOR_GROUP_SUM:
                  n++;
                  value += member->last_data;
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

static double clamp_adjust_factor(double factor) {
  if (factor < 0.0) return 0.0;
  if (factor > 2.0) return 2.0;
  return factor;
}

static bool prog_adjust_sensor_is_stale(ProgSensorAdjust *p, SensorBase *sensor, ulong now) {
  if (!p || !sensor || p->stale_timeout == 0) return false;

  ulong base_time = sensor->last_read ? sensor->last_read : os.powerup_lasttime;
  if (base_time == 0) return false;

  return (long)(now - base_time) >= (long)p->stale_timeout;
}

static double calc_sensor_watering_for_sensor(ProgSensorAdjust *p, SensorBase *sensor,
                                              bool *stale_active = nullptr,
                                              bool *fallback_used = nullptr) {
  if (stale_active) *stale_active = false;
  if (fallback_used) *fallback_used = false;
  if (!p || !sensor || !sensor->flags.enable) return 1.0;

  bool stale = prog_adjust_sensor_is_stale(p, sensor, os.now_tz());
  if (stale_active) *stale_active = stale;

  if (stale) {
    if (p->stale_policy == PROG_STALE_DISABLE) {
      if (fallback_used) *fallback_used = true;
      return 1.0;
    }
    if (p->stale_policy == PROG_STALE_FALLBACK) {
      if (fallback_used) *fallback_used = true;
      return clamp_adjust_factor(p->stale_fallback);
    }
  }

  if (!sensor->flags.data_ok) return 1.0;
  return calc_sensor_watering_int(p, sensor->last_data);
}

bool prog_adjust_is_stale(ProgSensorAdjust *p) {
  if (!p) return false;
  return prog_adjust_sensor_is_stale(p, sensor_by_nr(p->sensor), os.now_tz());
}

bool prog_adjust_uses_fallback(ProgSensorAdjust *p) {
  if (!p) return false;
  bool stale_active = false;
  bool fallback_used = false;
  calc_sensor_watering_for_sensor(p, sensor_by_nr(p->sensor), &stale_active, &fallback_used);
  return stale_active && fallback_used;
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
      result = result * calc_sensor_watering_for_sensor(p, sensor);
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
  obj[F("nr")] = nr;
  obj[F("type")] = type;
  obj[F("sensor")] = sensor;
  obj[F("prog")] = prog;
  obj[F("factor1")] = factor1;
  obj[F("factor2")] = factor2;
  obj[F("min")] = min;
  obj[F("max")] = max;
  obj[F("stale_timeout")] = stale_timeout;
  obj[F("stale_policy")] = stale_policy;
  obj[F("stale_fallback")] = stale_fallback;
  obj[F("name")] = name;
}

void ProgSensorAdjust::fromJson(ArduinoJson::JsonVariantConst obj) {
  nr = obj[F("nr")] | 0;
  type = obj[F("type")] | 0;
  sensor = obj[F("sensor")] | 0;
  prog = obj[F("prog")] | 0;
  factor1 = obj[F("factor1")] | 0.0;
  factor2 = obj[F("factor2")] | 0.0;
  min = obj[F("min")] | 0.0;
  max = obj[F("max")] | 0.0;
  stale_timeout = obj[F("stale_timeout")] | 0;
  stale_policy = obj[F("stale_policy")] | PROG_STALE_LAST_VALUE;
  if (stale_policy > PROG_STALE_FALLBACK) stale_policy = PROG_STALE_LAST_VALUE;
  stale_fallback = clamp_adjust_factor(obj[F("stale_fallback")] | 1.0);
  
  const char* nameStr = obj[F("name")] | "";
  SAFE_STRNCPY(name, nameStr, sizeof(name));
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
      result = result * calc_sensor_watering_for_sensor(p, sensor);
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
  
  // DEBUG_PRINTLN(F("prog_adjust_define"));
  
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
  obj["stale_timeout"] = 0;
  obj["stale_policy"] = PROG_STALE_LAST_VALUE;
  obj["stale_fallback"] = 1.0;
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

  // DEBUG_PRINTLN(F("prog_adjust_save"));

  const char *tmpfile = PROG_SENSOR_FILENAME ".tmp";
  const char *bakfile = PROG_SENSOR_FILENAME ".bak";

  // Create JSON document
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonArray array = doc.to<ArduinoJson::JsonArray>();

  // Serialize all prog sensor adjusts
  size_t expected = 0;
  for (auto &kv : progSensorAdjustsMap) {
    ProgSensorAdjust *pa = kv.second;
    if (pa) {
      ArduinoJson::JsonObject obj = array.add<ArduinoJson::JsonObject>();
      pa->toJson(obj);
      expected++;
    }
  }

  // Abort if the document could not be built completely (out of memory) so a
  // truncated write never clobbers the existing good file (cf. #263).
  if (doc.overflowed() || array.size() != expected) {
    DEBUG_PRINTLN(F("prog_adjust_save: JSON build overflowed, keeping previous file"));
    return;
  }

  // Write to a temp file first, validate, then atomically swap.
  if (file_exists(tmpfile)) remove_file(tmpfile);
  size_t written = 0;
  {
    FileWriter writer(tmpfile);
    written = ArduinoJson::serializeJson(doc, writer);
  }

  if (written < 2 || file_size(tmpfile) != written) {
    DEBUG_PRINTLN(F("prog_adjust_save: serialization failed, keeping previous file"));
    remove_file(tmpfile);
    return;
  }

  if (file_exists(PROG_SENSOR_FILENAME)) {
    if (file_exists(bakfile)) remove_file(bakfile);
    rename_file(PROG_SENSOR_FILENAME, bakfile);
  }
  if (!rename_file(tmpfile, PROG_SENSOR_FILENAME)) {
    if (file_exists(PROG_SENSOR_FILENAME)) remove_file(PROG_SENSOR_FILENAME);
    if (!rename_file(tmpfile, PROG_SENSOR_FILENAME)) {
      DEBUG_PRINTLN(F("prog_adjust_save: rename failed, restoring backup"));
      if (file_exists(bakfile)) rename_file(bakfile, PROG_SENSOR_FILENAME);
      remove_file(tmpfile);
    }
  }
}

// Parse a prog-adjust JSON file into progSensorAdjustsMap. Returns false if the
// file is missing or contains invalid/corrupt JSON.
static bool prog_adjust_load_file(const char *fn) {
  if (!file_exists(fn)) return false;

  FileReader reader(fn);
  ArduinoJson::JsonDocument doc;
  ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, reader);

  if (error) {
    DEBUG_PRINT(F("prog_adjust_load deserializeJson() failed: "));
    DEBUG_PRINTLN(error.c_str());
    return false;
  }

  if (!doc.is<ArduinoJson::JsonArray>()) {
    DEBUG_PRINTLN(F("prog_adjust JSON is not an array"));
    return false;
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

    progSensorAdjustsMap[pa->nr] = pa;
  }
  return true;
}

void prog_adjust_load() {
  // DEBUG_PRINTLN(F("prog_adjust_load"));

  // Clean up existing map
  for (auto &kv : progSensorAdjustsMap) {
    delete kv.second;
  }
  progSensorAdjustsMap.clear();

  if (prog_adjust_load_file(PROG_SENSOR_FILENAME)) return;

  // Primary missing or corrupt: try to recover from the backup before giving up.
  for (auto &kv : progSensorAdjustsMap) delete kv.second;
  progSensorAdjustsMap.clear();

  if (prog_adjust_load_file(PROG_SENSOR_FILENAME ".bak")) {
    DEBUG_PRINTLN(F("prog_adjust_load: recovered from backup"));
    prog_adjust_save();
    return;
  }

  if (!file_exists(PROG_SENSOR_FILENAME) && !file_exists(PROG_SENSOR_FILENAME ".bak")) {
    DEBUG_PRINTLN(F("No prog_adjust data found"));
  }
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
#elif defined(OSPI)
ulong diskFree() {
  struct statvfs stat;
  if (statvfs("/", &stat) == 0) {
    return (ulong)stat.f_bavail * stat.f_bsize;
  }
  return 0;
}
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
bool checkDiskFree() {
  if (diskFree() < MIN_DISK_FREE) {
    DEBUG_PRINT(F("fs has low space!"));
    return false;
  }
  return true;
}
#endif

// SensorBase default emitJson implementation
void SensorBase::emitJson(BufferFiller& bfill) const {
  ArduinoJson::JsonDocument *doc = new (std::nothrow) ArduinoJson::JsonDocument();
  if (!doc) {
    // Last-resort payload to keep the JSON stream valid under memory pressure.
    bfill.emit_p(PSTR("{\"nr\":$D,\"type\":$D}"), nr, type);
    return;
  }
  ArduinoJson::JsonObject obj = doc->to<ArduinoJson::JsonObject>();
  toJson(obj);

  // Temperature conversion removed — now handled by client app

  // Serialize direkt in den Zielbuffer (vermeidet Heap-String-Allokationen)
  const size_t cap = bfill.avail() + 1; // +1 für Nullterminator
  char* out = bfill.cursor();
  const size_t written = ArduinoJson::serializeJson(*doc, out, cap);
  bfill.advance(written);
  delete doc;
}

// SensorBase default implementation for getUnitId()
// Most sensor types override this method with their own implementation
unsigned char SensorBase::getUnitId() const {
  // Default fallback for unknown types
  // Group sensors have their own override in GroupSensor class
  return assigned_unitid;
}

const char* SensorBase::getUnit() const {
  int unitid = getUnitId();
  if (unitid == UNIT_USERDEF) return userdef_unit;
  if (unitid < 0 || unitid >= MAX_SENSOR_UNITNAMES)
    return sensor_unitNames[0];
  return sensor_unitNames[unitid];
}

// Wrapper functions for backward compatibility
const char *getSensorUnit(int unitid) {
  if (unitid == UNIT_USERDEF) return "?";
  if (unitid < 0 || unitid >= MAX_SENSOR_UNITNAMES)
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

// Static type-to-unit-id lookup — avoids allocating a sensor object on the stack.
// Mirrors the getUnitId() overrides in each sensor subclass.
unsigned char getSensorUnitId(int type) {
  switch (type) {
    // Truebner / TH100 RS485 Modbus
    case SENSOR_SMT100_MOIS:       return UNIT_PERCENT;
    case SENSOR_SMT100_TEMP:       return UNIT_DEGREE;
    case SENSOR_SMT100_PMTY:       return UNIT_DK;
    case SENSOR_TH100_MOIS:        return UNIT_HUM_PERCENT;
    case SENSOR_TH100_TEMP:        return UNIT_DEGREE;
    // ASB analog extension board
    case SENSOR_ANALOG_EXTENSION_BOARD:   return UNIT_VOLT;
    case SENSOR_ANALOG_EXTENSION_BOARD_P: return UNIT_LEVEL;
    case SENSOR_SMT50_MOIS:        return UNIT_PERCENT;
    case SENSOR_SMT50_TEMP:        return UNIT_DEGREE;
    case SENSOR_SMT100_ANALOG_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_ANALOG_TEMP: return UNIT_DEGREE;
    case SENSOR_VH400:             return UNIT_PERCENT;
    case SENSOR_THERM200:          return UNIT_DEGREE;
    case SENSOR_AQUAPLUMB:         return UNIT_PERCENT;
    case SENSOR_USERDEF:           return UNIT_USERDEF;
    // OSPi ADC
    case SENSOR_OSPI_ANALOG:       return UNIT_VOLT;
    case SENSOR_OSPI_ANALOG_P:     return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_MOIS: return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_TEMP: return UNIT_DEGREE;
    // Internal / system
    case SENSOR_INTERNAL_TEMP:     return UNIT_DEGREE;
    case SENSOR_FREE_MEMORY:       return UNIT_USERDEF;
    case SENSOR_FREE_STORE:        return UNIT_USERDEF;
    // FYTA
    case SENSOR_FYTA_MOISTURE:     return UNIT_PERCENT;
    case SENSOR_FYTA_TEMPERATURE:  return UNIT_DEGREE;
  #if defined(ESP32) || defined(OSPI)
    // Gardena
    case SENSOR_GARDENA_MOISTURE:  return UNIT_PERCENT;
    case SENSOR_GARDENA_TEMPERATURE:return UNIT_DEGREE;
  #endif
    // Weather
    case SENSOR_WEATHER_TEMP_F:    return UNIT_FAHRENHEIT;
    case SENSOR_WEATHER_TEMP_C:    return UNIT_DEGREE;
    case SENSOR_WEATHER_HUM:       return UNIT_HUM_PERCENT;
    case SENSOR_WEATHER_PRECIP_IN: return UNIT_INCH;
    case SENSOR_WEATHER_PRECIP_MM: return UNIT_MM;
    case SENSOR_WEATHER_WIND_MPH:  return UNIT_MPH;
    case SENSOR_WEATHER_WIND_KMH:  return UNIT_KMH;
    // Variable-unit or runtime-determined types
    case SENSOR_MQTT:
    case SENSOR_REMOTE_JSON:
    case SENSOR_REMOTE:
    case SENSOR_MODBUS_RTU:
    case SENSOR_BLE:
    case SENSOR_ZIGBEE:
    case SENSOR_FLOW_PULSE:
    default:                       return UNIT_USERDEF;
  }
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
#if defined(DISABLE_INFLUXDB)
  (void)sensor;
  return;
#else
  if (!os.influxdb.isEnabled() || !sensor)
    return;

  // Common setup (shared by all platforms)
  char devname_safe[64];
  char sensor_name_safe[sizeof(sensor->name) + 1];
  char unit_safe[16];

  devname_safe[0] = '\0';
  sensor_name_safe[0] = '\0';
  unit_safe[0] = '\0';

  os.sopt_load(SOPT_DEVICE_NAME, tmp_buffer);
  SAFE_STRNCPY(devname_safe, tmp_buffer, sizeof(devname_safe));
  SAFE_STRNCPY(sensor_name_safe, sensor->name, sizeof(sensor_name_safe));
  const char* unit = getSensorUnit(sensor);
  if (unit) {
    SAFE_STRNCPY(unit_safe, unit, sizeof(unit_safe));
  }

  #if defined(ESP8266) || defined(ESP32)
  Point sensor_data("analogsensor");
  sensor_data.addTag("devicename", devname_safe);
  snprintf(tmp_buffer, 10, "%d", sensor->nr);
  sensor_data.addTag("nr", tmp_buffer);
  sensor_data.addTag("name", sensor_name_safe);
  sensor_data.addTag("unit", unit_safe);
  sensor_data.addField("native_data", sensor->last_native_data);
  sensor_data.addField("data", sensor->last_data);
  os.influxdb.write_influx_data(sensor_data);

  #else
  // Backoff: skip InfluxDB 60s after failure
  static ulong influx_last_fail = 0;
  static int influx_fail_count = 0;
  if (influx_fail_count > 0 && (millis() - influx_last_fail) < 60000UL)
    return;

  influxdb_cpp::server_info * client = os.influxdb.get_client();
  if (!client)
    return;

  char nr_buf[10];
  snprintf(nr_buf, 10, "%d", sensor->nr);
  int rc = influxdb_cpp::builder()
    .meas("analogsensor")
    .tag("devicename", devname_safe)
    .tag("nr", nr_buf)
    .tag("name", sensor_name_safe)
    .tag("unit", unit_safe)
    .field("native_data", (long)sensor->last_native_data)
    .field("data", sensor->last_data, 2)
    .timestamp(millis())
    .post_http(*client, NULL, 5);

  if (rc != 0) {
    influx_fail_count++;
    influx_last_fail = millis();
  } else {
    influx_fail_count = 0;
  }

  #endif
#endif // DISABLE_INFLUXDB
}


//Value Monitoring

/**
 * @brief Serialize Monitor to JSON
 */
void Monitor::toJson(ArduinoJson::JsonObject obj) const {
  obj[F("nr")] = nr;
  obj[F("type")] = type;
  obj[F("sensor")] = sensor;
  obj[F("prog")] = prog;
  obj[F("zone")] = zone;
  obj[F("active")] = active;
  obj[F("time")] = time;
  obj[F("name")] = name;
  obj[F("maxRuntime")] = maxRuntime;
  obj[F("prio")] = prio;
  obj[F("reset_seconds")] = reset_seconds;
  obj[F("output_mode")] = output_mode;
  obj[F("stale_timeout")] = stale_timeout;
  obj[F("failsafe_active")] = failsafe_active;
  
  // Serialize Monitor_Union based on type
  ArduinoJson::JsonObject mObj = obj[F("m")].to<ArduinoJson::JsonObject>();
  switch(type) {
    case MONITOR_MIN:
    case MONITOR_MAX:
      mObj[F("value1")] = m.minmax.value1;
      mObj[F("value2")] = m.minmax.value2;
      break;
    case MONITOR_SENSOR12:
      mObj[F("sensor12")] = m.sensor12.sensor12;
      mObj[F("invers")] = m.sensor12.invers;
      break;
    case MONITOR_SET_SENSOR12:
      mObj[F("monitor")] = m.set_sensor12.monitor;
      mObj[F("sensor12")] = m.set_sensor12.sensor12;
      break;
    case MONITOR_AND:
    case MONITOR_OR:
    case MONITOR_XOR:
      mObj[F("monitor1")] = m.andorxor.monitor1;
      mObj[F("monitor2")] = m.andorxor.monitor2;
      mObj[F("monitor3")] = m.andorxor.monitor3;
      mObj[F("monitor4")] = m.andorxor.monitor4;
      mObj[F("invers1")] = m.andorxor.invers1;
      mObj[F("invers2")] = m.andorxor.invers2;
      mObj[F("invers3")] = m.andorxor.invers3;
      mObj[F("invers4")] = m.andorxor.invers4;
      break;
    case MONITOR_NOT:
      mObj[F("monitor")] = m.mnot.monitor;
      break;
    case MONITOR_TIME:
      mObj[F("time_from")] = m.mtime.time_from;
      mObj[F("time_to")] = m.mtime.time_to;
      mObj[F("weekdays")] = m.mtime.weekdays;
      break;
    case MONITOR_REMOTE:
      mObj[F("rmonitor")] = m.remote.rmonitor;
      mObj[F("ip")] = m.remote.ip;
      mObj[F("port")] = m.remote.port;
      break;
  }
}

/**
 * @brief Deserialize Monitor from JSON
 */
void Monitor::fromJson(ArduinoJson::JsonVariantConst obj) {
  nr = obj[F("nr")] | 0;
  type = obj[F("type")] | 0;
  sensor = obj[F("sensor")] | 0;
  prog = obj[F("prog")] | 0;
  zone = obj[F("zone")] | 0;
  active = obj[F("active")] | false;
  time = obj[F("time")] | 0;
  SAFE_STRNCPY(name, obj[F("name")] | "", sizeof(name));
  maxRuntime = obj[F("maxRuntime")] | 0;
  prio = obj[F("prio")] | 0;
  reset_seconds = obj[F("reset_seconds")] | 0;
  output_mode = obj[F("output_mode")] | 0;
  stale_timeout = obj[F("stale_timeout")] | 0;
  failsafe_active = obj[F("failsafe_active")] | 0;
  reset_time = 0;
  
  // Deserialize Monitor_Union based on type
  memset(&m, 0, sizeof(Monitor_Union_t));
  ArduinoJson::JsonVariantConst mVar = obj[F("m")];
  if (!mVar.isNull()) {
    switch(type) {
      case MONITOR_MIN:
      case MONITOR_MAX:
        m.minmax.value1 = mVar[F("value1")] | 0.0;
        m.minmax.value2 = mVar[F("value2")] | 0.0;
        break;
      case MONITOR_SENSOR12:
        m.sensor12.sensor12 = mVar[F("sensor12")] | 0;
        m.sensor12.invers = mVar[F("invers")] | false;
        break;
      case MONITOR_SET_SENSOR12:
        m.set_sensor12.monitor = mVar[F("monitor")] | 0;
        m.set_sensor12.sensor12 = mVar[F("sensor12")] | 0;
        break;
      case MONITOR_AND:
      case MONITOR_OR:
      case MONITOR_XOR:
        m.andorxor.monitor1 = mVar[F("monitor1")] | 0;
        m.andorxor.monitor2 = mVar[F("monitor2")] | 0;
        m.andorxor.monitor3 = mVar[F("monitor3")] | 0;
        m.andorxor.monitor4 = mVar[F("monitor4")] | 0;
        m.andorxor.invers1 = mVar[F("invers1")] | false;
        m.andorxor.invers2 = mVar[F("invers2")] | false;
        m.andorxor.invers3 = mVar[F("invers3")] | false;
        m.andorxor.invers4 = mVar[F("invers4")] | false;
        break;
      case MONITOR_NOT:
        m.mnot.monitor = mVar[F("monitor")] | 0;
        break;
      case MONITOR_TIME:
        m.mtime.time_from = mVar[F("time_from")] | 0;
        m.mtime.time_to = mVar[F("time_to")] | 0;
        m.mtime.weekdays = mVar[F("weekdays")] | 0;
        break;
      case MONITOR_REMOTE:
        m.remote.rmonitor = mVar[F("rmonitor")] | 0;
        m.remote.ip = mVar[F("ip")] | 0;
        m.remote.port = mVar[F("port")] | 0;
        break;
    }
  }
}

// Parse a monitor JSON file into monitorsMap. Returns false if the file is
// missing or contains invalid/corrupt JSON (in which case monitorsMap is left
// unchanged so a caller can fall back to a backup file).
static bool monitor_load_file(const char *fn) {
  if (!file_exists(fn)) return false;

  FileReader reader(fn);
  ArduinoJson::JsonDocument doc;
  ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, reader);

  if (error) {
    DEBUG_PRINT(F("monitor_load deserializeJson() failed: "));
    DEBUG_PRINTLN(error.c_str());
    return false;
  }

  if (!doc.is<ArduinoJson::JsonArray>()) {
    DEBUG_PRINTLN(F("monitor JSON is not an array"));
    return false;
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

    monitorsMap[mon->nr] = mon;
  }
  return true;
}

void monitor_load() {
  // DEBUG_PRINTLN(F("monitor_load"));

  // Clean up existing map
  for (auto &kv : monitorsMap) {
    delete kv.second;
  }
  monitorsMap.clear();

  // Primary file present and valid -> done.
  if (monitor_load_file(MONITOR_FILENAME)) {
    // DEBUG_PRINT(F("Loaded ")); DEBUG_PRINT(monitor_count()); DEBUG_PRINTLN(F(" monitors"));
    return;
  }

  // Primary missing or corrupt: try to recover from the backup written by
  // monitor_save(). This protects against an interrupted/failed previous save
  // that would otherwise wipe the whole monitor list (#263).
  for (auto &kv : monitorsMap) delete kv.second;
  monitorsMap.clear();

  if (monitor_load_file(MONITOR_FILENAME ".bak")) {
    DEBUG_PRINTLN(F("monitor_load: recovered monitors from backup"));
    monitor_save();  // re-materialize a valid primary file
    return;
  }

  if (!file_exists(MONITOR_FILENAME) && !file_exists(MONITOR_FILENAME ".bak")) {
    DEBUG_PRINTLN(F("No monitor data found"));
  }
}

void monitor_save() {
  if (!apiInit) return;

  // DEBUG_PRINTLN(F("monitor_save"));

  const char *tmpfile = MONITOR_FILENAME ".tmp";
  const char *bakfile = MONITOR_FILENAME ".bak";

  // Create JSON document
  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonArray array = doc.to<ArduinoJson::JsonArray>();

  // Serialize all monitors
  size_t expected = 0;
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    if (mon) {
      ArduinoJson::JsonObject obj = array.add<ArduinoJson::JsonObject>();
      mon->toJson(obj);
      expected++;
    }
  }

  // Bail out if the document could not be built completely (out of memory).
  // Writing a truncated document over the live file is exactly what caused the
  // total loss of all monitors reported in #263.
  if (doc.overflowed() || array.size() != expected) {
    DEBUG_PRINTLN(F("monitor_save: JSON build overflowed, keeping previous file"));
    return;
  }

  // Write to a temporary file first so a failed/partial write never clobbers
  // the existing good data.
  if (file_exists(tmpfile)) remove_file(tmpfile);
  size_t written = 0;
  {
    FileWriter writer(tmpfile);
    written = ArduinoJson::serializeJson(doc, writer);
  }  // writer flushes its buffer on destruction here

  // Validate the temp file: a valid serialization is at least "[]" (2 bytes)
  // and the on-disk size must match what serializeJson reported.
  if (written < 2 || file_size(tmpfile) != written) {
    DEBUG_PRINTLN(F("monitor_save: serialization failed, keeping previous file"));
    remove_file(tmpfile);
    return;  // previous good file (if any) stays intact
  }

  // Rotate the current good file to a backup, then atomically swap in the new
  // validated file.
  if (file_exists(MONITOR_FILENAME)) {
    if (file_exists(bakfile)) remove_file(bakfile);
    rename_file(MONITOR_FILENAME, bakfile);
  }

  if (!rename_file(tmpfile, MONITOR_FILENAME)) {
    // Some filesystems refuse rename onto an existing target; retry after
    // removing it.
    if (file_exists(MONITOR_FILENAME)) remove_file(MONITOR_FILENAME);
    if (!rename_file(tmpfile, MONITOR_FILENAME)) {
      // Last resort: restore from backup so we are not left without a file.
      DEBUG_PRINTLN(F("monitor_save: rename failed, restoring backup"));
      if (file_exists(bakfile)) rename_file(bakfile, MONITOR_FILENAME);
      remove_file(tmpfile);
    }
  }
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

bool monitor_define(uint nr, uint type, uint sensor, uint prog, uint zone, const Monitor_Union_t m, char * name, ulong maxRuntime, uint8_t prio, ulong reset_seconds, uint8_t output_mode, ulong stale_timeout, uint8_t failsafe_active) {
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
    p->output_mode = output_mode;
    p->stale_timeout = stale_timeout;
    p->failsafe_active = failsafe_active;
    SAFE_STRNCPY(p->name, name, sizeof(p->name));
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
    p->output_mode = output_mode;
    p->stale_timeout = stale_timeout;
    p->failsafe_active = failsafe_active;
    SAFE_STRNCPY(p->name, name, sizeof(p->name));
    
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

  // DEBUG_PRINTLN(F("start_monitor_action"));
  // DEBUG_PRINT(F("Zone: "));
  // DEBUG_PRINTLN(mon->zone);
  // DEBUG_PRINT(F("Max Runtime: "));
  // DEBUG_PRINTLN(mon->maxRuntime);

  if (mon->zone > 0) {
    uint sid = mon->zone-1;

		// schedule manual station
		// skip if the station is a master station
		// (because master cannot be scheduled independently)
		if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
			return;

		unsigned char bid = sid >> 3;
		unsigned char s = sid & 0x07;
		if (os.attrib_dis[bid] & (1 << s)) {
			return; // skip if the station is disabled/deactivated
		}

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
    // DEBUG_PRINTLN(F("start_monitor_action: queue not full"));
		if (q) {
			q->st = 0;
			q->dur = timer;
			q->sid = sid;
			q->pid = 253;
			schedule_all_stations(mon->time);
      // DEBUG_PRINTLN(F("start_monitor_action: schedule_all_stations"));
		} 
  }
}

void stop_monitor_action(Monitor_t * mon) {
  mon->time = os.now_tz();
  if (mon->zone > 0) {
    int sid = mon->zone-1;
    bool had_zone_entries = false;

    // Stop *all* queued/running entries for this zone, independent of program.
    // This matches the zone-level monitor expectation: one stop action should
    // clear the complete zone pipeline.
    for (int i = pd.nqueue - 1; i >= 0; i--) {
      RuntimeQueueStruct *q = &pd.queue[i];
      if (q->sid == sid) {
        q->deque_time = mon->time;
        had_zone_entries = true;
      }
    }

    if (had_zone_entries) {
      turn_off_station(sid, mon->time, 0);
      // DEBUG_PRINTLN(F("stop_monitor_action: turn_off_station"));
    }
  }

  if (mon->prog > 0) {
    stop_program(mon->prog);

    // Turn off all running/queued stations of this program (mon->prog)
    // Note: Ad-hoc "Run-Once with repeat" program deletion is now handled in stop_program()
    for (int i = pd.nqueue - 1; i >= 0; i--) {
      RuntimeQueueStruct *q = &pd.queue[i];
      if (q->pid == mon->prog || q->pid == (mon->prog | 0x80)) {
        q->deque_time = mon->time;
        turn_off_station(q->sid, mon->time, 0);
      }
    }
  }
}

void push_message(Monitor_t * mon, float value, int monidx) {
  uint32_t type; 
  switch(mon->prio) {
    case 0: type = NOTIFY_MONITOR_LOW; break;
    case 1: type = NOTIFY_MONITOR_MID; break;
    case 2: type = NOTIFY_MONITOR_HIGH; break;
    default: return;
  }
  char name[30];
  SAFE_STRNCPY(name, mon->name, sizeof(name));
  DEBUG_PRINT(F("monitoring: activated "));
  DEBUG_PRINT(name);
  DEBUG_PRINT(F(" - "));
  DEBUG_PRINTLN(type);
  notif.add(type, (uint32_t)mon->prio, value, (uint8_t)monidx);
}

bool get_monitor(uint nr, bool inv, bool defaultBool) {
  Monitor_t *mon = monitor_by_nr(nr);
  if (!mon) return defaultBool;
  return inv ? !mon->active : mon->active;
}

bool get_remote_monitor(Monitor_t *mon, bool defaultBool) {
  unsigned char ip[4];
  IP4_EXTRACT_BYTES(ip, mon->m.remote.ip);

  // DEBUG_PRINTLN(F("read_monitor_http"));

  char *p = tmp_buffer;
  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);

  bf.emit_p(PSTR("GET /ml?pw=$O&nr=$D"), SOPT_PASSWORD, mon->m.remote.rmonitor);
  bf.emit_p(PSTR(" HTTP/1.0\r\nHOST: $D.$D.$D.$D\r\n\r\n"), ip[0], ip[1], ip[2], ip[3]);

  // DEBUG_PRINTLN(p);

  char server[20];
  sprintf(server, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  int res = os.send_http_request(server, mon->m.remote.port, p, NULL, false, 500);
  if (res == HTTP_RQT_SUCCESS) {
    DEBUG_PRINTLN(F("Send Ok"));
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

// Read a monitor's pending (scratch) state during the combinational
// evaluation phase. Using the scratch image (eval_active) instead of the
// live `active` value makes logic monitors (NOT/AND/OR/XOR/SET_SENSOR12)
// independent of the order in which monitors are stored/evaluated.
static bool get_monitor_eval(uint nr, bool inv, bool defaultBool) {
  Monitor_t *mon = monitor_by_nr(nr);
  if (!mon) return defaultBool;
  return inv ? !mon->eval_active : mon->eval_active;
}

void check_monitors() {
  //DEBUG_PRINTLN(F("check_monitors"));
  time_os_t timeNow = os.now_tz();

  os.status.forced_sensor1 = 0;
  os.status.forced_sensor2 = 0;

  // ---------------------------------------------------------------------
  // Two-phase, order-independent evaluation.
  //
  // Phase 1 computes a stable "process image" of every monitor state into
  // the transient eval_active/eval_value fields without touching outputs:
  //   1a) input/leaf monitors (sensor min/max, sensor1/2, time, remote)
  //       are evaluated once - they do not depend on other monitors.
  //   1b) logic monitors (NOT/AND/OR/XOR/SET_SENSOR12) are evaluated
  //       repeatedly against the image until it no longer changes, so the
  //       result no longer depends on the monitor order (fixes the
  //       transient "signal and its inverse both active" glitches).
  // Phase 2 then applies the resulting states atomically (start/stop
  // actions, notifications and reset-timer handling).
  // ---------------------------------------------------------------------

  // Seed the image with the current states (used as latch input and as the
  // default for monitors that are not (re)computed this cycle).
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    mon->eval_active = mon->active;
    mon->eval_value = 0;
  }

  // Phase 1a: input / leaf monitors (independent of other monitors)
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;

    switch(mon->type) {
      case MONITOR_MIN:
      case MONITOR_MAX: {
        SensorBase * sensor = sensor_by_nr(mon->sensor);
        if (sensor && sensor->flags.data_ok) {
          mon->last_ok_time = timeNow;
          double value = sensor->last_data;
          mon->eval_value = value;

          double v_min = mon->m.minmax.value1 <= mon->m.minmax.value2 ? mon->m.minmax.value1 : mon->m.minmax.value2;
          double v_max = mon->m.minmax.value1 >= mon->m.minmax.value2 ? mon->m.minmax.value1 : mon->m.minmax.value2;

          if (v_min == v_max) {
            if (mon->type == MONITOR_MIN) {
              mon->eval_active = (value <= v_min);
            } else {
              mon->eval_active = (value >= v_max);
            }
          } else {
            // hysteresis: latch off the previous output state
            if (!mon->active) {
              if ((mon->type == MONITOR_MIN && value <= v_min) ||
                (mon->type == MONITOR_MAX && value >= v_max)) {
                mon->eval_active = true;
              }
            } else {
              if ((mon->type == MONITOR_MIN && value >= v_max) ||
                (mon->type == MONITOR_MAX && value <= v_min)) {
                mon->eval_active = false;
              }
            }
          }
        } else if (mon->stale_timeout > 0) {
          // Failsafe: the referenced sensor has no valid data. Once the data
          // has been stale for longer than stale_timeout, force the output to
          // the configured failsafe state instead of latching the last value.
          if (mon->last_ok_time == 0) mon->last_ok_time = timeNow; // seed from boot/first eval
          if (timeNow >= mon->last_ok_time + mon->stale_timeout) {
            mon->eval_active = (mon->failsafe_active != 0);
          }
          // else: within grace period -> keep previous state (seeded above)
        }
        // stale_timeout==0 -> legacy behavior: keep previous eval_active.
        break; }

      case MONITOR_SENSOR12: {
        if (mon->m.sensor12.sensor12 == 1) {
          if (os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_NONE || os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_SOIL) {
            mon->eval_active = mon->m.sensor12.invers ? !os.status.sensor1_active : os.status.sensor1_active;
          }
        } else if (mon->m.sensor12.sensor12 == 2) {
          if (os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_NONE || os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_RAIN || os.iopts[IOPT_SENSOR2_TYPE] == SENSOR_TYPE_SOIL) {
            mon->eval_active = mon->m.sensor12.invers ? !os.status.sensor2_active : os.status.sensor2_active;
          }
        }
        break;
      }

      case MONITOR_TIME: {
        uint32_t seconds_of_day = timeNow % 86400L;
        uint16_t time = (seconds_of_day / 3600) * 100 + (seconds_of_day % 3600) / 60; //HHMM
        uint8_t wday = (timeNow / 86400L + 3) % 7; //Monday = 0
        bool in_window = (mon->m.mtime.weekdays >> wday) & 0x01;
        if (mon->m.mtime.time_from > mon->m.mtime.time_to) // FROM > TO ? Over night value
          in_window &= time >= mon->m.mtime.time_from || time <= mon->m.mtime.time_to;
        else
          in_window &= time >= mon->m.mtime.time_from && time <= mon->m.mtime.time_to;

        if (mon->reset_seconds == 0) {
          mon->eval_active = in_window;
        } else {
          if (!in_window) {
            mon->eval_active = false;
            mon->reset_time = 0;
          } else {
            if (mon->reset_time == 0) {
              mon->eval_active = true;
            } else if (timeNow < mon->reset_time) {
              mon->eval_active = true;
            } else {
              mon->eval_active = false;
            }
          }
        }
        break;
      }

      case MONITOR_REMOTE:
        mon->eval_active = get_remote_monitor(mon, mon->active);
        break;

      default:
        // logic monitors are handled in phase 1b
        break;
    }
  }

  // Phase 1b: logic monitors, iterated until the image is stable.
  // The guard limit bounds the work for (accidental) cyclic definitions.
  size_t monCount = monitorsMap.size();
  size_t maxIter = monCount + 2;
  bool changed = true;
  for (size_t iter = 0; changed && iter < maxIter; iter++) {
    changed = false;
    for (auto &kv : monitorsMap) {
      Monitor_t *mon = kv.second;
      bool newState = mon->eval_active;

      switch(mon->type) {
        case MONITOR_SET_SENSOR12:
          newState = get_monitor_eval(mon->m.set_sensor12.monitor, false, false);
          break;
        case MONITOR_AND:
          newState = get_monitor_eval(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, true) &&
            get_monitor_eval(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, true) &&
            get_monitor_eval(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, true) &&
            get_monitor_eval(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, true);
          break;
        case MONITOR_OR:
          newState = get_monitor_eval(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, false) ||
            get_monitor_eval(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, false) ||
            get_monitor_eval(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, false) ||
            get_monitor_eval(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, false);
          break;
        case MONITOR_XOR:
          newState = get_monitor_eval(mon->m.andorxor.monitor1, mon->m.andorxor.invers1, false) ^
            get_monitor_eval(mon->m.andorxor.monitor2, mon->m.andorxor.invers2, false) ^
            get_monitor_eval(mon->m.andorxor.monitor3, mon->m.andorxor.invers3, false) ^
            get_monitor_eval(mon->m.andorxor.monitor4, mon->m.andorxor.invers4, false);
          break;
        case MONITOR_NOT:
          newState = get_monitor_eval(mon->m.mnot.monitor, true, false);
          break;
        default:
          continue; // leaf monitor, already final
      }

      if (newState != mon->eval_active) {
        mon->eval_active = newState;
        changed = true;
      }
    }
  }

  // Phase 1c: derive forced sensor1/2 bits from the final SET_SENSOR12 states.
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    if (mon->type == MONITOR_SET_SENSOR12) {
      if (mon->m.set_sensor12.sensor12 == 1) {
        os.status.forced_sensor1 = mon->eval_active;
      }
      if (mon->m.set_sensor12.sensor12 == 2) {
        os.status.forced_sensor2 = mon->eval_active;
      }
    }
  }

  // Phase 2: apply the computed states atomically (outputs + reset timers).
  int monidx = 0;
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    uint nr = mon->nr;

    bool wasActive = mon->active;
    double value = mon->eval_value;
    mon->active = mon->eval_active;

    bool stopOnly = (mon->output_mode == MONITOR_OUTPUT_STOPONLY);

    if (mon->active != wasActive) {
      if (mon->active) {
        if (mon->reset_seconds > 0) {
          mon->reset_time = timeNow + mon->reset_seconds; 
        } else {
          mon->reset_time = 0;
        }
        if (stopOnly)
          stop_monitor_action(mon);  // stop-only: an active condition forces the output OFF
        else
          start_monitor_action(mon);
        push_message(mon, value, monidx);
        mon = monitor_by_nr(nr); //restart because if send by mail we unloaded+reloaded the monitors
      } else {
        // Became inactive: a stop-only monitor must NOT take any action here
        // (it may only turn off, never start). It leaves the zone/program to
        // the normal scheduler again.
        if (!stopOnly)
          stop_monitor_action(mon);
      }
    } else if (mon->active) {
      if (mon->reset_time > 0 && mon->reset_time < timeNow) { //time is over
        mon->active = false;
        if (!stopOnly)
          stop_monitor_action(mon);
        mon->reset_time = timeNow + mon->reset_seconds; 
      } else if (mon->reset_time == 0 && mon->reset_seconds > 0) { //reset time not set, but reset seconds is set
        mon->reset_time = timeNow + mon->reset_seconds; 
      } else if (stopOnly && mon->reset_seconds == 0) {
        // Continuously suppress while active: if the scheduler (or a manual run)
        // re-started the zone/program, force it off again on the next cycle.
        // If reset_seconds > 0, we intentionally use pulse behavior: stop only
        // on each new activation edge (or periodic re-activation), not every cycle.
        stop_monitor_action(mon);
      }
    }

    monidx++;
  }
}

void replace_pid(uint old_pid, uint new_pid) {
  for (auto &kv : monitorsMap) {
    Monitor_t *mon = kv.second;
    if (mon->prog == old_pid) {
      // DEBUG_PRINT(F("replace_pid: "));
      // DEBUG_PRINT(old_pid);
      // DEBUG_PRINT(F(" with "));
      // DEBUG_PRINTLN(new_pid);
      mon->prog = new_pid;
    }
  }
  for (auto &kv : progSensorAdjustsMap) {
    ProgSensorAdjust *psa = kv.second;
    if (psa && psa->prog == old_pid) {
      // DEBUG_PRINT(F("replace_pid psa: "));
      // DEBUG_PRINT(old_pid);
      // DEBUG_PRINT(F(" with "));
      // DEBUG_PRINTLN(new_pid);
      psa->prog = new_pid;
    }
  }
  sensor_save_all();
}

bool is_program_blocked_by_monitor(unsigned char pid) {
  for (auto &kv : monitorsMap) {
    Monitor *mon = kv.second;
    if (mon && mon->prog == (uint)(pid + 1)) {
      if (!mon->active) {
        return true; // Program has an associated monitor and the monitor is NOT active.
      }
    }
  }
  return false;
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

static inline bool is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

const char* findSegment(const char* payload, const char* p, size_t length, const char* segment, size_t seg_len) {
  if (seg_len == 0) return p;
  size_t payload_offset = p - payload;
  if (payload_offset + seg_len > length) return NULL;
  
  for (size_t i = payload_offset; i <= length - seg_len; i++) {
    if (payload[i] == 0) break;
    if (payload[i] == segment[0] && strncmp(payload + i, segment, seg_len) == 0) {
      bool boundary_ok = true;
      if (is_word_char(segment[0]) && i > 0 && is_word_char(payload[i - 1])) {
        boundary_ok = false;
      }
      if (is_word_char(segment[seg_len - 1]) && i + seg_len < length && is_word_char(payload[i + seg_len])) {
        boundary_ok = false;
      }
      if (boundary_ok) {
        return payload + i;
      }
    }
  }
  return NULL;
}

int findValue(const char *payload, unsigned int length, const char *jsonFilter, double& value) {
  char *p = (char*)payload;				
  bool emptyFilter = !jsonFilter||!jsonFilter[0];

  char filterbuf[128];
  const char* segments[16];
  int segment_index[16];
  int segment_count = 0;
  int arrayIndex = 0;

  if (!emptyFilter) {
    strncpy(filterbuf, jsonFilter, sizeof(filterbuf)-1);
    filterbuf[sizeof(filterbuf)-1] = 0;

    char *token = strtok(filterbuf, "|");
    while (token && segment_count < 16) {
      segments[segment_count] = token;
      segment_index[segment_count] = -1;

      char *lb = strrchr(token, '[');
      if (lb) {
        char *rb = strchr(lb, ']');
        if (rb && rb[1] == 0 && rb > lb+1) {
          int idx = atoi(lb+1);
          if (idx < 0) idx = 0;
          segment_index[segment_count] = idx;
          *lb = 0;
        }
      }

      segment_count++;
      token = strtok(NULL, "|");
    }
  }

  for (int i = 0; !emptyFilter && i < segment_count && p; i++) {
    const char *seg = segments[i];
    size_t seg_len = strlen(seg);
    p = (char*)findSegment(payload, p, length, seg, seg_len);
    if (!p) break;
    p += seg_len;

    int idx = segment_index[i];
    if (idx >= 0) {
      if (i + 1 < segment_count) {
        const char *next_seg = segments[i + 1];
        size_t next_len = strlen(next_seg);
        for (int r = 0; r < idx && p; r++) {
          p = (char*)findSegment(payload, p, length, next_seg, next_len);
          if (p) p += next_len;
        }
      } else {
        arrayIndex = idx;
      }
    }
  }

  if (p) {
    char buf[30];
    // Advance to the requested array element by skipping preceding numeric tokens.
    for (int idx = 0; idx < arrayIndex; idx++) {
      p = strpbrk(p, "0123456789.-+");
      if (!p || p >= (char*)payload+length) { p = NULL; break; }
      while (p && *p && p < (char*)payload+length &&
             ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+')) p++;
    }
    if (p) p = strpbrk(p, "0123456789.-+");
    uint i = 0;
    while (p && i < sizeof(buf) && p < (char*)payload+length) {
      char ch = *p++;
      if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') {
        buf[i++] = ch;
      } else break;
    }
    buf[i] = 0;
    DEBUG_PRINT(F("result: "));
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
			p = (char*)findSegment(payload, p, length, jsonFilter, f-jsonFilter);
			jsonFilter = f+1;
		} else {
			p = (char*)findSegment(payload, p, length, jsonFilter, strlen(jsonFilter));
			break;
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
