/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Weather sensor header
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

#ifndef _SENSOR_WEATHER_H
#define _SENSOR_WEATHER_H

#include "sensors.h"
#include "SensorBase.hpp"

// C++ class wrapper for Weather sensors
/**
 * @brief Weather data sensor from configured weather service
 * @note Retrieves weather data (temperature, humidity, etc.) from weather APIs
 */
class WeatherSensor : public SensorBase {
public:
  /**
   * @brief Constructor
   * @param type Sensor type identifier
   */
  explicit WeatherSensor(uint type) : SensorBase() { this->type = type; }
  virtual ~WeatherSensor() {}

  /**
   * @brief Read weather data from configured weather service
   * @param time Current timestamp
   * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
   * @note Uses OpenSprinkler weather service configuration
   */
  virtual int read(unsigned long time) override;
  
  /**
   * @brief Get measurement unit identifier
   * @return Unit ID based on weather data type (temperature, humidity, etc.)
   */
  virtual unsigned char getUnitId() const override;
};

#endif // _SENSOR_WEATHER_H
