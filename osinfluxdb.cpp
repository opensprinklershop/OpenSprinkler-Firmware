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

#include "osinfluxdb.h"

#if defined(DISABLE_INFLUXDB)

// All methods are inline stubs in the header when DISABLE_INFLUXDB is set.

#else
#include "utils.h"
#include "defines.h"
#include "OpenSprinkler.h"

// tmp_buffer declared in sensors.h
extern OpenSprinkler os;

#if !defined(ESP32)
#define INFLUX_CONFIG_FILE "influx.json"
#else
#define INFLUX_CONFIG_FILE "/influx.json"
#endif

void OSInfluxDB::set_influx_config(int enabled, char *url, uint16_t port, char *org, char *bucket, char *token) {
    ArduinoJson::JsonDocument doc;
    doc["en"] = enabled;
    doc["url"] = url;
    doc["port"] = port;
    doc["org"] = org;
    doc["bucket"] = bucket;
    doc["token"] = token;
    set_influx_config(doc);
}

void OSInfluxDB::set_influx_config(ArduinoJson::JsonDocument &doc) {
    size_t size = ArduinoJson::serializeJson(doc, (char*)tmp_buffer, TMP_BUFFER_SIZE_L);
    remove_file(INFLUX_CONFIG_FILE);
    file_write_block(INFLUX_CONFIG_FILE, tmp_buffer, 0, size);

    enabled = doc["en"];
    initialized = true;
}

void OSInfluxDB::set_influx_config(const char *data) {
    while (*data == ' ' || *data == '\t' || *data == '\r' || *data == '\n') {
        data++;
    }

    remove_file(INFLUX_CONFIG_FILE);

    size_t size = strlen(data);
    if (size > 0 && data[0] == '{') {
        file_write_block(INFLUX_CONFIG_FILE, data, 0, size);
    } else {
        file_write_block(INFLUX_CONFIG_FILE, "{", 0, 1);
        file_write_block(INFLUX_CONFIG_FILE, data, 1, size);
        file_write_block(INFLUX_CONFIG_FILE, "}", size + 1, 1);
    }


    enabled = false;
    initialized = false;
}

void OSInfluxDB::get_influx_config(char *json) {
    json[0] = 0;
    if (file_exists(INFLUX_CONFIG_FILE))
    {
        ulong size = file_read_block(INFLUX_CONFIG_FILE, json, 0, TMP_BUFFER_SIZE_L);
        if (size >= TMP_BUFFER_SIZE_L) {
            size = TMP_BUFFER_SIZE_L - 1;
        }
        json[size] = 0;
    }

    // Ensure stored config is a single valid JSON object. Older malformed values
    // can end up as nested braces or truncated payloads, which breaks /jc output.
    ArduinoJson::JsonDocument doc;
    ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, json);
    if (json[0] == 0 || json[0] != '{' || error || doc.isNull() || !doc.as<ArduinoJson::JsonVariantConst>().is<ArduinoJson::JsonObjectConst>()) {
        strcpy(json, "{\"en\":0}");
        set_influx_config(json);
        return;
    }

    size_t normalized = ArduinoJson::serializeJson(doc, json, TMP_BUFFER_SIZE_L);
    if (normalized == 0 || normalized >= TMP_BUFFER_SIZE_L) {
        strcpy(json, "{\"en\":0}");
        set_influx_config(json);
    }
}

void OSInfluxDB::get_influx_config(ArduinoJson::JsonDocument &doc) {
    //DEBUG_PRINTLN("Load influx config");
    get_influx_config(tmp_buffer);
    ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, tmp_buffer);
	if (error || doc.isNull() || !doc.containsKey("en")) {
        if (error) {
            DEBUG_PRINT(F("influxdb: deserializeJson() failed: "));
		    DEBUG_PRINTLN(error.c_str());  
        }
        doc["en"] = 0;
		doc["url"] = "";
        doc["port"] = 8086;
		doc["org"] = "";
		doc["bucket"] = "";
		doc["token"] = "";
        set_influx_config(doc);
    }
    enabled = doc["en"];
    initialized = true; 
}

void OSInfluxDB::init() {
    ArduinoJson::JsonDocument doc;
    get_influx_config(doc);
    enabled = doc["en"];
    initialized = true; 
}

