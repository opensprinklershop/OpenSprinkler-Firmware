#include "radio_arbiter.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(ESP32) && defined(ARDUINO)
#include <WiFi.h>
#endif

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE) && defined(ARDUINO)
#include <esp_wifi.h>
#include <esp_coexist.h>
extern "C" {
#include <esp_coex_i154.h>
}
#include <esp_ieee802154.h>
#endif

#if defined(ENABLE_DEBUG) && defined(ARDUINO)
#define RA_DEBUG(...) Serial.printf(__VA_ARGS__)
#else
#define RA_DEBUG(...)
#endif

#ifndef RADIO_ARBITER_WEB_HOLD_MS
#define RADIO_ARBITER_WEB_HOLD_MS 1500UL
#endif

// Periodic BLE scan window: every BLE_WINDOW_INTERVAL_MS, allow BLE for
// BLE_WINDOW_DURATION_MS even if web priority is active.
#define BLE_WINDOW_INTERVAL_MS  30000UL
#define BLE_WINDOW_DURATION_MS  10000UL

#if defined(ARDUINO)
static volatile uint32_t s_web_priority_until_ms = 0;
static uint32_t s_last_ble_window_ms = 0;
static bool s_balanced_coex_applied = false;

// Radio ownership state
static volatile RadioOwner s_radio_owner = RADIO_OWNER_NONE;
static volatile uint32_t s_radio_owner_deadline = 0;  // 0 = indefinite

static bool time_is_before(uint32_t now, uint32_t deadline) {
    return (int32_t)(deadline - now) > 0;
}

bool radio_arbiter_acquire(RadioOwner owner, uint32_t duration_ms) {
    // Auto-expire current owner if deadline passed
    if (s_radio_owner != RADIO_OWNER_NONE && s_radio_owner_deadline != 0) {
        if (!time_is_before(millis(), s_radio_owner_deadline)) {
            RA_DEBUG("[RA] Owner %d expired, releasing\n", (int)s_radio_owner);
            s_radio_owner = RADIO_OWNER_NONE;
            s_radio_owner_deadline = 0;
        }
    }

    if (s_radio_owner != RADIO_OWNER_NONE && s_radio_owner != owner) {
        RA_DEBUG("[RA] Acquire DENIED: owner=%d requested=%d\n", (int)s_radio_owner, (int)owner);
        return false;
    }

    s_radio_owner = owner;
    s_radio_owner_deadline = (duration_ms > 0) ? (millis() + duration_ms) : 0;

    RA_DEBUG("[RA] Acquired by %d (duration=%lums)\n", (int)owner, (unsigned long)duration_ms);

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
    // Adjust PTI based on owner
    if (owner == RADIO_OWNER_ZIGBEE_SCAN || owner == RADIO_OWNER_MATTER) {
        if (WiFi.getMode() != WIFI_MODE_NULL) {
            esp_ieee802154_coex_config_t coex_cfg = {
                .idle    = IEEE802154_IDLE,
                .txrx    = IEEE802154_HIGH,
                .txrx_at = IEEE802154_HIGH,
            };
            esp_ieee802154_set_coex_config(coex_cfg);
            esp_coex_ieee802154_ack_pti_set(IEEE802154_HIGH);
        }
    }
#endif
    return true;
}

void radio_arbiter_release(RadioOwner owner) {
    if (s_radio_owner != owner) return;  // Safety check

    RA_DEBUG("[RA] Released by %d\n", (int)owner);
    s_radio_owner = RADIO_OWNER_NONE;
    s_radio_owner_deadline = 0;

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
    // Restore WiFi-friendly PTI
    if (WiFi.getMode() != WIFI_MODE_NULL) {
        esp_ieee802154_coex_config_t coex_cfg = {
            .idle    = IEEE802154_IDLE,
            .txrx    = IEEE802154_LOW,
            .txrx_at = IEEE802154_LOW,
        };
        esp_ieee802154_set_coex_config(coex_cfg);
        esp_coex_ieee802154_ack_pti_set(IEEE802154_LOW);
    }
#endif
}

RadioOwner radio_arbiter_get_owner() {
    // Auto-expire
    if (s_radio_owner != RADIO_OWNER_NONE && s_radio_owner_deadline != 0) {
        if (!time_is_before(millis(), s_radio_owner_deadline)) {
            s_radio_owner = RADIO_OWNER_NONE;
            s_radio_owner_deadline = 0;
        }
    }
    return s_radio_owner;
}

