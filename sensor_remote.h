/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Remote HTTP sensor header
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

#ifndef _SENSOR_REMOTE_H
#define _SENSOR_REMOTE_H

#include "sensors.h"
#include "Sensor.hpp"

// C++ class wrapper for Remote HTTP sensors
/**
 * @brief Remote HTTP sensor for fetching values from web APIs
 * @note Supports JSON filtering to extract specific values from HTTP responses
 */
class RemoteSensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit RemoteSensor(uint type) : SensorBase() { this->type = type; }
  virtual ~RemoteSensor() {}

  /**
   * @brief Read sensor value from remote HTTP endpoint
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Fetches data via HTTP GET and applies JSON filter if specified
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID from assigned_unitid field
   */
  virtual unsigned char getUnitId() const override;
  
  /**
   * @brief Extract JSON value using filter path
   * @param s Source JSON string
   * @param buf Output buffer for extracted value
   * @param maxlen Maximum buffer length
   * @return true if extraction successful, false otherwise
   * @note Filter format: "key1.key2.key3" for nested objects
   */
  static bool extract(char *s, char *buf, int maxlen);
};

#endif // _SENSOR_REMOTE_H
