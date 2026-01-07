/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Weather sensor implementation
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

#include "sensor_weather.h"
#include "OpenSprinkler.h"
#include "sensors.h"
#include "weather.h"
#include "sensor_remote.h"
#include "opensprinkler_server.h"

// Weather
time_t last_weather_time = 0;
time_t last_weather_time_eto = 0;
bool current_weather_ok = false;
bool current_weather_eto_ok = false;
double current_temp = 0.0;
double current_humidity = 0.0;
double current_precip = 0.0;
double current_wind = 0.0;
double current_eto = 0.0;
double current_radiation = 0.0;

void GetSensorWeather() {
#if defined(ESP8266) || defined(ESP32)
  if (!useEth)
    if (os.state != OS_STATE_CONNECTED || WiFi.status() != WL_CONNECTED) return;
#endif
  time_t time = os.now_tz();
  if (last_weather_time == 0) last_weather_time = time - 59 * 60;

  if (time < last_weather_time + 60 * 60) return;

  // use temp buffer to construct get command
  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);
  bf.emit_p(PSTR("weatherData?loc=$O&wto=$O&fwv=$D"), SOPT_LOCATION,
            SOPT_WEATHER_OPTS,
            (int)os.iopts[IOPT_FW_VERSION]);

  urlEncode(tmp_buffer);

  strcpy(ether_buffer, "GET /");
  strcat(ether_buffer, tmp_buffer);
  // because dst is part of tmp_buffer,
  // must load weather url AFTER dst is copied to ether_buffer

  // load weather url to tmp_buffer
  char *host = tmp_buffer;
  os.sopt_load(SOPT_WEATHERURL, host);

  strcat(ether_buffer, " HTTP/1.0\r\nHOST: ");
  strcat(ether_buffer, host);
  strcat(ether_buffer, "\r\nUser-Agent: ");
	strcat(ether_buffer, user_agent_string);
	strcat(ether_buffer, "\r\n\r\n");

  DEBUG_PRINTLN(F("GetSensorWeather"));
  DEBUG_PRINTLN(ether_buffer);

  last_weather_time = time;
  int ret = os.send_http_request(host, ether_buffer);
  if (ret == HTTP_RQT_SUCCESS) {
    DEBUG_PRINTLN(ether_buffer);

    char buf[20];
    char *s = strstr(ether_buffer, "\"temp\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_temp = atof(buf);
    }
    s = strstr(ether_buffer, "\"humidity\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_humidity = atof(buf);
    }
    s = strstr(ether_buffer, "\"precip\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_precip = atof(buf);
    }
    s = strstr(ether_buffer, "\"wind\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_wind = atof(buf);
    }
    char tmp[10];
    DEBUG_PRINT(F("temp: "));
    dtostrf(current_temp, 2, 2, tmp);
    DEBUG_PRINTLN(tmp)
    DEBUG_PRINT(F("humidity: "));
    dtostrf(current_humidity, 2, 2, tmp);
    DEBUG_PRINTLN(tmp)
    DEBUG_PRINT(F("precip: "));
    dtostrf(current_precip, 2, 2, tmp);
    DEBUG_PRINTLN(tmp)
    DEBUG_PRINT(F("wind: "));
    dtostrf(current_wind, 2, 2, tmp);
    DEBUG_PRINTLN(tmp)
    current_weather_ok = true;
  } else {
    current_weather_ok = false;
  }
}

