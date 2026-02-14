/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * IEEE 802.15.4 Radio Configuration - Implementation
 * Persists mode selection to /ieee802154.json on LittleFS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ieee802154_config.h"

#if defined(ESP32C5)

#include "ArduinoJson.hpp"
#include "sensors_util.h"
#include "OpenSprinkler.h"

extern OpenSprinkler os;

// Current mode, loaded once at boot
static IEEE802154Mode current_mode = IEEE802154Mode::IEEE_DISABLED;
static bool config_loaded = false;

IEEE802154Mode ieee802154_load_mode() {
    if (!file_exists(IEEE802154_CONFIG_FILENAME)) {
        DEBUG_PRINTLN(F("[IEEE802154] Config file not found, using DISABLED"));
        return IEEE802154Mode::IEEE_DISABLED;
    }

    ulong size = file_size(IEEE802154_CONFIG_FILENAME);
    if (size == 0 || size > 256) {
        DEBUG_PRINTLN(F("[IEEE802154] Config file empty or too large"));
        return IEEE802154Mode::IEEE_DISABLED;
    }

    FileReader reader(IEEE802154_CONFIG_FILENAME);
    ArduinoJson::JsonDocument doc;
    ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, reader);

    if (err) {
        DEBUG_PRINTF("[IEEE802154] JSON parse error: %s\n", err.c_str());
        return IEEE802154Mode::IEEE_DISABLED;
    }

    uint8_t mode_val = doc["mode"] | 0;
    if (mode_val > static_cast<uint8_t>(IEEE802154Mode::IEEE_ZIGBEE_CLIENT)) {
        DEBUG_PRINTF("[IEEE802154] Invalid mode value: %d\n", mode_val);
        return IEEE802154Mode::IEEE_DISABLED;
    }

    IEEE802154Mode mode = static_cast<IEEE802154Mode>(mode_val);
    DEBUG_PRINTF("[IEEE802154] Loaded mode: %d (%s)\n", mode_val, ieee802154_mode_name(mode));
    return mode;
}

bool ieee802154_save_mode(IEEE802154Mode mode) {
    // Remove old file
    if (file_exists(IEEE802154_CONFIG_FILENAME)) {
        remove_file(IEEE802154_CONFIG_FILENAME);
    }

    ArduinoJson::JsonDocument doc;
    doc["mode"] = static_cast<uint8_t>(mode);

    FileWriter writer(IEEE802154_CONFIG_FILENAME);
    size_t written = ArduinoJson::serializeJson(doc, writer);

    if (written == 0) {
        DEBUG_PRINTLN(F("[IEEE802154] Failed to write config file"));
        return false;
    }

    DEBUG_PRINTF("[IEEE802154] Saved mode: %d (%s)\n",
                 static_cast<uint8_t>(mode), ieee802154_mode_name(mode));
    return true;
}

IEEE802154Mode ieee802154_get_mode() {
    if (!config_loaded) {
        ieee802154_config_init();
    }
    return current_mode;
}

void ieee802154_config_init() {
    if (config_loaded) return;
    current_mode = ieee802154_load_mode();
    config_loaded = true;
    DEBUG_PRINTF("[IEEE802154] Config initialized: mode=%d (%s)\n",
                 static_cast<uint8_t>(current_mode), ieee802154_mode_name(current_mode));
}

const char* ieee802154_mode_name(IEEE802154Mode mode) {
    switch (mode) {
        case IEEE802154Mode::IEEE_DISABLED:       return "disabled";
        case IEEE802154Mode::IEEE_MATTER:         return "matter";
        case IEEE802154Mode::IEEE_ZIGBEE_GATEWAY: return "zigbee_gateway";
        case IEEE802154Mode::IEEE_ZIGBEE_CLIENT:  return "zigbee_client";
        default:                             return "unknown";
    }
}

#endif // ESP32C5
