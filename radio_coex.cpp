/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Dynamic WiFi / Zigbee / BLE Radio Coexistence Manager
 *
 * Band-aware prioritization:
 *   1. No ZigBee sensors configured   → WiFi only (ZigBee never gets locks)
 *   2. WiFi off, AP mode, or Ethernet → ZigBee/BLE permanent HIGH (100%)
 *   3. WiFi 5GHz + ZigBee sensors     → ZigBee-favored (txrx=MIDDLE, timed=HIGH)
 *   4. WiFi 2.4GHz + ZigBee sensors   → WiFi priority, ZigBee only via active-poll locks
 *   5. ZigBee join/pairing window     → WiFi disconnected max 10s (ZigBee 100%)
 *   6. Predictive boost               → ZigBee HIGH ±1s before expected report
 *
 * Uses esp_coex_preference_set() for scheduler tuning and
 * esp_ieee802154_coex_config_t for PTI-based priority.
 *
 * 2026 @ OpenSprinklerShop
 */

#if defined(ESP32C5)

#define USE_IEEE802154_COEX

#include "radio_coex.h"

#if defined(USE_IEEE802154_COEX)

#include "defines.h"
#include <Arduino.h>
#include <WiFi.h>
#include "sensors.h"
#include "SensorBase.hpp"

#include <esp_ieee802154.h>
#include <esp_coex_i154.h>
extern "C" {
#include <esp_coexist.h>
}

// Forward declarations for sensor access
extern uint sensor_count();
extern SensorBase* sensor_by_idx(uint idx);

// =========================================================================
// State (single-threaded, no mutex needed)
// =========================================================================
static coex_owner_t        s_lock_owner        = COEX_OWNER_NONE;
static uint32_t            s_lock_until        = 0;           // millis() deadline
static bool                s_ethernet          = false;       // Ethernet mode (no WiFi)
static uint8_t             s_current_pti       = 0xFF;        // track current PTI
static bool                s_initialized       = false;
static coex_strategy_t     s_current_strategy  = COEX_STRATEGY_WIFI_PRIO;
static coex_wifi_band_t    s_wifi_band         = COEX_WIFI_OFF;
static uint32_t            s_last_wifi_check   = 0;           // Track WiFi info update time

// WiFi pause state (ZigBee pairing: disconnect WiFi → 100% ZigBee → reconnect)
static uint32_t            s_wifi_pause_until      = 0;    // millis() when pause ends (0 = inactive)
static bool                s_wifi_was_on           = false; // WiFi was connected before pause
static uint32_t            s_wifi_pause_start      = 0;    // delayed disconnect (500ms after request)
static uint32_t            s_wifi_pause_cooldown   = 0;    // millis() — no new pause before this (post-reconnect guard)

// Join mode state — dedicated first-class coex state for ZigBee join/pairing windows.
// Persists for the full join window duration independent of the rolling 30s lock cap.
// On 5GHz: applies txrx=HIGH+timed=HIGH (vs txrx=LOW in balanced_50 normal mode).
// On 2.4GHz: WiFi is also disconnected via coex_request_wifi_pause.
static bool                s_join_mode_active      = false; // join mode currently active
static uint32_t            s_join_mode_until       = 0;    // millis() deadline (0 = manual exit)

// =========================================================================
// Internal: Helper functions
// =========================================================================

/// Detect WiFi band from connected AP's channel (1-13 = 2.4GHz, 36+ = 5GHz)
static coex_wifi_band_t coex_detect_wifi_band() {
    if (s_ethernet) return COEX_WIFI_OFF;
    
    //wifi_mode_t wmode = WiFi.getMode();
    
    // STA mode: check connected channel
    if (WiFi.status() == WL_CONNECTED) {
        uint8_t channel = WiFi.channel();
        // 2.4 GHz: channels 1-13; 5 GHz: channels 36-165
        if (channel >= 1 && channel <= 13) {
            return COEX_WIFI_2GHZ;
        } else if (channel >= 36) {
            return COEX_WIFI_5GHZ;
        }
    }
    
    return COEX_WIFI_UNKNOWN;
}

