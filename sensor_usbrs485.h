/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * USB RS485 sensor header
 * 2024 @ OpenSprinklerShop
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

#ifndef _SENSOR_USBRS485_H
#define _SENSOR_USBRS485_H

#if defined(OSPI)

#include "sensors.h"
#include "Sensor.hpp"

#include <modbus/modbus.h>
#include <modbus/modbus-rtu.h>

// Maximum number of RS485 devices
#ifndef MAX_RS485_DEVICES
#define MAX_RS485_DEVICES 16
#endif

// Global modbus devices array
extern modbus_t *modbusDevs[MAX_RS485_DEVICES];

// C++ class wrapper for USB RS485 sensors
/**
 * @brief USB-to-RS485 sensor implementation for OSPI platform
 * @note Uses libmodbus for Modbus RTU communication over USB serial adapters
 */
class UsbRs485Sensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit UsbRs485Sensor(uint type) : SensorBase() { this->type = type; }
  virtual ~UsbRs485Sensor() {}

  /**
   * @brief Read sensor value via USB RS485 adapter
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Uses Modbus RTU protocol over USB serial connection
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Set RS485 device address
   * @param newAddress New Modbus address to assign (1-247)
   * @return HTTP_RQT_SUCCESS on success, error code on failure
   */
  virtual int setAddress(uint8_t newAddress) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on sensor type
   */
  virtual unsigned char getUnitId() const override;

  /**
   * @brief Send Modbus RTU command via USB adapter
   * @param device USB device index (0 to MAX_RS485_DEVICES-1)
   * @param address Modbus device address
   * @param reg Register address
   * @param data Data value or function code
   * @param isbit True for bit operations, false for register operations
   * @return true on success, false on communication failure
   */
  static bool sendCommand(uint8_t device, uint8_t address, uint16_t reg, uint16_t data, bool isbit);
};

#endif // defined(OSPI)

#endif // _SENSOR_USBRS485_H
