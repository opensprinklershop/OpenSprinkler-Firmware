/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Sensor unit implementations - type-specific getUnitId() overrides
 * 2026 @ OpenSprinklerShop
 * Stefan Schmaltz (info@opensprinklershop.de)
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

/**
 * @brief Get measurement unit for FYTA sensor
 * @return assigned_unitid configured for this FYTA sensor type
 */
#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
#include "sensor_fyta.h"
unsigned char FytaSensor::getUnitId() const {
  return assigned_unitid;
}
#endif

/**
 * @brief Get measurement unit for Analog Sensor Board sensor
 * @return Unit ID based on sensor type (VOLT, PERCENT, DEGREE, etc.)
 * @note Maps various analog sensor types to appropriate units
 */
#if defined(ESP8266) || defined(ESP32)
#include "sensor_asb.h"
unsigned char AsbSensor::getUnitId() const {
  switch (type) {
    case SENSOR_ANALOG_EXTENSION_BOARD: return UNIT_VOLT;
    case SENSOR_ANALOG_EXTENSION_BOARD_P: return UNIT_LEVEL;
    case SENSOR_SMT50_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT50_TEMP: return UNIT_DEGREE;
    case SENSOR_SMT100_ANALOG_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_ANALOG_TEMP: return UNIT_DEGREE;
    case SENSOR_VH400: return UNIT_PERCENT;
    case SENSOR_THERM200: return UNIT_DEGREE;
    case SENSOR_AQUAPLUMB: return UNIT_PERCENT;
    case SENSOR_USERDEF:
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE: return UNIT_USERDEF;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for Modbus RTU sensor
 * @return Unit ID for SMT100/TH100 sensor types (PERCENT, DEGREE, etc.)
 * @note Supports moisture, temperature and permittivity readings
 */
#include "sensor_modbus_rtu.h"
unsigned char ModbusRtuSensor::getUnitId() const {
  switch (type) {
    case SENSOR_SMT100_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_TEMP: return UNIT_DEGREE;
    case SENSOR_SMT100_PMTY: return UNIT_DK;
    case SENSOR_TH100_MOIS: return UNIT_HUM_PERCENT;
    case SENSOR_TH100_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}

/**
 * @brief Get measurement unit for Truebner RS485 sensor
 * @return Unit ID for SMT100/TH100 sensor types (PERCENT, DEGREE, etc.)
 * @note Same unit mapping as Modbus RTU sensors
 */
#if defined(ESP8266) || defined(ESP32)
#include "sensor_truebner_rs485.h"
unsigned char TruebnerRS485Sensor::getUnitId() const {
  switch (type) {
    case SENSOR_SMT100_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_TEMP: return UNIT_DEGREE;
    case SENSOR_SMT100_PMTY: return UNIT_DK;
    case SENSOR_TH100_MOIS: return UNIT_HUM_PERCENT;
    case SENSOR_TH100_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for I2C-RS485 bridge sensor
 * @return Unit ID for SMT100/TH100 sensor types (PERCENT, DEGREE, etc.)
 * @note Same unit mapping as Modbus RTU sensors
 */
#if defined(ESP8266) || defined(ESP32)
#include "sensor_rs485_i2c.h"
unsigned char RS485I2CSensor::getUnitId() const {
  switch (type) {
    case SENSOR_SMT100_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_TEMP: return UNIT_DEGREE;
    case SENSOR_SMT100_PMTY: return UNIT_DK;
    case SENSOR_TH100_MOIS: return UNIT_HUM_PERCENT;
    case SENSOR_TH100_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for USB-RS485 sensor
 * @return Unit ID for SMT100/TH100 sensor types (PERCENT, DEGREE, etc.)
 * @note Same unit mapping as Modbus RTU sensors, OSPI platform only
 */
#if defined(OSPI)
#include "sensor_usbrs485.h"
unsigned char UsbRs485Sensor::getUnitId() const {
  switch (type) {
    case SENSOR_SMT100_MOIS: return UNIT_PERCENT;
    case SENSOR_SMT100_TEMP: return UNIT_DEGREE;
    case SENSOR_SMT100_PMTY: return UNIT_DK;
    case SENSOR_TH100_MOIS: return UNIT_HUM_PERCENT;
    case SENSOR_TH100_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for OSPI ADS1115 ADC sensor
 * @return Unit ID for analog sensor types (VOLT, PERCENT, DEGREE)
 * @note Supports various analog sensors including SMT50
 */
#ifdef ADS1115
#include "sensor_ospi_ads1115.h"
unsigned char OspiAds1115Sensor::getUnitId() const {
  switch (type) {
    case SENSOR_OSPI_ANALOG: return UNIT_VOLT;
    case SENSOR_OSPI_ANALOG_P: return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_MOIS: return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for OSPI PCF8591 ADC sensor
 * @return Unit ID for analog sensor types (VOLT, PERCENT, DEGREE)
 * @note Same unit mapping as ADS1115 sensors
 */
#ifdef PCF8591
#include "sensor_ospi_pcf8591.h"
unsigned char OspiPcf8591Sensor::getUnitId() const {
  switch (type) {
    case SENSOR_OSPI_ANALOG: return UNIT_VOLT;
    case SENSOR_OSPI_ANALOG_P: return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_MOIS: return UNIT_PERCENT;
    case SENSOR_OSPI_ANALOG_SMT50_TEMP: return UNIT_DEGREE;
    default: return UNIT_NONE;
  }
}
#endif

/**
 * @brief Get measurement unit for Remote HTTP sensor
 * @return assigned_unitid if configured, otherwise UNIT_USERDEF
 * @note Allows custom unit assignment for remote data sources
 */
#include "sensor_remote.h"
unsigned char RemoteSensor::getUnitId() const {
  return assigned_unitid > 0 ? assigned_unitid : UNIT_USERDEF;
}

/**
 * @brief Get measurement unit for MQTT sensor
 * @return assigned_unitid if configured, otherwise UNIT_USERDEF
 * @note Allows custom unit assignment for MQTT data sources
 */
#include "sensor_mqtt.h"
unsigned char MqttSensor::getUnitId() const {
  return assigned_unitid > 0 ? assigned_unitid : UNIT_USERDEF;
}

/**
 * @brief Get measurement unit for Weather sensor
 * @return Unit ID based on weather data type (FAHRENHEIT, DEGREE, PERCENT, etc.)
 * @note Supports temperature, humidity, precipitation and wind speed in various units
 */
#include "sensor_weather.h"
unsigned char WeatherSensor::getUnitId() const {
  switch (type) {
    case SENSOR_WEATHER_TEMP_F: return UNIT_FAHRENHEIT;
    case SENSOR_WEATHER_TEMP_C: return UNIT_DEGREE;
    case SENSOR_WEATHER_HUM: return UNIT_HUM_PERCENT;
    case SENSOR_WEATHER_PRECIP_IN: return UNIT_INCH;
    case SENSOR_WEATHER_PRECIP_MM: return UNIT_MM;
    case SENSOR_WEATHER_WIND_MPH: return UNIT_MPH;
    case SENSOR_WEATHER_WIND_KMH: return UNIT_KMH;
    default: return UNIT_NONE;
  }
}

/**
 * @brief Get measurement unit for Internal system sensor
 * @return Unit ID based on system metric type (USERDEF for memory/storage, DEGREE for temperature)
 * @note Platform-specific metrics (ESP has memory, OSPI has CPU temperature)
 */
#include "sensor_internal.h"
unsigned char InternalSensor::getUnitId() const {
  switch (type) {
#if defined(ESP8266) || defined(ESP32)
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE: return UNIT_USERDEF;
#endif
#if defined(OSPI)
    case SENSOR_OSPI_INTERNAL_TEMP: return UNIT_DEGREE;
#endif
    default: return UNIT_NONE;
  }
}

/**
 * @brief Get measurement unit for OSPI BLE sensor
 * @return Unit ID based on BLE sensor type (DEGREE, PERCENT, PASCAL)
 * @note Supports temperature, humidity and pressure sensors via BlueZ
 */
// Note: BLE sensor units moved to sensor_ble.cpp and sensor_ospi_ble.cpp
// Using assigned_unitid instead of type-based units
