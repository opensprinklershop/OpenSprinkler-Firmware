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

#include "sensor_remote_json.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include <new>

#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#elif defined(ESP32)
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#elif defined(OSPI)
#include "naett.h"
#include <unistd.h>
#endif

extern OpenSprinkler os;

void RemoteJsonSensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    SensorBase::fromJson(obj);
    if (obj.containsKey(F("url"))) {
        const char *u = obj[F("url")].as<const char*>();
        if (u) strncpy(url, u, sizeof(url)-1);
    }
    if (obj.containsKey(F("filter"))) {
        const char *f = obj[F("filter")].as<const char*>();
        if (f) strncpy(filter, f, sizeof(filter)-1);
    }
}

void RemoteJsonSensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    if (url[0]) obj[F("url")] = url;
    if (filter[0]) obj[F("filter")] = filter;
}

unsigned char RemoteJsonSensor::getUnitId() const {
    return assigned_unitid > 0 ? assigned_unitid : UNIT_USERDEF;
}

void RemoteJsonSensor::emitJson(BufferFiller& bfill) const {
    SensorBase::emitJson(bfill);
}

int RemoteJsonSensor::read(unsigned long time) {
    if (url[0] == '\0') {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }

    double extracted_value = -9999;
    bool value_extracted = false;

#if defined(ESP8266) || defined(ESP32)
    HTTPClient http;
    int httpCode = 0;
    bool opened = false;
    WiFiClientSecure *client_secure = nullptr;
    WiFiClient *client = nullptr;

    http.setTimeout(SENSOR_READ_TIMEOUT);

    if (strncmp(url, "https://", 8) == 0) {
        client_secure = new WiFiClientSecure();
        if (client_secure) {
            client_secure->setInsecure();
            client = client_secure;
        }
    } else {
        client = new WiFiClient();
    }

    if (!client) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }

    if (http.begin(*client, url)) {
        httpCode = http.GET();
        opened = true;
    }

    if (!opened || httpCode != 200) {
        if (opened) http.end();
        if (client_secure) delete client_secure;
        else if (client) delete client;
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        if (client_secure) delete client_secure;
        else if (client) delete client;
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // Parse filters
    char filter_copy[200];
    int num_segments = 0;
    const char* segments[16];
    int arrayIndex = 0;
    if (filter[0]) {
        strncpy(filter_copy, filter, sizeof(filter_copy) - 1);
        filter_copy[sizeof(filter_copy) - 1] = '\0';

        char *lb = strrchr(filter_copy, '[');
        if (lb) {
            char *rb = strchr(lb, ']');
            if (rb && rb > lb + 1) {
                arrayIndex = atoi(lb + 1);
                if (arrayIndex < 0) arrayIndex = 0;
                *lb = '\0'; // strip "[N]" so the segment search works with plain text key
            }
        }

        char* token = strtok(filter_copy, "|");
        while (token && num_segments < 16) {
            segments[num_segments++] = token;
            token = strtok(NULL, "|");
        }
    }

    char *chunkBuffer = new char[1536];
    if (!chunkBuffer) {
        http.end();
        if (client_secure) delete client_secure;
        else if (client) delete client;
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    int bufferLen = 0;
    int current_segment_idx = 0;
    const char* p = chunkBuffer;
    unsigned long start_ms = millis();

    while (true) {
        if (millis() - start_ms > SENSOR_READ_TIMEOUT) {
            DEBUG_PRINTLN(F("RemoteJsonSensor: Read timeout"));
            break;
        }

        // Fill buffer from stream if there's space and data
        if (stream->available() && bufferLen < 1400) {
            int to_read = 1536 - bufferLen - 1;
            int read_bytes = stream->read(reinterpret_cast<uint8_t*>(chunkBuffer + bufferLen), to_read);
            if (read_bytes > 0) {
                bufferLen += read_bytes;
                chunkBuffer[bufferLen] = '\0';
            }
        }

        if (bufferLen == 0 && !stream->available() && !stream->connected()) {
            break;
        }

        if (current_segment_idx < num_segments) {
            int current_seg_len = strlen(segments[current_segment_idx]);
            int search_len = bufferLen;
            if (stream->available()) {
                search_len = bufferLen - current_seg_len + 1;
            }
            if (search_len < 0) search_len = 0;

            const char* found = findSegment(chunkBuffer, p, search_len, segments[current_segment_idx], current_seg_len);
            if (found) {
                p = found + current_seg_len;
                current_segment_idx++;
                continue;
            } else {
                // Shift or wait for more data
                int p_offset = p - chunkBuffer;
                if (p_offset > 0) {
                    int keep_len = bufferLen - p_offset;
                    if (keep_len > 0) {
                        memmove(chunkBuffer, p, keep_len);
                        bufferLen = keep_len;
                    } else {
                        bufferLen = 0;
                    }
                    chunkBuffer[bufferLen] = '\0';
                    p = chunkBuffer;
                    continue;
                } else if (bufferLen >= 1400) {
                    // Buffer is full but p is already at the beginning of the buffer.
                    // This means the segment was not found in 1400 bytes. Discard old data.
                    int discard_len = bufferLen - current_seg_len + 1;
                    if (discard_len > 0) {
                        memmove(chunkBuffer, chunkBuffer + discard_len, current_seg_len - 1);
                        bufferLen = current_seg_len - 1;
                    } else {
                        bufferLen = 0;
                    }
                    chunkBuffer[bufferLen] = '\0';
                    p = chunkBuffer;
                    continue;
                }

                if (!stream->available()) {
                    if (!stream->connected()) {
                        break;
                    } else {
                        delay(1);
                        continue;
                    }
                }
            }
        } else {
            int remaining = bufferLen - (p - chunkBuffer);
            if (remaining < 256 && stream->available()) {
                int p_offset = p - chunkBuffer;
                int keep_len = bufferLen - p_offset;
                if (keep_len > 0) {
                    memmove(chunkBuffer, p, keep_len);
                    bufferLen = keep_len;
                } else {
                    bufferLen = 0;
                }
                chunkBuffer[bufferLen] = '\0';
                p = chunkBuffer;
                continue;
            }

            // Skip preceding numeric elements if arrayIndex > 0
            for (int idx = 0; idx < arrayIndex; idx++) {
                p = strpbrk(p, "0123456789.-+");
                if (!p || p >= chunkBuffer + bufferLen) { p = NULL; break; }
                while (p && *p && p < chunkBuffer + bufferLen &&
                       ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+')) p++;
            }

            char val_buf[32];
            const char* val_start = NULL;
            if (p) {
                val_start = strpbrk(p, "0123456789.-+nullNULL");
            }
            if (val_start && val_start < chunkBuffer + bufferLen) {
                int i = 0;
                const char* val_p = val_start;
                while (val_p && i < (int)sizeof(val_buf) - 1 && val_p < chunkBuffer + bufferLen) {
                    char ch = *val_p++;
                    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') {
                        val_buf[i++] = ch;
                    } else {
                        break;
                    }
                }
                val_buf[i] = '\0';
                extracted_value = -9999;
                if (sscanf(val_buf, "%lf", &extracted_value) > 0) {
                    value_extracted = true;
                }
            }
            break;
        }
    }

    http.end();
    delete[] chunkBuffer;
    if (client_secure) delete client_secure;
    else if (client) delete client;

#elif defined(OSPI)
    naettReq *req = naettRequest(url, naettMethod("GET"));
    naettRes *res = naettMake(req);
    
    unsigned long start_ms = millis();
    while (!naettComplete(res)) {
        if (millis() - start_ms > SENSOR_READ_TIMEOUT) {
            naettClose(res);
            naettFree(req);
            flags.data_ok = false;
            return HTTP_RQT_NOT_RECEIVED;
        }
        usleep(50 * 1000);
    }

    if (naettGetStatus(res) != 200) {
        naettClose(res);
        naettFree(req);
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }

    int bodyLength = 0;
    const char *responseBody = (char *)naettGetBody(res, &bodyLength);
    if (responseBody && bodyLength > 0) {
        int ok = findValue(responseBody, bodyLength, filter[0] ? filter : NULL, extracted_value);
        if (ok) {
            value_extracted = true;
        }
    }

    naettClose(res);
    naettFree(req);
#endif

    if (value_extracted && extracted_value >= -10000 && extracted_value <= 10000) {
        last_data = extracted_value;
        flags.data_ok = true;
        last_read = time;
        return HTTP_RQT_SUCCESS;
    } else {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
}