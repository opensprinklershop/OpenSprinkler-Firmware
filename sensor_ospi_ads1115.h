/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * sensors header file - OSPI
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

#ifndef _SENSOR_OSPI_ADS1115_H
#define _SENSOR_OSPI_ADS1115_H

#ifdef ADS1115

#include "sensors.h"
#include "SensorBase.hpp"

/**
 * @brief ADS1115 16-bit ADC sensor for OSPI platform
 * @note Provides high-precision analog readings via I2C interface
 */
class OspiAds1115Sensor : public SensorBase {
public:
    /**
     * @brief Constructor
     * @param type Sensor type identifier
     */
    explicit OspiAds1115Sensor(uint type) : SensorBase(type) {}
    virtual ~OspiAds1115Sensor() {}
    
    /**
     * @brief Read analog value from ADS1115 ADC
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
     * @note Reads voltage from specified channel and applies conversion formula
     */
    virtual int read(unsigned long time) override;
    
    /**
     * @brief Get measurement unit identifier
     * @return Unit ID based on sensor configuration
     */
    virtual unsigned char getUnitId() const override;
};

#endif // ADS1115

#endif // _SENSORS_H
