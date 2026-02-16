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
#include <esp_ota_ops.h>
#include <esp_partition.h>

extern OpenSprinkler os;

// Current mode, loaded once at boot
static IEEE802154Mode current_mode = IEEE802154Mode::IEEE_DISABLED;
static IEEE802154BootVariant current_boot_variant = IEEE802154BootVariant::MATTER;
static bool config_loaded = false;

static IEEE802154BootVariant detect_running_boot_variant() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        return IEEE802154BootVariant::MATTER;
    }

    switch (running->subtype) {
        case ESP_PARTITION_SUBTYPE_APP_OTA_0:
            return IEEE802154BootVariant::MATTER;
        case ESP_PARTITION_SUBTYPE_APP_OTA_1:
            return IEEE802154BootVariant::ZIGBEE;
        default:
            return IEEE802154BootVariant::MATTER;
    }
}

IEEE802154BootVariant ieee802154_boot_variant_for_mode(IEEE802154Mode mode) {
    switch (mode) {
        case IEEE802154Mode::IEEE_ZIGBEE_GATEWAY:
        case IEEE802154Mode::IEEE_ZIGBEE_CLIENT:
            return IEEE802154BootVariant::ZIGBEE;
        case IEEE802154Mode::IEEE_DISABLED:
        case IEEE802154Mode::IEEE_MATTER:
        default:
            return IEEE802154BootVariant::MATTER;
    }
}

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

    uint8_t boot_val = doc["bootVariant"] | 0;
    if (boot_val == static_cast<uint8_t>(IEEE802154BootVariant::MATTER)) {
        current_boot_variant = IEEE802154BootVariant::MATTER;
    } else if (boot_val == static_cast<uint8_t>(IEEE802154BootVariant::ZIGBEE)) {
        current_boot_variant = IEEE802154BootVariant::ZIGBEE;
    } else {
        current_boot_variant = ieee802154_boot_variant_for_mode(static_cast<IEEE802154Mode>(mode_val));
    }

    IEEE802154Mode mode = static_cast<IEEE802154Mode>(mode_val);
    DEBUG_PRINTF("[IEEE802154] Loaded mode: %d (%s), boot=%d (%s)\n",
                 mode_val,
                 ieee802154_mode_name(mode),
                 static_cast<uint8_t>(current_boot_variant),
                 ieee802154_boot_variant_name(current_boot_variant));
    return mode;
}

bool ieee802154_save_mode(IEEE802154Mode mode) {
    return ieee802154_save_config(mode, ieee802154_boot_variant_for_mode(mode));
}

bool ieee802154_save_config(IEEE802154Mode mode, IEEE802154BootVariant boot_variant) {
    // Remove old file
    if (file_exists(IEEE802154_CONFIG_FILENAME)) {
        remove_file(IEEE802154_CONFIG_FILENAME);
    }

    ArduinoJson::JsonDocument doc;
    doc["mode"] = static_cast<uint8_t>(mode);
    doc["bootVariant"] = static_cast<uint8_t>(boot_variant);

    FileWriter writer(IEEE802154_CONFIG_FILENAME);
    size_t written = ArduinoJson::serializeJson(doc, writer);

    if (written == 0) {
        DEBUG_PRINTLN(F("[IEEE802154] Failed to write config file"));
        return false;
    }

    current_mode = mode;
    current_boot_variant = boot_variant;

    DEBUG_PRINTF("[IEEE802154] Saved mode: %d (%s), boot=%d (%s)\n",
                 static_cast<uint8_t>(mode),
                 ieee802154_mode_name(mode),
                 static_cast<uint8_t>(boot_variant),
                 ieee802154_boot_variant_name(boot_variant));
    return true;
}

IEEE802154Mode ieee802154_get_mode() {
    if (!config_loaded) {
        ieee802154_config_init();
    }
    return current_mode;
}

IEEE802154BootVariant ieee802154_get_boot_variant() {
    if (!config_loaded) {
        ieee802154_config_init();
    }
    return current_boot_variant;
}

bool ieee802154_select_otf_boot_variant(IEEE802154BootVariant variant) {
    esp_partition_subtype_t subtype = (variant == IEEE802154BootVariant::ZIGBEE)
        ? ESP_PARTITION_SUBTYPE_APP_OTA_1
        : ESP_PARTITION_SUBTYPE_APP_OTA_0;

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        subtype,
        nullptr
    );

    if (!partition) {
        DEBUG_PRINTF("[IEEE802154] Target boot partition not found for %s\n",
                     ieee802154_boot_variant_name(variant));
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        DEBUG_PRINTF("[IEEE802154] Failed to set boot partition %s: %d\n",
                     ieee802154_boot_variant_name(variant),
                     (int)err);
        return false;
    }

    DEBUG_PRINTF("[IEEE802154] Next boot partition set to %s\n",
                 ieee802154_boot_variant_name(variant));
    return true;
}

void ieee802154_config_init() {
    if (config_loaded) return;
    current_boot_variant = detect_running_boot_variant();
    current_mode = ieee802154_load_mode();
    config_loaded = true;
    DEBUG_PRINTF("[IEEE802154] Config initialized: mode=%d (%s), running_boot=%d (%s), configured_boot=%d (%s)\n",
                 static_cast<uint8_t>(current_mode),
                 ieee802154_mode_name(current_mode),
                 static_cast<uint8_t>(detect_running_boot_variant()),
                 ieee802154_boot_variant_name(detect_running_boot_variant()),
                 static_cast<uint8_t>(current_boot_variant),
                 ieee802154_boot_variant_name(current_boot_variant));
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

const char* ieee802154_boot_variant_name(IEEE802154BootVariant variant) {
    switch (variant) {
        case IEEE802154BootVariant::MATTER: return "matter";
        case IEEE802154BootVariant::ZIGBEE: return "zigbee";
        default: return "unknown";
    }
}

#endif // ESP32C5
