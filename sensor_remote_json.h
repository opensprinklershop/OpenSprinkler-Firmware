/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 * Remote JSON Sensor Implementation by Stefan Schmaltz (info@opensprinklershop.de)
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

#ifndef _SENSOR_REMOTE_JSON_H
#define _SENSOR_REMOTE_JSON_H

#include "sensors.h"
#include "SensorBase.hpp"

/**
 * @brief Remote JSON sensor class for querying arbitrary REST APIs with streaming filtration
 * @note Designed to handle huge payloads with low memory buffer matching
 */
class RemoteJsonSensor : public SensorBase {
public:
    char url[200] = {0};            // HTTP or HTTPS URL to retrieve JSON from
    char filter[200] = {0};         // JSON property key filter string (e.g., outer|inner|target)

    /**
     * @brief Constructor
     * @param type Sensor type identifier
     */
    explicit RemoteJsonSensor(uint type) : SensorBase(type) {}
    virtual ~RemoteJsonSensor() {}
    
    /**
     * @brief Poll the remote JSON URL, stream response and parse dynamically
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS if valid value extracted, HTTP_RQT_NOT_RECEIVED otherwise
     */
    virtual int read(unsigned long time) override;

    /**
     * @brief Deserialize sensor configuration from JSON
     * @param obj JSON object containing sensor configuration (url, filter)
     */
    virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;
    
    /**
     * @brief Serialize sensor configuration to JSON
     * @param obj JSON object to populate with sensor configuration
     */
    virtual void toJson(ArduinoJson::JsonObject obj) const override;
    
    /**
     * @brief Get measurement unit identifier
     * @return Unit ID for this sensor (e.g. customized or userdefined)
     */
    virtual unsigned char getUnitId() const override;
    
    /**
     * @brief Emit sensor data as JSON to BufferFiller
     * @param bfill BufferFiller object for output
     */
    virtual void emitJson(BufferFiller& bfill) const override;
};

#endif // _SENSOR_REMOTE_JSON_H