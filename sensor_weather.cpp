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
#include "utils.h"

// Weather
static time_t last_weather_time = 0;
static time_t last_weather_time_eto = 0;
static time_t last_weather_data_time = 0;
static time_t last_weather_data_time_eto = 0;
static bool current_weather_ok = false;
static bool current_weather_eto_ok = false;
static double current_temp = 0.0;
static double current_humidity = 0.0;
static double current_precip = 0.0;
static double current_wind = 0.0;
static double current_eto = 0.0;
static double current_radiation = 0.0;

// Set by the callbacks when a valid weather response with usable data was parsed.
// The weather service can return HTTP 200 with an error body (e.g. "&errCode=50&scale=100")
// that contains no sensor data; without this flag such a response would be treated as a
// success, freezing the last value and delaying the next retry by a full hour.
static bool weather_response_valid = false;
static bool weather_eto_response_valid = false;

#define WEATHER_FETCH_INTERVAL_S   (60 * 60)   // refresh weather data hourly
#define WEATHER_FETCH_RETRY_S      (5 * 60)    // retry after a failed fetch
#define WEATHER_FETCH_PENDING_MAX_S 40         // ESP32: async request without callback counts as failed

#if defined(ESP32)
// On the ESP32 the fetch runs in a background task (send_http_request_async); the
// response callback finishes the state update. Doing it synchronously from the
// sensor loop stalled the whole main loop (HTTP server, station scheduling) for the
// complete DNS/connect timeout sequence whenever the weather host did not resolve.
static volatile bool weather_pending = false;
static volatile bool weather_eto_pending = false;
static time_t weather_pending_since = 0;
static time_t weather_eto_pending_since = 0;
#endif

bool weather_sensor_should_refresh_now(uint type, ulong sensor_last_read) {
  if (type >= SENSOR_WEATHER_TEMP_F && type <= SENSOR_WEATHER_WIND_KMH) {
    return current_weather_ok && last_weather_data_time > 0 && sensor_last_read < (ulong)last_weather_data_time;
  }
  if (type == SENSOR_WEATHER_ETO || type == SENSOR_WEATHER_RADIATION) {
    return current_weather_eto_ok && last_weather_data_time_eto > 0 && sensor_last_read < (ulong)last_weather_data_time_eto;
  }
  return false;
}

static int parse_weather_errcode(const char *buffer) {
  const char *e = strstr(buffer, "errCode");
  if (!e) return 0;
  e = strchr(e, '=');
  return e ? atoi(e + 1) : 0;
}

// Bookkeeping after a fetch attempt: on success publish the data timestamp (this is
// what weather_sensor_should_refresh_now() keys on); on failure schedule a retry
// without invalidating previously fetched values.
static void weather_fetch_done(bool valid, time_t &last_time, time_t &last_data_time, bool &ok) {
  time_t now = os.now_tz();
  if (valid) {
    ok = true;
    last_data_time = now;
    last_time = now;
  } else {
    last_time = now - (WEATHER_FETCH_INTERVAL_S - WEATHER_FETCH_RETRY_S);
    DEBUG_PRINTLN(F("Weather: no valid weather data received"));
  }
}

static void sensor_weather_callback(char *buffer) {
  peel_http_header(buffer);
  weather_response_valid = false;
  if (parse_weather_errcode(buffer) != 0) {
    DEBUG_PRINTLN(F("Weather: service returned errCode, keeping previous data"));
  } else {
    char buf[20];
    char *s = strstr(buffer, "\"temp\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_temp = atof(buf);
      weather_response_valid = true;
    }
    s = strstr(buffer, "\"humidity\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_humidity = atof(buf);
    }
    s = strstr(buffer, "\"precip\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_precip = atof(buf);
    }
    s = strstr(buffer, "\"wind\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_wind = atof(buf);
    }
    DEBUG_PRINTF("Weather: temp=%.1f hum=%.1f precip=%.2f wind=%.1f\n",
                 current_temp, current_humidity, current_precip, current_wind);
  }
#if defined(ESP32)
  weather_fetch_done(weather_response_valid, last_weather_time, last_weather_data_time, current_weather_ok);
  weather_pending = false;
#endif
}

