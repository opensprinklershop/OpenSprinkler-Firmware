/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Analog Sensor Board (ASB) sensor header
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

#ifndef _SENSOR_ASB_H
#define _SENSOR_ASB_H

#if defined(ESP8266) || defined(ESP32)

#include "sensors.h"
#include "SensorBase.hpp"

// C++ class wrapper for Analog Sensor Board (ASB) sensors
/**
 * @brief Analog Sensor Board sensor integration
 * @note Supports various analog sensors connected via ASB (SMT50, SMT100, VH400, etc.)
 */
class AsbSensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier (determines conversion formula)
   */
  explicit AsbSensor(uint type) : SensorBase() { this->type = type; }
  virtual ~AsbSensor() {}

  /**
   * @brief Read analog sensor value from ASB
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Applies sensor-specific conversion formulas based on type
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on sensor type (VOLT, PERCENT, DEGREE, etc.)
   */
  virtual unsigned char getUnitId() const override;
};

#endif // defined(ESP8266) || defined(ESP32)

#endif // _SENSOR_ASB_H
