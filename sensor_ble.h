/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Bluetooth LE sensor header file (ESP32 Arduino BLE)
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

#ifndef _SENSOR_BLE_H
#define _SENSOR_BLE_H

#if defined(ESP32) && defined(OS_ENABLE_BLE)

#include "sensors.h"
#include "SensorBase.hpp"
#include "sensor_payload_decoder.h"

/**
 * @brief Known BLE sensor types (for discovery filtering)
 */
enum BLESensorType : uint8_t {
    BLE_TYPE_UNKNOWN = 0,
    BLE_TYPE_GOVEE_H5074,    // Govee H5074 (UUID 0xec88, 7 byte mfg data)
    BLE_TYPE_GOVEE_H5075,    // Govee H5075/H5072/H5100/etc (UUID 0xec88, 6 byte mfg data)
    BLE_TYPE_GOVEE_H5179,    // Govee H5179 (UUID 0xec88, 9 byte mfg data)
    BLE_TYPE_GOVEE_H5177,    // Govee H5177/H5174 (Mfg 0x0001, 6 byte data)
    BLE_TYPE_GOVEE_MEAT,     // Govee meat thermometers (H5181-H5184)
    BLE_TYPE_XIAOMI,         // Xiaomi LYWSD03MMC etc.
    BLE_TYPE_GENERIC_GATT,   // Generic GATT-based sensor
    // BMS (Battery Management System) types - require GATT bidirectional communication
    BLE_TYPE_BMS_JBD,        // JBD/Xiaoxiang BMS (very common in LiFePO4 batteries)
    BLE_TYPE_BMS_DALY,       // Daly BMS
    BLE_TYPE_BMS_ANT,        // ANT BMS
    BLE_TYPE_BMS_JIKONG,     // JK/Jikong BMS
};

/**
 * @brief Structure to hold discovered BLE device information
 */
struct BLEDeviceInfo {
    uint8_t address[6];           // BLE MAC address
    char name[32];                // Device name
    int16_t rssi;                 // Signal strength
    bool is_new;                  // Flag for newly discovered device
    uint32_t last_seen;           // Timestamp of last advertisement

    // Optional: advertised primary service UUID (if present in the advertisement)
    // Stored as canonical string (e.g. "0000180a-0000-1000-8000-00805f9b34fb")
    char service_uuid[40];
    
    // Sensor type (for filtering known sensors)
    BLESensorType sensor_type;
    
    // Cached advertisement data for sensors that broadcast readings
    // (Govee, Xiaomi, etc. - they don't need GATT connection)
    float adv_temperature;        // Temperature from advertisement
    float adv_humidity;           // Humidity from advertisement  
    uint8_t adv_battery;          // Battery % from advertisement
    bool has_adv_data;            // True if adv_* fields are valid
    
    // BMS-specific cached data (requires GATT connection)
    float bms_voltage;            // Total pack voltage (V)
    float bms_current;            // Current (A, positive=charging, negative=discharging)
    uint8_t bms_soc;              // State of charge (%)
    float bms_temperature;        // Average cell temperature (°C)
    uint16_t bms_cycles;          // Charge cycles
    bool has_bms_data;            // True if bms_* fields are valid
};

/**
 * @brief Convert a BLE UUID to a human-readable name (best-effort)
 * @param uuid UUID string, e.g. "180A", "0x180A", or "0000180a-0000-1000-8000-00805f9b34fb".
 *            If the string contains a suffix like "|10" it will be ignored.
 * @return Pointer to a constant name string. Returns "Unknown" if not recognized.
 */
const char* ble_uuid_to_name(const char* uuid);


/**
 * @brief BLE sensor class for ESP32 Arduino
 * @note Uses ESP32 BLE library for communication
 * 
 * Configuration via sensor JSON:
 * - name: BLE device MAC address (e.g., "AA:BB:CC:DD:EE:FF")
 * - userdef_unit: GATT characteristic UUID (optionally with format: "UUID|format_id")
 *   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
 * 
 * The sensor uses SENSOR_BLE (96) type and automatically decodes payload based on
 * configured format or auto-detects common GATT characteristic formats.
 */
class BLESensor : public SensorBase {
public:
    // BLE-specific persistent fields (stored in sensors.json via toJson/fromJson)
    char characteristic_uuid_cfg[40] = {0};
    char mac_address_cfg[24] = {0};
    uint8_t payload_format_cfg = (uint8_t)FORMAT_TEMP_001;
    
    // For advertisement-based sensors (Govee etc.): which value to report
    // Uses assigned_unitid from base class:
    // - Unit 2 (°C) or 3 (°F) → temperature
    // - Unit 5 (%) → humidity  
    // - Other units → battery

    /**
     * @brief Constructor
     * @param type Sensor type identifier (SENSOR_BLE)
     */
    explicit BLESensor(uint type) : SensorBase(type) {}
    virtual ~BLESensor() {}

    virtual void fromJson(ArduinoJson::JsonVariantConst obj) override;
    virtual void toJson(ArduinoJson::JsonObject obj) const override;
    
    /**
     * @brief Read value from BLE device
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS on successful read, HTTP_RQT_NOT_RECEIVED on error
     */
    virtual int read(unsigned long time) override;
   
    virtual const char* getUnit() const override;
    virtual unsigned char getUnitId() const override;
private:
    /**
     * @brief Store sensor result and mark data as valid
     * @param value The measured value (already in correct unit, e.g., 34.59 for °C)
     * @param time Current timestamp
     * @note Handles last_data, last_native_data, repeat_data, repeat_native, repeat_read
     */
    void store_result(double value, unsigned long time);
};

/**
 * @brief Initialize BLE sensor subsystem
 */
void sensor_ble_init();

/**
 * @brief Stop BLE subsystem (frees RF resources for WiFi)
 */
void sensor_ble_stop();

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
 * @brief Returns true if BLE subsystem is currently active
 */
bool sensor_ble_is_active();

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

/**
 * @brief Find a cached device by MAC address
 * @param mac_address MAC address string (e.g. "AA:BB:CC:DD:EE:FF")
 * @return Pointer to device info or nullptr if not found
 */
const BLEDeviceInfo* sensor_ble_find_device(const char* mac_address);

/**
 * @brief Check if a device has advertisement-based sensor data (Govee etc.)
 * @param device Pointer to device info
 * @return true if device broadcasts sensor data via advertisement
 */
bool sensor_ble_is_adv_sensor(const BLEDeviceInfo* device);

#endif // ESP32

#endif // _SENSOR_BLE_H