static void sensor_weather_eto_callback(char *buffer) {
  peel_http_header(buffer);
  weather_eto_response_valid = false;
  if (parse_weather_errcode(buffer) != 0) {
    DEBUG_PRINTLN(F("WeatherEto: service returned errCode, keeping previous data"));
  } else {
    char buf[20];
    char *s = strstr(buffer, "\"eto\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_eto = atof(buf) * 25.4;  // convert to mm
      weather_eto_response_valid = true;
    }
    s = strstr(buffer, "\"radiation\":");
    if (s && RemoteSensor::extract(s, buf, sizeof(buf))) {
      current_radiation = atof(buf);
    }
    DEBUG_PRINTF("WeatherEto: eto=%.2f radiation=%.1f\n", current_eto, current_radiation);
  }
#if defined(ESP32)
  weather_fetch_done(weather_eto_response_valid, last_weather_time_eto, last_weather_data_time_eto, current_weather_eto_ok);
  weather_eto_pending = false;
#endif
}

// Sends "GET /<tmp_buffer> HTTP/1.0" to the configured weather server (SOPT_WEATHERURL,
// "http://"/"https://" prefix and ":port" are honoured; default plain HTTP on port 80,
// the weather service serves weatherData on both). tmp_buffer must hold the
// url-encoded path+query on entry; it is reused for the host name.
// ESP32: queued to a background task, result arrives via callback (HTTP_RQT_SUCCESS
// here only means "queued"). Other platforms: synchronous.
static int weather_fetch(void (*callback)(char*)) {
  strcpy(ether_buffer, "GET /");
  strcat(ether_buffer, tmp_buffer);

  char *host = tmp_buffer;
  os.sopt_load(SOPT_WEATHERURL, host);
  if (!host[0]) {
    DEBUG_PRINTLN(F("Weather: no weather server URL configured"));
    return HTTP_RQT_CONNECT_ERR;
  }

  bool use_ssl = false;
  uint16_t port = 80;
  if (strncmp_P(host, PSTR("http://"), 7) == 0) {
    host += 7;
  } else if (strncmp_P(host, PSTR("https://"), 8) == 0) {
    host += 8;
    use_ssl = true;
    port = 443;
  }
  char *slash = strchr(host, '/');
  if (slash) *slash = 0;
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = 0;
    port = atoi(colon + 1);
  }
#if defined(ESP8266)
  // No heap budget for a BearSSL session on the sensor path; weatherData is also served over HTTP.
  if (use_ssl) {
    use_ssl = false;
    if (port == 443) port = 80;
  }
#endif

  strcat(ether_buffer, " HTTP/1.0\r\nHOST: ");
  strcat(ether_buffer, host);
  strcat(ether_buffer, "\r\nUser-Agent: ");
  strcat(ether_buffer, user_agent_string);
  strcat(ether_buffer, "\r\n\r\n");

#if defined(ESP32)
  // Weather server forwards to an upstream provider; cold cache can exceed 12 s.
  return os.send_http_request_async(host, port, ether_buffer, callback, use_ssl, 20000);
#else
  return os.send_http_request(host, port, ether_buffer, callback, use_ssl);
#endif
}

#if defined(ESP32)
// Returns true while an async fetch is still in flight. A request whose task never
// invoked the callback (DNS/connect/timeout failure, "http request busy") is given up
// after WEATHER_FETCH_PENDING_MAX_S and scheduled for a retry.
static bool weather_fetch_pending(volatile bool &pending, time_t &pending_since, time_t now,
                                  time_t &last_time) {
  if (!pending) return false;
  if (now - pending_since < WEATHER_FETCH_PENDING_MAX_S) return true;
  pending = false;
  last_time = now - (WEATHER_FETCH_INTERVAL_S - WEATHER_FETCH_RETRY_S);
  DEBUG_PRINTLN(F("Weather: async fetch did not complete"));
  return false;
}
#endif

