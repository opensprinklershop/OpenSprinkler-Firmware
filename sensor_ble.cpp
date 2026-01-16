/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 * Analog Sensor API by Stefan Schmaltz (info@opensprinklershop.de)
 *
 * Bluetooth LE sensor implementation - Arduino ESP32 BLE
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

 #if defined(ESP32) && defined(OS_ENABLE_BLE)

#include "sensor_ble.h"
#include "sensors.h"
#include "OpenSprinkler.h"
#include "sensor_payload_decoder.h"
#include <vector>
#include <ctype.h>

extern OpenSprinkler os;

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// BLE scan instance
static bool ble_initialized = false;
static BLEScan* pBLEScan = nullptr;
static uint32_t scan_end_time = 0;
static bool scanning_active = false;

// Reuse a single client instance; do not delete clients manually.
// Deinit is deferred to the BLE maintenance loop to avoid heap corruption.
static BLEClient* ble_client = nullptr;
static bool ble_in_read = false;
static bool ble_stop_requested = false;
static uint32_t ble_stop_request_time = 0;

static const uint32_t BLE_CONNECT_TIMEOUT_MS = 400; // keep BLE reads short

// Auto-stop timer for discovery mode (to free WiFi RF resources)
static unsigned long ble_auto_stop_time = 0;
static bool ble_discovery_mode = false;

// Discovered devices storage (dynamically allocated)
static std::vector<BLEDeviceInfo> discovered_ble_devices;

// ============================================================================
// Govee BLE Advertisement Data Decoder
// Based on: https://github.com/wcbonner/GoveeBTTempLogger
//           https://github.com/yduf/Govee-monitor
// ============================================================================

/**
 * @brief Detect Govee sensor type from device name
 * @param name Device name (e.g. "GVH5075_427B")
 * @return Sensor type enum
 */
static BLESensorType govee_detect_type_from_name(const char* name) {
    if (!name || !name[0]) return BLE_TYPE_UNKNOWN;
    
    if (strstr(name, "GVH5074") || strstr(name, "Govee_H5074"))
        return BLE_TYPE_GOVEE_H5074;
    if (strstr(name, "GVH5075") || strstr(name, "GVH5072") || 
        strstr(name, "GVH5100") || strstr(name, "GVH5101") ||
        strstr(name, "GVH5102") || strstr(name, "GVH5105"))
        return BLE_TYPE_GOVEE_H5075;
    if (strstr(name, "GVH5179") || strstr(name, "GV5179") || strstr(name, "Govee_H5179"))
        return BLE_TYPE_GOVEE_H5179;
    if (strstr(name, "GVH5177") || strstr(name, "GVH5174"))
        return BLE_TYPE_GOVEE_H5177;
    if (strstr(name, "GVH5181") || strstr(name, "GVH5182") || 
        strstr(name, "GVH5183") || strstr(name, "GVH5184") ||
        strstr(name, "GVH5055"))
        return BLE_TYPE_GOVEE_MEAT;
    if (strstr(name, "LYWSD") || strstr(name, "MJ_HT"))
        return BLE_TYPE_XIAOMI;
        
    return BLE_TYPE_UNKNOWN;
}

/**
 * @brief Decode Govee H5074 manufacturer data (7 bytes, Manufacturer ID 0xec88)
 * Format: [temp_low] [temp_high] [hum_low] [hum_high] [battery] [??] [??]
 */
static bool govee_decode_h5074(const uint8_t* data, size_t len, float* temp, float* hum, uint8_t* battery) {
    if (len < 6) return false;
    // H5074 uses different encoding: temp/hum as 16-bit LE with 0.01 precision
    int16_t iTemp = (int16_t)(data[2] << 8 | data[1]);
    int iHum = (int)(data[4] << 8 | data[3]);
    *temp = (float)iTemp / 100.0f;
    *hum = (float)iHum / 100.0f;
    *battery = data[5];
    return true;
}

/**
 * @brief Decode Govee H5075/H5072/H5100 manufacturer data (6 bytes, Manufacturer ID 0xec88)
 * Format: [byte0] [temp_high] [temp_mid] [temp_low] [battery]
 * Temperature + humidity are encoded together: TTTTTHHH (19 bits temp * 10, 10 bits hum * 10)
 */
static bool govee_decode_h5075(const uint8_t* data, size_t len, float* temp, float* hum, uint8_t* battery) {
    if (len < 5) return false;
    
    // 3-byte encoding: iTemp contains both temp and humidity
    // From GoveeBTTempLogger: int(Data[1]) << 16 | int(Data[2]) << 8 | int(Data[3])
    int32_t iTemp = ((int32_t)data[1] << 16) | ((int32_t)data[2] << 8) | (int32_t)data[3];
    
    bool bNegative = (iTemp & 0x800000) != 0;  // Check sign bit
    iTemp = iTemp & 0x7FFFF;  // Mask off sign bit (19 bits)
    
    // Temperature: divide by 10000, then by 10 => first 3 digits / 10
    // Humidity: modulo 1000, then divide by 10 => last 3 digits / 10
    *temp = (float)(iTemp / 1000) / 10.0f;
    if (bNegative) *temp = -*temp;
    
    *hum = (float)(iTemp % 1000) / 10.0f;
    *battery = data[4];
    
    return true;
}

/**
 * @brief Decode Govee H5179 manufacturer data (9 bytes, Manufacturer ID 0xec88)
 * Format: [0] [1] [2] [3] [temp_low] [temp_high] [hum_low] [hum_high] [battery]
 */
static bool govee_decode_h5179(const uint8_t* data, size_t len, float* temp, float* hum, uint8_t* battery) {
    if (len < 9) return false;
    
    // From GoveeBTTempLogger: temp/hum at bytes 4-7 as 16-bit LE
    int16_t iTemp = (int16_t)(data[5] << 8 | data[4]);
    int iHum = (int)(data[7] << 8 | data[6]);
    *temp = (float)iTemp / 100.0f;
    *hum = (float)iHum / 100.0f;
    *battery = data[8];
    return true;
}

/**
 * @brief Decode Govee H5177/H5174 manufacturer data (6 bytes, Manufacturer ID 0x0001)
 * Same 3-byte encoding as H5075
 */
static bool govee_decode_h5177(const uint8_t* data, size_t len, float* temp, float* hum, uint8_t* battery) {
    if (len < 6) return false;
    
    // Same encoding as H5075 but with different offsets
    int32_t iTemp = ((int32_t)data[2] << 16) | ((int32_t)data[3] << 8) | (int32_t)data[4];
    
    bool bNegative = (iTemp & 0x800000) != 0;
    iTemp = iTemp & 0x7FFFF;
    
    *temp = (float)(iTemp / 1000) / 10.0f;
    if (bNegative) *temp = -*temp;
    
    *hum = (float)(iTemp % 1000) / 10.0f;
    *battery = data[5];
    
    return true;
}

/**
 * @brief Try to decode Govee advertisement data
 * @param manufacturer_id 16-bit manufacturer ID from BLE advertisement
 * @param data Manufacturer data bytes
 * @param len Length of data
 * @param device_name Device name (for type detection)
 * @param out_temp Output: temperature
 * @param out_hum Output: humidity
 * @param out_battery Output: battery percentage
 * @param out_type Output: detected sensor type
 * @return true if successfully decoded
 */
