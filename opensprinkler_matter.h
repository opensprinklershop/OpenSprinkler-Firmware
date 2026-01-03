/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Matter (CHIP) protocol support for smart home integration
 *
 * Jan 2026 @ OpenSprinkler.com
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

#ifndef _OPENSPRINKLER_MATTER_H
#define _OPENSPRINKLER_MATTER_H

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <cstdint>
#endif

#ifdef ENABLE_MATTER

#include "defines.h"

// Matter event types (from Arduino ESP32 SDK)
#define MATTER_EVENT_COMMISSIONED                 0x01
#define MATTER_EVENT_DECOMMISSIONED               0x02
#define MATTER_EVENT_FABRIC_ADDED                 0x03
#define MATTER_EVENT_FABRIC_REMOVED               0x04
#define MATTER_EVENT_WIFI_CONNECTIVITY_CHANGE     0x10
#define MATTER_EVENT_THREAD_CONNECTIVITY_CHANGE   0x11

// Matter device types
#define MATTER_DEVICE_TYPE_AGGREGATOR  0x000E
#define MATTER_DEVICE_TYPE_VALVE       0x0042
#define MATTER_DEVICE_TYPE_FLOW_SENSOR 0x0306

// Matter initialization and management
void matter_init();
void matter_loop();
void matter_shutdown();

// Station control callbacks
void matter_station_on(unsigned char sid);
void matter_station_off(unsigned char sid);
void matter_update_station_status(unsigned char sid, bool on);

// Sensor integration
void matter_update_flow_rate(float gpm);
void matter_update_sensor_value(unsigned char sensor_id, float value);

// Status and diagnostics
bool matter_is_commissioned();
uint8_t matter_get_fabric_count();
void matter_factory_reset();

#else
// Empty stubs when Matter is disabled
inline void matter_init() {}
inline void matter_loop() {}
inline void matter_shutdown() {}
inline void matter_station_on(unsigned char sid) { (void)sid; }
inline void matter_station_off(unsigned char sid) { (void)sid; }
inline void matter_update_station_status(unsigned char sid, bool on) { (void)sid; (void)on; }
inline void matter_update_flow_rate(float gpm) { (void)gpm; }
inline void matter_update_sensor_value(unsigned char sensor_id, float value) { (void)sensor_id; (void)value; }
inline bool matter_is_commissioned() { return false; }
inline uint8_t matter_get_fabric_count() { return 0; }
inline void matter_factory_reset() {}
#endif // ENABLE_MATTER

#endif // _OPENSPRINKLER_MATTER_H
