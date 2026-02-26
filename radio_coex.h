/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Dynamic WiFi / Zigbee / BLE Radio Coexistence Manager
 *
 * The ESP32-C5 shares a single 2.4 GHz radio between WiFi, BLE and
 * IEEE 802.15.4 (Zigbee/Matter).  The hardware coexistence arbiter
 * uses a Priority Table Index (PTI) to decide who gets the radio.
 *
 * Priority scheme (single-threaded):
 *   - No ZigBee sensors configured    → ZigBee completely disabled (WiFi only)
 *   - Ethernet / WiFi off / AP mode   → ZigBee permanent HIGH (100%)
 *   - WiFi 5GHz + ZigBee sensors      → ZigBee-favored (txrx=MIDDLE, timed=HIGH,
 *                                        PREFER_BT — no RF overlap with WiFi)
 *   - WiFi 2.4GHz + ZigBee sensors    → WiFi priority, ZigBee via active-poll
 *                                        locks only (polling mode)
 *   - ZigBee sensor join (pairing)    → WiFi disconnected for max 10s,
 *                                        ZigBee gets 100%, then WiFi restored
 *   - Predictive boost                → ZigBee HIGH ±1s before expected report
 *
 * Key design rules:
 *   1. No sensors → never grant ZigBee locks
 *   2. Detect WiFi band (5GHz vs 2.4GHz) from channel info
 *   3. Count active ZigBee sensors to adjust coex strategy
 *   4. On 5GHz: esp_coex_preference_set(ESP_COEX_PREFER_BT), txrx_at=HIGH
 *   5. Zigbee/BLE can request locks (10s sensor, 30s join)
 *   6. Lock expiry triggers automatic strategy re-evaluation
 *
 * 2026 @ OpenSprinklerShop
 */

#ifndef _RADIO_COEX_H
#define _RADIO_COEX_H

#include <cstdint>

#if defined(ESP32C5)

/// Lock duration limits (milliseconds)
static const uint32_t COEX_LOCK_MAX_SENSOR_MS = 10000;  // max 10s for sensor read/write
static const uint32_t COEX_LOCK_MAX_JOIN_MS   = 30000;  // max 30s for join/pair
static const uint32_t COEX_WIFI_INFO_UPDATE_INTERVAL = 5000;  // Check WiFi band every 5s

/// WiFi band detection
enum coex_wifi_band_t : uint8_t {
    COEX_WIFI_UNKNOWN = 0,
    COEX_WIFI_2GHZ    = 1,
    COEX_WIFI_5GHZ    = 2,
    COEX_WIFI_OFF     = 3,
};

/// Coexistence strategies
enum coex_strategy_t : uint8_t {
    COEX_STRATEGY_ZIGBEE_HIGH  = 0,  // ZigBee permanent HIGH (WiFi off/AP/Ethernet)
    COEX_STRATEGY_WIFI_PRIO    = 1,  // WiFi priority, ZigBee via active-poll locks only (2.4GHz)
    COEX_STRATEGY_BALANCED_50  = 2,  // 50/50 scheduler (5GHz — no physical RF overlap)
    COEX_STRATEGY_WIFI_ONLY    = 3,  // No ZigBee sensors: WiFi only, no ZigBee locks granted
    COEX_STRATEGY_ZIGBEE_JOIN  = 4,  // ZigBee join/pairing mode: txrx=HIGH+timed=HIGH, full duration
};

/// Who is requesting the radio lock
enum coex_owner_t : uint8_t {
    COEX_OWNER_NONE   = 0,
    COEX_OWNER_ZIGBEE = 1,
    COEX_OWNER_BLE    = 2,
};

/// Initialise the coex manager (call once after radio stack init)
void coex_init();

/// Periodic update — call from main sensor loop (handles lock expiry & WiFi band detection)
void coex_loop();

/// Notify WiFi connection state change (call when WiFi connects/disconnects/changes AP).
/// This updates internal WiFi band detection (2.4GHz vs 5GHz) and re-evaluates strategy.
void coex_update_wifi_info();

/// Request exclusive radio lock for Zigbee/BLE sensor operations.
/// Returns true if the lock was acquired. timeout_ms is clamped to the
/// owner-appropriate maximum (10s sensor, 30s join).
bool coex_request_lock(coex_owner_t owner, uint32_t timeout_ms);

/// Release a previously acquired radio lock. Only the current owner
/// (or COEX_OWNER_NONE to force-release) can release it.
void coex_release_lock(coex_owner_t owner);

/// Notify that Ethernet mode is active (WiFi not needed).
/// Once set, Zigbee/BLE get permanent HIGH priority without needing locks.
void coex_set_ethernet_mode(bool enabled);

/// Mark WiFi as actively serving an HTTP request / MQTT / OTA
/// (informational — extends no-lock WiFi window awareness for debug)
void coex_notify_wifi_active();

/// Temporarily disconnect WiFi and give ZigBee 100% for up to duration_ms
/// (max 10000 ms). WiFi is automatically reconnected afterwards.
/// Call this just before opening the ZigBee join window so new devices can pair
/// without WiFi interference on the shared 2.4 GHz radio.
void coex_request_wifi_pause(uint32_t duration_ms);

/// Check if BLE background scan is currently allowed
bool coex_allow_ble_scan();

/// Enter or exit Zigbee join mode.
/// In join mode the coex manager applies its highest radio priority (txrx=HIGH+timed=HIGH)
/// for the specified duration regardless of which WiFi band is in use.
/// On 2.4GHz WiFi is also disconnected (same as coex_request_wifi_pause) to eliminate
/// RF contention for the full join window.
/// When duration_ms == 0 the mode must be exited explicitly with active=false.
void coex_set_join_mode(bool active, uint32_t duration_ms = 0);

/// Returns true while a ZigBee join/pairing window is active.
bool coex_is_join_mode();

/// Check if Zigbee active operations (read/bind) are allowed
bool coex_allow_zigbee_active();

/// Check if Zigbee currently holds the radio lock
bool coex_is_zigbee_locked();

/// Get current WiFi band detection (2.4GHz, 5GHz, or OFF)
coex_wifi_band_t coex_get_wifi_band();

/// Get current coex strategy
coex_strategy_t coex_get_strategy();

/// Human-readable status for debug output
const char* coex_get_status();

#else
// Non-ESP32C5 stubs
inline void coex_init() {}
inline void coex_loop() {}
inline void coex_update_wifi_info() {}
inline bool coex_request_lock(uint8_t, uint32_t) { return true; }
inline void coex_release_lock(uint8_t) {}
inline void coex_set_ethernet_mode(bool) {}
inline void coex_notify_wifi_active() {}
inline void coex_request_wifi_pause(uint32_t) {}
inline bool coex_allow_ble_scan() { return true; }
inline bool coex_allow_zigbee_active() { return true; }
inline bool coex_is_zigbee_locked() { return false; }
inline coex_wifi_band_t coex_get_wifi_band() { return COEX_WIFI_UNKNOWN; }
inline coex_strategy_t coex_get_strategy() { return COEX_STRATEGY_WIFI_PRIO; }
inline const char* coex_get_status() { return "n/a"; }
#endif

#endif // _RADIO_COEX_H