/// Count ZigBee sensors from global sensorsMap
static uint32_t coex_count_zigbee_sensors() {
    #if defined(OS_ENABLE_ZIGBEE)
    uint32_t count = 0;
    uint total = sensor_count();
    for (uint idx = 0; idx < total; idx++) {
        SensorBase *sensor = sensor_by_idx(idx);
        if (sensor && sensor->type == SENSOR_ZIGBEE) {
            count++;
        }
    }
    return count;
    #else
    return 0;
    #endif
}

/// Determine the optimal coex strategy based on WiFi band and ZigBee sensor count
static coex_strategy_t coex_evaluate_strategy(coex_wifi_band_t band) {
    uint32_t zb_count = coex_count_zigbee_sensors();

    // No ZigBee sensors → ZigBee completely silent regardless of band
    if (zb_count == 0) return COEX_STRATEGY_WIFI_ONLY;

    // Ethernet or WiFi off → ZigBee/BLE permanent HIGH
    if (band == COEX_WIFI_OFF) return COEX_STRATEGY_ZIGBEE_HIGH;

    // WiFi 5GHz + ZigBee → 50:50 (different bands, no physical RF conflict)
    if (band == COEX_WIFI_5GHZ) return COEX_STRATEGY_BALANCED_50;

    // WiFi 2.4GHz + ZigBee → WiFi priority, ZigBee only during active-poll locks
    if (band == COEX_WIFI_2GHZ) return COEX_STRATEGY_WIFI_PRIO;

    // Unknown band + ZigBee → safe default (WiFi priority)
    return COEX_STRATEGY_WIFI_PRIO;
}

/// Apply coex priority based on strategy (skips if unchanged)
static void coex_apply(uint8_t idle_pti, uint8_t txrx_pti, uint8_t txrx_at_pti) {
    if (s_current_pti == txrx_pti) return;
    
    esp_ieee802154_coex_config_t cfg = {
        .idle    = (ieee802154_coex_event_t)idle_pti,
        .txrx    = (ieee802154_coex_event_t)txrx_pti,
        .txrx_at = (ieee802154_coex_event_t)txrx_at_pti,
    };
    esp_ieee802154_set_coex_config(cfg);
    s_current_pti = txrx_pti;
}

/// Set ZigBee/BLE permanent HIGH priority (no WiFi contention)
static inline void coex_set_zigbee_high() {
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    coex_apply(IEEE802154_IDLE, IEEE802154_MIDDLE, IEEE802154_HIGH);
    DEBUG_PRINTLN(F("[COEX] ZigBee/BLE permanent HIGH (offline/Ethernet/pairing)"));
}

/// Set WiFi priority with ZigBee LOW — 2.4GHz active-poll mode
static inline void coex_set_wifi_priority() {
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    coex_apply(IEEE802154_IDLE, IEEE802154_LOW, IEEE802154_LOW);
    DEBUG_PRINTLN(F("[COEX] WiFi priority / ZigBee active-poll only (2.4GHz)"));
}

/// Set ZigBee-favored mode for 5GHz — regular txrx balanced, timed slots (ACKs/wakeups) get HIGH priority.
/// On 5GHz there is no RF overlap with ZigBee, so we can afford to lean toward ZigBee
/// without impacting WiFi throughput meaningfully.
static inline void coex_set_balanced_50() {
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    coex_apply(IEEE802154_IDLE, IEEE802154_LOW, IEEE802154_HIGH);
    DEBUG_PRINTLN(F("[COEX] ZigBee-favored (5GHz): txrx=LOW, timed-slots=HIGH"));
}

/// Set WiFi only — ZigBee gets minimum/idle PTI, no locks granted
static inline void coex_set_wifi_only() {
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    coex_apply(IEEE802154_IDLE, IEEE802154_IDLE, IEEE802154_IDLE);
    DEBUG_PRINTLN(F("[COEX] WiFi only (no ZigBee sensors configured)"));
}

