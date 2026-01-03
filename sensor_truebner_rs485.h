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

#ifndef _SENSOR_TRUEBNER_RS485_H
#define _SENSOR_TRUEBNER_RS485_H

#include "sensors.h"
#include "SensorBase.hpp"

/**
 * @brief Initialize Truebner RS485 sensor subsystem
 * @note Sets up serial communication for Truebner SMT100 devices
 */
void sensor_truebner_rs485_init();

/**
 * @brief Free Truebner RS485 resources and cleanup
 * @note Closes serial ports and releases allocated resources
 */
void sensor_truebner_rs485_free();

// C++ class wrapper (incremental migration)
/**
 * @brief Truebner SMT100 soil sensor via RS485
 * @note Supports moisture and temperature readings from Truebner SMT100 sensors
 */
class TruebnerRS485Sensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit TruebnerRS485Sensor(uint type) : SensorBase(type) {}
  virtual ~TruebnerRS485Sensor() {}

  /**
   * @brief Read sensor value from Truebner SMT100 device
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Uses RS485 serial communication with device-specific protocol
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Set RS485 device address
   * @param newAddress New address to assign (0-247)
   * @return HTTP_RQT_SUCCESS on success, error code on failure
   */
  virtual int setAddress(uint8_t newAddress) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on sensor type (PERCENT for moisture, DEGREE for temperature)
   */
  virtual unsigned char getUnitId() const override;
};

#endif
