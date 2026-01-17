/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Zigbee sensor header file (ESP32-C5 native Zigbee)
 * 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _SENSOR_ZIGBEE_H
#define _SENSOR_ZIGBEE_H

#include "sensors.h"

/**
 * @brief Structure to hold discovered Zigbee device information
 */
struct ZigbeeDeviceInfo {
    uint64_t ieee_addr;           // IEEE address
    uint16_t short_addr;          // Short network address
    char model_id[32];            // Model identifier
    char manufacturer[32];        // Manufacturer name
    uint8_t endpoint;             // Primary endpoint
    uint16_t device_id;           // Zigbee device ID
    bool is_new;                  // Flag for newly discovered device
};

#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)

#include "SensorBase.hpp"

/**
 * @brief Start Zigbee as END DEVICE (called after WiFi is connected)
 * 
 * IMPORTANT: This runs Zigbee in END DEVICE mode, which is the ONLY mode
 * that supports WiFi coexistence on ESP32-C5/C6!
 * 
 * - Coordinator + WiFi = NOT SUPPORTED (will break WiFi connection)
 * - Router + WiFi = NOT SUPPORTED  
 * - End Device + WiFi STA = SUPPORTED (stable performance)
 * 
 * The device will join an existing Zigbee network (e.g., Zigbee2MQTT)
 * and receive attribute reports from other sensors via bindings.
 * 
 * @note Call after WiFi STA is fully connected
 * @note Bindings must be configured via the Zigbee coordinator
 */
void sensor_zigbee_start();

/**
 * @brief Returns true if Zigbee End Device is currently active
 */
bool sensor_zigbee_is_active();

/**
 * @brief Start Zigbee End Device if needed (best-effort)
 * @return true if Zigbee is active after the call
 * @note Only works in WiFi STA mode, not in SOFTAP mode
 */
bool sensor_zigbee_ensure_started();

/**
 * @brief Force a factory reset of Zigbee NVRAM on next start
 * Call this when NVRAM data is corrupted or when switching device modes.
 * The actual reset happens on the next call to sensor_zigbee_start().
 */
void sensor_zigbee_factory_reset();

/**
 * @brief Configure sensor to receive reports from a Zigbee device
 * @param nr Sensor number
 * @param device_ieee IEEE address of Zigbee device (e.g., "0x00124b001f8e5678")
 * @note Actual binding must be done via Zigbee coordinator (e.g., Zigbee2MQTT)
 */
void sensor_zigbee_bind_device(uint nr, const char *device_ieee);

/**
 * @brief Unbind from a Zigbee device
 * @param nr Sensor number
 * @param device_ieee IEEE address of Zigbee device
 * @note Removes binding for device
 */
void sensor_zigbee_unbind_device(uint nr, const char *device_ieee);

/**
 * @brief Open Zigbee network for device pairing (legacy, does nothing in End Device mode)
 * @param duration Duration in seconds (ignored)
 * @note In End Device mode, pairing is controlled by the Zigbee coordinator
 * @note Use Zigbee2MQTT to pair new devices
 */
void sensor_zigbee_open_network(uint16_t duration = 60);

/**
 * @brief Zigbee maintenance loop (call periodically from main loop)
 * @note Prints bound devices for debugging
 */
void sensor_zigbee_loop();

/**
 * @brief Get list of discovered Zigbee devices
 * @param devices Array to store discovered devices
 * @param max_devices Maximum number of devices to return
 * @return Number of devices found
 * @note Call after opening network to discover new devices
 */
int sensor_zigbee_get_discovered_devices(ZigbeeDeviceInfo* devices, int max_devices);

/**
 * @brief Clear the "new device" flags
 * @note Called after user has been notified of new devices
 */
void sensor_zigbee_clear_new_device_flags();

/**
 * @brief Actively read an attribute from a Zigbee device
 * @param device_ieee IEEE address of target device
 * @param endpoint Endpoint number
 * @param cluster_id Cluster ID to read from
 * @param attribute_id Attribute ID to read
 * @return true if read request sent successfully
 * @note Response will arrive via zigbee_attribute_callback
 */
bool sensor_zigbee_read_attribute(uint64_t device_ieee, uint8_t endpoint, 
                                   uint16_t cluster_id, uint16_t attribute_id);

/**
 * @brief Zigbee sensor class for ESP32-C5 native Zigbee
 * @note Uses ESP32-C5 built-in Zigbee radio for direct communication
 * @note Supports Tuya soil moisture sensor and other Zigbee devices
 * 
 * ESP32-C5 acts as Zigbee Coordinator and receives reports from End Devices.
 * 
 * Supported Zigbee Clusters:
 * - Soil Moisture Measurement (0x0408)
 * - Temperature Measurement (0x0402)
 * - Relative Humidity Measurement (0x0405)
 * - Power Configuration (0x0001) - for battery level
 * 
 * Example Tuya soil moisture sensor:
 * - Endpoint: 1
 * - Cluster: 0x0408 (Soil Moisture)
 * - Attribute: 0x0000 (MeasuredValue)
 */