void GetSensorWeatherEto() {
#if defined(ESP8266) || defined(ESP32)
  if (!useEth)
    if (os.state != OS_STATE_CONNECTED || WiFi.status() != WL_CONNECTED) return;
#endif
  time_t time = os.now_tz();
  if (last_weather_time_eto == 0) last_weather_time_eto = time - 59 * 60;

  if (time < last_weather_time_eto + 60 * 60) return;

  // use temp buffer to construct get command
  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);
  bf.emit_p(PSTR("$D?loc=$O&wto=$O&fwv=$D"),
								WEATHER_METHOD_ETO,
								SOPT_LOCATION,
								SOPT_WEATHER_OPTS,
								(int)os.iopts[IOPT_FW_VERSION]);
  
  urlEncode(tmp_buffer);

  strcpy(ether_buffer, "GET /");
  strcat(ether_buffer, tmp_buffer);
  // because dst is part of tmp_buffer,
  // must load weather url AFTER dst is copied to ether_buffer

  // load weather url to tmp_buffer
  char *host = tmp_buffer;
  os.sopt_load(SOPT_WEATHERURL, host);

  strcat(ether_buffer, " HTTP/1.0\r\nHOST: ");
  strcat(ether_buffer, host);
  strcat(ether_buffer, "\r\nUser-Agent: ");
	strcat(ether_buffer, user_agent_string);
	strcat(ether_buffer, "\r\n\r\n");

  DEBUG_PRINTLN(F("GetSensorWeather"));
  DEBUG_PRINTLN(ether_buffer);

  last_weather_time_eto = time;
  int ret = os.send_http_request(host, ether_buffer);
  if (ret == HTTP_RQT_SUCCESS) {
    DEBUG_PRINTLN(ether_buffer);

    char buf[20];
    char *s = strstr(ether_buffer, "\"eto\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_eto = atof(buf) * 25.4;  // convert to mm
    }
    s = strstr(ether_buffer, "\"radiation\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_radiation = atof(buf);
    }
    current_weather_eto_ok = true;
  } else {
    current_weather_eto_ok = false;
  }
}


int WeatherSensor::read(unsigned long time) {
  if (!this->flags.enable) return HTTP_RQT_NOT_RECEIVED;

  // Handle basic weather sensors
  if (this->type >= SENSOR_WEATHER_TEMP_F && this->type <= SENSOR_WEATHER_WIND_KMH) {
    GetSensorWeather();
    if (!current_weather_ok) return HTTP_RQT_NOT_RECEIVED;

    DEBUG_PRINT(F("Reading sensor "));
    DEBUG_PRINTLN(this->name);

    this->last_read = time;
    this->last_native_data = 0;
    this->flags.data_ok = true;

    switch (this->type) {
      case SENSOR_WEATHER_TEMP_F:
        this->last_data = current_temp;
        break;
      case SENSOR_WEATHER_TEMP_C:
        this->last_data = (current_temp - 32.0) / 1.8;
        break;
      case SENSOR_WEATHER_HUM:
        this->last_data = current_humidity;
        break;
      case SENSOR_WEATHER_PRECIP_IN:
        this->last_data = current_precip;
        break;
      case SENSOR_WEATHER_PRECIP_MM:
        this->last_data = current_precip * 25.4;
        break;
      case SENSOR_WEATHER_WIND_MPH:
        this->last_data = current_wind;
        break;
      case SENSOR_WEATHER_WIND_KMH:
        this->last_data = current_wind * 1.609344;
        break;
      default:
        return HTTP_RQT_NOT_RECEIVED;
    }
    return HTTP_RQT_SUCCESS;
  }

  // Handle ETO and radiation sensors
  if (this->type == SENSOR_WEATHER_ETO || this->type == SENSOR_WEATHER_RADIATION) {
    GetSensorWeatherEto();
    if (!current_weather_eto_ok) return HTTP_RQT_NOT_RECEIVED;

    DEBUG_PRINT(F("Reading sensor "));
    DEBUG_PRINTLN(this->name);

    this->last_read = time;
    this->last_native_data = 0;
    this->flags.data_ok = true;

    switch (this->type) {
      case SENSOR_WEATHER_ETO:
        this->last_data = current_eto;
        break;
      case SENSOR_WEATHER_RADIATION:
        this->last_data = current_radiation;
        break;
      default:
        return HTTP_RQT_NOT_RECEIVED;
    }
    return HTTP_RQT_SUCCESS;
  }

  return HTTP_RQT_NOT_RECEIVED;
}