/// Set ZigBee/BLE HIGH when lock is held
static inline void coex_set_radio_lock_high() {
    // During join mode PTI is already elevated for beacon exchange.
    // Don't downgrade — skip the PTI change and only track the lock.
    if (s_join_mode_active) {
        DEBUG_PRINTLN(F("[COEX] Lock acquired (join mode active, PTI unchanged)"));
        return;
    }
    coex_apply(IEEE802154_IDLE, IEEE802154_MIDDLE, IEEE802154_HIGH);
    DEBUG_PRINTLN(F("[COEX] Lock active: ZigBee/BLE HIGH priority"));
}

/// Set ZigBee join mode priority — band-aware.
/// The ESP32-C5 has a single radio with time-division multiplexing, so even
/// on 5GHz WiFi, aggressive txrx priority starves WiFi of radio time.
///   - 2.4GHz / WiFi off: txrx=HIGH (WiFi is disconnected anyway)
///   - 5GHz:              txrx=MIDDLE (elevated but WiFi-safe)
static inline void coex_set_zigbee_join_mode() {
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    if (s_wifi_band == COEX_WIFI_5GHZ) {
        // 5GHz: MIDDLE avoids WiFi starvation on the shared TDM radio
        coex_apply(IEEE802154_IDLE, IEEE802154_MIDDLE, IEEE802154_HIGH);
        DEBUG_PRINTLN(F("[COEX] Join mode (5GHz): txrx=MIDDLE, timed=HIGH"));
    } else {
        // 2.4GHz or WiFi off: max ZigBee priority (WiFi paused/absent)
        coex_apply(IEEE802154_IDLE, IEEE802154_HIGH, IEEE802154_HIGH);
        DEBUG_PRINTLN(F("[COEX] Join mode (2.4GHz): txrx=HIGH, timed=HIGH"));
    }
}

/// Apply the current strategy's default priority (not during a lock)
static void coex_apply_strategy() {
    // Join mode is a cross-cutting state that overrides the band-based strategy
    // for the duration of the join window.  Check it first so we never hand back
    // a lower PTI while a join is still in progress.
    if (s_join_mode_active) {
        coex_set_zigbee_join_mode();
        return;
    }

    DEBUG_PRINTF("[COEX] Applying strategy %s (band=%d)\n", coex_get_status(), (int)s_wifi_band);

    switch (s_current_strategy) {
        case COEX_STRATEGY_ZIGBEE_HIGH:
            coex_set_zigbee_high();
            break;
        case COEX_STRATEGY_BALANCED_50:
            coex_set_balanced_50();
            break;
        case COEX_STRATEGY_WIFI_ONLY:
            coex_set_wifi_only();
            break;
        case COEX_STRATEGY_WIFI_PRIO:
        default:
            coex_set_wifi_priority();
            break;
    }
}

// =========================================================================
// Public API
// =========================================================================

void coex_init() {
    s_lock_owner = COEX_OWNER_NONE;
    s_lock_until = 0;
    s_ethernet   = false;
    s_wifi_band  = COEX_WIFI_OFF;
    s_current_strategy = COEX_STRATEGY_WIFI_PRIO;
    s_wifi_pause_until    = 0;
    s_wifi_pause_start    = 0;
    s_wifi_pause_cooldown = 0;
    s_join_mode_active    = false;
    s_join_mode_until     = 0;
    
    // Boot: start with WiFi priority
    coex_apply_strategy();
    s_initialized = true;
    DEBUG_PRINTLN(F("[COEX] Band-aware coex manager initialized (WiFi priority at boot)"));
}

