/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * sensors header file
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

#ifndef _SENSORS_H
#define _SENSORS_H

#if defined(ARDUINO)
#include <Arduino.h>
#include <sys/stat.h>
#elif defined(OSPI)  // headers for RPI/BBB
#include <stdio.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/ioctl.h>
extern "C" {
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>
}
#else  // generic native / DEMO builds (e.g. Windows)
#include <stdio.h>
#include <limits.h>
  #if defined(__has_include)
    #if __has_include(<sys/time.h>)
      #include <sys/time.h>
    #endif
  #else
    #include <sys/time.h>
  #endif
#endif
#include <map>
#include "defines.h"
#include "utils.h"
#include "notifier.h"
#if defined(ESP8266) || defined(ESP32)
#include <ADS1X15.h>
#endif
#include "program.h"

// Files
#if !defined(ESP32)
#define SENSOR_FILENAME_JSON "sensors.json"   // sensor configuration (JSON format)
#define PROG_SENSOR_FILENAME "progsensor.json"  // sensor to program assign filename (JSON format)
#define SENSORLOG_FILENAME1 "sensorlog.dat"   // analog sensor log filename
#define SENSORLOG_FILENAME2 "sensorlog2.dat"  // analog sensor log filename2
#define MONITOR_FILENAME "monitors.json"

#define SENSORLOG_FILENAME_WEEK1 \
  "sensorlogW1.dat"  // analog sensor log filename for  week average
#define SENSORLOG_FILENAME_WEEK2 \
  "sensorlogW2.dat"  // analog sensor log filename2 for week average
#define SENSORLOG_FILENAME_MONTH1 \
  "sensorlogM1.dat"  // analog sensor log filename for month average
#define SENSORLOG_FILENAME_MONTH2 \
  "sensorlogM2.dat"  // analog sensor log filename2 for month average

#else
#define SENSOR_FILENAME_JSON "/sensors.json"  // sensor configuration (JSON format)
#define PROG_SENSOR_FILENAME "/progsensor.json"  // sensor to program assign filename (JSON format)
#define SENSORLOG_FILENAME1 "/sensorlog.dat"   // analog sensor log filename
#define SENSORLOG_FILENAME2 "/sensorlog2.dat"  // analog sensor log filename2
#define MONITOR_FILENAME "/monitors.json"

#define SENSORLOG_FILENAME_WEEK1 \
  "/sensorlogW1.dat"  // analog sensor log filename for  week average
#define SENSORLOG_FILENAME_WEEK2 \
  "/sensorlogW2.dat"  // analog sensor log filename2 for week average
#define SENSORLOG_FILENAME_MONTH1 \
  "/sensorlogM1.dat"  // analog sensor log filename for month average
#define SENSORLOG_FILENAME_MONTH2 \
  "/sensorlogM2.dat"  // analog sensor log filename2 for month average


#endif

// MaxLogSize
#if defined(ESP32) || defined(OSPI)
#define MAX_LOG_SIZE 2097152 // 2MB max
#else 
#define MAX_LOG_SIZE 8000
#endif

// Sensor types:
#define SENSOR_NONE                     0   // None or deleted sensor

#define RS485_SENSORS_START             1   // Generic RS485 sensor id
#define RS485_SENSORS_END               9  // Generic RS485 sensor id
#define SENSOR_SMT100_MOIS              1   // Truebner SMT100 RS485, moisture mode
#define SENSOR_SMT100_TEMP              2   // Truebner SMT100 RS485, temperature mode
#define SENSOR_SMT100_PMTY              3   // Truebner SMT100 RS485, permittivity mode
#define SENSOR_TH100_MOIS               4   // Truebner TH100 RS485,  humidity mode
#define SENSOR_TH100_TEMP               5   // Truebner TH100 RS485,  temperature mode
#define SENSOR_RS485                    9  // RS485 generic sensor

