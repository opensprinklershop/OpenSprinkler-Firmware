/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Sensor Payload Decoder - Common decoding functions for BLE and Zigbee
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

#ifndef _SENSOR_PAYLOAD_DECODER_H
#define _SENSOR_PAYLOAD_DECODER_H

#include <stdint.h>

/**
 * @brief Payload decoding formats
 */
enum PayloadFormat {
    FORMAT_RAW = 0,           // Raw bytes (no conversion)
    FORMAT_UINT8 = 1,         // Unsigned 8-bit integer
    FORMAT_INT8 = 2,          // Signed 8-bit integer
    FORMAT_UINT16_LE = 3,     // Unsigned 16-bit little-endian
    FORMAT_INT16_LE = 4,      // Signed 16-bit little-endian
    FORMAT_UINT16_BE = 5,     // Unsigned 16-bit big-endian
    FORMAT_INT16_BE = 6,      // Signed 16-bit big-endian
    FORMAT_UINT32_LE = 7,     // Unsigned 32-bit little-endian
    FORMAT_INT32_LE = 8,      // Signed 32-bit little-endian
    FORMAT_FLOAT_LE = 9,      // 32-bit float little-endian
    FORMAT_TEMP_001 = 10,     // Temperature in 0.01°C steps (int16_le / 100)
    FORMAT_HUM_001 = 11,      // Humidity in 0.01% steps (uint16_le / 100)
    FORMAT_PRESS_PA = 12,     // Pressure in Pascal (uint16_le or uint32_le)
    FORMAT_XIAOMI_TEMP = 20,  // Xiaomi temperature format (int16_le / 100)
    FORMAT_XIAOMI_HUM = 21,   // Xiaomi humidity format (uint16_le / 100)
    FORMAT_TUYA_SOIL = 30,    // Tuya soil moisture (uint16_le / 100)
};

/**
 * @brief Decode payload to double value
 * @param data Raw data buffer
 * @param len Length of data
 * @param format Decoding format (see PayloadFormat enum)
 * @param result Pointer to store decoded value
 * @return true if decoding successful
 */
