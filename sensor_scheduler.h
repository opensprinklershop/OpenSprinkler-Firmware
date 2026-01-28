/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Sensor Lazy-Loading by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Sensor Scheduler Header - Time-based lazy-loading sensor management
 * Jan 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _SENSOR_SCHEDULER_H
#define _SENSOR_SCHEDULER_H

#include "sensor_metadata.h"

// Forward declaration instead of full include to avoid circular dependency
class SensorBase;

// =====================================================
// Scheduler Configuration
// =====================================================
#define SCHEDULER_MIN_INTERVAL      10    // Minimum read interval (seconds)
#define SCHEDULER_MAX_BACKOFF       3600  // Maximum backoff on repeated failures (1 hour)
#define SCHEDULER_FAILURE_THRESHOLD 3     // Consecutive failures before backoff

// =====================================================
// Core Scheduler Functions
// =====================================================

/**
 * @brief Initialize sensor scheduler - loads only metadata from Flash
 * @param detect_boards If true, detect ASB/RS485 hardware boards
 * 
 * Replaces: sensor_api_init() for the new lazy-loading architecture
 * Memory impact: ~40-50 bytes per sensor instead of 200-300 bytes
 */
void sensor_scheduler_init(bool detect_boards);

/**
 * @brief Main scheduler loop - reads sensors whose time has come
 * 
 * Replaces: sensor_api_loop() + sensor_read_all() combination
 * Only processes sensors where next_read_time <= now
 * Sensor objects are instantiated, read, then immediately freed
 */
void sensor_scheduler_loop();

/**
 * @brief Reload metadata from Flash (after sensor config changes)
 */
void sensor_scheduler_reload();

/**
 * @brief Free all scheduler resources
 */
void sensor_scheduler_free();

// =====================================================
// Sensor Access Functions (Cached Values)
// =====================================================

/**
 * @brief Get cached sensor value (instant, no Flash access)
 * @param nr Sensor number
 * @return Cached value, or 0.0 if not found/invalid
 */
double sensor_get_cached_value(uint nr);

/**
 * @brief Get cached native (raw) sensor value
 * @param nr Sensor number
 * @return Cached native value, or 0 if not found
 */
uint32_t sensor_get_cached_native(uint nr);

/**
 * @brief Check if sensor has valid cached data
 * @param nr Sensor number
 * @return true if cached data is valid
 */
bool sensor_has_valid_data(uint nr);

/**
 * @brief Get sensor metadata (for iteration/display)
 * @param nr Sensor number
 * @return Pointer to metadata, or nullptr if not found
 */
const SensorMetadata* sensor_get_metadata(uint nr);

/**
 * @brief Get all sensor metadata for iteration
 * @return Reference to metadata map
 */
const SensorScheduleMap& sensor_get_all_metadata();

/**
 * @brief Get sensor count
 * @return Number of configured sensors
 */
size_t sensor_get_count();

// =====================================================
// On-Demand Sensor Loading
// =====================================================

/**
 * @brief Load a single sensor from Flash (on-demand)
 * @param nr Sensor number to load
 * @return Newly allocated SensorBase* (caller must delete!), or nullptr
 * 
 * IMPORTANT: Caller is responsible for deleting the returned object!
 * Allocates in PSRAM if available.
 */
SensorBase* sensor_load_single(uint nr);

/**
 * @brief Force immediate read of a sensor (e.g., from HTTP API)
 * @param nr Sensor number
 * @return HTTP_RQT_SUCCESS on success, HTTP_RQT_NOT_RECEIVED on error
 * 
 * Loads sensor from Flash, reads, updates cache, then frees sensor.
 */
int sensor_read_single_now(uint nr);

/**
 * @brief Schedule a sensor for immediate reading in next loop
 * @param nr Sensor number
 */
void sensor_schedule_immediate(uint nr);

// =====================================================
// Metadata Update Functions
// =====================================================

/**
 * @brief Update cached value after successful read
 * @param nr Sensor number
 * @param value New value
 * @param native_value Native/raw value
 * @param timestamp Read timestamp
 */
void sensor_update_cache(uint nr, double value, uint32_t native_value, uint32_t timestamp);

/**
 * @brief Mark sensor as having invalid data (read failed)
 * @param nr Sensor number
 */
void sensor_invalidate_cache(uint nr);

/**
 * @brief Update sensor flags in metadata cache
 * @param nr Sensor number
 * @param enable Enable flag
 * @param log Log flag
 * @param show Show flag
 */
void sensor_update_flags(uint nr, bool enable, bool log, bool show);

// =====================================================
// Compatibility Layer (for gradual migration)
// =====================================================

/**
 * @brief Legacy compatibility: Check if scheduler is initialized
 * Replaces: apiInit check
 */
bool sensor_scheduler_ready();

/**
 * @brief Legacy iterator - iterates through metadata
 * For backwards compatibility with code expecting sensor iteration
 */
SensorScheduleIterator sensor_metadata_begin();
SensorScheduleIterator sensor_metadata_end();

#endif // _SENSOR_SCHEDULER_H
