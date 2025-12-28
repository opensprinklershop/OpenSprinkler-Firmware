/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Bluetooth LE sensor header file (ESP32 Arduino BLE)
 * 2025 @ OpenSprinklerShop
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

#ifndef _SENSOR_BLE_H
#define _SENSOR_BLE_H

#include "sensors.h"
#include "Sensor.hpp"

/**
 * @brief Structure to hold discovered BLE device information
 */
struct BLEDeviceInfo {
    uint8_t address[6];           // BLE MAC address
    char name[32];                // Device name
    int16_t rssi;                 // Signal strength
    bool is_new;                  // Flag for newly discovered device
    uint32_t last_seen;           // Timestamp of last advertisement
};

#if defined(ESP32)

/**
 * @brief BLE sensor class for ESP32 Arduino
 * @note Uses ESP32 BLE library for communication
 * 
 * Configuration via sensor JSON:
 * - name: BLE device MAC address (e.g., "AA:BB:CC:DD:EE:FF")
 * - userdef_unit: GATT characteristic UUID (optionally with format: "UUID|format_id")
 *   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
 * 
 * Supported sensor types:
 * - SENSOR_BLE_TEMP: Temperature sensor
 * - SENSOR_BLE_HUMIDITY: Humidity sensor
 * - SENSOR_BLE_PRESSURE: Pressure sensor
 */
class BLESensor : public SensorBase {
public:
    /**
     * @brief Constructor
     * @param type Sensor type identifier (SENSOR_BLE_TEMP, SENSOR_BLE_HUMIDITY, SENSOR_BLE_PRESSURE)
     */
    explicit BLESensor(uint type) : SensorBase(type) {}
    virtual ~BLESensor() {}
    
    /**
     * @brief Read value from BLE device
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
     */
    virtual int read(unsigned long time) override;
    
    /**
     * @brief Get measurement unit for this sensor
     * @return Unit ID (UNIT_DEGREE, UNIT_PERCENT, UNIT_PASCAL)
     */
    virtual unsigned char getUnitId() const override;
};

/**
 * @brief Initialize BLE sensor subsystem
 */
void sensor_ble_init();

/**
 * @brief Start BLE scanning for devices
 * @param duration Duration in seconds (default: 5)
 */
void sensor_ble_start_scan(uint16_t duration = 5);

/**
 * @brief BLE maintenance loop (call periodically from main loop)
 */
void sensor_ble_loop();

/**
 * @brief Get list of discovered BLE devices
 * @param devices Array to store discovered devices
 * @param max_devices Maximum number of devices to return
 * @return Number of devices found
 */
int sensor_ble_get_discovered_devices(BLEDeviceInfo* devices, int max_devices);

/**
 * @brief Clear the "new device" flags
 */
void sensor_ble_clear_new_device_flags();

#endif // ESP32

#endif // _SENSOR_BLE_H