static bool govee_decode_adv_data(uint16_t manufacturer_id, const uint8_t* data, size_t len,
                                   const char* device_name,
                                   float* out_temp, float* out_hum, uint8_t* out_battery,
                                   BLESensorType* out_type) {
    if (!data || len < 5) return false;
    
    // First try to detect type from name
    BLESensorType type = govee_detect_type_from_name(device_name);
    
    // If type unknown, try to detect from manufacturer ID and data length
    if (type == BLE_TYPE_UNKNOWN) {
        if (manufacturer_id == 0xec88) {
            if (len == 7) type = BLE_TYPE_GOVEE_H5074;
            else if (len == 6) type = BLE_TYPE_GOVEE_H5075;
            else if (len == 9) type = BLE_TYPE_GOVEE_H5179;
        } else if (manufacturer_id == 0x0001 && len == 6) {
            type = BLE_TYPE_GOVEE_H5177;
        }
    }
    
    bool success = false;
    switch (type) {
        case BLE_TYPE_GOVEE_H5074:
            success = govee_decode_h5074(data, len, out_temp, out_hum, out_battery);
            break;
        case BLE_TYPE_GOVEE_H5075:
            success = govee_decode_h5075(data, len, out_temp, out_hum, out_battery);
            break;
        case BLE_TYPE_GOVEE_H5179:
            success = govee_decode_h5179(data, len, out_temp, out_hum, out_battery);
            break;
        case BLE_TYPE_GOVEE_H5177:
            success = govee_decode_h5177(data, len, out_temp, out_hum, out_battery);
            break;
        default:
            return false;
    }
    
    if (success && out_type) {
        *out_type = type;
    }
    
    return success;
}

// ============================================================================
// BMS (Battery Management System) Decoders
// Based on: https://github.com/patman15/BMS_BLE-HA (aiobmsble)
// BMS sensors require GATT bidirectional communication (connect, write, read)
// ============================================================================

// JBD BMS Protocol Constants (very common in LiFePO4 batteries)
// Also known as Xiaoxiang BMS, Overkill Solar, etc.
static const uint8_t JBD_CMD_READ_BASIC     = 0x03;  // Read basic info (voltage, current, SOC)
static const uint8_t JBD_CMD_READ_CELLS     = 0x04;  // Read cell voltages
static const uint8_t JBD_HEAD_CMD           = 0xDD;  // Command header
static const uint8_t JBD_HEAD_RSP           = 0xDD;  // Response header
static const uint8_t JBD_READ_FLAG          = 0xA5;  // Read command flag
static const uint8_t JBD_TAIL               = 0x77;  // End byte

// JBD BMS GATT UUIDs (16-bit form, needs to be expanded to full 128-bit)
static const char* JBD_SERVICE_UUID  = "0000ff00-0000-1000-8000-00805f9b34fb";
static const char* JBD_TX_CHAR_UUID  = "0000ff02-0000-1000-8000-00805f9b34fb";  // Write to this
static const char* JBD_RX_CHAR_UUID  = "0000ff01-0000-1000-8000-00805f9b34fb";  // Read from this

/**
 * @brief Build a JBD BMS read command
 * @param cmd Command byte (0x03=basic info, 0x04=cell voltages)
 * @param out_buf Output buffer (must be at least 7 bytes)
 * @return Length of command
 */
static uint8_t jbd_build_command(uint8_t cmd, uint8_t* out_buf) {
    // Format: DD A5 cmd 00 FF(checksum) 77
    out_buf[0] = JBD_HEAD_CMD;
    out_buf[1] = JBD_READ_FLAG;
    out_buf[2] = cmd;
    out_buf[3] = 0x00;  // Length = 0 for read commands
    
    // Checksum: 0x10000 - (A5 + cmd + len)
    uint16_t checksum = 0x10000 - (JBD_READ_FLAG + cmd + 0x00);
    out_buf[4] = (checksum >> 8) & 0xFF;
    out_buf[5] = checksum & 0xFF;
    out_buf[6] = JBD_TAIL;
    
    return 7;
}

/**
 * @brief Parse JBD BMS basic info response (command 0x03)
 * @param data Response data (after header validation)
 * @param len Data length
 * @param out_voltage Total pack voltage (V)
 * @param out_current Current (A, positive=charging)
 * @param out_soc State of charge (%)
 * @param out_temp Average temperature (°C)
 * @param out_cycles Charge cycles
 * @return true if successfully parsed
 * 
 * Response format (34 bytes typical):
 * [0-1] Total voltage (mV, big-endian)
 * [2-3] Current (10mA, big-endian, signed)
 * [4-5] Remaining capacity (10mAh)
 * [6-7] Nominal capacity (10mAh)
 * [8-9] Cycles
 * [10-11] Production date
 * [12-15] Balance status
 * [16-17] Protection status
 * [18] Software version
 * [19] SOC (%)
 * [20] FET status
 * [21] Cell count
 * [22] NTC count
 * [23+] NTC temperatures (2 bytes each, 0.1K, offset 2731)
 */
static bool jbd_parse_basic_info(const uint8_t* data, size_t len, 
                                  float* out_voltage, float* out_current, 
                                  uint8_t* out_soc, float* out_temp, 
                                  uint16_t* out_cycles) {
    if (len < 23) return false;
    
    // Total voltage: bytes 0-1, big-endian, in 10mV units
    uint16_t voltage_raw = ((uint16_t)data[0] << 8) | data[1];
    *out_voltage = (float)voltage_raw / 100.0f;  // Convert to V
    
    // Current: bytes 2-3, big-endian, signed, in 10mA units
    int16_t current_raw = ((int16_t)data[2] << 8) | data[3];
    *out_current = (float)current_raw / 100.0f;  // Convert to A
    
    // Cycles: bytes 8-9
    *out_cycles = ((uint16_t)data[8] << 8) | data[9];
    
    // SOC: byte 19
    *out_soc = data[19];
    
    // Temperature: byte 22 = NTC count, then 2 bytes per NTC
    uint8_t ntc_count = data[22];
    if (ntc_count > 0 && len >= 23 + ntc_count * 2) {
        // Read first NTC temperature
        uint16_t temp_raw = ((uint16_t)data[23] << 8) | data[24];
        // Temperature in 0.1K, offset 2731 (273.1K = 0°C)
        *out_temp = ((float)temp_raw - 2731.0f) / 10.0f;
    } else {
        *out_temp = 0.0f;
    }
    
    return true;
}

/**
 * @brief Validate JBD BMS response frame
 * @param data Full response including header
 * @param len Total length
 * @param out_payload Pointer to payload start
 * @param out_payload_len Payload length
 * @return true if frame is valid
 * 
 * Frame format: DD cmd status len [payload...] checksum_hi checksum_lo 77
 */
static bool jbd_validate_response(const uint8_t* data, size_t len,
                                   const uint8_t** out_payload, size_t* out_payload_len) {
    if (len < 7) return false;  // Minimum frame size
    
    // Check header and tail
    if (data[0] != JBD_HEAD_RSP) return false;
    if (data[len-1] != JBD_TAIL) return false;
    
    // Check status (byte 2): 0x00 = OK
    if (data[2] != 0x00) return false;
    
    // Get payload length (byte 3)
    uint8_t payload_len = data[3];
    if (len < (size_t)(payload_len + 7)) return false;
    
    // Verify checksum
    uint16_t checksum_calc = 0;
    for (size_t i = 2; i < 4 + payload_len; i++) {
        checksum_calc += data[i];
    }
    checksum_calc = 0x10000 - checksum_calc;
    
    uint16_t checksum_recv = ((uint16_t)data[4 + payload_len] << 8) | data[5 + payload_len];
    if (checksum_calc != checksum_recv) return false;
    
    *out_payload = &data[4];
    *out_payload_len = payload_len;
    return true;
}

