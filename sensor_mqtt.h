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

#ifndef _SENSOR_MQTT_H
#define _SENSOR_MQTT_H

#include "sensors.h"

/**
 * @brief Initialize MQTT sensor subsystem
 * @note Registers MQTT callback handler for sensor updates
 */
void sensor_mqtt_init();

/**
 * @brief Subscribe to MQTT topic for a specific sensor
 * @param nr Sensor number
 * @param type URL type (SENSORURL_TYPE_TOPIC for MQTT topics)
 * @param urlstr MQTT topic to subscribe to
 * @note Updates sensor state to indicate MQTT subscription is active
 */
void sensor_mqtt_subscribe(uint nr, uint type, const char *urlstr);

/**
 * @brief Unsubscribe from MQTT topic for a specific sensor
 * @param nr Sensor number
 * @param type URL type (SENSORURL_TYPE_TOPIC for MQTT topics)
 * @param urlstr MQTT topic to unsubscribe from
 * @note Marks sensor as no longer receiving MQTT updates
 */
void sensor_mqtt_unsubscribe(uint nr, uint type, const char *urlstr);

#include "SensorBase.hpp"

/**
 * @brief MQTT sensor class for receiving sensor data via MQTT topics
 * @note Supports wildcards (+ for single level, # for multi-level) in topic patterns
 */
class MqttSensor : public SensorBase {
public:
// MQTT-specific fields
    char url[200] = {0};            // URL for HTTP sensors or Host for MQTT
    char topic[200] = {0};          // MQTT topic
    char filter[200] = {0};         // JSON filter for MQTT

    /**
     * @brief Constructor
     * @param type Sensor type identifier
     */
    explicit MqttSensor(uint type) : SensorBase(type) {}
    virtual ~MqttSensor() {}
    
    /**
     * @brief Read sensor value from MQTT topic
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS if data was pushed via MQTT, HTTP_RQT_NOT_RECEIVED otherwise
     * @note Subscribes to MQTT topic on first read if not already subscribed
     */
    virtual int read(unsigned long time) override;

    /**
     * @brief Deserialize sensor configuration from JSON
     * @param obj JSON object containing sensor configuration (url, topic, filter)
     * @note Automatically subscribes to MQTT topic when topic field is set
     */
    virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;
    
    /**
     * @brief Serialize sensor configuration to JSON
     * @param obj JSON object to populate with sensor configuration
     */
    virtual void toJson(ArduinoJson::JsonObject obj) const override;
    
    /**
     * @brief Get measurement unit identifier
     * @return Unit ID for this sensor type
     */
    virtual unsigned char getUnitId() const override;
    
    /**
     * @brief Emit sensor data as JSON to BufferFiller
     * @param bfill BufferFiller object for output
     */
    virtual void emitJson(BufferFiller& bfill) const override;

    // Moved helpers
    /**
     * @brief Check if MQTT topic matches a pattern with wildcards
     * @param mtopic Actual MQTT topic received
     * @param pattern Pattern to match against (supports + for single level, # for multi-level)
     * @return true if topic matches pattern, false otherwise
     * @note + matches single level (e.g., sensor/+/data), # matches multiple levels (e.g., sensor/#)
     */
    static bool filterMatches(const char* mtopic, const char* pattern);
    
#if defined(ARDUINO)
    /**
     * @brief MQTT message callback for Arduino platforms
     * @param mtopic MQTT topic that triggered the callback
     * @param payload Message payload data
     * @param length Length of payload in bytes
     * @note Updates matching sensors with received values and marks them for re-reading
     */
    static void callback(char* mtopic, byte* payload, unsigned int length);
#else
    /**
     * @brief MQTT message callback for Linux platforms
     * @param mosq Mosquitto client instance
     * @param obj User data object (unused)
     * @param msg Received MQTT message
     * @note Updates matching sensors with received values and marks them for re-reading
     */
    static void callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);
#endif
};

#endif // _SENSOR_MQTT_H