void GetSensorWeather() {
#if defined(ESP8266) || defined(ESP32)
  if (!useEth)
    if (os.state != OS_STATE_CONNECTED || WiFi.status() != WL_CONNECTED) return;
#endif
  time_t time = os.now_tz();
#if defined(ESP32)
  if (weather_fetch_pending(weather_pending, weather_pending_since, time, last_weather_time)) return;
#endif
  if (last_weather_time == 0) last_weather_time = time - WEATHER_FETCH_INTERVAL_S;
  if (time < last_weather_time + WEATHER_FETCH_INTERVAL_S) return;

  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);
  bf.emit_p(PSTR("weatherData?loc=$O&wto=$O&fwv=$D"), SOPT_LOCATION,
            SOPT_WEATHER_OPTS,
            (int)os.iopts[IOPT_FW_VERSION]);
  urlEncode(tmp_buffer);

#if defined(ESP32)
  weather_pending = true;
  weather_pending_since = time;
  if (weather_fetch(sensor_weather_callback) != HTTP_RQT_SUCCESS) {
    weather_pending = false;
    weather_fetch_done(false, last_weather_time, last_weather_data_time, current_weather_ok);
  }
#else
  int ret = weather_fetch(sensor_weather_callback);
  weather_fetch_done(ret == HTTP_RQT_SUCCESS && weather_response_valid,
                     last_weather_time, last_weather_data_time, current_weather_ok);
#endif
}

void GetSensorWeatherEto() {
#if defined(ESP8266) || defined(ESP32)
  if (!useEth)
    if (os.state != OS_STATE_CONNECTED || WiFi.status() != WL_CONNECTED) return;
#endif
  time_t time = os.now_tz();
#if defined(ESP32)
  if (weather_fetch_pending(weather_eto_pending, weather_eto_pending_since, time, last_weather_time_eto)) return;
#endif
  if (last_weather_time_eto == 0) last_weather_time_eto = time - WEATHER_FETCH_INTERVAL_S;
  if (time < last_weather_time_eto + WEATHER_FETCH_INTERVAL_S) return;

  BufferFiller bf = BufferFiller(tmp_buffer, TMP_BUFFER_SIZE);
  bf.emit_p(PSTR("$D?loc=$O&wto=$O&fwv=$D"),
            WEATHER_METHOD_ETO,
            SOPT_LOCATION,
            SOPT_WEATHER_OPTS,
            (int)os.iopts[IOPT_FW_VERSION]);
  urlEncode(tmp_buffer);

#if defined(ESP32)
  weather_eto_pending = true;
  weather_eto_pending_since = time;
  if (weather_fetch(sensor_weather_eto_callback) != HTTP_RQT_SUCCESS) {
    weather_eto_pending = false;
    weather_fetch_done(false, last_weather_time_eto, last_weather_data_time_eto, current_weather_eto_ok);
  }
#else
  int ret = weather_fetch(sensor_weather_eto_callback);
  weather_fetch_done(ret == HTTP_RQT_SUCCESS && weather_eto_response_valid,
                     last_weather_time_eto, last_weather_data_time_eto, current_weather_eto_ok);
#endif
}


int WeatherSensor::read(unsigned long time) {
  if (!this->flags.enable) return HTTP_RQT_NOT_RECEIVED;

  // Handle basic weather sensors
  if (this->type >= SENSOR_WEATHER_TEMP_F && this->type <= SENSOR_WEATHER_WIND_KMH) {
    GetSensorWeather();
    if (!current_weather_ok) {
      this->flags.data_ok = false;
      return HTTP_RQT_NOT_RECEIVED;
    }

    // DEBUG_PRINT(F("Reading sensor "));
    // DEBUG_PRINTLN(this->name);

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
        this->flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    return HTTP_RQT_SUCCESS;
  }

  // Handle ETO and radiation sensors
  if (this->type == SENSOR_WEATHER_ETO || this->type == SENSOR_WEATHER_RADIATION) {
    GetSensorWeatherEto();
    if (!current_weather_eto_ok) {
      this->flags.data_ok = false;
      return HTTP_RQT_NOT_RECEIVED;
    }

    // DEBUG_PRINT(F("Reading sensor "));
    // DEBUG_PRINTLN(this->name);

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
        this->flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    return HTTP_RQT_SUCCESS;
  }

  this->flags.data_ok = false;
  return HTTP_RQT_NOT_RECEIVED;
}
