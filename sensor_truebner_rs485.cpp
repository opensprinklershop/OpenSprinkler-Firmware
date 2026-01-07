/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * sensors header file
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
#include "sensor_truebner_rs485.h"
#include "sensors.h"
#include "OpenSprinkler.h"

extern OpenSprinkler os;

#define MAX_RS485_DEVICES 4
#define RS485_TRUEBNER1_ADDR 0x38
#define RS485_TRUEBNER2_ADDR 0x39
#define RS485_TRUEBNER3_ADDR 0x3A
#define RS485_TRUEBNER4_ADDR 0x3B

static uint i2c_rs485_allocated[MAX_RS485_DEVICES];

void sensor_truebner_rs485_init() {
    if (detect_i2c(RS485_TRUEBNER1_ADDR)) {    
        DEBUG_PRINTF(F("Found Truebner RS485 at address %02x\n"), RS485_TRUEBNER1_ADDR);
        add_asb_detected_boards(RS485_TRUEBNER1);
    }
    if (detect_i2c(RS485_TRUEBNER2_ADDR)) {    
        DEBUG_PRINTF(F("Found Truebner RS485 at address %02x\n"), RS485_TRUEBNER2_ADDR);
        add_asb_detected_boards(RS485_TRUEBNER2);
    }
    if (detect_i2c(RS485_TRUEBNER3_ADDR)) {    
        DEBUG_PRINTF(F("Found Truebner RS485 at address %02x\n"), RS485_TRUEBNER3_ADDR);
        add_asb_detected_boards(RS485_TRUEBNER3);
    }
    if (detect_i2c(RS485_TRUEBNER4_ADDR)) {    
        DEBUG_PRINTF(F("Found Truebner RS485 at address %02x\n"), RS485_TRUEBNER4_ADDR);
        add_asb_detected_boards(RS485_TRUEBNER4);
    }
}

void sensor_truebner_rs485_free() {
  memset(i2c_rs485_allocated, 0, sizeof(i2c_rs485_allocated));
}

// C++ class methods
int TruebnerRS485Sensor::read(unsigned long /*time*/) {
  int device = port;
  if (device >= MAX_RS485_DEVICES || (get_asb_detected_boards() & (RS485_TRUEBNER1 << device)) == 0)
    return HTTP_RQT_NOT_RECEIVED;

  if (i2c_rs485_allocated[device] > 0 && i2c_rs485_allocated[device] != nr) {
    repeat_read = 1000;
    DEBUG_PRINT(F("cant' read, allocated by sensor "));
    DEBUG_PRINTLN(i2c_rs485_allocated[device]);
    SensorBase *t = sensor_by_nr(i2c_rs485_allocated[device]);
    if (!t || !t->flags.enable)
      i2c_rs485_allocated[device] = 0; //breakout
    return HTTP_RQT_NOT_RECEIVED;
  }

  DEBUG_PRINTLN(F("read_sensor_rs485: check-ok"));

  bool isTemp = type == SENSOR_SMT100_TEMP || type == SENSOR_TH100_TEMP;
  bool isMois = type == SENSOR_SMT100_MOIS || type == SENSOR_TH100_MOIS;
  uint8_t senstype = isTemp ? 0x00 : isMois ? 0x01 : 0x02;

  if (repeat_read == 0 || repeat_read == 1000) {
    Wire.beginTransmission(RS485_TRUEBNER1_ADDR + device);
    Wire.write((uint8_t)id);
    Wire.write(senstype);
    if (Wire.endTransmission() == 0) {
      DEBUG_PRINTF(F("read_sensor_rs485: request send: %d - %d\n"), id,
                   senstype);
      repeat_read = 1;
      i2c_rs485_allocated[device] = nr;
    }
    return HTTP_RQT_NOT_RECEIVED;
    // delay(500);
  }

  if (Wire.requestFrom((uint8_t)(RS485_TRUEBNER1_ADDR + device), (size_t)4, true)) {
    // read the incoming bytes:
    uint8_t addr = Wire.read();
    uint8_t reg = Wire.read();
    uint8_t low_byte = Wire.read();
    uint8_t high_byte = Wire.read();
    if (addr == id && reg == senstype) {
      uint16_t data = (high_byte << 8) | low_byte;
      DEBUG_PRINTF(F("read_sensor_rs485: result: %d - %d (%d %d)\n"), id,
                   data, low_byte, high_byte);
      double value = isTemp ? (data / 100.0) - 100.0 : (isMois ? data / 100.0 : data);
      last_native_data = data;
      last_data = value;
      DEBUG_PRINTLN(last_data);

      flags.data_ok = true;

      repeat_read = 0;
      i2c_rs485_allocated[device] = 0;
      return HTTP_RQT_SUCCESS;
    }
  }

  repeat_read++;
  if (repeat_read > 4) {  // timeout
    repeat_read = 0;
    flags.data_ok = false;
    i2c_rs485_allocated[device] = 0;
    DEBUG_PRINTLN(F("read_sensor_rs485: timeout"));
  }
  DEBUG_PRINTLN(F("read_sensor_rs485: exit"));
  return HTTP_RQT_NOT_RECEIVED;
}



#if defined(ESP8266) || defined(ESP32)
/**
 * @brief Set the sensor address i2c
 *
 * @param sensor
 * @param new_address
 * @return int
 */
int TruebnerRS485Sensor::setAddress(uint8_t new_address) {
  DEBUG_PRINTLN(F("set_sensor_address_rs485"));
  int device = port;
  if (device >= MAX_RS485_DEVICES || (get_asb_detected_boards() & (RS485_TRUEBNER1 << device)) == 0)
    return HTTP_RQT_NOT_RECEIVED;

  if (i2c_rs485_allocated[device] > 0) {
    DEBUG_PRINT(F("sensor currently allocated by "));
    DEBUG_PRINTLN(i2c_rs485_allocated[device]);
    SensorBase *t = sensor_by_nr(i2c_rs485_allocated[device]);
    if (!t || !t->flags.enable)
      i2c_rs485_allocated[device] = 0; //breakout
    return HTTP_RQT_NOT_RECEIVED;
  }

  Wire.beginTransmission(RS485_TRUEBNER1_ADDR + device);
  Wire.write(254);
  Wire.write(new_address);
  Wire.endTransmission();
  delay(3000);
  Wire.requestFrom((uint8_t)(RS485_TRUEBNER1_ADDR + device), (size_t)1, true);
  if (Wire.available()) {
    delay(10);
    uint8_t modbus_address = Wire.read();
    if (modbus_address == new_address) return HTTP_RQT_SUCCESS;
  }
  return HTTP_RQT_NOT_RECEIVED;
}

#endif