void coex_loop() {
    if (!s_initialized) return;

    // Periodically update WiFi band detection (every 5 seconds)
    uint32_t now = millis();
    if ((int32_t)(now - s_last_wifi_check) >= (int32_t)COEX_WIFI_INFO_UPDATE_INTERVAL) {
        s_last_wifi_check = now;
        
        coex_wifi_band_t new_band = coex_detect_wifi_band();
        if (new_band != s_wifi_band) {
            s_wifi_band = new_band;

            // Re-evaluate strategy based on new WiFi band
            coex_strategy_t new_strategy = coex_evaluate_strategy(s_wifi_band);
            if (new_strategy != s_current_strategy) {
                s_current_strategy = new_strategy;
                DEBUG_PRINTF("[COEX] Strategy changed: band=%d, strategy=%d\n", 
                            (int)s_wifi_band, (int)s_current_strategy);
                
                // If no lock is held, apply the new strategy immediately
                if (s_lock_owner == COEX_OWNER_NONE) {
                    coex_apply_strategy();
                }
            }
        }
    }

    // Check lock expiry
    if (s_lock_owner != COEX_OWNER_NONE) {
        if ((int32_t)(millis() - s_lock_until) >= 0) {
            DEBUG_PRINTF("[COEX] Lock expired (owner=%d) → apply strategy\n", s_lock_owner);
            s_lock_owner = COEX_OWNER_NONE;
            s_lock_until = 0;
            coex_apply_strategy();
        }
    }

    // Check join mode expiry (independent of the 30s lock cap)
    if (s_join_mode_active && s_join_mode_until != 0 &&
        (int32_t)(millis() - s_join_mode_until) >= 0) {
        s_join_mode_active = false;
        s_join_mode_until  = 0;
        DEBUG_PRINTLN(F("[COEX] Join mode expired → returning to normal strategy"));
        if (s_lock_owner == COEX_OWNER_NONE) {
            coex_apply_strategy();
        }
    }

    // WiFi pause: deferred disconnect (allows HTTP response to flush first)
    if (s_wifi_pause_start != 0 && (int32_t)(millis() - s_wifi_pause_start) >= 0) {
        s_wifi_pause_start = 0;
        s_wifi_was_on = (WiFi.status() == WL_CONNECTED);
        if (s_wifi_was_on) {
            WiFi.disconnect(false, false);  // keep credentials, just drop connection
            DEBUG_PRINTLN(F("[COEX] WiFi disconnected for ZigBee pairing"));
        }
    }
    // WiFi pause expiry: reconnect WiFi and return to normal
    if (s_wifi_pause_until != 0 && s_wifi_pause_start == 0 &&
        (int32_t)(millis() - s_wifi_pause_until) >= 0) {
        s_wifi_pause_until = 0;
        if (s_wifi_was_on) {
            s_wifi_was_on = false;
            WiFi.reconnect();
            // Block new pauses for 60 s after reconnect to prevent the join-window
            // loop from immediately triggering another disconnect/reconnect cycle.
            s_wifi_pause_cooldown = millis() + 60000UL;
            DEBUG_PRINTLN(F("[COEX] WiFi reconnect after ZigBee pairing window"));
        }
        s_last_wifi_check = 0;  // force immediate band re-detection
    }
}

void coex_update_wifi_info() {
    // Force WiFi band re-detection on next coex_loop() call
    s_last_wifi_check = 0;
}

