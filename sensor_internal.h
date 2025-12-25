/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Internal system sensor header
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

#ifndef _SENSOR_INTERNAL_H
#define _SENSOR_INTERNAL_H

#include "sensors.h"
#include "Sensor.hpp"

// C++ class wrapper for Internal system sensors (memory, storage, temperature)
/**
 * @brief Internal system monitoring sensors
 * @note Provides system metrics like free memory, storage space, CPU temperature
 */
class InternalSensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier (determines which metric to monitor)
   */
  explicit InternalSensor(uint type) : SensorBase() { this->type = type; }
  virtual ~InternalSensor() {}

  /**
   * @brief Read internal system metric
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Reads various system metrics based on sensor type (memory, storage, temperature)
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on metric type (BYTES for memory, DEGREE for temperature, etc.)
   */
  virtual unsigned char getUnitId() const override;
};

#endif // _SENSOR_INTERNAL_H