#define ASB_SENSORS_START               10  // starting id for ASB sensors
#define ASB_SENSORS_END                 49  // ending id for ASB sensors
#define SENSOR_ANALOG_EXTENSION_BOARD   10  // New OpenSprinkler analog extension board x8 - voltage mode 0..4V
#define SENSOR_ANALOG_EXTENSION_BOARD_P 11  // New OpenSprinkler analog extension board x8 - percent 0..3.3V to 0..100%
#define SENSOR_SMT50_MOIS               15  // New OpenSprinkler analog extension board x8 - SMT50 VWC [%] = (U * 50) : 3
#define SENSOR_SMT50_TEMP               16  // New OpenSprinkler analog extension board x8 - SMT50 T [°C] = (U – 0,5) * 100
#define SENSOR_SMT100_ANALOG_MOIS       17  // New OpenSprinkler analog extension board x8 - SMT100 VWC [%] = (U * 100) : 3
#define SENSOR_SMT100_ANALOG_TEMP       18  // New OpenSprinkler analog extension board x8 - SMT50 T [°C] = (U * 100) : 3 - 40

#define SENSOR_VH400                    30  // New OpenSprinkler analog extension board x8 - Vegetronix VH400
#define SENSOR_THERM200                 31  // New OpenSprinkler analog extension board x8 - Vegetronix THERM200
#define SENSOR_AQUAPLUMB                32  // New OpenSprinkler analog extension board x8 - Vegetronix Aquaplumb

#define SENSOR_USERDEF                  49  // New OpenSprinkler analog extension board x8 - User defined sensor

#define OSPI_SENSORS_START              50  // starting id for Old OSPi sensors
#define OSPI_SENSORS_END                59  // ending id for Old OSPi sensors
#define SENSOR_OSPI_ANALOG              50  // Old OSPi analog input - voltage mode 0..3.3V
#define SENSOR_OSPI_ANALOG_P            51  // Old OSPi analog input - percent 0..3.3V to 0...100%
#define SENSOR_OSPI_ANALOG_SMT50_MOIS   52  // Old OSPi analog input - SMT50 VWC [%] = (U * 50) : 3
#define SENSOR_OSPI_ANALOG_SMT50_TEMP   53  // Old OSPi analog input - SMT50 T [°C] = (U – 0,5) * 100
#define SENSOR_OSPI_INTERNAL_TEMP       54  // Internal OSPI Temperature

#define INDEPENDENT_SENSORS_START       60  // starting id for independent sensors
#define SENSOR_FYTA_MOISTURE            60  // FYTA moisture sensor
#define SENSOR_FYTA_TEMPERATURE         61  // FYTA temperature sensor
 
#define SENSOR_MQTT                     90  // subscribe to a MQTT server and query a value

#define SENSOR_ZIGBEE                   95  // Zigbee sensor (generic, via Zigbee2MQTT)
#define SENSOR_BLE                      96  // BLE (Bluetooth Low Energy) sensor

#define SENSOR_REMOTE                   100 // Remote sensor of an remote opensprinkler
#define SENSOR_WEATHER_TEMP_F           101 // Weather service - temperature (Fahrenheit)
#define SENSOR_WEATHER_TEMP_C           102 // Weather service - temperature (Celcius)
#define SENSOR_WEATHER_HUM              103 // Weather service - humidity (%)
#define SENSOR_WEATHER_PRECIP_IN        105 // Weather service - precip (inch)
#define SENSOR_WEATHER_PRECIP_MM        106 // Weather service - precip (mm)
#define SENSOR_WEATHER_WIND_MPH         107 // Weather service - wind (mph)
#define SENSOR_WEATHER_WIND_KMH         108 // Weather service - wind (kmh)
#define SENSOR_WEATHER_ETO              109 // Weather service - ETO
#define SENSOR_WEATHER_RADIATION        110 // Weather service - radiation

#define SENSOR_GROUP_MIN                1000 // Sensor group with min value
#define SENSOR_GROUP_MAX                1001 // Sensor group with max value
#define SENSOR_GROUP_AVG                1002 // Sensor group with avg value
#define SENSOR_GROUP_SUM                1003 // Sensor group with sum value