bool coex_request_lock(coex_owner_t owner, uint32_t timeout_ms) {
    if (!s_initialized || owner == COEX_OWNER_NONE) return false;

    // In ZIGBEE_HIGH strategy: always grant (no WiFi contention)
    if (s_current_strategy == COEX_STRATEGY_ZIGBEE_HIGH) return true;

    // In WIFI_ONLY strategy: never grant ZigBee locks (no sensors configured)
    if (s_current_strategy == COEX_STRATEGY_WIFI_ONLY) {
        DEBUG_PRINTF("[COEX] Lock denied in WIFI_ONLY mode (owner=%d)\n", owner);
        return false;
    }

    // Clamp timeout to maximum allowed
    uint32_t max_ms = COEX_LOCK_MAX_SENSOR_MS; // 10s default
    if (timeout_ms > COEX_LOCK_MAX_SENSOR_MS) {
        max_ms = COEX_LOCK_MAX_JOIN_MS; // allow up to 30s for join
    }
    if (timeout_ms > max_ms) timeout_ms = max_ms;

    // Single-threaded: no mutex needed
    if (s_lock_owner == COEX_OWNER_NONE || s_lock_owner == owner) {
        // Grant or extend
        s_lock_owner = owner;
        uint32_t new_until = millis() + timeout_ms;
        // Only extend, never shorten an existing lock
        if (s_lock_owner == owner && (int32_t)(new_until - s_lock_until) < 0) {
            new_until = s_lock_until;
        }
        s_lock_until = new_until;
        coex_set_radio_lock_high();
        DEBUG_PRINTF("[COEX] Lock acquired (owner=%d, timeout=%lums, strategy=%d)\n", 
                    owner, (unsigned long)timeout_ms, (int)s_current_strategy);
        return true;
    } else {
        // Another owner holds the lock
        DEBUG_PRINTF("[COEX] Lock denied (owner=%d, held by=%d)\n", owner, s_lock_owner);
        return false;
    }
}

void coex_release_lock(coex_owner_t owner) {
    if (!s_initialized) return;

    // Single-threaded: no mutex needed
    if (s_lock_owner == owner || owner == COEX_OWNER_NONE) {
        DEBUG_PRINTF("[COEX] Lock released (owner=%d) → apply strategy\n", s_lock_owner);
        s_lock_owner = COEX_OWNER_NONE;
        s_lock_until = 0;
        coex_apply_strategy();
    }
}

void coex_set_ethernet_mode(bool enabled) {
    if (!s_initialized) return;

    // Single-threaded: no mutex needed
    s_ethernet = enabled;
    if (enabled) {
        // Ethernet: WiFi off, Zigbee/BLE get max priority
        s_wifi_band = COEX_WIFI_OFF;
        s_current_strategy = COEX_STRATEGY_ZIGBEE_HIGH;
        s_lock_owner = COEX_OWNER_NONE;
        s_lock_until = 0;
        coex_apply_strategy();
        DEBUG_PRINTLN(F("[COEX] Ethernet mode → Zigbee/BLE permanent HIGH priority"));
    } else {
        s_ethernet = false;
        coex_update_wifi_info();  // Force WiFi band re-detection
        DEBUG_PRINTLN(F("[COEX] Ethernet mode disabled → re-evaluate WiFi"));
    }
}

void coex_notify_wifi_active() {
    // Re-assert WiFi priority during active HTTP/MQTT/OTA operations.
    // On 2.4 GHz with ZigBee: re-apply PREFER_WIFI so the coex scheduler
    // doesn't hand ZigBee radio time while a TCP stream is in flight.
    if (!s_initialized) return;
    if (s_lock_owner == COEX_OWNER_NONE &&
        (s_current_strategy == COEX_STRATEGY_WIFI_PRIO ||
         s_current_strategy == COEX_STRATEGY_WIFI_ONLY)) {
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    }
}