/**
 * @brief Detect BMS type from device name
 * @param name Device name (e.g. "xiaoxiang BMS", "JBD-...", "DL-...")
 * @return Sensor type enum
 */
static BLESensorType bms_detect_type_from_name(const char* name) {
    if (!name || !name[0]) return BLE_TYPE_UNKNOWN;
    
    // JBD/Xiaoxiang BMS variants
    if (strstr(name, "xiaoxiang") || strstr(name, "Xiaoxiang") ||
        strstr(name, "JBD") || strstr(name, "jbd") ||
        strstr(name, "SP0") || strstr(name, "SP1") ||  // Overkill Solar
        strstr(name, "GJ-") || strstr(name, "SL-"))    // Various rebrands
        return BLE_TYPE_BMS_JBD;
    
    // Daly BMS
    if (strstr(name, "DL-") || strstr(name, "Daly"))
        return BLE_TYPE_BMS_DALY;
    
    // ANT BMS
    if (strstr(name, "ANT-") || strstr(name, "Ant BMS"))
        return BLE_TYPE_BMS_ANT;
    
    // JK/Jikong BMS
    if (strstr(name, "JK-") || strstr(name, "JK_") || strstr(name, "Jikong"))
        return BLE_TYPE_BMS_JIKONG;
    
    return BLE_TYPE_UNKNOWN;
}

/**
 * @brief Check if sensor type is a BMS
 */
static bool sensor_ble_is_bms_type(BLESensorType type) {
    return type == BLE_TYPE_BMS_JBD ||
           type == BLE_TYPE_BMS_DALY ||
           type == BLE_TYPE_BMS_ANT ||
           type == BLE_TYPE_BMS_JIKONG;
}

static void ble_sanitize_json_string(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    dst[0] = 0;

    if (!src || *src == 0) {
        strncpy(dst, "Unknown", dst_len - 1);
        dst[dst_len - 1] = 0;
        return;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != 0 && di < dst_len - 1; si++) {
        unsigned char c = (unsigned char)src[si];

        // Drop ASCII control chars (incl. \n/\r/\t) and DEL
        if (c < 0x20 || c == 0x7F) {
            continue;
        }

        // Keep it ASCII-printable only to avoid invalid UTF-8 in JSON consumers
        if (c > 0x7E) {
            continue;
        }

        // Prevent breaking JSON if downstream formatter does not escape
        if (c == '"' || c == '\\') {
            dst[di++] = '\'';
            continue;
        }

        dst[di++] = (char)c;
    }
    dst[di] = 0;

    // If everything got stripped, fall back
    if (dst[0] == 0) {
        strncpy(dst, "Unknown", dst_len - 1);
        dst[dst_len - 1] = 0;
    }
}

static void ble_parse_uuid_and_format_legacy(const char* unit_str, char* out_uuid, size_t out_uuid_len, uint8_t* out_fmt) {
    if (out_uuid && out_uuid_len) {
        out_uuid[0] = 0;
    }
    if (out_fmt) {
        *out_fmt = (uint8_t)FORMAT_TEMP_001;
    }
    if (!unit_str || !unit_str[0] || !out_uuid || out_uuid_len == 0) {
        return;
    }

    char tmp[64] = {0};
    strncpy(tmp, unit_str, sizeof(tmp) - 1);

    char* pipe = strchr(tmp, '|');
    if (pipe) {
        *pipe = 0;
        pipe++;
        int fmt = atoi(pipe);
        if (fmt >= 0 && fmt <= 30 && out_fmt) {
            *out_fmt = (uint8_t)fmt;
        }
    }

    // copy UUID part
    strncpy(out_uuid, tmp, out_uuid_len - 1);
    out_uuid[out_uuid_len - 1] = 0;
}

static bool ble_is_mac_string(const char* s) {
    if (!s) return false;
    // Expected form: "AA:BB:CC:DD:EE:FF" (17 chars)
    if (strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) {
            if (s[i] != ':') return false;
        } else {
            if (!isxdigit((unsigned char)s[i])) return false;
        }
    }
    return true;
}

static void ble_copy_stripped(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return;
    dst[0] = 0;
    if (!src) return;

    size_t di = 0;
    for (size_t si = 0; src[si] != 0 && di < dst_len - 1; si++) {
        char c = src[si];
        // Stop at legacy "UUID|fmt" delimiter
        if (c == '|') break;
        // Strip whitespace/control characters
        if (c == '\r' || c == '\n' || c == '\t' || isspace((unsigned char)c)) continue;
        dst[di++] = c;
    }
    dst[di] = 0;
}

void BLESensor::fromJson(ArduinoJson::JsonVariantConst obj) {
    SensorBase::fromJson(obj);

    // Parse MAC address (preferred: "mac" field)
    if (obj.containsKey("mac")) {
        const char* m = obj["mac"].as<const char*>();
        if (m) {
            ble_copy_stripped(mac_address_cfg, sizeof(mac_address_cfg), m);
        }
    } else if (ble_is_mac_string(name)) {
        // Backward-compat: legacy configs used name as MAC. Copy into dedicated field.
        ble_copy_stripped(mac_address_cfg, sizeof(mac_address_cfg), name);
    }

    // Optional explicit BLE config (preferred)
    if (obj.containsKey("char_uuid")) {
        const char* u = obj["char_uuid"].as<const char*>();
        if (u) {
            ble_copy_stripped(characteristic_uuid_cfg, sizeof(characteristic_uuid_cfg), u);
        }
    } else if (obj.containsKey("uuid")) {
        const char* u = obj["uuid"].as<const char*>();
        if (u) {
            ble_copy_stripped(characteristic_uuid_cfg, sizeof(characteristic_uuid_cfg), u);
        }
    }

    if (obj.containsKey("format")) {
        int fmt = obj["format"].as<int>();
        if (fmt >= 0 && fmt <= 30) {
            payload_format_cfg = (uint8_t)fmt;
        }
    }

    // Backward-compat: parse legacy `unit` field (userdef_unit) if no explicit uuid provided
    if (!characteristic_uuid_cfg[0] && userdef_unit[0]) {
        uint8_t fmt = (uint8_t)FORMAT_TEMP_001;
        ble_parse_uuid_and_format_legacy(userdef_unit, characteristic_uuid_cfg, sizeof(characteristic_uuid_cfg), &fmt);
        payload_format_cfg = fmt;
        // Ensure legacy parsing didn't leave whitespace/control chars
        char cleaned[sizeof(characteristic_uuid_cfg)] = {0};
        ble_copy_stripped(cleaned, sizeof(cleaned), characteristic_uuid_cfg);
        strncpy(characteristic_uuid_cfg, cleaned, sizeof(characteristic_uuid_cfg) - 1);
        characteristic_uuid_cfg[sizeof(characteristic_uuid_cfg) - 1] = 0;
    }
    
    // Note: For advertisement-based sensors (Govee etc.), the value selection
    // is now based on assigned_unitid from the base class:
    // - Unit 2 (°C) or 3 (°F) → temperature
    // - Unit 5 (%) → humidity  
    // - Other units → battery
}

void BLESensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    if (!obj) return;

    // BLE-specific fields
    if (mac_address_cfg[0]) obj["mac"] = mac_address_cfg;
    if (characteristic_uuid_cfg[0]) obj["char_uuid"] = characteristic_uuid_cfg;
    if (payload_format_cfg != (uint8_t)FORMAT_TEMP_001) obj["format"] = payload_format_cfg;
    // Note: assigned_unitid is handled by SensorBase::toJson
}

