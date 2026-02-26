/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * ZigBee ESP-IDF Wrapper Functions
 * 
 * This file contains wrapper functions for ESP-IDF ZigBee API calls that
 * are not yet abstracted by the Arduino Zigbee Library (as of 2026).
 * 
 * PURPOSE:
 * - Centralize ESP-IDF direct calls for easier future migration
 * - Document WHY each call is necessary
 * - Provide consistent locking patterns
 * - Enable easy replacement when Arduino API adds these features
 * 
 * BACKGROUND:
 * The Arduino Zigbee Library (framework-arduinoespressif32/libraries/Zigbee)
 * is designed primarily for End Device sensors (sending data) and basic
 * Coordinators. OpenSprinkler needs Client Mode features (reading from
 * remote sensors) that require direct ESP-IDF/ZBOSS access.
 * 
 * 2026 @ OpenSprinklerShop
 */

#ifndef _ZIGBEE_ESPIDF_WRAPPERS_H
#define _ZIGBEE_ESPIDF_WRAPPERS_H

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include <stdint.h>
#include <Arduino.h>
#include "esp_zigbee_core.h"

/**
 * @brief ZBOSS Lock Wrapper - Acquire exclusive access to ZigBee stack
 * 
 * NEEDED BECAUSE: ZBOSS (ZigBee stack) is not thread-safe. All operations
 * that access internal tables or send commands MUST be protected by this lock.
 * 
 * ARDUINO STATUS: Arduino ZigbeeCore does NOT expose lock/unlock methods (2026)
 * 
 * @param timeout_ticks FreeRTOS timeout (typically portMAX_DELAY for blocking)
 * @return true if lock acquired
 */
inline bool zigbee_lock_acquire(uint32_t timeout_ticks = portMAX_DELAY) {
    esp_zb_lock_acquire(timeout_ticks);
    return true;
}

/**
 * @brief ZBOSS Lock Wrapper - Release exclusive access to ZigBee stack
 * 
 * Must be called after zigbee_lock_acquire() to allow other tasks/threads
 * to access ZigBee stack.
 */
inline void zigbee_lock_release() {
    esp_zb_lock_release();
}

/**
 * @brief Convert IEEE address (64-bit) to Short Network Address (16-bit)
 * 
 * NEEDED BECAUSE: Remote devices in a ZigBee network have both:
 * - IEEE address (64-bit, globally unique, stored in sensor config)
 * - Short address (16-bit, network-local, assigned by coordinator)
 * To send unicast commands, we need the short address, which is looked up
 * from the ZigBee network address table maintained by ZBOSS.
 * 
 * ARDUINO STATUS: Arduino ZigbeeEP has NO address resolution API for remote
 * devices (only for self). readManufacturer/readModel internally use short
 * addresses passed by user.
 * 
 * @param device_ieee IEEE address as uint64_t (big-endian presentation format)
 * @param[out] short_addr Resolved short address (0xFFFF = not found)
 * @return true if device found in address table
 * 
 * MUTEXING: Caller MUST hold zigbee_lock_acquire() before calling!
 */
inline bool zigbee_ieee_to_short_addr(uint64_t device_ieee, uint16_t* short_addr) {
    if (!short_addr) return false;
    
    // Convert uint64_t to esp_zb_ieee_addr_t (uint8_t[8], little-endian)
    esp_zb_ieee_addr_t ieee_le;
    for (int i = 0; i < 8; i++) {
        ieee_le[i] = (uint8_t)(device_ieee >> (i * 8));
    }
    
    // Query ZBOSS address table
    *short_addr = esp_zb_address_short_by_ieee(ieee_le);
    
    // 0xFFFF = device not in table, 0xFFFE = invalid/not joined
    return (*short_addr != 0xFFFF && *short_addr != 0xFFFE);
}

/**
 * @brief RAII-style ZBOSS lock guard
 * 
 * Usage:
 *   {
 *     ZigbeeLockGuard lock;
 *     // ... ZBOSS operations ...
 *   } // auto-release on scope exit
 */
class ZigbeeLockGuard {
public:
    ZigbeeLockGuard() { zigbee_lock_acquire(); }
    ~ZigbeeLockGuard() { zigbee_lock_release(); }
    
    // Non-copyable
    ZigbeeLockGuard(const ZigbeeLockGuard&) = delete;
    ZigbeeLockGuard& operator=(const ZigbeeLockGuard&) = delete;
};

/**
 * @brief Send ZCL Read Attributes command to remote device
 * 
 * NEEDED BECAUSE: Arduino Zigbee Library only supports reading Basic Cluster
 * attributes (Manufacturer, Model) via readManufacturer/readModel. We need
 * to read arbitrary clusters (Temperature, Humidity, Soil Moisture, etc.)
 * from remote sensors.
 * 
 * ARDUINO STATUS: NO generalized attribute reading API (2026)
 * 
 * @param device_ieee IEEE address of target device
 * @param short_addr Short address of target (from zigbee_ieee_to_short_addr)
 * @param endpoint Target endpoint number
 * @param cluster_id ZCL Cluster ID (e.g. 0x0402 = Temperature Measurement)
 * @param attribute_id Attribute ID within cluster
 * @param[out] tsn Transaction Sequence Number (for matching response)
 * @return true if command sent successfully
 * 
 * MUTEXING: Caller MUST hold zigbee_lock_acquire() before calling!
 */
bool zigbee_read_remote_attribute(
    uint64_t device_ieee,
    uint16_t short_addr,
    uint8_t endpoint,
    uint16_t cluster_id,
    uint16_t attribute_id,
    uint8_t* tsn = nullptr
);

// Future wrappers can be added here as needed

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
#endif // _ZIGBEE_ESPIDF_WRAPPERS_H
