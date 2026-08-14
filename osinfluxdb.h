/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX/ESP8266) Firmware
 * Copyright (C) 2024 by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * OpenSprinkler library header file
 * Sep 2024 @ OpenSprinklerShop.de
 *
 * This file is part of the OpenSprinkler library
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

#ifndef _OSINFLUX_H
#define _OSINFLUX_H
#include "ArduinoJson.hpp"

#if defined(DISABLE_INFLUXDB)

// InfluxDB integration is disabled (e.g. for demo/native builds without deps).
class OSInfluxDB {
private:
    bool enabled = false;
    bool initialized = true;

public:
    ~OSInfluxDB() = default;
    void set_influx_config(int enabled, char *url, uint16_t port, char *org, char *bucket, char *token) {
        (void)enabled; (void)url; (void)port; (void)org; (void)bucket; (void)token;
        this->enabled = false;
        this->initialized = true;
    }
    void set_influx_config(ArduinoJson::JsonDocument &doc) {
        (void)doc;
        enabled = false;
        initialized = true;
    }
    void set_influx_config(const char *json) {
        (void)json;
        enabled = false;
        initialized = true;
    }
    void get_influx_config(ArduinoJson::JsonDocument &doc) {
        doc["en"] = 0;
    }
    void get_influx_config(char *json) {
        if (json) {
            strcpy(json, "{\"en\":0}");
        }
    }
    bool isEnabled() { return false; }
    void suspend() {}
    void resume() {}
    void push_message(uint32_t type, uint32_t lval, float fval, const char* sval) {
        (void)type; (void)lval; (void)fval; (void)sval;
    }
};

#else

#if defined(OSPI)
#include "influxdb.hpp"
#elif !defined(ESP8266) && !defined(ESP32)
// DEMO / generic native builds (esp. Windows) don't ship influxdb-cpp.
// The integration is compiled out in osinfluxdb.cpp when DEMO is set.
struct influxdb_cpp_server_info_stub;
#endif
// ESP8266/ESP32: stateless line-protocol sender, no client library required.

class OSInfluxDB {
private:
    #if defined(OSPI)
    influxdb_cpp::server_info * client;
    #elif !defined(ESP8266) && !defined(ESP32)
    void * client;
    #endif
    // ESP8266/ESP32: stateless, no persistent client held.
    bool enabled;
    bool initialized;
    void init();
    #if defined(ESP8266) || defined(ESP32)
    void influx_post_line(const char* line); // build HTTP POST + send statelessly
    #endif
    void influxdb_send_state(const char *name, int state);
    void influxdb_send_station(const char *name, uint32_t station, int state);
    void influxdb_send_program(const char *name, uint32_t nr, float level);
    void influxdb_send_flowsensor(const char *name, uint32_t count, float volume);
    void influxdb_send_flowalert(const char *name, uint32_t station, int f1, int f2, int f3, int f4, int f5);
    void influxdb_send_warning(const char *name, uint32_t level, float value);

public:
    ~OSInfluxDB();
    void set_influx_config(int enabled, char *url, uint16_t port, char *org, char *bucket, char *token);
    void set_influx_config(ArduinoJson::JsonDocument &doc);
    void set_influx_config(const char *json);
    void get_influx_config(ArduinoJson::JsonDocument &doc);
    void get_influx_config(char *json);
    bool isEnabled();
    void suspend(); // free client and disable (e.g. to free RAM before sending e-mail)
    void resume();  // re-read config from storage and re-enable if configured
    #if defined(ESP8266) || defined(ESP32)
    // Stateless line-protocol writer: assembles "<measurement>,<tagset> <fieldset>"
    // and POSTs it via OpenSprinkler::send_http_request (no persistent client/RAM).
    void write_influx_line(const char* measurement, const char* tagset, const char* fieldset);
    // Escape a tag key/value per InfluxDB line protocol. Returns bytes written.
    static size_t influx_escape(char* dst, size_t cap, const char* src);
    #elif defined(OSPI)
    influxdb_cpp::server_info * get_client();
    #endif
    void push_message(uint32_t type, uint32_t lval, float fval, const char* sval);
};

#endif // DISABLE_INFLUXDB
#endif