static bool ble_uuid_extract_16bit(const char* uuid_in, uint16_t* out_uuid16) {
    if (!uuid_in || !out_uuid16) return false;

    // Copy until '|' or end, trim leading/trailing whitespace
    char buf[64];
    size_t n = 0;
    while (uuid_in[n] && uuid_in[n] != '|' && n < sizeof(buf) - 1) {
        buf[n] = uuid_in[n];
        n++;
    }
    buf[n] = '\0';

    // Trim left
    char* s = buf;
    while (*s && isspace((unsigned char)*s)) s++;
    // Trim right
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end[-1] = '\0';
        end--;
    }

    if (*s == '\0') return false;

    // Accept 0xNNNN
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    // Check Bluetooth base UUID form: 0000xxxx-0000-1000-8000-00805f9b34fb
    // (case-insensitive)
    const char* base_suffix = "-0000-1000-8000-00805f9b34fb";
    const size_t base_suffix_len = strlen(base_suffix);
    size_t s_len = strlen(s);
    if (s_len == 36) {
        // Must end with base suffix
        const char* suffix = s + (36 - base_suffix_len);
        bool suffix_ok = true;
        for (size_t i = 0; i < base_suffix_len; i++) {
            char a = (char)tolower((unsigned char)suffix[i]);
            char b = (char)tolower((unsigned char)base_suffix[i]);
            if (a != b) { suffix_ok = false; break; }
        }
        // Must start with 0000 + 4 hex
        if (suffix_ok &&
            s[0] == '0' && s[1] == '0' && s[2] == '0' && s[3] == '0' &&
            isxdigit((unsigned char)s[4]) && isxdigit((unsigned char)s[5]) &&
            isxdigit((unsigned char)s[6]) && isxdigit((unsigned char)s[7]) &&
            s[8] == '-') {
            char hex4[5] = { s[4], s[5], s[6], s[7], 0 };
            *out_uuid16 = (uint16_t)strtoul(hex4, nullptr, 16);
            return true;
        }
    }

    // Accept plain 4-hex UUIDs
    if (strlen(s) == 4 &&
        isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) &&
        isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3])) {
        *out_uuid16 = (uint16_t)strtoul(s, nullptr, 16);
        return true;
    }

    return false;
}

static const char* ble_uuid16_to_name(uint16_t uuid16) {
    switch (uuid16) {
        // Common services
        case 0x1800: return "Generic Access";
        case 0x1801: return "Generic Attribute";
        case 0x180A: return "Device Information";
        case 0x180F: return "Battery Service";
        case 0x181A: return "Environmental Sensing";

        // Common characteristics
        case 0x2A00: return "Device Name";
        case 0x2A01: return "Appearance";
        case 0x2A19: return "Battery Level";
        case 0x2A24: return "Model Number String";
        case 0x2A25: return "Serial Number String";
        case 0x2A26: return "Firmware Revision String";
        case 0x2A27: return "Hardware Revision String";
        case 0x2A28: return "Software Revision String";
        case 0x2A29: return "Manufacturer Name String";
        case 0x2A6D: return "Pressure";
        case 0x2A6E: return "Temperature";
        case 0x2A6F: return "Humidity";
        case 0x2A73: return "Barometric Pressure Trend";

        default: return "Unknown";
    }
}

const char* ble_uuid_to_name(const char* uuid) {
    uint16_t uuid16 = 0;
    if (ble_uuid_extract_16bit(uuid, &uuid16)) {
        return ble_uuid16_to_name(uuid16);
    }
    return "Unknown";
}

/**
 * @brief Callback class for BLE scan results
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        DEBUG_PRINT("BLE Device found: ");
        DEBUG_PRINTLN(advertisedDevice.getAddress().toString().c_str());

        // Get device address (format: "aa:bb:cc:dd:ee:ff")
        String addr_str = advertisedDevice.getAddress().toString();
        uint8_t addr_bytes[6];
        
        // Parse address string to bytes
        if (sscanf(addr_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &addr_bytes[0], &addr_bytes[1], &addr_bytes[2],
                   &addr_bytes[3], &addr_bytes[4], &addr_bytes[5]) != 6) {
            return; // Invalid address format
        }
        
        // Get device name
        char device_name[32] = "Unknown";
        if (advertisedDevice.haveName()) {
            ble_sanitize_json_string(device_name, sizeof(device_name), advertisedDevice.getName().c_str());
        }
        
        // Try to decode Govee advertisement data
        float adv_temp = 0, adv_hum = 0;
        uint8_t adv_battery = 0;
        BLESensorType sensor_type = BLE_TYPE_UNKNOWN;
        bool has_adv_data = false;
        
        // Check for manufacturer data (where Govee sends sensor readings)
        if (advertisedDevice.haveManufacturerData()) {
            String mfg_data = advertisedDevice.getManufacturerData();
            if (mfg_data.length() >= 2) {
                // First 2 bytes are manufacturer ID (little-endian)
                uint16_t mfg_id = (uint8_t)mfg_data[0] | ((uint8_t)mfg_data[1] << 8);
                const uint8_t* payload = (const uint8_t*)mfg_data.c_str() + 2;
                size_t payload_len = mfg_data.length() - 2;
                
                DEBUG_PRINT("  Manufacturer ID: 0x");
                DEBUG_PRINT(String(mfg_id, HEX).c_str());
                DEBUG_PRINT(" Data len: ");
                DEBUG_PRINTLN(payload_len);
                
                has_adv_data = govee_decode_adv_data(mfg_id, payload, payload_len, device_name,
                                                     &adv_temp, &adv_hum, &adv_battery, &sensor_type);
                
                if (has_adv_data) {
                    DEBUG_PRINT("  -> Govee decoded: ");
                    DEBUG_PRINT(adv_temp);
                    DEBUG_PRINT("C, ");
                    DEBUG_PRINT(adv_hum);
                    DEBUG_PRINT("%, Bat:");
                    DEBUG_PRINTLN(adv_battery);
                }
            }
        }
        
        // Also try to detect type from name if not yet detected
        if (sensor_type == BLE_TYPE_UNKNOWN) {
            sensor_type = govee_detect_type_from_name(device_name);
        }
        
        // Try to detect BMS from name if not yet detected
        if (sensor_type == BLE_TYPE_UNKNOWN) {
            sensor_type = bms_detect_type_from_name(device_name);
        }
        
        // Check service UUID for ec88 (Govee indicator) or ff00 (JBD BMS indicator)
        char service_uuid_str[40] = {0};
        if (advertisedDevice.haveServiceUUID()) {
            String svc = advertisedDevice.getServiceUUID().toString();
            strncpy(service_uuid_str, svc.c_str(), sizeof(service_uuid_str) - 1);
            
            // ec88 is the Govee service UUID
            if (svc.indexOf("ec88") >= 0 || svc.indexOf("EC88") >= 0) {
                if (sensor_type == BLE_TYPE_UNKNOWN) {
                    sensor_type = BLE_TYPE_GOVEE_H5075; // Default Govee type
                }
            }
            
            // ff00 is the JBD BMS service UUID
            if (svc.indexOf("ff00") >= 0 || svc.indexOf("FF00") >= 0) {
                if (sensor_type == BLE_TYPE_UNKNOWN) {
                    sensor_type = BLE_TYPE_BMS_JBD;
                }
            }
            
            DEBUG_PRINT("  Service UUID: ");
            DEBUG_PRINT(svc.c_str());
            DEBUG_PRINT(" (");
            DEBUG_PRINT(ble_uuid_to_name(svc.c_str()));
            DEBUG_PRINTLN(")");
        }
        
        // FILTER: Only add devices with known sensor type OR advertisement data
        // This filters out generic BLE devices like phones, headphones, etc.
        if (sensor_type == BLE_TYPE_UNKNOWN && !has_adv_data) {
            // Check if name looks like a sensor
            if (strstr(device_name, "GVH") == nullptr &&
                strstr(device_name, "Govee") == nullptr &&
                strstr(device_name, "LYWSD") == nullptr &&
                strstr(device_name, "MJ_HT") == nullptr &&
                strstr(device_name, "ATC_") == nullptr &&
                strstr(device_name, "Temp") == nullptr &&
                strstr(device_name, "Thermo") == nullptr &&
                // BMS patterns
                strstr(device_name, "BMS") == nullptr &&
                strstr(device_name, "xiaoxiang") == nullptr &&
                strstr(device_name, "JBD") == nullptr &&
                strstr(device_name, "DL-") == nullptr &&
                strstr(device_name, "JK-") == nullptr &&
                strstr(device_name, "ANT-") == nullptr &&
                strstr(device_name, "SP0") == nullptr &&
                strstr(device_name, "SP1") == nullptr) {
                DEBUG_PRINTLN("  -> Filtered out (not a known sensor)");
                return; // Skip this device
            }
            sensor_type = BLE_TYPE_GENERIC_GATT; // Mark as generic for later GATT read
        }
        
        // Check if device already exists in list
        bool already_exists = false;
        BLEDeviceInfo* existing_device = nullptr;
        for (auto& dev : discovered_ble_devices) {
            if (memcmp(dev.address, addr_bytes, 6) == 0) {
                already_exists = true;
                existing_device = &dev;
                break;
            }
        }
        
        if (already_exists) {
            // Update existing device
            existing_device->rssi = advertisedDevice.getRSSI();
            existing_device->last_seen = millis();
            existing_device->is_new = true;
            
            strncpy(existing_device->name, device_name, sizeof(existing_device->name) - 1);
            
            if (service_uuid_str[0]) {
                strncpy(existing_device->service_uuid, service_uuid_str, sizeof(existing_device->service_uuid) - 1);
            }
            
            // Update sensor data if available
            if (has_adv_data) {
                existing_device->adv_temperature = adv_temp;
                existing_device->adv_humidity = adv_hum;
                existing_device->adv_battery = adv_battery;
                existing_device->has_adv_data = true;
            }
            if (sensor_type != BLE_TYPE_UNKNOWN) {
                existing_device->sensor_type = sensor_type;
            }
        } else {
            // Add new device
            BLEDeviceInfo new_dev;
            memset(&new_dev, 0, sizeof(new_dev));
            memcpy(new_dev.address, addr_bytes, 6);
            strncpy(new_dev.name, device_name, sizeof(new_dev.name) - 1);
            
            new_dev.rssi = advertisedDevice.getRSSI();
            new_dev.is_new = true;
            new_dev.last_seen = millis();
            
            if (service_uuid_str[0]) {
                strncpy(new_dev.service_uuid, service_uuid_str, sizeof(new_dev.service_uuid) - 1);
            }
            
            new_dev.sensor_type = sensor_type;
            new_dev.adv_temperature = adv_temp;
            new_dev.adv_humidity = adv_hum;
            new_dev.adv_battery = adv_battery;
            new_dev.has_adv_data = has_adv_data;
            
            discovered_ble_devices.push_back(new_dev);

            DEBUG_PRINT("New BLE sensor added: ");
            DEBUG_PRINT(new_dev.name);
            DEBUG_PRINT(" type=");
            DEBUG_PRINTLN((int)sensor_type);
        }
    }
};

static MyAdvertisedDeviceCallbacks ble_scan_callbacks;

/**
 * @brief Initialize BLE sensor subsystem
 */