//Diagnostic
#define SENSOR_FREE_MEMORY              10000 //Free memory
#define SENSOR_FREE_STORE               10001 //Free storage

#define SENSOR_READ_TIMEOUT 3000  // ms

#define MIN_DISK_FREE 8192  // 8Kb min

#define MAX_SENSOR_REPEAT_READ 32000  // max reads for calculating avg
#define MAX_SENSOR_READ_TIME 1        // second for reading sensors

// detected Analog Sensor Boards:
#define ASB_BOARD1 0x0001
#define ASB_BOARD2 0x0002
#define OSPI_PCF8591 0x0004
#define OSPI_ADS1115 0x0008
#define RS485_TRUEBNER1 0x0020
#define RS485_TRUEBNER2 0x0040
#define RS485_TRUEBNER3 0x0080
#define RS485_TRUEBNER4 0x0100
#define OSPI_USB_RS485 0x0200
#define ASB_I2C_RS485 0x0400

typedef struct SensorFlags {
  uint enable : 1;   // enabled
  uint log : 1;      // log data enabled
  uint data_ok : 1;  // last data is ok
  uint show : 1;     // show on mainpage
} SensorFlags_t;

#define RS485FLAGS_DATATYPE_UINT16 0 // 2 bytes
#define RS485FLAGS_DATATYPE_INT16  1 // 2 bytes
#define RS485FLAGS_DATATYPE_UINT32 2 // 4 bytes
#define RS485FLAGS_DATATYPE_INT32  3 // 4 bytes
#define RS485FLAGS_DATATYPE_FLOAT  4 // 4 bytes
#define RS485FLAGS_DATATYPE_DOUBLE 5 // 8 bytes

typedef struct RS485Flags { // 0 is default
  uint parity : 2;      // use even/odd parity (0=none,1=even,2=odd)
  uint stopbits : 1;    // use 2 stop bits (0=1 stop bit, 1=2 stop bits)
  uint speed : 3;       // 0=9600, 1=19200, 2=38400, 3=57600, 4=115200
  uint swapped : 1;     // swapped low/high byte (0=big endian, 1=little endian)
  uint datatype: 3;     // 0=uint16 (2 bytes), 1=int16 (2 bytes) 2=uint32 (4 bytes), 3=int32 (4 bytes), 4=float (4 bytes),5=double (8 bytes)
} RS485Flags_t;

// Sensor persistent storage type is now SensorBase (forward declaration for now, full definition in Sensor.hpp at end of file)
class SensorBase; // forward
#define SENSOR_STORE_SIZE 0 // legacy size removed

// Definition of a log data
typedef struct SensorLog {
  uint nr;  // sensor-nr
  ulong time;
  uint32_t native_data;
  double data;
} SensorLog_t;
#define SENSORLOG_STORE_SIZE (sizeof(SensorLog_t))

// Sensor to program data
// Adjustment is formula
//    min max  factor1 factor2
//    10..90 -> 5..1 factor1 > factor2
//     a   b    c  d
//    (b-sensorData) / (b-a) * (c-d) + d
//
//    10..90 -> 1..5 factor1 < factor2
//     a   b    c  d
//    (sensorData-a) / (b-a) * (d-c) + c

#define PROG_DELETE 0          // deleted
#define PROG_LINEAR 1          // formula see above
#define PROG_DIGITAL_MIN 2     // under or equal min : factor1 else factor2
#define PROG_DIGITAL_MAX 3     // over or equal max  : factor2 else factor1
#define PROG_DIGITAL_MINMAX 4  // under min or over max : factor1 else factor2
#define PROG_NONE 99           // No adjustment

/**
 * @brief Program sensor adjustment class
 * @note Defines how sensor values influence program watering adjustments
 */
class ProgSensorAdjust {
public:
  uint nr;      // adjust-nr 1..x
  uint type;    // PROG_XYZ type=0 -->delete
  uint sensor;  // sensor-nr
  uint prog;    // program-nr=pid
  double factor1;
  double factor2;
  double min;
  double max;
  char name[30];
  
