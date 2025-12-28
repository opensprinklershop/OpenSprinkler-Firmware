/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Bluetooth LE sensor implementation - OSPI (Raspberry Pi with BlueZ)
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

#ifdef OSPI

#include "sensor_ospi_ble.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "sensor_payload_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

extern OpenSprinkler os;

// BLE adapter state
static int ble_adapter_id = -1;
static int ble_device_handle = -1;
static bool ble_initialized = false;

/**
 * @brief Initialize OSPI BLE subsystem
 */
bool sensor_ospi_ble_init() {
    if (ble_initialized) {
        DEBUG_PRINTLN("BLE already initialized");
        return true;
    }
    
    DEBUG_PRINTLN("Initializing BlueZ BLE subsystem...");
    
    // Find first available Bluetooth adapter
    ble_adapter_id = hci_get_route(NULL);
    if (ble_adapter_id < 0) {
        DEBUG_PRINTLN("ERROR: No Bluetooth adapter found");
        return false;
    }
    
    DEBUG_PRINT("Found Bluetooth adapter hci");
    DEBUG_PRINTLN(ble_adapter_id);
    
    // Open HCI socket
    ble_device_handle = hci_open_dev(ble_adapter_id);
    if (ble_device_handle < 0) {
        DEBUG_PRINTLN("ERROR: Cannot open Bluetooth adapter");
        return false;
    }
    
    ble_initialized = true;
    DEBUG_PRINTLN("BLE initialized successfully");
    return true;
}

/**
 * @brief Scan for BLE devices
 */
void sensor_ospi_ble_scan(int duration) {
    if (!ble_initialized) {
        if (!sensor_ospi_ble_init()) {
            return;
        }
    }
    
    DEBUG_PRINT("Starting BLE scan for ");
    DEBUG_PRINT(duration);
    DEBUG_PRINTLN(" seconds...");
    
    // Note: This is a simplified implementation
    // For production, use BlueZ D-Bus API for better device management
    // and to avoid requiring root privileges
    
    inquiry_info *devices = NULL;
    int max_devices = 255;
    int num_devices;
    int flags = IREQ_CACHE_FLUSH;
    
    devices = (inquiry_info*)malloc(max_devices * sizeof(inquiry_info));
    if (!devices) {
        DEBUG_PRINTLN("ERROR: Memory allocation failed");
        return;
    }
    
    num_devices = hci_inquiry(ble_adapter_id, duration, max_devices, NULL, &devices, flags);
    if (num_devices < 0) {
        DEBUG_PRINTLN("ERROR: HCI inquiry failed");
        free(devices);
        return;
    }
    
    DEBUG_PRINT("Found ");
    DEBUG_PRINT(num_devices);
    DEBUG_PRINTLN(" devices");
    
    // Process discovered devices (store in global list)
    // This is simplified - full implementation would use D-Bus for BLE LE scanning
    
    free(devices);
}

/**
 * @brief Get list of discovered BLE devices
 */
int sensor_ospi_ble_get_devices(char* json_buffer, int buffer_size) {
    if (!json_buffer || buffer_size < 3) {
        return -1;
    }
    
    // Simplified implementation - return empty array
    // Full implementation would query BlueZ via D-Bus
    strncpy(json_buffer, "[]", buffer_size);
    return 0;
}

/**
 * @brief Read a GATT characteristic from a BLE device
 * @note This is a simplified implementation using gatttool command
 * Production implementation should use BlueZ D-Bus GATT API
 */
int sensor_ospi_ble_read_characteristic(
    const char* mac_address,
    const char* characteristic_uuid,
    uint8_t* value_buffer,
    int buffer_size
) {
    if (!mac_address || !characteristic_uuid || !value_buffer || buffer_size < 1) {
        return -1;
    }
    
    DEBUG_PRINT("Reading BLE characteristic ");
    DEBUG_PRINT(characteristic_uuid);
    DEBUG_PRINT(" from device ");
    DEBUG_PRINTLN(mac_address);
    
    // Use gatttool to read characteristic (requires gatttool to be installed)
    char cmd[128];
    snprintf(cmd, sizeof(cmd), 
             "timeout 10 gatttool -b %s --char-read -u %s 2>/dev/null",
             mac_address, characteristic_uuid);
    
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        DEBUG_PRINTLN("ERROR: Failed to execute gatttool");
        return -1;
    }
    
    char line[128];
    int bytes_read = 0;
    
    // Parse gatttool output: "Characteristic value/descriptor: 01 02 03 04"
    if (fgets(line, sizeof(line), pipe) != NULL) {
        char* value_start = strstr(line, ":");
        if (value_start) {
            value_start += 2; // Skip ": "
            
            // Parse hex bytes
            char* token = strtok(value_start, " \n");
            while (token && bytes_read < buffer_size) {
                unsigned int byte_val;
                if (sscanf(token, "%02x", &byte_val) == 1) {
                    value_buffer[bytes_read++] = (uint8_t)byte_val;
                }
                token = strtok(NULL, " \n");
            }
        }
    }
    
    int result = pclose(pipe);
    if (result != 0) {
        DEBUG_PRINTLN("ERROR: gatttool failed");
        return -1;
    }
    
    DEBUG_PRINT("Read ");
    DEBUG_PRINT(bytes_read);
    DEBUG_PRINTLN(" bytes");
    
    return bytes_read;
}