bool radio_arbiter_is_owner(RadioOwner owner) {
    return radio_arbiter_get_owner() == owner;
}

void radio_arbiter_mark_web_activity() {
    const uint32_t now = millis();
    const uint32_t new_deadline = now + RADIO_ARBITER_WEB_HOLD_MS;
    if (!time_is_before(new_deadline, s_web_priority_until_ms)) {
        s_web_priority_until_ms = new_deadline;
    }
}

bool radio_arbiter_is_web_priority_active() {
    const uint32_t now = millis();
    return time_is_before(now, s_web_priority_until_ms);
}

bool radio_arbiter_allow_ble_scan() {
    // Block BLE if another owner holds the radio (Zigbee scan, Matter)
    RadioOwner owner = radio_arbiter_get_owner();
    if (owner == RADIO_OWNER_ZIGBEE_SCAN || owner == RADIO_OWNER_MATTER) {
        return false;
    }

    // Always allow if web priority is not active
    if (!radio_arbiter_is_web_priority_active()) return true;

    // Web priority IS active — check if we're in a periodic BLE window
    const uint32_t now = millis();
    uint32_t elapsed_since_window = now - s_last_ble_window_ms;
    if (elapsed_since_window >= BLE_WINDOW_INTERVAL_MS) {
        s_last_ble_window_ms = now;
        RA_DEBUG("[RA] BLE scan window opened (periodic during web activity)\n");
        return true;
    }
    if (elapsed_since_window < BLE_WINDOW_DURATION_MS) {
        return true;
    }
    return false;
}

bool radio_arbiter_allow_zigbee_active_ops() {
    // Block Zigbee active ops if BLE scan or Matter owns the radio
    RadioOwner owner = radio_arbiter_get_owner();
    if (owner == RADIO_OWNER_BLE_SCAN || owner == RADIO_OWNER_MATTER) {
        return false;
    }
    return true;
}

void radio_arbiter_debug_state() {
    const uint32_t now = millis();
    bool web_active = radio_arbiter_is_web_priority_active();
    bool ble_allowed = radio_arbiter_allow_ble_scan();
    RA_DEBUG("[RA] web_priority=%d ble_allowed=%d owner=%d now=%lu deadline=%lu\n",
            web_active, ble_allowed, (int)radio_arbiter_get_owner(),
            (unsigned long)now, (unsigned long)s_web_priority_until_ms);
}

void radio_arbiter_apply_balanced_coex_once() {
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
    if (s_balanced_coex_applied) {
        return;
    }

    if (WiFi.getMode() == WIFI_MODE_NULL) {
        return;
    }

    WiFi.setSleep(false);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    (void)esp_coex_wifi_i154_enable();
    s_balanced_coex_applied = true;
    RA_DEBUG("[RA] Coex base configured once: PREFER_BALANCE + WiFi/i154 enabled\n");
#endif
}

void radio_arbiter_ensure_wifi() {
#if defined(ESP32) && defined(ARDUINO)
    wifi_mode_t wmode = WiFi.getMode();
    if (wmode == WIFI_MODE_NULL || wmode == WIFI_MODE_AP) return;  // Ethernet or AP mode

    if (WiFi.status() != WL_CONNECTED) {
        RA_DEBUG("[RA] WiFi disconnected after radio operation — triggering reconnect\n");
        WiFi.disconnect(false);
        delay(100);
        WiFi.reconnect();
    }
#endif
}

#else

void radio_arbiter_mark_web_activity() {}
bool radio_arbiter_is_web_priority_active() { return false; }
bool radio_arbiter_allow_ble_scan() { return true; }
bool radio_arbiter_allow_zigbee_active_ops() { return true; }
void radio_arbiter_debug_state() {}
void radio_arbiter_apply_balanced_coex_once() {}
bool radio_arbiter_acquire(RadioOwner, uint32_t) { return true; }
void radio_arbiter_release(RadioOwner) {}
RadioOwner radio_arbiter_get_owner() { return RADIO_OWNER_NONE; }
bool radio_arbiter_is_owner(RadioOwner) { return false; }
void radio_arbiter_ensure_wifi() {}

#endif
