/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Remote HTTP sensor implementation
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

#include "sensor_remote.h"
#include "OpenSprinkler.h"
#include "sensors.h"
#include "utils.h"
#include "opensprinkler_server.h"

extern OpenSprinkler os;
// ether_buffer and tmp_buffer declared in sensors.h

bool RemoteSensor::extract(char *s, char *buf, int maxlen) {
  s = strstr(s, ":");
  if (!s) return false;
  s++;
  while (*s == ' ') s++;  // skip spaces
  char *e = strstr(s, ",");
  char *f = strstr(s, "}");
  if (!e && !f) return false;
  if (f && f < e) e = f;
  int l = e - s;
  if (l < 1 || l >= maxlen) return false;
  strncpy(buf, s, l);
  buf[l] = 0;
  return true;
}

int RemoteSensor::read(unsigned long time) {
  unsigned char ip[4];
  IP4_EXTRACT_BYTES(ip, this->ip);
  ulong prev_last_read = this->last_read;

  // DEBUG_PRINTLN(F("RemoteSensor::read"));

  char *p = tmp_buffer;
  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);

  bf.emit_p(PSTR("GET /sg?pw=$O&nr=$D"), SOPT_PASSWORD, this->id);
  bf.emit_p(PSTR(" HTTP/1.0\r\nHOST: $D.$D.$D.$D\r\n\r\n"), ip[0], ip[1], ip[2],
            ip[3]);

  // DEBUG_PRINTLN(p);

  char server[20];
  sprintf(server, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  int res = os.send_http_request(server, this->port, p, NULL, false, 500);
  if (res == HTTP_RQT_SUCCESS) {
    DEBUG_PRINTLN("Send Ok");
    p = ether_buffer;
    DEBUG_PRINTLN(p);

    char buf[20];
    char *s = strstr(p, "\"nativedata\":");
    if (s && extract(s, buf, sizeof(buf))) {
      this->last_native_data = strtoul(buf, NULL, 0);
    }
    s = strstr(p, "\"data\":");
    if (s && extract(s, buf, sizeof(buf))) {
      double value = -1;
      int ok = sscanf(buf, "%lf", &value);
      if (ok && (value != this->last_data || !this->flags.data_ok ||
                 time - prev_last_read > 6000)) {
        this->last_data = value;
      } else if (!ok) {
        return HTTP_RQT_NOT_RECEIVED;
      }
      this->flags.data_ok = true;
    } else {
      return HTTP_RQT_NOT_RECEIVED;
    }
    s = strstr(p, "\"unitid\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      this->unitid = atoi(buf);
      this->assigned_unitid = this->unitid;
    }
    s = strstr(p, "\"unit\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      urlDecodeAndUnescape(buf);
      strncpy(this->userdef_unit, buf, sizeof(this->userdef_unit) - 1);
    }
    s = strstr(p, "\"last\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      (void)strtoul(buf, NULL, 0);
    }

    this->last_read = time;

    return HTTP_RQT_SUCCESS;
  }
  return res;
}
