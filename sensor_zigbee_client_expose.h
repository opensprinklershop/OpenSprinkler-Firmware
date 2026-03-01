/* OpenSprinkler ZigBee Client Mode — Expose local data to ZigBee hub
 *
 * Creates ZCL endpoints that report OS sensor values, zone on/off control,
 * program start/stop, and rain sensor state to the ZigBee coordinator/hub.
 *
 * All endpoints must be registered via client_expose_create_endpoints()
 * BEFORE Zigbee.begin() is called.
 *
 * 2026 @ OpenSprinklerShop
 */
#pragma once

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

/// Create ZCL endpoints for OS sensors, zones, programs, rain sensor.
/// Must be called BEFORE Zigbee.begin().
void client_expose_create_endpoints();

/// Periodic update: sync OS sensor values and zone states to ZCL attributes.
/// Call from client_zigbee_loop_internal().
void client_expose_update_loop();

#endif