/**
 * @brief Read value from BLE sensor
 */
int OspiBLESensor::read(unsigned long time) {
    if (!flags.enable) return HTTP_RQT_NOT_RECEIVED;
    
    if (!ble_initialized) {
        if (!sensor_ospi_ble_init()) {
            return HTTP_RQT_NOT_RECEIVED;
        }
    }
    
    // Parse sensor configuration from JSON
    // Expected format in name: MAC address (e.g., "AA:BB:CC:DD:EE:FF")
    // Expected format in userdef_unit: Characteristic UUID (optionally with format: "UUID|format_id")
    //   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
    
    const char* mac_address = name; // Use name field for MAC
    char characteristic_uuid[128] = {0};
    PayloadFormat format = FORMAT_TEMP_001; // Default auto-detect
    
    // Parse userdef_unit field: "UUID" or "UUID|format"
    if (userdef_unit && strlen(userdef_unit) > 0) {
        strncpy(characteristic_uuid, userdef_unit, sizeof(characteristic_uuid) - 1);
        
        // Check for format specifier after |
        char* pipe = strchr(characteristic_uuid, '|');
        if (pipe) {
            *pipe = '\0'; // Terminate UUID
            pipe++; // Move to format part
            int fmt = atoi(pipe);
            if (fmt >= 0 && fmt <= 30) {
                format = (PayloadFormat)fmt;
            }
        }
    }
    
    if (!mac_address || strlen(mac_address) == 0) {
        DEBUG_PRINTLN(F("ERROR: BLE MAC address not configured in name field"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (strlen(characteristic_uuid) == 0) {
        DEBUG_PRINTLN(F("ERROR: BLE characteristic UUID not configured in userdef_unit field"));
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Read characteristic value
    uint8_t value_buffer[20];
    int bytes_read = sensor_ospi_ble_read_characteristic(
        mac_address,
        characteristic_uuid,
        value_buffer,
        sizeof(value_buffer)
    );
    
    if (bytes_read < 0) {
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Parse value based on sensor type
    double parsed_value = 0.0;
    bool parse_ok = false;
    
    // Decode based on sensor type
    switch (type) {
        case SENSOR_BLE_TEMP:
            if (format != FORMAT_TEMP_001) {
                parse_ok = decode_payload(value_buffer, bytes_read, format, &parsed_value);
            } else {
                parse_ok = auto_decode_sensor(value_buffer, bytes_read, "temperature", &parsed_value);
            }
            break;
            
        case SENSOR_BLE_HUMIDITY:
            if (format != FORMAT_TEMP_001) {
                parse_ok = decode_payload(value_buffer, bytes_read, format, &parsed_value);
            } else {
                parse_ok = auto_decode_sensor(value_buffer, bytes_read, "humidity", &parsed_value);
            }
            break;
            
        case SENSOR_BLE_PRESSURE:
            if (format != FORMAT_TEMP_001) {
                parse_ok = decode_payload(value_buffer, bytes_read, format, &parsed_value);
            } else {
                parse_ok = auto_decode_sensor(value_buffer, bytes_read, "pressure", &parsed_value);
            }
            break;
            
        default:
            DEBUG_PRINTLN("ERROR: Unknown BLE sensor type");
            return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (!parse_ok) {
        DEBUG_PRINTLN("ERROR: Failed to parse BLE value");
        flags.data_ok = false;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Update sensor data with averaging
    repeat_data += parsed_value;
    repeat_native += bytes_read; // Store bytes as "native" value
    
    if (++repeat_read < MAX_SENSOR_REPEAT_READ && time < last_read + read_interval) {
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    // Calculate average
    parsed_value = repeat_data / repeat_read;
    
    last_data = parsed_value;
    last_native_data = repeat_native / repeat_read;
    flags.data_ok = true;
    last_read = time;
    
    // Reset averaging counters
    repeat_data = parsed_value;
    repeat_native = last_native_data;
    repeat_read = 1;
    
    DEBUG_PRINT("BLE sensor value: ");
    DEBUG_PRINTLN(parsed_value);
    
    return HTTP_RQT_SUCCESS;
}

/**
 * @brief Get measurement unit for BLE sensor
 */
unsigned char OspiBLESensor::getUnitId() const {
    switch (type) {
        case SENSOR_BLE_TEMP:
            return UNIT_DEGREE;
        case SENSOR_BLE_HUMIDITY:
            return UNIT_PERCENT;
        case SENSOR_BLE_PRESSURE:
            return UNIT_PASCAL;
        default:
            return UNIT_NONE;
    }
}

#endif // OSPI