void sensor_ble_init() {
    if (ble_initialized) {
        return;
    }
    
    // Note: WiFi mode check removed from init - will be checked in read() instead
    // This prevents exceptions when sensor_api_init() is called before WiFi is started

    DEBUG_PRINTLN("Initializing BLE...");
    
    // Initialize BLE
    BLEDevice::init("OpenSprinkler");

    DEBUG_PRINTLN("BLE initialized");
    
    ble_initialized = true;
}

static BLEClient* ble_get_client() {
    if (!ble_initialized) return nullptr;
    if (!ble_client) {
        ble_client = BLEDevice::createClient();
    }
    return ble_client;
}

static void sensor_ble_stop_now() {
    if (!ble_initialized) {
        return;
    }

    // Stop scanning if active
    if (scanning_active && pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        scanning_active = false;
    }

    // Disconnect client if present (never delete it manually)
    if (ble_client) {
        ble_client->disconnect();
        ble_client = nullptr;
    }

    BLEDevice::deinit(false);
    ble_initialized = false;
    pBLEScan = nullptr;

    DEBUG_PRINTLN("BLE stopped - RF resources freed for WiFi");
}

/**
 * @brief Stop BLE subsystem (frees RF resources)
 */
void sensor_ble_stop() {
    if (!ble_initialized) {
        return;
    }
    // Avoid deinit/free from within BLE callbacks or immediately after connect failures.
    // Defer stop to the maintenance loop.
    if (!ble_stop_requested) {
        DEBUG_PRINTLN("Stopping BLE to free RF resources...");
    }
    ble_stop_requested = true;
    ble_stop_request_time = millis();
}

/**
 * @brief Start BLE scanning
 */
void sensor_ble_start_scan(uint16_t duration) {
    if (!ble_initialized) {
        sensor_ble_init();
        if (!ble_initialized) {
            return;
        }
    }

    if (!pBLEScan) {
        // Configure scan only when discovery is requested.
        pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks);
        pBLEScan->setActiveScan(true); // Active scan uses more power but gets more info
        pBLEScan->setInterval(100);
        pBLEScan->setWindow(99);
    }
    
    if (scanning_active) {
        return;
    }
    
    // Limit discovery time to max 10 seconds to minimize WiFi interference
    const uint16_t MAX_DISCOVERY_TIME = 10;
    uint16_t actual_duration = (duration > MAX_DISCOVERY_TIME) ? MAX_DISCOVERY_TIME : duration;

    DEBUG_PRINT("Starting BLE scan for ");
    DEBUG_PRINT(actual_duration);
    DEBUG_PRINTLN(" seconds (will auto-stop to free WiFi RF)...");
    
    // Clear old scan results
    pBLEScan->clearResults();
    
    // Start scan (non-blocking)
    pBLEScan->start(actual_duration, false);
    scanning_active = true;
    scan_end_time = millis() + (actual_duration * 1000);
    
    // Set auto-stop timer
    ble_discovery_mode = true;
    ble_auto_stop_time = millis() + (actual_duration * 1000UL);
}

/**
 * @brief BLE maintenance loop
 */