  /**
   * @brief Constructor
   */
  ProgSensorAdjust() : nr(0), type(0), sensor(0), prog(0), 
                       factor1(0.0), factor2(0.0), min(0.0), max(0.0) {
    name[0] = 0;
  }
  
  /**
   * @brief Serialize to JSON
   * @param obj JSON object to write to
   */
  void toJson(ArduinoJson::JsonObject obj) const;
  
  /**
   * @brief Deserialize from JSON
   * @param obj JSON object to read from
   */
  void fromJson(ArduinoJson::JsonVariantConst obj);
};



#define MONITOR_DELETE 0
#define MONITOR_MIN 1
#define MONITOR_MAX 2
#define MONITOR_SENSOR12 3 // Read Digital OS Sensors Rain/Soil Moisture 
#define MONITOR_SET_SENSOR12 4 // Write Digital OS Sensors Rain/Soil Moisture 
#define MONITOR_AND 10
#define MONITOR_OR 11
#define MONITOR_XOR 12
#define MONITOR_NOT 13
#define MONITOR_TIME 14
#define MONITOR_REMOTE 100

// MQTT sensor configuration types (used in sensor_mqtt_subscribe/unsubscribe)
#define SENSORURL_TYPE_URL 0     // URL for Host/Path
#define SENSORURL_TYPE_TOPIC 1   // TOPIC for MQTT
#define SENSORURL_TYPE_FILTER 2  // JSON Filter for MQTT

typedef struct Monitor_MINMAX { // type = 1+2
  double value1;  // MIN/MAX
  double value2; // Secondary
} Monitor_MINMAX_t;

typedef struct Monitor_SENSOR12 { // type = 3
  uint16_t sensor12;
  bool invers : 1;
} Monitor_SENSOR12_t;

typedef struct Monitor_SET_SENSOR12 { // type = 4
  uint16_t monitor;
  uint16_t sensor12;
} Monitor_SET_SENSOR12_t;

typedef struct Monitor_ANDORXOR { // type = 10+11+12
  uint16_t monitor1;
  uint16_t monitor2;
  uint16_t monitor3;
  uint16_t monitor4;
  bool invers1 : 1;
  bool invers2 : 1;
  bool invers3 : 1;
  bool invers4 : 1;
} Monitor_ANDORXOR_t;

typedef struct Monitor_NOT { // type = 13
  uint16_t monitor;
} Monitor_NOT_t;

typedef struct Monitor_TIME { // type = 14
  uint16_t time_from; //Format: HHMM
  uint16_t time_to;   //Format: HHMM
  uint8_t weekdays;  //bit 0=monday
} Monitor_TIME_t;

typedef struct Monitor_REMOTE { // type = 100
  uint16_t rmonitor;
  uint32_t ip;
  uint16_t port;
} Monitor_REMOTE_t;

typedef union Monitor_Union {
    Monitor_MINMAX_t minmax;     // type = 1+2
    Monitor_SENSOR12_t sensor12; // type = 3
    Monitor_SET_SENSOR12_t set_sensor12; // type = 4
    Monitor_ANDORXOR_t andorxor; // type = 10+11+12
    Monitor_NOT_t mnot; // type = 13
    Monitor_TIME_t mtime; // type = 14
    Monitor_REMOTE_t remote; //type = 100
} Monitor_Union_t;

/**
 * @brief Monitor class for value monitoring
 * @note Defines conditions that trigger actions based on sensor values, time, or logic
 */
class Monitor {
public:
  uint nr;
  uint type;     // MONITOR_TYPES
  uint sensor;   // sensor-nr
  uint prog;     // program-nr=pid
  uint zone;     // Zone
  Monitor_Union_t m;
  boolean active;
  ulong time;
  char name[30];
  ulong maxRuntime;
  uint8_t prio;
  ulong reset_seconds;
  unsigned char undef[16];  // for later
  ulong reset_time; // time to reset
  
