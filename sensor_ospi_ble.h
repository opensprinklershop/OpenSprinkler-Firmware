/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Bluetooth LE sensor header file - OSPI (Raspberry Pi with BlueZ)
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

#ifndef _SENSOR_OSPI_BLE_H
#define _SENSOR_OSPI_BLE_H

#ifdef OSPI

#include "sensors.h"
#include "SensorBase.hpp"

/**
 * @brief BLE sensor for OSPI platform using BlueZ
 * @note Uses BlueZ D-Bus API for BLE communication on Linux/Raspberry Pi
 * 
 * Configuration via sensor JSON:
 * - "mac": BLE device MAC address (e.g., "AA:BB:CC:DD:EE:FF")
 * - "service": GATT service UUID (optional, auto-discover if not specified)
 * - "characteristic": GATT characteristic UUID to read
 * - "parse": Data parsing mode ("raw", "int16", "uint16", "float", "temperature", "humidity")
 * 
 * The sensor uses SENSOR_BLE (96) type and decodes payload based on configured
 * format or auto-detects common GATT characteristic formats.
 */
class OspiBLESensor : public SensorBase {
public:
    /**
     * @brief Constructor
     * @param type Sensor type identifier (SENSOR_BLE)
     */
    explicit OspiBLESensor(uint type) : SensorBase(type) {}
    virtual ~OspiBLESensor() {}
    
    /**
     * @brief Read value from BLE device
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
     * @note Uses BlueZ D-Bus API to connect and read GATT characteristic
     */
    virtual int read(unsigned long time) override;
    
    /**
     * @brief Get measurement unit for this sensor
     * @return Unit ID from assigned_unitid (configured via sensor setup)
     */
    virtual unsigned char getUnitId() const override;
};

/**
 * @brief Initialize OSPI BLE subsystem
 * @note Initializes BlueZ adapter via D-Bus
 * @return true if initialization successful
 */
bool sensor_ospi_ble_init();

/**
 * @brief Scan for BLE devices
 * @param duration Scan duration in seconds (default: 10)
 * @note Uses BlueZ discovery API
 */
void sensor_ospi_ble_scan(int duration = 10);

/**
 * @brief Get list of discovered BLE devices
 * @param json_buffer Buffer to store JSON array of devices
 * @param buffer_size Size of json_buffer
 * @return Number of devices found, or -1 on error
 * @note Returns JSON: [{"mac":"AA:BB:CC:DD:EE:FF","name":"Device","rssi":-70},...]
 */
int sensor_ospi_ble_get_devices(char* json_buffer, int buffer_size);

/**
 * @brief Read a GATT characteristic from a BLE device
 * @param mac_address BLE device MAC address
 * @param characteristic_uuid UUID of characteristic to read
 * @param value_buffer Buffer to store read value
 * @param buffer_size Size of value_buffer
 * @return Number of bytes read, or -1 on error
 * @note Connects to device, reads characteristic, and disconnects
 */
int sensor_ospi_ble_read_characteristic(
    const char* mac_address,
    const char* characteristic_uuid,
    uint8_t* value_buffer,
    int buffer_size
);

#endif // OSPI

#endif // _SENSOR_OSPI_BLE_H
