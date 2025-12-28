/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * sensors header file
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

#ifndef _SENSOR_RS485_I2C_H
#define _SENSOR_RS485_I2C_H

#include "sensors.h"
#include "Sensor.hpp"

#if defined(ESP8266) || defined(ESP32)

#define ASB_I2C_RS485_ADDR1 0x50 //0xA2>>1 A0=VCC A1=SCL
#define ASB_I2C_RS485_ADDR2 0x51 //0xA2>>1 A0=GND A1=SCL
#define ASB_I2C_RS485_ADDR3 0x52 //0xA2>>1 A0=SCL A1=SCL

/**
 * @brief Initialize I2C-RS485 bridge subsystem
 * @note Sets up I2C communication with RS485 bridge device
 */
void sensor_rs485_i2c_init();

/**
 * @brief Send Modbus command via I2C-RS485 bridge
 * @param address Modbus device address (1-247)
 * @param reg Register address to read/write
 * @param data Data value or function code
 * @param isbit True for bit operations, false for register operations
 * @return Command result code
 * @note Low-level function, use RS485I2CSensor::sendCommand instead
 */
int send_i2c_rs485_command(uint8_t address, uint16_t reg, uint16_t data, bool isbit);

// C++ wrapper
/**
 * @brief RS485 sensor over I2C bridge (for ESP platforms)
 * @note Uses I2C-to-RS485 converter at address ASB_I2C_RS485_ADDR (0x48)
 */
class RS485I2CSensor : public SensorBase {
public:
  RS485Flags_t rs485_flags = {};  // RS485 specific flags
  uint8_t rs485_code = 0;         // RS485 function code
  uint16_t rs485_reg = 0;         // RS485 register address

  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit RS485I2CSensor(uint type) : SensorBase(type) {}
  virtual ~RS485I2CSensor() {}
  
  /**
   * @brief Read sensor value via I2C-RS485 bridge
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Handles asynchronous reading via repeat_read mechanism
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Set RS485 device address via I2C command
   * @param newAddress New Modbus address to assign to the device
   * @return HTTP_RQT_SUCCESS on success, error code on failure
   * @note Sends special command to change device address permanently
   */
  virtual int setAddress(uint8_t newAddress) override;

  /**
   * @brief Serialize sensor configuration to JSON
   * @param obj JSON object to populate with RS485 flags, code, and register
   */
  virtual void toJson(ArduinoJson::JsonObject obj) const override;
  
  /**
   * @brief Deserialize sensor configuration from JSON
   * @param obj JSON object containing RS485 configuration
   */
  virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on sensor type
   */
  virtual unsigned char getUnitId() const override;
  
  /**
   * @brief Emit sensor data as JSON to BufferFiller
   * @param bfill BufferFiller object for output
   */
  virtual void emitJson(BufferFiller& bfill) const override;

  // Class-level helpers moved from global functions
  /**
   * @brief Send Modbus command via I2C-RS485 bridge
   * @param address Modbus device address (1-247)
   * @param reg Register address to read/write
   * @param data Data value or function code
   * @param isbit True for bit operations, false for register operations
   * @return true on success, false on communication failure
   */
  static int sendCommand(uint8_t address, uint16_t reg, uint16_t data, bool isbit);
};

#endif

#endif