void coex_request_wifi_pause(uint32_t duration_ms) {
    if (!s_initialized) return;
    if (duration_ms == 0) return;
    if (duration_ms > 10000) duration_ms = 10000;  // hard cap at 10s

    // On 5 GHz WiFi there is no RF overlap with ZigBee/IEEE 802.15.4 (which
    // operates on 2.4 GHz).  No disconnect or strategy change needed — the
    // active radio lock already boosts ZigBee PTI for the duration of the
    // join window without disturbing the coex strategy or WiFi connection.
    if (s_wifi_band == COEX_WIFI_5GHZ) {
        DEBUG_PRINTLN(F("[COEX] WiFi pause skipped: 5GHz — no RF conflict, ZigBee priority via lock"));
        return;
    }

    // After a previous pause ended and WiFi reconnected, guard against an
    // immediate re-pause triggered by a still-active join window loop.
    if (s_wifi_pause_cooldown != 0 &&
        (int32_t)(millis() - s_wifi_pause_cooldown) < 0) {
        DEBUG_PRINTLN(F("[COEX] WiFi pause blocked: cooldown after previous reconnect"));
        return;
    }

    // If a pause is already in progress, extend it
    if (s_wifi_pause_until != 0) {
        uint32_t new_end = millis() + 500 + duration_ms;
        if ((int32_t)(new_end - s_wifi_pause_until) > 0) s_wifi_pause_until = new_end;
        return;
    }

    // Schedule delayed WiFi disconnect (500ms from now allows HTTP response to flush)
    s_wifi_pause_start = millis() + 500;
    s_wifi_pause_until = s_wifi_pause_start + duration_ms;

    // PTI boost is handled by join mode (caller must have called coex_set_join_mode).
    // Previously this function also overrode s_current_strategy = COEX_STRATEGY_ZIGBEE_HIGH;
    // that is no longer needed here.
    DEBUG_PRINTF("[COEX] WiFi pause requested: %lu ms \u2192 ZigBee gets 100%%\n",
                 (unsigned long)duration_ms);
}

bool coex_allow_ble_scan() {
    // Before init: allow (no contention management yet)
    if (!s_initialized) return true;

    // Never allow if Zigbee holds the lock
    return (s_lock_owner != COEX_OWNER_ZIGBEE);
}

void coex_set_join_mode(bool active, uint32_t duration_ms) {
    if (!s_initialized) return;

    if (active) {
        // Already in join mode? Just extend the deadline — PTI is already set.
        if (s_join_mode_active) {
            if (duration_ms > 0) {
                uint32_t new_until = millis() + duration_ms;
                if (s_join_mode_until == 0 || (int32_t)(new_until - s_join_mode_until) > 0) {
                    s_join_mode_until = new_until;
                }
            }
            return;
        }
        s_join_mode_active = true;
        s_join_mode_until  = (duration_ms > 0) ? (millis() + duration_ms) : 0;
        DEBUG_PRINTF("[COEX] Join mode entered (duration=%lu ms, band=%d)\n",
                     (unsigned long)duration_ms, (int)s_wifi_band);
        // On 2.4 GHz we also disconnect WiFi to eliminate RF contention.
        if (s_wifi_band == COEX_WIFI_2GHZ) {
            uint32_t pause_ms = (duration_ms > 0 && duration_ms < 10000) ? duration_ms : 10000;
            coex_request_wifi_pause(pause_ms);
        }
        // Apply band-aware join PTI immediately
        coex_set_zigbee_join_mode();
    } else {
        if (!s_join_mode_active) return;  // already exited (e.g. auto-expired)
        s_join_mode_active = false;
        s_join_mode_until  = 0;
        DEBUG_PRINTLN(F("[COEX] Join mode exited"));
        // Force immediate band re-detection so strategy is re-derived from
        // actual WiFi state (which may now be reconnected).
        s_last_wifi_check = 0;
        if (s_lock_owner == COEX_OWNER_NONE) {
            coex_apply_strategy();
        }
    }
}

bool coex_is_join_mode() {
    return s_join_mode_active;
}

bool coex_allow_zigbee_active() {
    // Before init: allow (no contention management yet)
    if (!s_initialized) return true;

    // Never allow if BLE holds the lock
    return (s_lock_owner != COEX_OWNER_BLE);
}

bool coex_is_zigbee_locked() {
    if (!s_initialized) return false;
    return (s_lock_owner == COEX_OWNER_ZIGBEE);
}

coex_wifi_band_t coex_get_wifi_band() {
    return s_wifi_band;
}

coex_strategy_t coex_get_strategy() {
    if (s_join_mode_active) return COEX_STRATEGY_ZIGBEE_JOIN;
    return s_current_strategy;
}

