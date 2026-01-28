/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Sensor Lazy-Loading by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Sensor Metadata - Lightweight runtime cache for lazy-loading architecture
 * Jan 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _SENSOR_METADATA_H
#define _SENSOR_METADATA_H

#include <stdint.h>
#include <map>
#include <vector>

// Ensure 'uint' is defined for all platforms
// On Arduino/ESP, it's typically defined, but we need it for standalone inclusion
#ifndef uint
typedef unsigned int uint;
#endif

// Compact sensor flags for metadata cache (1 byte instead of full SensorFlags_t)
#define SENSOR_META_FLAG_ENABLE   0x01
#define SENSOR_META_FLAG_LOG      0x02
#define SENSOR_META_FLAG_SHOW     0x04
#define SENSOR_META_FLAG_DATA_OK  0x08

/**
 * @brief Minimal sensor metadata for lazy-loading architecture
 * 
 * Only 40-50 bytes per sensor instead of 200-300 bytes for full SensorBase object.
 * Full sensor configuration is loaded on-demand from Flash (sensors.json).
 */
struct SensorMetadata {
  uint nr;                        // Sensor-ID (1..n)
  uint type;                      // Sensor type (for factory instantiation)
  uint read_interval;             // Read interval in seconds
  uint32_t next_read_time;        // Next scheduled read time (os.now_tz())
  double cached_value;            // Last read value (for fast access)
  uint32_t cached_native_value;   // Last native/raw value
  uint32_t last_read_time;        // Timestamp of last successful read
  uint8_t flags_cache;            // Compact flags: enable/log/show/data_ok
  uint8_t consecutive_failures;   // Track read failures for backoff
  char name[20];                  // Short name for logging/debugging (truncated from 30)
  
  // Default constructor
  SensorMetadata() : 
    nr(0), type(0), read_interval(60), next_read_time(0),
    cached_value(0.0), cached_native_value(0), last_read_time(0),
    flags_cache(0), consecutive_failures(0) {
    name[0] = '\0';
  }
  
  // Helper methods
  bool isEnabled() const { return (flags_cache & SENSOR_META_FLAG_ENABLE) != 0; }
  bool shouldLog() const { return (flags_cache & SENSOR_META_FLAG_LOG) != 0; }
  bool shouldShow() const { return (flags_cache & SENSOR_META_FLAG_SHOW) != 0; }
  bool hasValidData() const { return (flags_cache & SENSOR_META_FLAG_DATA_OK) != 0; }
  
  void setDataValid(bool valid) {
    if (valid) flags_cache |= SENSOR_META_FLAG_DATA_OK;
    else flags_cache &= ~SENSOR_META_FLAG_DATA_OK;
  }
  
  void setEnabled(bool enabled) {
    if (enabled) flags_cache |= SENSOR_META_FLAG_ENABLE;
    else flags_cache &= ~SENSOR_META_FLAG_ENABLE;
  }
};

/**
 * @brief Minimal program-sensor link for lazy-loading
 * Full ProgSensorAdjust loaded on-demand when program starts
 */
struct ProgSensorLink {
  uint prog_nr;       // Program number
  uint sensor_nr;     // Linked sensor number
  uint8_t adjust_type; // Type of adjustment (for quick filtering)
};

/**
 * @brief Minimal monitor metadata
 * Full Monitor loaded on-demand when triggered
 */
struct MonitorMetadata {
  uint nr;             // Monitor ID
  uint sensor_nr;      // Monitored sensor
  uint check_interval; // Check interval in seconds
  uint32_t next_check_time;
  uint8_t flags_cache; // enable/active
};

// Type definitions for metadata maps
typedef std::map<uint, SensorMetadata> SensorScheduleMap;
typedef std::map<uint, SensorMetadata>::iterator SensorScheduleIterator;
typedef std::vector<ProgSensorLink> ProgSensorLinkList;
typedef std::map<uint, MonitorMetadata> MonitorScheduleMap;

#endif // _SENSOR_METADATA_H
