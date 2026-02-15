/* OpenSprinkler Unified (ESP32-C5) Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * Zigbee Gateway/Coordinator mode - internal header
 * These functions are called by sensor_zigbee.cpp runtime dispatch.
 * Do NOT call directly from outside the Zigbee sensor module.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _SENSOR_ZIGBEE_GW_H
#define _SENSOR_ZIGBEE_GW_H

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

/**
 * @brief Start Zigbee in Coordinator/Gateway mode
 * @note Only call when ieee802154_get_mode() == IEEE_ZIGBEE_GATEWAY
 */
void sensor_zigbee_gw_start();

/**
 * @brief Stop Zigbee Coordinator/Gateway
 */
void sensor_zigbee_gw_stop();

/**
 * @brief Check if Zigbee Gateway is currently active
 */
bool sensor_zigbee_gw_is_active();

/**
 * @brief Ensure Zigbee Gateway is started (best-effort)
 * @return true if gateway is active after the call
 */
bool sensor_zigbee_gw_ensure_started();

/**
 * @brief Gateway maintenance loop (call periodically from main loop)
 */
void sensor_zigbee_gw_loop();

/**
 * @brief Open Zigbee network for device pairing (Coordinator only)
 * @param duration Duration in seconds (max 254)
 */
void sensor_zigbee_gw_open_network(uint16_t duration);

/**
 * @brief Force a factory reset of Zigbee NVRAM for gateway mode
 */
void sensor_zigbee_gw_factory_reset();

/**
 * @brief Get list of discovered devices (Gateway mode)
 * @param out Output buffer for device info structs
 * @param max_devices Maximum number of entries to copy
 * @return Number of devices actually copied
 */
int sensor_zigbee_gw_get_discovered_devices(ZigbeeDeviceInfo* out, int max_devices);

/**
 * @brief Clear "is_new" flag on all discovered devices (Gateway mode)
 */
void sensor_zigbee_gw_clear_new_device_flags();

/**
 * @brief Process cached attribute reports from Zigbee devices (Gateway mode)
 * Called from the shared zigbee_attribute_callback dispatcher.
 * @param ieee_addr Device IEEE address
 * @param endpoint Endpoint number
 * @param cluster_id Cluster ID
 * @param attr_id Attribute ID
 * @param value Attribute value
 * @param lqi Link Quality Indicator
 */
void sensor_zigbee_gw_process_reports(uint64_t ieee_addr, uint8_t endpoint,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       int32_t value, uint8_t lqi);

/**
 * @brief Query Basic Cluster (ManufacturerName + ModelIdentifier) from a device
 * @param short_addr Short network address of the target device
 * @param endpoint Endpoint to query (usually 1)
 * @note Response arrives asynchronously via zbAttributeRead callback
 */
void sensor_zigbee_gw_query_basic_cluster(uint16_t short_addr, uint8_t endpoint);

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
#endif // _SENSOR_ZIGBEE_GW_H
