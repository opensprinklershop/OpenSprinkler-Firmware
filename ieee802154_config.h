/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * IEEE 802.15.4 Radio Configuration
 * Runtime-selectable mode: Disabled / Matter / ZigBee Gateway / ZigBee Client
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _IEEE802154_CONFIG_H
#define _IEEE802154_CONFIG_H

#include <cstdint>

/**
 * @brief IEEE 802.15.4 radio operating modes
 * 
 * Only one mode can be active at a time since Matter and ZigBee share
 * the same IEEE 802.15.4 radio on ESP32-C5.
 * Changing the mode requires a reboot.
 */
enum class IEEE802154Mode : uint8_t {
    IEEE_DISABLED       = 0,   ///< No IEEE 802.15.4 - radio off (default)
    IEEE_MATTER         = 1,   ///< Matter protocol (HomeKit, Google Home, Alexa)
    IEEE_ZIGBEE_GATEWAY = 2,   ///< ZigBee Coordinator/Gateway mode (manage devices)
    IEEE_ZIGBEE_CLIENT  = 3    ///< ZigBee End Device mode (join existing network)
};

#if defined(ESP32C5)

/**
 * @brief IEEE 802.15.4 configuration filename
 * Stored as JSON on LittleFS: {"mode": 0..3}
 */
#define IEEE802154_CONFIG_FILENAME "/ieee802154.json"

/**
 * @brief Load the IEEE 802.15.4 mode from persistent storage
 * @return The configured mode, or DISABLED if not configured / error
 */
IEEE802154Mode ieee802154_load_mode();

/**
 * @brief Save the IEEE 802.15.4 mode to persistent storage
 * @param mode The mode to save
 * @return true if saved successfully
 */
bool ieee802154_save_mode(IEEE802154Mode mode);

/**
 * @brief Get the currently active IEEE 802.15.4 mode (loaded at boot)
 * @return Current mode
 */
IEEE802154Mode ieee802154_get_mode();

/**
 * @brief Check if Matter is enabled (mode == IEEE_MATTER)
 * @return true if Matter mode is active
 */
inline bool ieee802154_is_matter() {
    return ieee802154_get_mode() == IEEE802154Mode::IEEE_MATTER;
}

/**
 * @brief Check if ZigBee Gateway is enabled (mode == IEEE_ZIGBEE_GATEWAY)
 * @return true if ZigBee Gateway mode is active
 */
inline bool ieee802154_is_zigbee_gw() {
    return ieee802154_get_mode() == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY;
}

/**
 * @brief Check if ZigBee Client is enabled (mode == IEEE_ZIGBEE_CLIENT)
 * @return true if ZigBee Client mode is active
 */
inline bool ieee802154_is_zigbee_client() {
    return ieee802154_get_mode() == IEEE802154Mode::IEEE_ZIGBEE_CLIENT;
}

/**
 * @brief Check if any ZigBee mode is enabled (Gateway or Client)
 * @return true if any ZigBee mode is active
 */
inline bool ieee802154_is_zigbee() {
    IEEE802154Mode m = ieee802154_get_mode();
    return m == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY || m == IEEE802154Mode::IEEE_ZIGBEE_CLIENT;
}

/**
 * @brief Check if IEEE 802.15.4 radio is enabled (any mode except IEEE_DISABLED)
 * @return true if radio should be active
 */
inline bool ieee802154_is_enabled() {
    return ieee802154_get_mode() != IEEE802154Mode::IEEE_DISABLED;
}

/**
 * @brief Initialize the IEEE 802.15.4 config subsystem
 * Loads the mode from persistent storage. Call early in boot.
 */
void ieee802154_config_init();

/**
 * @brief Get mode name as string
 * @param mode The mode to describe
 * @return Human-readable name
 */
const char* ieee802154_mode_name(IEEE802154Mode mode);

#else
// Non-ESP32C5: stubs
inline IEEE802154Mode ieee802154_get_mode() { return IEEE802154Mode::IEEE_DISABLED; }
inline bool ieee802154_is_matter() { return false; }
inline bool ieee802154_is_zigbee_gw() { return false; }
inline bool ieee802154_is_zigbee_client() { return false; }
inline bool ieee802154_is_zigbee() { return false; }
inline bool ieee802154_is_enabled() { return false; }
inline void ieee802154_config_init() {}
inline const char* ieee802154_mode_name(IEEE802154Mode) { return "disabled"; }
#endif // ESP32C5

#endif // _IEEE802154_CONFIG_H
