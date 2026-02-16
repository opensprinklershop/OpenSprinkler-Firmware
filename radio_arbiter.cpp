#include "radio_arbiter.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE) && defined(ARDUINO)
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
extern "C" {
#include <esp_coex_i154.h>
}
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
// This prevents web UI polling from permanently starving BLE.
#define BLE_WINDOW_INTERVAL_MS  30000UL  // 30 s between windows
#define BLE_WINDOW_DURATION_MS  10000UL  // 10 s window

#if defined(ARDUINO)
static volatile uint32_t s_web_priority_until_ms = 0;
static uint32_t s_last_ble_window_ms = 0;  // last time a BLE window started
static bool s_balanced_coex_applied = false;

static bool time_is_before(uint32_t now, uint32_t deadline) {
    return (int32_t)(deadline - now) > 0;
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
    // Always allow if web priority is not active
    if (!radio_arbiter_is_web_priority_active()) return true;

    // Web priority IS active — check if we're in a periodic BLE window
    const uint32_t now = millis();
    uint32_t elapsed_since_window = now - s_last_ble_window_ms;
    if (elapsed_since_window >= BLE_WINDOW_INTERVAL_MS) {
        // Start a new BLE window
        s_last_ble_window_ms = now;
        RA_DEBUG("[RA] BLE scan window opened (periodic during web activity)\n");
        return true;
    }
    if (elapsed_since_window < BLE_WINDOW_DURATION_MS) {
        // Still inside the current BLE window
        return true;
    }
    return false;
}

bool radio_arbiter_allow_zigbee_active_ops() {
    // Zigbee ZCL reads are lightweight and coexist with WiFi on ESP32-C5
    // without interference — no web-priority gating needed.
    return true;
}

void radio_arbiter_debug_state() {
    const uint32_t now = millis();
    bool web_active = radio_arbiter_is_web_priority_active();
    bool ble_allowed = radio_arbiter_allow_ble_scan();
    RA_DEBUG("[RA] web_priority=%d ble_allowed=%d now=%lu deadline=%lu\n",
            web_active, ble_allowed, (unsigned long)now,
            (unsigned long)s_web_priority_until_ms);
}

void radio_arbiter_apply_balanced_coex_once() {
#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
    if (s_balanced_coex_applied) {
        return;
    }

    if (WiFi.getMode() == WIFI_MODE_NULL) {
        // Ethernet mode: no WiFi radio, so nothing to configure.
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

#else

void radio_arbiter_mark_web_activity() {}
bool radio_arbiter_is_web_priority_active() { return false; }
bool radio_arbiter_allow_ble_scan() { return true; }
bool radio_arbiter_allow_zigbee_active_ops() { return true; }
void radio_arbiter_debug_state() {}
void radio_arbiter_apply_balanced_coex_once() {}

#endif