  /**
   * @brief Constructor
   */
  Monitor() : nr(0), type(0), sensor(0), prog(0), zone(0), 
              active(false), time(0), maxRuntime(0), prio(0), 
              reset_seconds(0), reset_time(0) {
    memset(&m, 0, sizeof(Monitor_Union_t));
    memset(undef, 0, sizeof(undef));
    name[0] = 0;
  }
  
  /**
   * @brief Serialize to JSON
   * @param obj JSON object to write to
   */
  void toJson(ArduinoJson::JsonObject obj) const;
  
  /**
   * @brief Deserialize from JSON
   * @param obj JSON object to read from
   */
  void fromJson(ArduinoJson::JsonVariantConst obj);
};

typedef Monitor Monitor_t;
#define MONITOR_STORE_SIZE (sizeof(Monitor_t) - 2*sizeof(ulong))

#define UNIT_NONE 0
#define UNIT_PERCENT 1
#define UNIT_DEGREE 2
#define UNIT_FAHRENHEIT 3
#define UNIT_VOLT 4
#define UNIT_HUM_PERCENT 5
#define UNIT_INCH 6
#define UNIT_MM 7
#define UNIT_MPH 8
#define UNIT_KMH 9
#define UNIT_LEVEL 10
#define UNIT_DK 11 //Permitivität
#define UNIT_LM 12 //Lumen
#define UNIT_LX 13 //Lux
#define UNIT_USERDEF 99

// Unitnames
//  extern const char* sensor_unitNames[];

#define ASB_BOARD_ADDR1a 0x50 //0x48 shifted for rs485
#define ASB_BOARD_ADDR1b 0x49
#define ASB_BOARD_ADDR2a 0x4A
#define ASB_BOARD_ADDR2b 0x4B

void sensor_api_init(boolean detect_boards);
uint16_t get_asb_detected_boards();
boolean sensor_type_supported(int type);
void add_asb_detected_boards(uint16_t board);
void sensor_save_all();
void sensor_api_free();

// Deprecated: prefer sensor_map access
SensorBase *getSensors();
const char *getSensorUnit(int unitid);
const char *getSensorUnit(SensorBase *sensor);
unsigned char getSensorUnitId(int type);
unsigned char getSensorUnitId(SensorBase *sensor);

extern char ether_buffer[];
extern char tmp_buffer[];
extern OpenSprinkler os;
extern ProgramData pd;
extern NotifQueue notif;
extern const char *user_agent_string;

// Utils:
uint16_t CRC16(unsigned char buf[], int len);

// Sensor API functions:
int sensor_delete(uint nr);
int sensor_define(ArduinoJson::JsonVariantConst json, bool save = false);
int sensor_define_userdef(uint nr, int16_t factor, int16_t divider,
                          const char *userdef_unit, int16_t offset_mv,
                          int16_t offset2, int16_t sensor_define_userdef);
void sensor_load();
void sensor_save();
uint sensor_count();
boolean sensor_isgroup(const SensorBase *sensor);
void sensor_update_groups();

void read_all_sensors(boolean online);

SensorBase *sensor_by_nr(uint nr);
SensorBase *sensor_by_idx(uint idx);

// Helper to iterate sensors - call with iterator=NULL to get first, then pass iterator for next
typedef std::map<uint, SensorBase*>::iterator SensorIterator;
SensorIterator sensors_iterate_begin();
SensorBase* sensors_iterate_next(SensorIterator& it);

// Helper to iterate program adjustments
typedef std::map<uint, ProgSensorAdjust*>::iterator ProgAdjustIterator;
ProgAdjustIterator prog_adjust_iterate_begin();
ProgSensorAdjust* prog_adjust_iterate_next(ProgAdjustIterator& it);

// Helper to iterate monitors
typedef std::map<uint, Monitor*>::iterator MonitorIterator;
MonitorIterator monitor_iterate_begin();
Monitor* monitor_iterate_next(MonitorIterator& it);

