#ifndef _RADIO_ARBITER_H
#define _RADIO_ARBITER_H

#include <stdint.h>

/**
 * @brief Mark current time as web activity.
 *
 * Extends a short high-priority window where HTTP/HTTPS traffic should get
 * preferred radio/CPU access over background BLE scan and Zigbee active reads.
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
 *
 * Sets balanced coexistence preference and enables WiFi<->802.15.4
 * coexistence bridge on ESP32-C5 Zigbee builds. Subsequent calls are no-ops.
 */
void radio_arbiter_apply_balanced_coex_once();

#endif // _RADIO_ARBITER_H
