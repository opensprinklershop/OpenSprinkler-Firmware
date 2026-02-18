#ifndef _RADIO_ARBITER_H
#define _RADIO_ARBITER_H

#include <stdint.h>

/**
 * @brief Radio owner — who currently holds exclusive scan/init priority.
 *
 * The owner gets elevated radio priority (PTI HIGH for Zigbee, etc.).
 * When RADIO_OWNER_NONE, WiFi gets default highest priority.
 */
enum RadioOwner : uint8_t {
    RADIO_OWNER_NONE        = 0,  ///< No owner — WiFi default priority
    RADIO_OWNER_WIFI        = 1,  ///< WiFi scan / reconnect
    RADIO_OWNER_BLE_SCAN    = 2,  ///< BLE discovery scan (user-triggered)
    RADIO_OWNER_ZIGBEE_SCAN = 3,  ///< Zigbee permit-join window
    RADIO_OWNER_MATTER      = 4,  ///< Matter commissioning / init
};

/**
 * @brief Acquire exclusive radio ownership.
 * @param owner The requesting owner
 * @param duration_ms How long the ownership lasts (0 = indefinite until release)
 * @return true if acquired, false if another owner holds the lock
 */
bool radio_arbiter_acquire(RadioOwner owner, uint32_t duration_ms = 0);

/**
 * @brief Release radio ownership.
 * @param owner Must match current owner (safety check)
 */
void radio_arbiter_release(RadioOwner owner);

/**
 * @brief Get current radio owner.
 */
RadioOwner radio_arbiter_get_owner();

/**
 * @brief Check if a specific owner currently holds the lock.
 */
bool radio_arbiter_is_owner(RadioOwner owner);

/**
 * @brief Mark current time as web activity.
 */
void radio_arbiter_mark_web_activity();

/**
 * @brief Returns true while web/HTTP priority window is active.
 */
bool radio_arbiter_is_web_priority_active();

/**
 * @brief Returns true if BLE scans are currently allowed.
 */
bool radio_arbiter_allow_ble_scan();

/**
 * @brief Returns true if Zigbee active operations are currently allowed.
 */
bool radio_arbiter_allow_zigbee_active_ops();

/**
 * @brief Print current radio arbiter state (debug).
 */
void radio_arbiter_debug_state();

/**
 * @brief Apply WiFi/802.15.4 coexistence base settings once.
 */
void radio_arbiter_apply_balanced_coex_once();

/**
 * @brief Force WiFi reconnection (call after Zigbee scan ends).
 * Checks if WiFi is disconnected and triggers reconnect.
 */
void radio_arbiter_ensure_wifi();

#endif // _RADIO_ARBITER_H