int read_sensor(SensorBase *sensor,
                ulong time);  // sensor value goes to last_native_data/last_data

// Sensorlog API functions:
#define LOG_STD   0
#define LOG_WEEK  1
#define LOG_MONTH 2
bool sensorlog_add(uint8_t log, SensorLog_t *sensorlog);
bool sensorlog_add(uint8_t log, SensorBase *sensor, ulong time);
void sensorlog_clear_all();
void sensorlog_clear(bool std, bool week, bool month);
ulong sensorlog_clear_sensor(uint sensorNr, uint8_t log, bool use_under,
                             double under, bool use_over, double over, time_t before, time_t after);
SensorLog_t *sensorlog_load(uint8_t log, ulong pos);
SensorLog_t *sensorlog_load(uint8_t log, ulong idx, SensorLog_t *sensorlog);
int sensorlog_load2(uint8_t log, ulong idx, int count, SensorLog_t *sensorlog);
ulong sensorlog_filesize(uint8_t log);
ulong sensorlog_size(uint8_t log);
ulong findLogPosition(uint8_t log, ulong after);
const char *getlogfile(uint8_t log);
const char *getlogfile2(uint8_t log);
void checkLogSwitch(uint8_t log);
void checkLogSwitchAfterWrite(uint8_t log);

//influxdb
void add_influx_data(SensorBase *sensor);

// MQTT sensor subscription management
void sensor_mqtt_subscribe(uint nr, uint type, const char *urlstr);
void sensor_mqtt_unsubscribe(uint nr, uint type, const char *urlstr);

// Set Sensor Address for SMT100:
int set_sensor_address(SensorBase *sensor, uint8_t new_address);

// Calc watering adjustment:
int prog_adjust_define(ArduinoJson::JsonVariantConst json, bool save = true);
int prog_adjust_define(uint nr, uint type, uint sensor, uint prog,
                       double factor1, double factor2, double min, double max, char * name);
int prog_adjust_delete(uint nr);
void prog_adjust_save();
void prog_adjust_load();
uint prog_adjust_count();
ProgSensorAdjust *prog_adjust_by_nr(uint nr);
ProgSensorAdjust *prog_adjust_by_idx(uint idx);
double calc_sensor_watering(uint prog);
double calc_sensor_watering_by_nr(uint nr);
double calc_sensor_watering_int(ProgSensorAdjust *p, double sensorData);

void GetSensorWeather();
void GetSensorWeatherEto();
// PUSH Message to MQTT and others:
void push_message(SensorBase *sensor);

void detect_asb_board();

//Value Monitoring
void monitor_load();
void monitor_save();
int monitor_count();
int monitor_delete(uint nr);
bool monitor_define(uint nr, uint type, uint sensor, uint prog, uint zone, const Monitor_Union_t m, char * name, ulong maxRuntime, uint8_t prio, ulong reset_seconds = 0);
Monitor_t * monitor_by_nr(uint nr);
Monitor_t * monitor_by_idx(uint idx);
void check_monitors();

#if defined(OSPI)
boolean send_rs485_command(uint8_t device, uint8_t address, uint16_t reg,uint16_t data, bool isbit);
#endif
#if defined(ESP8266) || defined(ESP32)
boolean send_rs485_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit);
#endif

#if defined(OSPI)
boolean send_rs485_command(uint8_t device, uint8_t address, uint16_t reg,uint16_t data, bool isbit);
#endif
#if defined(ESP8266) || defined(ESP32)
boolean send_rs485_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit);
#endif

#if defined(ESP8266) || defined(ESP32) 
ulong diskFree();
bool checkDiskFree();  // true: disk space Ok, false: Out of disk space
#endif

void replace_pid(uint old_pid, uint new_pid);
int findValue(const char *payload, unsigned int length, const char *jsonFilter, double& value);
int findString(const char *payload, unsigned int length, const char *jsonFilter, String& value);

#endif  // _SENSORS_H