void sensor_ble_loop() {
    if (!ble_initialized) {
        return;
    }
    
    uint32_t now = millis();

    // Deferred stop (avoids heap corruption when deinit happens during/after BLE stack activity)
    if (ble_stop_requested && !ble_in_read) {
        if ((uint32_t)(now - ble_stop_request_time) >= 200) {
            ble_stop_requested = false;
            sensor_ble_stop_now();
            ble_discovery_mode = false;
            ble_auto_stop_time = 0;
            scanning_active = false;
            return;
        }
    }
    
    // Check if discovery mode auto-stop timer expired
    if (ble_discovery_mode && ble_auto_stop_time > 0) {
        if (now >= ble_auto_stop_time) {
            DEBUG_PRINTLN("BLE discovery time expired - stopping BLE to free WiFi RF");
            sensor_ble_stop();
            ble_discovery_mode = false;
            ble_auto_stop_time = 0;
            scanning_active = false;
            return; // Exit early since we just stopped
        }
    }
    
    // Check if scan has completed
    if (scanning_active && now >= scan_end_time) {
        if (pBLEScan->isScanning()) {
            pBLEScan->stop();
        }
        scanning_active = false;
        DEBUG_PRINT("BLE scan completed. Found ");
        DEBUG_PRINT(discovered_ble_devices.size());
        DEBUG_PRINTLN(" devices.");
    }
    
    // Remove stale devices (not seen in 5 minutes)
    discovered_ble_devices.erase(
        std::remove_if(discovered_ble_devices.begin(), discovered_ble_devices.end(),
            [now](const BLEDeviceInfo& dev) {
                return (now - dev.last_seen > 300000);  // 5 minutes
            }),
        discovered_ble_devices.end()
    );
}

bool sensor_ble_is_active() {
    return ble_initialized;
}

/**
 * @brief Get list of discovered BLE devices
 */
int sensor_ble_get_discovered_devices(BLEDeviceInfo* devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;
    
    int count = (discovered_ble_devices.size() < (size_t)max_devices) ? discovered_ble_devices.size() : max_devices;
    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &discovered_ble_devices[i], sizeof(BLEDeviceInfo));
    }
    return count;
}

/**
 * @brief Clear new device flags
 */
void sensor_ble_clear_new_device_flags() {
    for (auto& dev : discovered_ble_devices) {
        dev.is_new = false;
    }
}

/**
 * @brief Find a cached device by MAC address
 */
const BLEDeviceInfo* sensor_ble_find_device(const char* mac_address) {
    if (!mac_address || !mac_address[0]) return nullptr;
    
    uint8_t addr_bytes[6];
    if (sscanf(mac_address, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &addr_bytes[0], &addr_bytes[1], &addr_bytes[2],
               &addr_bytes[3], &addr_bytes[4], &addr_bytes[5]) != 6) {
        return nullptr;
    }
    
    for (const auto& dev : discovered_ble_devices) {
        if (memcmp(dev.address, addr_bytes, 6) == 0) {
            return &dev;
        }
    }
    return nullptr;
}

/**
 * @brief Check if a device has advertisement-based sensor data
 */
bool sensor_ble_is_adv_sensor(const BLEDeviceInfo* device) {
    if (!device) return false;
    return device->has_adv_data || 
           device->sensor_type == BLE_TYPE_GOVEE_H5074 ||
           device->sensor_type == BLE_TYPE_GOVEE_H5075 ||
           device->sensor_type == BLE_TYPE_GOVEE_H5177 ||
           device->sensor_type == BLE_TYPE_GOVEE_H5179 ||
           device->sensor_type == BLE_TYPE_GOVEE_MEAT ||
           device->sensor_type == BLE_TYPE_XIAOMI;
}

/**
 * @brief Store sensor result - centralized method to set all result fields
 * @param value The measured value (already in correct unit, e.g., 34.59 for °C)
 * @param time Current timestamp
 */
void BLESensor::store_result(double value, unsigned long time) {
    flags.data_ok = 1;
    last_data = value;                      // Store as-is (e.g., 34.59 for 34.59°C)
    last_native_data = (int32_t)(value * 100.0);  // Native: integer representation
    last_read = time;
    repeat_data = last_data;
    repeat_native = last_native_data;
    repeat_read = 1;  // Signal that data is available
    
    DEBUG_PRINT(F("BLE sensor value: "));
    DEBUG_PRINTLN(value);
}

/**
 * @brief Select the appropriate value from cached device data based on assigned_unitid
 * @param cached_dev Cached device info with advertisement data
 * @return The selected value (temperature, humidity, or battery)
 */
static double ble_select_adv_value(const BLEDeviceInfo* cached_dev, unsigned char unitid) {
    // assigned_unitid determines which value to return:
    // Unit 2 (°C) → temperature (also default for Govee)
    // Unit 3 (°F) → temperature (convert from °C)
    // Unit 5 (%) → humidity  
    // Unit 10 (%) or higher → battery
    DEBUG_PRINT(F("ble_select_adv_value: unitid="));
    DEBUG_PRINT(unitid);
    DEBUG_PRINT(F(" temp="));
    DEBUG_PRINT(cached_dev->adv_temperature);
    DEBUG_PRINT(F(" hum="));
    DEBUG_PRINT(cached_dev->adv_humidity);
    DEBUG_PRINT(F(" bat="));
    DEBUG_PRINTLN(cached_dev->adv_battery);
    
    double result = 0.0;
    switch (unitid) {
        case 0: // Default - return temperature for Govee sensors
        case 2: // °C - Temperature
            result = cached_dev->adv_temperature;
            break;
        case 3: // °F - Temperature (convert from °C)
            result = cached_dev->adv_temperature * 9.0 / 5.0 + 32.0;
            break;
        case 5: // % - Humidity
            result = cached_dev->adv_humidity;
            break;
        default: // Battery (unit 10 or other)
            result = (double)cached_dev->adv_battery;
            break;
    }
    DEBUG_PRINT(F("  -> selected: "));
    DEBUG_PRINTLN(result);
    return result;
}

/**
 * @brief Select the appropriate value from BMS data based on assigned_unitid
 * @param voltage Pack voltage
 * @param current Pack current
 * @param soc State of charge
 * @param temperature Pack temperature
 * @param unitid Unit ID from sensor config
 * @return The selected value
 */
static double ble_select_bms_value(float voltage, float current, uint8_t soc, 
                                    float temperature, unsigned char unitid) {
    // Unit 4 (V) → voltage
    // Unit 2/3 (°C/°F) → temperature
    // Unit 5/10 (%) → SOC
    // Other → current (A)
    switch (unitid) {
        case 4: // V - Voltage
            return voltage;
        case 2: // °C - Temperature
        case 3: // °F - Temperature
            return temperature;
        case 5:  // % - Humidity (used for SOC)
        case 10: // % - Level (used for SOC)
            return (double)soc;
        default: // Current (A) as default
            return current;
    }
}

/**
 * @brief Read value from BLE sensor
 * Power-saving mode: BLE is turned on/off dynamically
 * First call (repeat_read == 0): Turn on BLE, read data, set repeat_read = 1
 * Second call (repeat_read == 1): Return data, set repeat_read = 0, turn off BLE
 */
