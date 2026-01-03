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

#ifndef _SENSOR_MODBUS_RTU_H
#define _SENSOR_MODBUS_RTU_H

#include "sensors.h"
#include "SensorBase.hpp"

/**
 * @brief Free Modbus RTU resources and cleanup
 * @note Closes all active Modbus TCP connections
 */
void sensor_modbus_rtu_free();

/**
 * @brief Send Modbus RTU command via TCP/IP
 * @param ip IP address of Modbus TCP gateway
 * @param port TCP port number
 * @param address Modbus device address
 * @param reg Register address
 * @param data Data value or function code
 * @param isbit True for bit operations, false for register operations
 * @return true on success, false on failure
 * @note Low-level function, use ModbusRtuSensor::sendCommand instead
 */
bool send_modbus_rtu_command(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg,uint16_t data, bool isbit);

/**
 * @brief Set sensor address via IP command
 * @param sensor Sensor to configure
 * @param new_address New Modbus address
 * @return HTTP_RQT_SUCCESS on success, error code on failure
 * @note Low-level function, use ModbusRtuSensor::setAddressIp instead
 */
int set_sensor_address_ip(SensorBase *sensor, uint8_t new_address);

// C++ helper class for Modbus RTU/IP sensors
/**
 * @brief Modbus RTU over TCP/IP sensor implementation
 * @note Supports Modbus TCP protocol for reading holding registers and coils
 */
class ModbusRtuSensor : public SensorBase {
public:
  RS485Flags_t rs485_flags = {};  // RS485 specific flags
  uint8_t rs485_code = 0;         // RS485 function code
  uint16_t rs485_reg = 0;         // RS485 register address

  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit ModbusRtuSensor(uint type) : SensorBase(type) {};
  virtual ~ModbusRtuSensor() {}

  /**
   * @brief Read sensor value via Modbus TCP
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, error code on failure
   * @note Uses IP address, port, device address and register from sensor configuration
   */
  virtual int read(unsigned long time) override;

  /**
   * @brief Set Modbus device address via special command
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
   * @brief Serialize sensor configuration to JSON
   * @param obj JSON object to populate with Modbus configuration
   */
  virtual void toJson(ArduinoJson::JsonObject obj) const override;
  
  /**
   * @brief Deserialize sensor configuration from JSON
   * @param obj JSON object containing Modbus configuration (flags, code, register)
   */
  virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;

  // Class-level helpers moved from global functions
  /**
   * @brief Send Modbus TCP command to device
   * @param ip IP address of Modbus TCP gateway
   * @param port TCP port number
   * @param address Modbus device address
   * @param reg Register address
   * @param data Data value or function code
   * @param isbit True for bit operations, false for register operations
   * @return true on success, false on communication failure
   */
  static bool sendCommand(uint32_t ip, uint16_t port, uint8_t address, uint16_t reg, uint16_t data, bool isbit);
  
  /**
   * @brief Set Modbus device address via IP command
   * @param sensor Sensor to configure
   * @param newAddress New Modbus address to assign
   * @return HTTP_RQT_SUCCESS on success, error code on failure
   */
  static int setAddressIp(SensorBase* sensor, uint8_t newAddress);
};

#endif
