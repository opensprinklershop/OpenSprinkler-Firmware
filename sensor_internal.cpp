/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Internal system sensor implementation
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

#include "sensor_internal.h"
#include "OpenSprinkler.h"
#include "sensors.h"

#if defined(ESP8266) || defined(ESP32)
#include <FS.h>
#include <LittleFS.h>
extern size_t freeMemory();
#endif

#if defined(OSPI)
#include <stdio.h>
#include <stdlib.h>
#endif

int InternalSensor::read(unsigned long time) {
  if (!this->flags.enable) return HTTP_RQT_NOT_RECEIVED;

  switch (this->type) {
#if defined(ESP8266) || defined(ESP32)
    case SENSOR_FREE_MEMORY: {
      uint32_t fm = freeMemory();
      if (this->last_native_data == fm) {
        this->flags.data_ok = true;
        return HTTP_RQT_NOT_RECEIVED;
      }
      this->last_native_data = fm;
      this->last_data = fm/1000;
      this->last_read = time;
      this->flags.data_ok = true;
      return HTTP_RQT_SUCCESS;
    }
    
    case SENSOR_FREE_STORE: {
#if defined(ESP8266)
      struct FSInfo fsinfo;
      boolean ok = LittleFS.info(fsinfo);
      if (ok) {
        uint32_t fd = fsinfo.totalBytes - fsinfo.usedBytes;
        if (this->last_native_data == fd) {
          this->flags.data_ok = true;
          return HTTP_RQT_NOT_RECEIVED;
        }
#elif defined(ESP32)
      boolean ok = LittleFS.totalBytes() > 0;
      if (ok) {
        uint32_t fd = LittleFS.totalBytes() - LittleFS.usedBytes();
#endif
        if (this->last_native_data == fd) {
          this->flags.data_ok = true;
          return HTTP_RQT_NOT_RECEIVED;
        }
        this->last_native_data = fd;
        this->last_data = fd/1000;
      }
      this->flags.data_ok = ok;
      this->last_read = time;
      return HTTP_RQT_SUCCESS;
    }
#endif // defined(ESP8266) || defined(ESP32)

#if defined(ESP32)
    case SENSOR_INTERNAL_TEMP: {
      float temp = temperatureRead();
      int32_t temp_milli = (int32_t)(temp * 1000.0f);
      if (this->last_native_data == (uint32_t)temp_milli) {
        this->flags.data_ok = true;
        return HTTP_RQT_NOT_RECEIVED;
      }

      this->last_read = time;
      this->last_native_data = (uint32_t)temp_milli;
      this->last_data = temp;
      this->flags.data_ok = true;
      return HTTP_RQT_SUCCESS;
    }
#endif // defined(ESP32)

#if defined(OSPI)
    case SENSOR_INTERNAL_TEMP: {
      char buf[10];
      size_t res = 0;
      FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
      if (fp) {
        res = fread(buf, 1, 10, fp);
        fclose(fp);
      }
      if (!res)
        return HTTP_RQT_NOT_RECEIVED;

      this->last_read = time;
      this->last_native_data = strtol(buf, NULL, 0);
      this->last_data = (double)this->last_native_data / 1000;
      this->flags.data_ok = true;

      return HTTP_RQT_SUCCESS;
    }
#endif // defined(OSPI)

    default:
      return HTTP_RQT_NOT_RECEIVED;
  }
}


const char* InternalSensor::getUnit() const {
  switch(type) {
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE:
      return "KB"; 
    case SENSOR_INTERNAL_TEMP:
      return "°C";
  }
  return SensorBase::getUnit();
}

unsigned char InternalSensor::getUnitId() const {
  switch(type) {
    case SENSOR_FREE_MEMORY:
    case SENSOR_FREE_STORE:
      return UNIT_USERDEF;
    case SENSOR_INTERNAL_TEMP:
      return UNIT_DEGREE;
  }
  return SensorBase::getUnitId();
}