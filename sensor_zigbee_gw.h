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
 * @brief Remaining gateway permit-join window in seconds
 */
uint16_t sensor_zigbee_gw_get_join_window_remaining();

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

/**
 * @brief Query Basic Cluster version/manufacturer/model attributes by IEEE address
 * @param device_ieee IEEE address of target device
 * @param endpoint Target endpoint
 * @return true if the read request was sent
 */
bool sensor_zigbee_gw_query_basic_cluster_by_ieee(uint64_t device_ieee, uint8_t endpoint);

/**
 * @brief Actively read one attribute from a remote device in Gateway mode
 * @param device_ieee IEEE address of target device
 * @param endpoint Target endpoint
 * @param cluster_id Cluster ID
 * @param attribute_id Attribute ID
 * @return true if read request was queued successfully
 */
bool sensor_zigbee_gw_read_attribute(uint64_t device_ieee, uint8_t endpoint,
                                     uint16_t cluster_id, uint16_t attribute_id);

/**
 * @brief Send a ZCL Configure Reporting command to a remote device
 * @param device_ieee IEEE address of target device
 * @param endpoint Target endpoint
 * @param cluster_id Cluster ID containing the attribute
 * @param attr_id Attribute ID to configure reporting for
 * @param min_interval Minimum reporting interval in seconds
 * @param max_interval Maximum reporting interval in seconds
 * @return true if the command was sent successfully
 * @note For sleeping end devices (AQARA, etc.) this is the preferred way to
 *       obtain periodic data — the device wakes up and pushes reports on its own.
 */
bool sensor_zigbee_gw_configure_reporting(uint64_t device_ieee, uint8_t endpoint,
                                           uint16_t cluster_id, uint16_t attr_id,
                                           uint16_t min_interval, uint16_t max_interval);

/**
 * @brief Send a Tuya DP query for a device (Gateway mode)
 * @param device_ieee IEEE address of target device
 * @param endpoint Target endpoint
 * @return true if query was sent
 */
bool sensor_zigbee_gw_request_dp_query(uint64_t device_ieee, uint8_t endpoint);

/**
 * @brief Actively send OFF to every configured Zigbee station.
 *
 * Called from the global "Stop All Stations" code paths so the physical
 * valves are guaranteed to receive the off command. Delivery is confirmed
 * (and retried up to 4× over 10 s) by the verify/retry queue.
 */
void sensor_zigbee_gw_force_off_all_stations();

/**
 * @brief Register a pending switch-state verification for a Zigbee station.
 * Called from switch_zigbeestation after each ON/OFF command is sent.
 */
void sensor_zigbee_station_verify_register(uint8_t sid, uint64_t ieee, uint8_t endpoint, uint8_t dp_id, bool expected_on);

/**
 * @brief Return the current Zigbee station switch status.
 * 0 = confirmed off, 1 = pending while last confirmed off, 2 = switch error,
 * 3 = confirmed on, 4 = pending while last confirmed on.
 */
uint8_t sensor_zigbee_station_status_code(uint8_t sid);

/**
 * @brief Clear a remembered Zigbee switch error for a station.
 */
void sensor_zigbee_station_clear_error(uint8_t sid);

/**
 * @brief Check for timed-out station switch verifications and publish alerts.
 * Called from sensor_zigbee_gw_loop() on every pass.
 */
void sensor_zigbee_station_verify_tick();

#endif // ESP32C5 && OS_ENABLE_ZIGBEE
#endif // _SENSOR_ZIGBEE_GW_H