int BLESensor::read(unsigned long time) {
    if (!flags.enable) return HTTP_RQT_NOT_RECEIVED;
    
    // CRITICAL: Do NOT use BLE in WiFi SOFTAP mode (RF coexistence conflict)
    // Exception: Ethernet mode (no WiFi RF usage)
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        flags.data_ok = false;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }
    
    if (repeat_read == 0) {
        ble_in_read = true;

        auto fail = [&](BLEClient* client, bool stop_ble) -> int {
            flags.data_ok = false;
            last_read = time;
            if (client) client->disconnect();
            if (stop_ble) sensor_ble_stop();
            ble_in_read = false;
            return HTTP_RQT_NOT_RECEIVED;
        };

        auto fail_no_cleanup = [&]() -> int {
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        };

        auto cleanup = [&](BLEClient* client) {
            if (client) client->disconnect();
            sensor_ble_stop();
            ble_in_read = false;
        };

        // First call: Turn on BLE and read data
        DEBUG_PRINT("BLE sensor read (turn on): ");
        DEBUG_PRINTLN(name);
        
        // Start BLE if not running
        if (!ble_initialized) {
            sensor_ble_init();
            if (!ble_initialized) {
                return fail(nullptr, false);
            }
        }
    
        // Parse sensor configuration from JSON
        // Expected format in mac_address_cfg: MAC address (e.g., "AA:BB:CC:DD:EE:FF")
        // Expected format in userdef_unit: Characteristic UUID (optionally with format: "UUID|format_id")
        //   Example: "00002a1c-0000-1000-8000-00805f9b34fb" or "00002a1c-0000-1000-8000-00805f9b34fb|10"
        
        const char* mac_address = mac_address_cfg;
        if ((!mac_address || mac_address[0] == 0) && ble_is_mac_string(name)) {
            // Legacy fallback only if name is actually a MAC
            mac_address = name;
        }
        
        char characteristic_uuid[128] = {0};
        PayloadFormat format = (PayloadFormat)payload_format_cfg;

        // Preferred: explicit BLE config fields
        if (characteristic_uuid_cfg[0]) {
            ble_copy_stripped(characteristic_uuid, sizeof(characteristic_uuid), characteristic_uuid_cfg);
        } else if (userdef_unit && strlen(userdef_unit) > 0) {
            // Backward-compat: parse legacy userdef_unit field: "UUID" or "UUID|format"
            uint8_t fmt = (uint8_t)FORMAT_TEMP_001;
            ble_parse_uuid_and_format_legacy(userdef_unit, characteristic_uuid, sizeof(characteristic_uuid), &fmt);
            format = (PayloadFormat)fmt;
        }
        
        if (!mac_address || mac_address[0] == 0 || !ble_is_mac_string(mac_address)) {
            DEBUG_PRINTLN(F("ERROR: BLE MAC address not configured/invalid (mac field)"));
            flags.enable = false;
            return fail(nullptr, true);
        }
        
        // =====================================================================
        // ADVERTISEMENT-BASED SENSORS (Govee, Xiaomi, etc.)
        // These sensors broadcast data in their advertisements - no GATT needed
        // =====================================================================
        
        // Check if we have cached advertisement data for this device
        const BLEDeviceInfo* cached_dev = sensor_ble_find_device(mac_address);
        
        // If device not in cache OR is an adv-type sensor without fresh data, do a scan first
        bool needs_scan = false;
        if (!cached_dev) {
            // Device not in cache - need to scan to discover it
            DEBUG_PRINTLN(F("Device not in cache, starting discovery scan..."));
            needs_scan = true;
        } else if (sensor_ble_is_adv_sensor(cached_dev)) {
            // Known advertisement-based sensor - check if data is fresh
            if (!cached_dev->has_adv_data || (millis() - cached_dev->last_seen > 300000)) {
                DEBUG_PRINTLN(F("Advertisement sensor needs fresh data, starting scan..."));
                needs_scan = true;
            }
        }
        
        if (needs_scan) {
            DEBUG_PRINTLN(F("Starting BLE scan for sensor discovery..."));
            sensor_ble_start_scan(5); // 5 second scan
            
            // Wait for scan to complete (blocking, but short)
            uint32_t scan_start = millis();
            while (scanning_active && (millis() - scan_start < 6000)) {
                delay(100);
                sensor_ble_loop();
            }
            
            // Re-check cache after scan
            cached_dev = sensor_ble_find_device(mac_address);
            if (cached_dev) {
                DEBUG_PRINT(F("Found device after scan: "));
                DEBUG_PRINT(cached_dev->name);
                DEBUG_PRINT(F(" type="));
                DEBUG_PRINTLN((int)cached_dev->sensor_type);
            } else {
                DEBUG_PRINTLN(F("Device still not found after scan"));
            }
        }
        
        // Now check if we have usable advertisement data
        if (cached_dev && sensor_ble_is_adv_sensor(cached_dev) && cached_dev->has_adv_data) {
            // Use cached advertisement data (Govee etc.)
            DEBUG_PRINT(F("Using advertisement data from: "));
            DEBUG_PRINTLN(cached_dev->name);
            DEBUG_PRINT(F("  Temp: "));
            DEBUG_PRINT(cached_dev->adv_temperature);
            DEBUG_PRINT(F("C, Hum: "));
            DEBUG_PRINT(cached_dev->adv_humidity);
            DEBUG_PRINT(F("%, Bat: "));
            DEBUG_PRINTLN(cached_dev->adv_battery);
            
            // Check data freshness (must be seen in last 5 minutes)
            uint32_t now = millis();
            if (now - cached_dev->last_seen < 300000) {
                double parsed_value = ble_select_adv_value(cached_dev, assigned_unitid);
                store_result(parsed_value, time);
                
                sensor_ble_stop();
                ble_in_read = false;
                return HTTP_RQT_NOT_RECEIVED; // Will return data on next call
            } else {
                DEBUG_PRINTLN(F("Data too old even after scan"));
            }
        }
        
        // If it's a known advertisement sensor but we couldn't get data, don't try GATT
        if (cached_dev && sensor_ble_is_adv_sensor(cached_dev)) {
            DEBUG_PRINTLN(F("Advertisement sensor - skipping GATT (not supported)"));
            sensor_ble_stop();
            ble_in_read = false;
            return fail(nullptr, false);  // Will retry on next cycle
        }
        
        // =====================================================================
        // BMS SENSORS (Battery Management Systems)
        // Require GATT bidirectional communication (connect, write cmd, read response)
        // =====================================================================
        
        if (cached_dev && sensor_ble_is_bms_type(cached_dev->sensor_type)) {
            DEBUG_PRINT(F("Reading BMS sensor: "));
            DEBUG_PRINTLN(cached_dev->name);
            
            // Only JBD BMS is currently implemented
            if (cached_dev->sensor_type == BLE_TYPE_BMS_JBD) {
                BLEAddress bleAddress(mac_address);
                
                BLEClient* pClient = ble_get_client();
                if (!pClient) {
                    return fail(nullptr, false);
                }
                
                if (!pClient->connect(bleAddress, 0, BLE_CONNECT_TIMEOUT_MS)) {
                    DEBUG_PRINTLN(F("Failed to connect to JBD BMS"));
                    return fail(nullptr, true);
                }
                
                DEBUG_PRINTLN(F("Connected to JBD BMS"));
                
                // Get the BMS service
                BLERemoteService* pService = pClient->getService(BLEUUID(JBD_SERVICE_UUID));
                if (!pService) {
                    DEBUG_PRINTLN(F("JBD BMS service not found"));
                    return fail(pClient, true);
                }
                
                // Get TX characteristic (write commands here)
                BLERemoteCharacteristic* pTxChar = pService->getCharacteristic(BLEUUID(JBD_TX_CHAR_UUID));
                if (!pTxChar || !pTxChar->canWrite()) {
                    DEBUG_PRINTLN(F("JBD TX characteristic not found or not writable"));
                    return fail(pClient, true);
                }
                
                // Get RX characteristic (read responses here)
                BLERemoteCharacteristic* pRxChar = pService->getCharacteristic(BLEUUID(JBD_RX_CHAR_UUID));
                if (!pRxChar || !pRxChar->canRead()) {
                    DEBUG_PRINTLN(F("JBD RX characteristic not found or not readable"));
                    return fail(pClient, true);
                }
                
                // Build and send the "read basic info" command
                uint8_t cmd_buf[7];
                uint8_t cmd_len = jbd_build_command(JBD_CMD_READ_BASIC, cmd_buf);
                
                DEBUG_PRINTLN(F("Sending JBD read command..."));
                pTxChar->writeValue(cmd_buf, cmd_len);
                
                // Small delay for BMS to process
                delay(100);
                
                // Read response
                String response = pRxChar->readValue().c_str();
                
                if (response.length() < 7) {
                    DEBUG_PRINT(F("JBD response too short: "));
                    DEBUG_PRINTLN(response.length());
                    return fail(pClient, true);
                }
                
                DEBUG_PRINT(F("JBD response length: "));
                DEBUG_PRINTLN(response.length());
                
                // Validate and parse response
                const uint8_t* resp_data = (const uint8_t*)response.c_str();
                const uint8_t* payload = nullptr;
                size_t payload_len = 0;
                
                if (!jbd_validate_response(resp_data, response.length(), &payload, &payload_len)) {
                    DEBUG_PRINTLN(F("JBD response validation failed"));
                    return fail(pClient, true);
                }
                
                float voltage = 0, current = 0, temperature = 0;
                uint8_t soc = 0;
                uint16_t cycles = 0;
                
                if (!jbd_parse_basic_info(payload, payload_len, &voltage, &current, &soc, &temperature, &cycles)) {
                    DEBUG_PRINTLN(F("JBD parse failed"));
                    return fail(pClient, true);
                }
                
                DEBUG_PRINT(F("JBD BMS: V="));
                DEBUG_PRINT(voltage);
                DEBUG_PRINT(F(" I="));
                DEBUG_PRINT(current);
                DEBUG_PRINT(F(" SOC="));
                DEBUG_PRINT(soc);
                DEBUG_PRINT(F("% T="));
                DEBUG_PRINT(temperature);
                DEBUG_PRINT(F("C Cycles="));
                DEBUG_PRINTLN(cycles);
                
                cleanup(pClient);
                
                // Update cached device info (non-const access via find)
                for (auto& dev : discovered_ble_devices) {
                    if (memcmp(dev.address, cached_dev->address, 6) == 0) {
                        dev.bms_voltage = voltage;
                        dev.bms_current = current;
                        dev.bms_soc = soc;
                        dev.bms_temperature = temperature;
                        dev.bms_cycles = cycles;
                        dev.has_bms_data = true;
                        dev.last_seen = millis();
                        break;
                    }
                }
                
                // Select value based on assigned_unitid and store result
                double parsed_value = ble_select_bms_value(voltage, current, soc, temperature, assigned_unitid);
                store_result(parsed_value, time);
                
                sensor_ble_stop();
                ble_in_read = false;
                return HTTP_RQT_NOT_RECEIVED;
            } else {
                DEBUG_PRINT(F("BMS type not yet implemented: "));
                DEBUG_PRINTLN((int)cached_dev->sensor_type);
            }
        }
        
        // =====================================================================
        // GATT-BASED SENSORS (generic BLE sensors)
        // Need to connect and read characteristic
        // =====================================================================
        
        if (strlen(characteristic_uuid) == 0) {
            DEBUG_PRINTLN(F("ERROR: BLE characteristic UUID not configured (char_uuid/uuid or legacy unit)"));
            flags.enable = false;
            return fail(nullptr, true);
        }
        
        DEBUG_PRINT("Reading BLE sensor: ");
        DEBUG_PRINT(mac_address);
        DEBUG_PRINT(" characteristic: ");
        DEBUG_PRINT(characteristic_uuid);
        DEBUG_PRINT(" (");
        DEBUG_PRINT(ble_uuid_to_name(characteristic_uuid));
        DEBUG_PRINTLN(")");
        
        // Parse MAC address
        BLEAddress bleAddress(mac_address);
        
        // Connect to BLE device (short timeout; client is reused, not deleted)
        BLEClient* pClient = ble_get_client();
        if (!pClient) {
            return fail(nullptr, false);
        }
        if (!pClient->connect(bleAddress, 0, BLE_CONNECT_TIMEOUT_MS)) {
            DEBUG_PRINTLN(F("Failed to connect to BLE device"));
            return fail(nullptr, true);
        }
        
        DEBUG_PRINTLN(F("Connected to BLE device"));
        
        // Sanitize characteristic UUID first
        char characteristic_uuid_clean[128] = {0};
        ble_copy_stripped(characteristic_uuid_clean, sizeof(characteristic_uuid_clean), characteristic_uuid);
        BLEUUID targetCharUUID(characteristic_uuid_clean);
        
        // Try to find characteristic by searching ALL services
        // Many BLE sensors (Govee, Xiaomi, etc.) use proprietary services
        BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
        BLERemoteService* pRemoteService = nullptr;
        
        // First try standard Environmental Sensing service (0x181A)
        pRemoteService = pClient->getService(BLEUUID((uint16_t)0x181A));
        if (pRemoteService != nullptr) {
            pRemoteCharacteristic = pRemoteService->getCharacteristic(targetCharUUID);
        }
        
        // If not found, search through all services for the characteristic
        if (pRemoteCharacteristic == nullptr) {
            DEBUG_PRINTLN(F("Searching all services for characteristic..."));
            std::map<std::string, BLERemoteService*>* services = pClient->getServices();
            if (services != nullptr) {
                for (auto& svcPair : *services) {
                    BLERemoteService* svc = svcPair.second;
                    if (svc == nullptr) continue;
                    
                    DEBUG_PRINT(F("  Checking service: "));
                    DEBUG_PRINTLN(svcPair.first.c_str());
                    
                    pRemoteCharacteristic = svc->getCharacteristic(targetCharUUID);
                    if (pRemoteCharacteristic != nullptr) {
                        pRemoteService = svc;
                        DEBUG_PRINTLN(F("  -> Found characteristic!"));
                        break;
                    }
                }
            }
        }
        
        if (pRemoteCharacteristic == nullptr) {
            DEBUG_PRINTLN(F("Failed to find characteristic in any service"));
            return fail(pClient, true);
        }
        
        // Read value
        if (!pRemoteCharacteristic->canRead()) {
            DEBUG_PRINTLN("Characteristic is not readable");
            return fail(pClient, true);
        }
        
        String value_str = pRemoteCharacteristic->readValue().c_str();
        
        DEBUG_PRINT("Read ");
        DEBUG_PRINT(value_str.length());
        DEBUG_PRINTLN(" bytes from BLE characteristic");
        
        // Disconnect
        cleanup(pClient);
        
        if (value_str.length() == 0) {
            DEBUG_PRINTLN(F("No data received from BLE device"));
            return fail_no_cleanup();
        }
        
        // Parse payload using decoder
        double parsed_value = 0.0;
        
        if (!decode_payload((const uint8_t*)value_str.c_str(), value_str.length(), format, &parsed_value)) {
            DEBUG_PRINTLN(F("Failed to decode BLE payload"));
            return fail_no_cleanup();
        }
    
        // Store value using centralized method
        store_result(parsed_value, time);
        
        // Don't turn off BLE yet - wait for second read() call
        return HTTP_RQT_NOT_RECEIVED; // Will return data on next call
        
    } else {
        // Second call (repeat_read == 1): Return data and turn off BLE
        DEBUG_PRINT("BLE sensor read (turn off): ");
        DEBUG_PRINTLN(name);
        
        repeat_read = 0; // Reset for next cycle
        last_read = time;
        if (flags.data_ok) {
            return HTTP_RQT_SUCCESS; // Adds data to log
        } else {
            return HTTP_RQT_NOT_RECEIVED;
        }
    }
}

unsigned char BLESensor::getUnitId() const {
    return assigned_unitid;
}

const char * BLESensor::getUnit() const {
    if (assigned_unitid == UNIT_USERDEF) {
        return userdef_unit;
    }
    return getSensorUnit(assigned_unitid);
}

#endif // ESP32