const char* coex_get_status() {
    static char buf[64];
    const char *band_str = "?";
    const char *strat_str = "?";
    
    switch (s_wifi_band) {
        case COEX_WIFI_OFF:     band_str = "off"; break;
        case COEX_WIFI_2GHZ:    band_str = "2.4GHz"; break;
        case COEX_WIFI_5GHZ:    band_str = "5GHz"; break;
        default:                band_str = "unknown"; break;
    }
    
    if (s_join_mode_active) {
        strat_str = "zigbee-join";
    } else {
        switch (s_current_strategy) {
            case COEX_STRATEGY_ZIGBEE_HIGH: strat_str = "zigbee-high"; break;
            case COEX_STRATEGY_WIFI_PRIO:   strat_str = "wifi-prio"; break;
            case COEX_STRATEGY_BALANCED_50: strat_str = "balanced-50"; break;
            case COEX_STRATEGY_WIFI_ONLY:   strat_str = "wifi-only"; break;
            default:                        strat_str = "unknown"; break;
        }
    }
    
    snprintf(buf, sizeof(buf), "%s/%s/%s", band_str, strat_str,
             s_lock_owner == COEX_OWNER_ZIGBEE ? "zigbee-lock" :
             s_lock_owner == COEX_OWNER_BLE ? "ble-lock" : "no-lock");
    
    return buf;
}

#else
/// Initialise the coex manager (call once after radio stack init)
void coex_init() {};

/// Periodic update — call from main sensor loop (handles lock expiry & WiFi band detection)
void coex_loop() {};

/// Notify WiFi connection state change (call when WiFi connects/disconnects/changes AP).
/// This updates internal WiFi band detection (2.4GHz vs 5GHz) and re-evaluates strategy.
void coex_update_wifi_info() {};

/// Request exclusive radio lock for Zigbee/BLE sensor operations.
/// Returns true if the lock was acquired. timeout_ms is clamped to the
/// owner-appropriate maximum (10s sensor, 30s join).
bool coex_request_lock(coex_owner_t owner, uint32_t timeout_ms) { return true; }

/// Release a previously acquired radio lock. Only the current owner
/// (or COEX_OWNER_NONE to force-release) can release it.
void coex_release_lock(coex_owner_t owner)  {};

/// Notify that Ethernet mode is active (WiFi not needed).
/// Once set, Zigbee/BLE get permanent HIGH priority without needing locks.
void coex_set_ethernet_mode(bool enabled) {};

/// Mark WiFi as actively serving an HTTP request / MQTT / OTA
/// (informational — extends no-lock WiFi window awareness for debug)
void coex_notify_wifi_active() {};

/// Temporarily disconnect WiFi and give ZigBee 100% for up to duration_ms
/// (max 10000 ms). WiFi is automatically reconnected afterwards.
/// Call this just before opening the ZigBee join window so new devices can pair
/// without WiFi interference on the shared 2.4 GHz radio.
void coex_request_wifi_pause(uint32_t duration_ms) {};

/// Check if BLE background scan is currently allowed
bool coex_allow_ble_scan() { return true; }

/// Check if Zigbee active operations (read/bind) are allowed
bool coex_allow_zigbee_active() { return true; }

/// Check if Zigbee currently holds the radio lock
bool coex_is_zigbee_locked() { return false; }

/// Enter or exit dedicated Zigbee join mode
void coex_set_join_mode(bool active, uint32_t duration_ms) {}

/// Returns true while a ZigBee join/pairing window is active.
bool coex_is_join_mode() { return false; }

/// Get current WiFi band detection (2.4GHz, 5GHz, or OFF)
coex_wifi_band_t coex_get_wifi_band() { return COEX_WIFI_OFF; }

/// Get current coex strategy
coex_strategy_t coex_get_strategy() { return COEX_STRATEGY_BALANCED_50; }

/// Human-readable status for debug output
const char* coex_get_status() { return "disabled"; }


#endif

#endif // ESP32C5