inline bool decode_payload(const uint8_t* data, int len, PayloadFormat format, double* result) {
    if (!data || !result || len < 1) {
        return false;
    }
    
    switch (format) {
        case FORMAT_RAW:
            // Just return first byte as-is
            *result = (double)data[0];
            return true;
            
        case FORMAT_UINT8:
            if (len < 1) return false;
            *result = (double)data[0];
            return true;
            
        case FORMAT_INT8:
            if (len < 1) return false;
            *result = (double)(int8_t)data[0];
            return true;
            
        case FORMAT_UINT16_LE:
            if (len < 2) return false;
            *result = (double)(data[0] | (data[1] << 8));
            return true;
            
        case FORMAT_INT16_LE:
        case FORMAT_XIAOMI_TEMP:
            if (len < 2) return false;
            *result = (double)(int16_t)(data[0] | (data[1] << 8));
            if (format == FORMAT_XIAOMI_TEMP) {
                *result /= 100.0; // Convert to °C
            }
            return true;
            
        case FORMAT_UINT16_BE:
            if (len < 2) return false;
            *result = (double)(data[1] | (data[0] << 8));
            return true;
            
        case FORMAT_INT16_BE:
            if (len < 2) return false;
            *result = (double)(int16_t)(data[1] | (data[0] << 8));
            return true;
            
        case FORMAT_UINT32_LE:
            if (len < 4) return false;
            *result = (double)(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
            return true;
            
        case FORMAT_INT32_LE:
            if (len < 4) return false;
            *result = (double)(int32_t)(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
            return true;
            
        case FORMAT_FLOAT_LE:
            if (len < 4) return false;
            {
                uint32_t raw = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
                float f;
                memcpy(&f, &raw, sizeof(float));
                *result = (double)f;
            }
            return true;
            
        case FORMAT_TEMP_001:
            // Temperature in 0.01°C steps (e.g., 2250 = 22.50°C)
            if (len < 2) return false;
            *result = (double)(int16_t)(data[0] | (data[1] << 8)) / 100.0;
            return true;
            
        case FORMAT_HUM_001:
        case FORMAT_XIAOMI_HUM:
        case FORMAT_TUYA_SOIL:
            // Humidity/Moisture in 0.01% steps (e.g., 6520 = 65.20%)
            if (len < 2) return false;
            *result = (double)(uint16_t)(data[0] | (data[1] << 8)) / 100.0;
            return true;
            
        case FORMAT_PRESS_PA:
            // Pressure in Pascal - can be 16-bit or 32-bit
            if (len >= 4) {
                *result = (double)(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
            } else if (len >= 2) {
                *result = (double)(data[0] | (data[1] << 8));
            } else {
                return false;
            }
            return true;
            
        default:
            return false;
    }
}

/**
 * @brief Auto-detect common sensor data formats
 * @param data Raw data buffer
 * @param len Length of data
 * @param sensor_type Hint about sensor type (temp, humidity, etc.)
 * @param result Pointer to store decoded value
 * @return true if decoding successful
 */
inline bool auto_decode_sensor(const uint8_t* data, int len, const char* sensor_type, double* result) {
    if (!data || !result || !sensor_type || len < 1) {
        return false;
    }
    
    // Temperature sensors - try common formats
    if (strstr(sensor_type, "temp") || strstr(sensor_type, "TEMP")) {
        // Try int16 with 0.01°C resolution first (most common)
        if (len >= 2 && decode_payload(data, len, FORMAT_TEMP_001, result)) {
            if (*result >= -40.0 && *result <= 125.0) { // Sanity check
                return true;
            }
        }
        // Try float format
        if (len >= 4 && decode_payload(data, len, FORMAT_FLOAT_LE, result)) {
            if (*result >= -40.0 && *result <= 125.0) {
                return true;
            }
        }
    }
    
    // Humidity sensors
    if (strstr(sensor_type, "hum") || strstr(sensor_type, "HUM") || 
        strstr(sensor_type, "moisture") || strstr(sensor_type, "MOISTURE")) {
        // Try uint16 with 0.01% resolution
        if (len >= 2 && decode_payload(data, len, FORMAT_HUM_001, result)) {
            if (*result >= 0.0 && *result <= 100.0) { // Sanity check
                return true;
            }
        }
    }
    
    // Pressure sensors
    if (strstr(sensor_type, "press") || strstr(sensor_type, "PRESS")) {
        if (decode_payload(data, len, FORMAT_PRESS_PA, result)) {
            if (*result >= 30000.0 && *result <= 110000.0) { // Reasonable range in Pa
                return true;
            }
        }
    }
    
    // Default: try as uint16
    return decode_payload(data, len, FORMAT_UINT16_LE, result);
}

/**
 * @brief Decode Xiaomi MiFlora advertising data
 * @param data Advertisement data
 * @param len Length of data
 * @param temperature Pointer to store temperature (°C)
 * @param moisture Pointer to store moisture (%)
 * @param light Pointer to store light (lux)
 * @param conductivity Pointer to store conductivity (µS/cm)
 * @param battery Pointer to store battery (%)
 * @return Number of values decoded
 */
inline int decode_xiaomi_miflora(const uint8_t* data, int len, 
                                  double* temperature, double* moisture, 
                                  double* light, double* conductivity, 
                                  double* battery) {
    if (!data || len < 14) return 0;
    
    int count = 0;
    
    // Xiaomi MiFlora format (service data):
    // [0-1]: Temperature (int16_le, 0.1°C)
    // [2]: Moisture (uint8, %)
    // [3-6]: Light (uint32_le, lux)
    // [7-8]: Conductivity (uint16_le, µS/cm)
    // [9]: Battery (uint8, %)
    
    if (temperature) {
        *temperature = (double)(int16_t)(data[0] | (data[1] << 8)) / 10.0;
        count++;
    }
    
    if (moisture) {
        *moisture = (double)data[2];
        count++;
    }
    
    if (light && len >= 7) {
        *light = (double)(data[3] | (data[4] << 8) | (data[5] << 16) | (data[6] << 24));
        count++;
    }
    
    if (conductivity && len >= 9) {
        *conductivity = (double)(data[7] | (data[8] << 8));
        count++;
    }
    
    if (battery && len >= 10) {
        *battery = (double)data[9];
        count++;
    }
    
    return count;
}

#endif // _SENSOR_PAYLOAD_DECODER_H