class ZigbeeSensor : public SensorBase {
public:
    // Zigbee-specific fields
    uint64_t device_ieee = 0;         // IEEE address as 64-bit integer (e.g., 0x00124b001f8e5678)
    uint8_t endpoint = 1;             // Zigbee endpoint (usually 1)
    uint16_t cluster_id = 0x0408;     // Cluster ID (0x0408=soil moisture, 0x0402=temperature)
    uint16_t attribute_id = 0x0000;   // Attribute ID (0x0000=MeasuredValue)
    
    // User-defined conversion parameters (like AsbSensor SENSOR_USERDEF)
    int32_t factor = 0;               // Multiplication factor for value conversion
    int32_t divider = 0;              // Division factor for value conversion
    int32_t offset_mv = 0;            // Zero-point offset in millivolts (applied before factor/divider)
    int32_t offset2 = 0;              // Offset in 0.01 unit (applied after factor/divider)
    
    // Runtime state
    bool device_bound = false;        // Track binding state
    uint32_t last_battery = 100;      // Last reported battery level (0-100%)
    uint8_t last_lqi = 0;             // Last reported LQI (Link Quality Indicator, 0-255)
    uint32_t poll_interval = 60000;   // Active polling interval in ms (default: 60 seconds)
    uint32_t last_poll_time = 0;      // Timestamp of last active poll

    /**
     * @brief Constructor
     * @param type Sensor type identifier (SENSOR_ZIGBEE)
     */
    explicit ZigbeeSensor(uint type) : SensorBase(type) {
        // Cluster and attribute will be configured via MQTT or JSON
        // Default to temperature measurement cluster
        cluster_id = 0x0402;    // Temperature Measurement cluster
        attribute_id = 0x0000;  // MeasuredValue attribute
    }
    
    virtual ~ZigbeeSensor() {}
    
    /**
     * @brief Initialize Zigbee sensor
     * @return true if initialization successful
     * @note Connects to MQTT broker and subscribes to device topic
     */
    virtual bool init() override;
    
    /**
     * @brief Cleanup Zigbee sensor resources
     * @note Unsubscribes from MQTT topics
     */
    virtual void deinit() override;
    
    /**
     * @brief Read sensor value from Zigbee device
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS if data received, HTTP_RQT_NOT_RECEIVED otherwise
     * @note Supports both passive reports and active polling
     * @note If poll_interval > 0, actively polls device when interval expires
     */
    virtual int read(unsigned long time) override;

    /**
     * @brief Deserialize sensor configuration from JSON
     * @param obj JSON object containing sensor configuration
     * @note Fields: device_ieee (string), endpoint, cluster_id, attribute_id
     */
    virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;
    
    /**
     * @brief Serialize sensor configuration to JSON
     * @param obj JSON object to populate with sensor configuration
     */
    virtual void toJson(ArduinoJson::JsonObject obj) const override;
    
    /**
     * @brief Get measurement unit identifier
     * @return Unit ID based on sensor type and value_path
     */
    virtual unsigned char getUnitId() const override;
    
    /**
     * @brief Emit sensor data as JSON to BufferFiller
     * @param bfill Output buffer for JSON data
     */
    virtual void emitJson(BufferFiller& bfill) const override;
    
    /**
     * @brief Zigbee attribute report callback
     * @param ieee_addr Device IEEE address
     * @param endpoint Endpoint number
     * @param cluster_id Cluster ID
     * @param attr_id Attribute ID
     * @param value Attribute value
     * @note Called when Zigbee device sends attribute report
     */
    static void zigbee_attribute_callback(uint64_t ieee_addr, uint8_t endpoint, 
                                         uint16_t cluster_id, uint16_t attr_id, 
                                         int32_t value, uint8_t lqi);

    /**
     * @brief Convert IEEE address from uint64_t to string
     * @param buffer Output buffer
     * @param bufferSize Buffer size
     * @return Formatted IEEE address string (e.g., "0x00124B001F8E5678")
     */
    const char* getIeeeString(char* buffer, size_t bufferSize) const;
    
    /**
     * @brief Parse IEEE address string to uint64_t
     * @param ieee_str IEEE address string (e.g., "0x00124B001F8E5678" or "00:12:4B:00:1F:8E:56:78")
     * @return IEEE address as 64-bit integer
     */
    static uint64_t parseIeeeAddress(const char* ieee_str);
};

#endif // ESP32C5 && OS_ENABLE_ZIGBEE

#endif // _SENSOR_ZIGBEE_H