boolean OSInfluxDB::isEnabled() {
    if (!initialized) {
        init();
    }
    return enabled; 
}

void OSInfluxDB::suspend() {
    enabled = false;
    initialized = false;
}

void OSInfluxDB::resume() {
    init();
}

OSInfluxDB::~OSInfluxDB() {
}

size_t OSInfluxDB::influx_escape(char* dst, size_t cap, const char* src) {
    size_t o = 0;
    if (!dst || cap == 0) return 0;
    for (; src && *src; src++) {
        char c = *src;
        bool esc = (c == ',' || c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (o + (esc ? 2u : 1u) >= cap) break;
        if (esc) dst[o++] = '\\';
        dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

void OSInfluxDB::write_influx_line(const char* measurement, const char* tagset, const char* fieldset) {
    if (!measurement || !fieldset || !fieldset[0]) return;
    char line[384];
    if (tagset && tagset[0])
        snprintf(line, sizeof(line), "%s,%s %s", measurement, tagset, fieldset);
    else
        snprintf(line, sizeof(line), "%s %s", measurement, fieldset);
    influx_post_line(line);
}

// Stateless InfluxDB v2 write: read config, build one HTTP POST into ether_buffer
// and send it via OpenSprinkler::send_http_request (opens/closes the socket and
// allocates any TLS buffers only for the duration of the request).
void OSInfluxDB::influx_post_line(const char* line) {
    if (!initialized) init();
    if (!enabled || !line || !line[0]) return;

    // Copy config into locals: the ArduinoJson string values point into
    // tmp_buffer, which get_influx_config() overwrites and which we reuse below.
    char url[128]; char org[64]; char bucket[64]; char token[200];
    int port = 8086;
    url[0] = org[0] = bucket[0] = token[0] = 0;
    {
        ArduinoJson::JsonDocument doc;
        get_influx_config(doc);
        if ((int)(doc["en"] | 0) == 0) return;
        SAFE_STRNCPY(url, (const char*)(doc["url"] | ""), sizeof(url));
        port = doc["port"] | 8086; if (port == 0) port = 8086;
        SAFE_STRNCPY(org, (const char*)(doc["org"] | ""), sizeof(org));
        SAFE_STRNCPY(bucket, (const char*)(doc["bucket"] | ""), sizeof(bucket));
        SAFE_STRNCPY(token, (const char*)(doc["token"] | ""), sizeof(token));
    }
    if (url[0] == 0) return;

    // Parse scheme + host (+ optional inline :port) + path prefix from the URL.
    bool usessl;
    const char* rest = url;
    if (strncmp(rest, "https://", 8) == 0) { usessl = true; rest += 8; }
    else if (strncmp(rest, "http://", 7) == 0) { usessl = false; rest += 7; }
    else { usessl = (port == 443); }

    char host[100];
    const char* slash = strchr(rest, '/');
    size_t hostlen = slash ? (size_t)(slash - rest) : strlen(rest);
    if (hostlen == 0 || hostlen >= sizeof(host)) return;
    memcpy(host, rest, hostlen); host[hostlen] = 0;
    char* colon = strchr(host, ':');
    if (colon) { *colon = 0; int p = atoi(colon + 1); if (p > 0) port = p; }

    char pathprefix[64]; pathprefix[0] = 0;
    if (slash) {
        strncpy(pathprefix, slash, sizeof(pathprefix) - 1);
        pathprefix[sizeof(pathprefix) - 1] = 0;
        size_t pl = strlen(pathprefix);
        while (pl > 0 && pathprefix[pl - 1] == '/') pathprefix[--pl] = 0; // drop trailing '/'
    }

    int n = snprintf(ether_buffer, ETHER_BUFFER_SIZE,
        "POST %s/api/v2/write?org=%s&bucket=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Token %s\r\n"
        "User-Agent: OpenSprinkler\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        pathprefix, org, bucket, host, token, (int)strlen(line), line);
    if (n <= 0 || n >= (int)ETHER_BUFFER_SIZE) {
        DEBUG_PRINTLN(F("influxdb: request exceeds buffer"));
        return;
    }

    os.send_http_request(host, (uint16_t)port, ether_buffer, NULL, usessl, 5000, false);
}




// Build "devicename=<escaped>" into dst.
static void influx_devicename_tag(char* dst, size_t cap) {
    char raw[64]; raw[0] = 0;
    os.sopt_load(SOPT_DEVICE_NAME, raw, sizeof(raw) - 1);
    const char* pfx = "devicename=";
    size_t o = 0;
    while (*pfx && o + 1 < cap) dst[o++] = *pfx++;
    o += OSInfluxDB::influx_escape(dst + o, cap - o, raw);
    dst[o] = 0;
}

void OSInfluxDB::influxdb_send_state(const char *name, int state) {
    char tags[256], nameesc[64], fields[48];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",name=%s", nameesc);
    snprintf(fields, sizeof(fields), "state=%di", state);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::influxdb_send_station(const char *name, uint32_t station, int state) {
    char tags[256], nameesc[64], fields[64];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",name=%s", nameesc);
    snprintf(fields, sizeof(fields), "station=%lui,state=%di", (unsigned long)station, state);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::influxdb_send_program(const char *name, uint32_t nr, float level) {
    char tags[256], nameesc[64], fields[64];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",name=%s", nameesc);
    snprintf(fields, sizeof(fields), "program=%lui,level=%.2f", (unsigned long)nr, level);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::influxdb_send_flowsensor(const char *name, uint32_t count, float volume) {
    char tags[256], nameesc[64], fields[64];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",name=%s", nameesc);
    snprintf(fields, sizeof(fields), "count=%lui,volume=%.2f", (unsigned long)count, volume);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::influxdb_send_flowalert(const char *name, uint32_t station, int f1, int f2, int f3, int f4, int f5) {
    char tags[256], nameesc[64], fields[96];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",name=%s", nameesc);
    snprintf(fields, sizeof(fields), "station=%lui,flowrate=%.2f,duration=%di,alert_setpoint=%.2f",
        (unsigned long)station, (double)f1 + (double)f2 / 100.0, f3, (double)f4 + (double)f5 / 100.0);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::influxdb_send_warning(const char *name, uint32_t level, float value) {
    char tags[256], nameesc[64], fields[64];
    influx_devicename_tag(tags, sizeof(tags));
    influx_escape(nameesc, sizeof(nameesc), name);
    size_t tl = strlen(tags);
    snprintf(tags + tl, sizeof(tags) - tl, ",warning=%s", nameesc);
    snprintf(fields, sizeof(fields), "level=%di,currentvalue=%.2f", (int)level, value);
    write_influx_line("opensprinkler", tags, fields);
}

void OSInfluxDB::push_message(uint32_t type, uint32_t lval, float fval, const char* sval) {
    if (!isEnabled()) return;

   	switch(type) {
		case  NOTIFY_STATION_ON:
			influxdb_send_station("station", lval, 1);
			break;

		case NOTIFY_FLOW_ALERT:{
			//influxdb_send_flowalert("flowalert", lval, f1, f2, f3, f4, f5);
			break;
		}

		case NOTIFY_STATION_OFF:
			influxdb_send_station("station", lval, 0);
			break;

		case NOTIFY_PROGRAM_SCHED:
			influxdb_send_program("program sched", lval, fval);
			break;

		case NOTIFY_PROGRAM_END:
			influxdb_send_program("program end", lval, 0);
			break;

		case NOTIFY_SENSOR1:
			influxdb_send_state("sensor1", (int)fval);
			break;

		case NOTIFY_SENSOR2:
			influxdb_send_state("sensor2", (int)fval);
			break;

		case NOTIFY_RAINDELAY:
			influxdb_send_state("raindelay", (int)fval);
			break;

		case NOTIFY_FLOWSENSOR:
            influxdb_send_flowsensor("flowsensor", lval, (float)lval * os.get_flow_volume_per_pulse());
			break;

		case NOTIFY_WEATHER_UPDATE:
			influxdb_send_state("waterlevel", (int)fval);
			break;

		case NOTIFY_REBOOT:
			break;

		case NOTIFY_MONITOR_LOW: 
		case NOTIFY_MONITOR_MID:
		case NOTIFY_MONITOR_HIGH:
			influxdb_send_flowsensor(sval, lval, fval);
			break;

	}
}

#endif // DISABLE_INFLUXDB
