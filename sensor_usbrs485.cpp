/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * USB RS485 sensor implementation
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

#if defined(OSPI)

#include "sensor_usbrs485.h"
#include "OpenSprinkler.h"
#include "sensors.h"
#include <modbus/modbus.h>
#include <modbus/modbus-rtu.h>

// Global modbus devices array definition
#ifndef MAX_RS485_DEVICES
#define MAX_RS485_DEVICES 16
#endif
modbus_t *modbusDevs[MAX_RS485_DEVICES] = {nullptr};

/**
 * @brief Raspberry PI / Linux RS485 Interface via USB adapter
 *
 * @param time Current time
 * @return int HTTP_RQT_* status code
 */
int UsbRs485Sensor::read(unsigned long time) {
  DEBUG_PRINTLN(F("UsbRs485Sensor::read"));
  
  int device = this->port;
  if (device >= MAX_RS485_DEVICES || !modbusDevs[device])
    return HTTP_RQT_NOT_RECEIVED;

  DEBUG_PRINTLN(F("UsbRs485Sensor::read check-ok"));

  uint8_t buffer[10];
  bool isTemp = this->type == SENSOR_SMT100_TEMP || this->type == SENSOR_TH100_TEMP;
  bool isMois = this->type == SENSOR_SMT100_MOIS || this->type == SENSOR_TH100_MOIS;
  uint8_t type = isTemp ? 0x00 : isMois ? 0x01 : 0x02;

  uint16_t tab_reg[3] = {0};
  modbus_set_slave(modbusDevs[device], this->id);
  if (modbus_read_registers(modbusDevs[device], type, 2, tab_reg) > 0) {
    uint16_t data = tab_reg[0];
    DEBUG_PRINTF(F("UsbRs485Sensor::read result: %d - %d\n"), this->id, data);
    double value = isTemp ? (data / 100.0) - 100.0 : (isMois ? data / 100.0 : data);
    this->last_native_data = data;
    this->last_data = value;
    DEBUG_PRINTLN(this->last_data);
    this->flags.data_ok = true;
    return HTTP_RQT_SUCCESS;
  }
  DEBUG_PRINTLN(F("UsbRs485Sensor::read exit"));
  return HTTP_RQT_NOT_RECEIVED;
}

/**
 * @brief Set SMT100/TH100 sensor address
 *
 * @param newAddress New modbus address to set
 * @return int HTTP_RQT_* status code
 */
int UsbRs485Sensor::setAddress(uint8_t newAddress) {
  DEBUG_PRINTLN(F("UsbRs485Sensor::setAddress"));
  
  int device = this->port;
  if (device >= MAX_RS485_DEVICES || !modbusDevs[device])
    return HTTP_RQT_NOT_RECEIVED;

  uint8_t request[10];
  request[0] = 0xFD; // 253=Truebner Broadcast
  request[1] = 0x06; // Write
  request[2] = 0x00;
  request[3] = 0x04; // Register address
  request[4] = 0x00;
  request[5] = newAddress;
  
  if (modbus_send_raw_request(modbusDevs[device], request, 6) > 0) {
    modbus_flush(modbusDevs[device]);
    return HTTP_RQT_SUCCESS;
  }
  return HTTP_RQT_NOT_RECEIVED;
}

/**
 * @brief Send RS485 command to device
 *
 * @param device Device index
 * @param address Modbus address
 * @param reg Register address
 * @param data Data to write
 * @param isbit True for bit write, false for register write
 * @return bool Success status
 */
bool UsbRs485Sensor::sendCommand(uint8_t device, uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  if (device >= MAX_RS485_DEVICES || !modbusDevs[device])
    return false;

  modbus_set_slave(modbusDevs[device], address);
  if (isbit)
    return modbus_write_bit(modbusDevs[device], reg, data) > 0;
  return modbus_write_register(modbusDevs[device], reg, data) > 0;
}

// Free function wrapper for backward compatibility (used by OpenSprinkler::switch_modbusStation)
boolean send_rs485_command(uint8_t device, uint8_t address, uint16_t reg, uint16_t data, bool isbit) {
  return UsbRs485Sensor::sendCommand(device, address, reg, data, isbit);
}

#endif // defined(OSPI)
