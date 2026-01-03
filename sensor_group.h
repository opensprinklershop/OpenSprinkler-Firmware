/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Group sensor header (MIN/MAX/AVG/SUM)
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

#ifndef _SENSOR_GROUP_H
#define _SENSOR_GROUP_H

#include "sensors.h"
#include "SensorBase.hpp"

// Group sensor - aggregates values from member sensors (MIN/MAX/AVG/SUM)
/**
 * @brief Aggregates values from multiple sensors using MIN/MAX/AVG/SUM operations
 * @note Members are specified in sensor configuration, operation type determines aggregation method
 */
class GroupSensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier (determines aggregation operation)
   */
  explicit GroupSensor(uint type) : SensorBase(type) {}
  virtual ~GroupSensor() {}

  /**
   * @brief Calculate aggregated value from member sensors
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS if any member has valid data, HTTP_RQT_NOT_RECEIVED otherwise
   * @note Applies MIN/MAX/AVG/SUM operation based on sensor type
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID from first valid member sensor
   */
  virtual unsigned char getUnitId() const override;
};

#endif // _SENSOR_GROUP_H
