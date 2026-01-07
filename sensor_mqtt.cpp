/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Utility functions
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
#include "sensor_mqtt.h"
#include "sensors.h"
#include "mqtt.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"

extern OpenSprinkler os;

void sensor_mqtt_init() {
    os.mqtt.setCallback(2, MqttSensor::callback);
}

void MqttSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
	SensorBase::fromJson(obj);
	    // MQTT-specific fields
    if (obj.containsKey("url")) {
      const char *u = obj["url"].as<const char*>();
      if (u) strncpy(url, u, sizeof(url)-1);
    }
    if (obj.containsKey("topic")) {
      const char *t = obj["topic"].as<const char*>();
      if (t) {
        strncpy(topic, t, sizeof(topic)-1);
        // Subscribe to MQTT topic when it's set
        if (topic[0]) {
          sensor_mqtt_subscribe(nr, SENSORURL_TYPE_TOPIC, topic);
        }
      }
    }
    if (obj.containsKey("filter")) {
      const char *f = obj["filter"].as<const char*>();
      if (f) strncpy(filter, f, sizeof(filter)-1);
    }
}

void MqttSensor::toJson(ArduinoJson::JsonObject obj) const {
	SensorBase::toJson(obj);
		// MQTT-specific fields
	if (url[0]) obj["url"] = url;
	if (topic[0]) obj["topic"] = topic;
	if (filter[0]) obj["filter"] = filter;
}
/**
 * @brief 
 * 
 * @param mtopic reported topic opensprinkler/analogsensor/name
 * @param pattern topic pattern  opensprinkler/ <star> or opensprinkler/# or opensprinkler/#/abc/#
 * @return true 
 * @return false 
 */
bool MqttSensor::filterMatches(const char* mtopic, const char* pattern) {
    if (!mtopic || !pattern) return false;

    const char *mp = mtopic;
    const char *pp = pattern;
    while (*pp && *mp) {
        char ch1 = *mp++;
        char ch2 = *pp++;
        if (ch2 == '+') { //level ok up to "/"
            while (*mp) {
                if (ch1 == '/')
                    break;
                ch1 = *mp++;
            }
        } else if (ch2 == '#') { //multilevel
            const char *p = strpbrk(pp, "#+");
            if (!p) return true;
            if (strncmp(pp, mp, p-pp) == 0) {
                mp = mp + (p-pp);
                pp = p;
            }
        }
        if (ch1 != ch2)
            return false;
        else if (ch1 == 0 && ch2 == 0)
            return true;
        else if (ch1 == 0 || ch2 == 0)
            return false;
    }
    return true;
}

/**
 * @brief mqtt callback
 * 
 */
#if defined(ARDUINO)
void MqttSensor::callback(char* mtopic, byte* payload, unsigned int length) {
#else
void MqttSensor::callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    DEBUG_PRINTLN("sensor_mqtt_callback0");
	char* mtopic = (char*)msg->topic;
	byte* payload = (byte*)msg->payload;
	unsigned int length = msg->payloadlen;
#endif

    DEBUG_PRINTLN("sensor_mqtt_callback1");

	if (!mtopic || !payload) return;
	time_t now = os.now_tz();
	SensorIterator it = sensors_iterate_begin();
	SensorBase *sensor = sensors_iterate_next(it);
	while (sensor) {
		if (sensor->type == SENSOR_MQTT && sensor->last_read != now) {
			// Use toJson to get MQTT-specific fields
			ArduinoJson::JsonDocument doc;
			ArduinoJson::JsonObject obj = doc.to<ArduinoJson::JsonObject>();
			sensor->toJson(obj);
			const char* topic = obj["topic"] | "";
			const char* filter = obj["filter"] | "";
			
			DEBUG_PRINT("mtopic: "); DEBUG_PRINTLN(mtopic);
			DEBUG_PRINT("topic:  "); DEBUG_PRINTLN(topic);
			
			if (topic[0] && MqttSensor::filterMatches(mtopic, topic)) {
				double value = 0;
				int ok = findValue((char*)payload, length, filter[0] ? filter : NULL, value);
				if (ok && value >= -10000 && value <= 10000 && (value != sensor->last_data || !sensor->flags.data_ok || now-sensor->last_read > 6000)) {
					sensor->last_data = value;
					sensor->flags.data_ok = true;
					sensor->last_read = now;	
					sensor->mqtt_push = true;
					sensor->repeat_read = 1; //This will call read_sensor_mqtt
					DEBUG_PRINTLN("sensor_mqtt_callback2");
				}
			}
		}
		sensor = sensors_iterate_next(it);
	}
    DEBUG_PRINTLN("sensor_mqtt_callback3");
}

int MqttSensor::read(unsigned long time) {
	if (!os.mqtt.enabled() || !os.mqtt.connected()) {
		flags.data_ok = false;
		mqtt_init = false;
	} else if (mqtt_push) {
		DEBUG_PRINTLN("read_sensor_mqtt: push data");
		mqtt_push = false;
		repeat_read = 0;
		return HTTP_RQT_SUCCESS; //Adds also data to the log + push data
	} else {
		repeat_read = 0;
		last_read = time;
        DEBUG_PRINT("read_sensor_mqtt1: ");
		DEBUG_PRINTLN(name);
		if (topic[0]) {
            DEBUG_PRINT("subscribe: ");
            DEBUG_PRINTLN(topic);
			os.mqtt.subscribe(topic);
			mqtt_init = true;
		}
	}
	return HTTP_RQT_NOT_RECEIVED;
}

void sensor_mqtt_subscribe(uint nr, uint type, const char *urlstr) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (urlstr && urlstr[0] && type == SENSORURL_TYPE_TOPIC && sensor && sensor->type == SENSOR_MQTT) {
	    DEBUG_PRINT("sensor_mqtt_subscribe1: ");
		DEBUG_PRINTLN(sensor->name);
        DEBUG_PRINT("subscribe: ");
        DEBUG_PRINTLN(urlstr);
		if (!os.mqtt.subscribe(urlstr))
			DEBUG_PRINTLN("error subscribe!!");
	    sensor->mqtt_init = true;
    }
}

void MqttSensor::emitJson(BufferFiller& bfill) const {
	SensorBase::emitJson(bfill);
}

void sensor_mqtt_unsubscribe(uint nr, uint type, const char *urlstr) {
    SensorBase* sensor = sensor_by_nr(nr);
    if (urlstr && urlstr[0] && type == SENSORURL_TYPE_TOPIC && sensor && sensor->type == SENSOR_MQTT) {
	    DEBUG_PRINT("sensor_mqtt_unsubscribe1: ");
		DEBUG_PRINTLN(sensor->name);
        DEBUG_PRINT("unsubscribe: ");
        DEBUG_PRINTLN(urlstr);
		if (!os.mqtt.unsubscribe(urlstr))
			DEBUG_PRINTLN("error unsubscribe!!");
		sensor->mqtt_init = false;
    }
}
