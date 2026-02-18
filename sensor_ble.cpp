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
#include "ieee802154_config.h"
#ifdef ENABLE_MATTER
#include "opensprinkler_matter.h"
#endif

extern OpenSprinkler os;

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/semphr.h>
#include "radio_arbiter.h"

// Forward declarations
void ble_semaphore_init();
static bool ble_ensure_initialized(const char* reason);
static BLEClient* ble_get_client();
static bool ble_uuid_extract_16bit(const char* uuid_in, uint16_t* out_uuid16);

// BLE state
static bool ble_initialized = false;
static BLEScan* pBLEScan = nullptr;

// Background passive scan: runs continuously to catch broadcast sensors (Govee, Xiaomi)
static bool bg_scan_active = false;
static uint32_t bg_scan_restart_at = 0;
static const uint32_t BG_SCAN_DURATION = 30;    // 30 seconds per cycle
static const uint32_t BG_SCAN_RESTART_MS = 2000; // 2s pause between cycles

// User-requested discovery scan (active, high duty cycle)
static bool discovery_scan_active = false;
static uint32_t discovery_scan_end = 0;

// ============================================================================
// BLE Thread Safety: Semaphore to prevent simultaneous Matter + Sensor access
// ============================================================================
static SemaphoreHandle_t ble_access_semaphore = nullptr;
static uint8_t ble_lock_depth = 0;

static bool ble_sensor_lock_acquire(uint32_t timeout_ms, bool* acquired_new = nullptr) {
    if (acquired_new) *acquired_new = false;
    if (!ble_access_semaphore) ble_semaphore_init();
    if (!ble_access_semaphore) return false;
    if (ble_lock_depth > 0) { ble_lock_depth++; return true; }
    if (xSemaphoreTake(ble_access_semaphore, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ble_lock_depth = 1;
        if (acquired_new) *acquired_new = true;
        return true;
    }
    return false;
}

static void ble_sensor_lock_release() {
    if (ble_lock_depth == 0) return;
    ble_lock_depth--;
    if (ble_lock_depth == 0 && ble_access_semaphore) {
        xSemaphoreGive(ble_access_semaphore);
    }
}

// Reuse a single client instance
static BLEClient* EXT_RAM_BSS_ATTR ble_client = nullptr;
static const uint32_t BLE_CONNECT_TIMEOUT_MS = 400;

// BLE init retry control
static bool ble_init_failed = false;
static uint32_t ble_init_retry_at = 0;

// Background scan completion callback
static void ble_bg_scan_complete_cb(BLEScanResults results) {
    bg_scan_active = false;
    bg_scan_restart_at = millis() + BG_SCAN_RESTART_MS;
}

// Discovery scan completion callback
static void ble_discovery_scan_complete_cb(BLEScanResults results) {
    discovery_scan_active = false;
    discovery_scan_end = 0;
    radio_arbiter_release(RADIO_OWNER_BLE_SCAN);
    // Background scan will auto-restart from sensor_ble_loop()
}

// Start background passive scan (low duty cycle, catches broadcast advertisements)
static void ble_bg_scan_start() {
    if (!ble_initialized || !pBLEScan) return;
    if (discovery_scan_active) return;
    if (bg_scan_active) return;

    pBLEScan->setActiveScan(false);   // Passive: no SCAN_REQ overhead
    pBLEScan->setInterval(320);       // 200ms interval
    pBLEScan->setWindow(160);         // 100ms window → 50% duty cycle
    pBLEScan->clearResults();
    pBLEScan->start(BG_SCAN_DURATION, ble_bg_scan_complete_cb, false);
    bg_scan_active = true;
}

// Stop background scan (for GATT operations or discovery scan)
static void ble_bg_scan_stop() {
    if (!bg_scan_active) return;
    if (pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
    }
    bg_scan_active = false;
}

// Discovered devices storage
static std::vector<BLEDeviceInfo> discovered_ble_devices;

// ============================================================================
// Ignored BLE MAC hash set — lives in PSRAM, suppresses repeated log output
// for unmanaged devices that are seen during background scan.
// Open-addressing hash table: each slot is 6 bytes (MAC) + 1 byte (occupied flag).
// ============================================================================
#define BLE_IGNORE_SLOTS 128  // must be power of 2
struct BleIgnoreSlot { uint8_t mac[6]; uint8_t occupied; };
static BleIgnoreSlot EXT_RAM_BSS_ATTR ble_ignore_table[BLE_IGNORE_SLOTS];

static uint32_t ble_ignore_hash(const uint8_t* mac) {
    // FNV-1a 32-bit over 6 bytes
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

/** Returns true if mac is already in the ignore set. */
static bool ble_ignore_contains(const uint8_t* mac) {
    uint32_t idx = ble_ignore_hash(mac) & (BLE_IGNORE_SLOTS - 1);
    for (int probe = 0; probe < 8; probe++) {
        uint32_t slot = (idx + probe) & (BLE_IGNORE_SLOTS - 1);
        if (!ble_ignore_table[slot].occupied) return false;
        if (memcmp(ble_ignore_table[slot].mac, mac, 6) == 0) return true;
    }
    return false;
}

/** Insert mac into the ignore set (best-effort, may fail if full). */
static void ble_ignore_insert(const uint8_t* mac) {
    uint32_t idx = ble_ignore_hash(mac) & (BLE_IGNORE_SLOTS - 1);
    for (int probe = 0; probe < 8; probe++) {
        uint32_t slot = (idx + probe) & (BLE_IGNORE_SLOTS - 1);
        if (!ble_ignore_table[slot].occupied) {
            memcpy(ble_ignore_table[slot].mac, mac, 6);
            ble_ignore_table[slot].occupied = 1;
            return;
        }
        if (memcmp(ble_ignore_table[slot].mac, mac, 6) == 0) return; // already present
    }
}

// ============================================================================
// Managed BLE MAC cache — rebuilt periodically from main loop,
// read lock-free from the NimBLE scan callback (onResult).
// ============================================================================
#define BLE_MAX_MANAGED_MACS 32
static uint8_t  managed_ble_macs[BLE_MAX_MANAGED_MACS][6];
static volatile int managed_ble_mac_count = 0;
static uint32_t managed_ble_mac_refresh_at = 0;

/** Rebuild the managed-MAC cache from the sensor list (main-loop only). */
static void ble_refresh_managed_macs() {
    int count = 0;
    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL && count < BLE_MAX_MANAGED_MACS) {
        if (!sensor || sensor->type != SENSOR_BLE) continue;
        BLESensor* ble = static_cast<BLESensor*>(sensor);
        if (!ble->mac_address_cfg[0]) continue;
        uint8_t addr[6];
        if (sscanf(ble->mac_address_cfg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]) == 6) {
            memcpy(managed_ble_macs[count], addr, 6);
            count++;
        }
    }
    managed_ble_mac_count = count;  // atomic-width write visible to callback
    managed_ble_mac_refresh_at = millis() + 10000; // refresh every 10 s
}

/** Check whether addr matches a managed (configured) BLE sensor MAC. */
static bool ble_is_managed_mac(const uint8_t* addr) {
    int n = managed_ble_mac_count; // snapshot
    for (int i = 0; i < n; i++) {
        if (memcmp(managed_ble_macs[i], addr, 6) == 0) return true;
    }
    return false;
}

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
        strstr(name, "GVH5102") || strstr(name, "GVH5104") ||
        strstr(name, "GVH5105") || strstr(name, "GVH5110"))
        return BLE_TYPE_GOVEE_H5075;
    if (strstr(name, "GVH5179") || strstr(name, "GV5179") || strstr(name, "Govee_H5179"))
        return BLE_TYPE_GOVEE_H5179;
    if (strstr(name, "GVH5177") || strstr(name, "GVH5174"))
        return BLE_TYPE_GOVEE_H5177;
    if (strstr(name, "GVH5181") || strstr(name, "GVH5182") || 
        strstr(name, "GVH5183") || strstr(name, "GVH5184") ||
        strstr(name, "GVH5055") || strstr(name, "GVH5054"))
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
    if (len < 6) return false;
    
    // H5075/H510x format per GoveeBTTempLogger:
    // int(Data[1]) << 16 | int(Data[2]) << 8 | int(Data[3])
    int32_t iTemp = ((int32_t)data[1] << 16) | ((int32_t)data[2] << 8) | (int32_t)data[3];
    *battery = data[4];
    
    bool bNegative = (iTemp & 0x800000) != 0;  // Check sign bit
    iTemp = iTemp & 0x7FFFF;  // Mask off sign bit (19 bits)
    
    // Temperature: divide by 10000, then by 10 => first 3 digits / 10
    // Humidity: modulo 1000, then divide by 10 => last 3 digits / 10
    *temp = (float)(iTemp / 1000) / 10.0f;
    if (bNegative) *temp = -*temp;
    
    *hum = (float)(iTemp % 1000) / 10.0f;
    
    return true;
}

/**
 * @brief Decode Govee meat thermometer advertisement data
 * Supports models handled in GoveeBTTempLogger (H5181/H5182/H5183/H5184/H5055)
 * Only current value is used (no min/max); humidity is 0 for these devices.
 */
static bool govee_decode_meat(const uint8_t* data, size_t len, float* temp, float* hum, uint8_t* battery) {
    if (!data || len < 14) return false;

    // H5181/H5183 commonly use 14-byte payloads (current temp at bytes 8..9)
    if (len == 14) {
        int16_t t0 = (int16_t)(((uint16_t)data[8] << 8) | data[9]);
        *temp = (float)t0 / 100.0f;
        *hum = 0.0f;
        *battery = data[5] & 0x7F;
        return true;
    }

    // H5182/H5184 commonly use 17-byte payloads (probe1 current temp at bytes 8..9)
    if (len == 17) {
        int16_t t0 = (int16_t)(((uint16_t)data[8] << 8) | data[9]);
        *temp = (float)t0 / 100.0f;
        *hum = 0.0f;
        *battery = data[5] & 0x7F;
        return true;
    }

    // H5055 commonly uses 20-byte payloads (current probe temp at bytes 5..6)
    if (len == 20) {
        int16_t t0 = (int16_t)(((uint16_t)data[6] << 8) | data[5]);
        *temp = (float)t0;
        *hum = 0.0f;
        *battery = data[2] & 0x7F;
        return true;
    }

    return false;
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

    // Apple iBeacon payloads (0x004C) can coexist with Govee service UUIDs
    // but do NOT contain Govee temperature/humidity payload.
    // Never decode these as Govee sensor readings.
    if (manufacturer_id == 0x004c) {
        return false;
    }
    
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
        } else if ((len == 14 || len == 17 || len == 20) && manufacturer_id != 0x004c) {
            // Meat thermometers often use vendor-specific IDs, infer by payload shape
            type = BLE_TYPE_GOVEE_MEAT;
        }
    }

    // Validate manufacturer + payload length for each decoder to avoid false positives.
    switch (type) {
        case BLE_TYPE_GOVEE_H5074:
            if (!(manufacturer_id == 0xec88 && len == 7)) return false;
            break;
        case BLE_TYPE_GOVEE_H5075:
            if (!(manufacturer_id == 0xec88 && len == 6)) return false;
            break;
        case BLE_TYPE_GOVEE_H5179:
            if (!(manufacturer_id == 0xec88 && len == 9)) return false;
            break;
        case BLE_TYPE_GOVEE_H5177:
            if (!(manufacturer_id == 0x0001 && len == 6)) return false;
            break;
        case BLE_TYPE_GOVEE_MEAT:
            if (!(len == 14 || len == 17 || len == 20)) return false;
            break;
        default:
            return false;
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
        case BLE_TYPE_GOVEE_MEAT:
            success = govee_decode_meat(data, len, out_temp, out_hum, out_battery);
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

static bool ble_addr_has_prefix(const uint8_t addr[6], uint8_t b0, uint8_t b1, uint8_t b2) {
    return addr && addr[0] == b0 && addr[1] == b1 && addr[2] == b2;
}

static bool ble_addr_is_known_govee(const uint8_t addr[6]) {
    // Known Govee OUI seen on H5075 devices
    return ble_addr_has_prefix(addr, 0xA4, 0xC1, 0x38);
}

static bool ble_is_govee_type(BLESensorType type) {
    return type == BLE_TYPE_GOVEE_H5074 || type == BLE_TYPE_GOVEE_H5075 ||
           type == BLE_TYPE_GOVEE_H5177 || type == BLE_TYPE_GOVEE_H5179 ||
           type == BLE_TYPE_GOVEE_MEAT;
}

static bool ble_decode_raw_service_data_ec88(BLEAdvertisedDevice& advertisedDevice,
                                             const char* device_name,
                                             float* adv_temp, float* adv_hum,
                                             uint8_t* adv_battery, BLESensorType* sensor_type,
                                             bool* saw_ec88_service) {
    const uint8_t* payload = advertisedDevice.getPayload();
    size_t payload_len = advertisedDevice.getPayloadLength();
    if (!payload || payload_len < 4) return false;

    size_t idx = 0;
    while (idx + 1 < payload_len) {
        uint8_t ad_len = payload[idx];
        if (ad_len == 0) break;

        // AD structure: [len][type][data...], len excludes first len byte
        if (idx + 1 + ad_len > payload_len) break;

        uint8_t ad_type = payload[idx + 1];
        if (ad_type == 0x16 && ad_len >= 3) {
            const uint8_t* ad_data = payload + idx + 2;
            uint16_t uuid16 = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8);
            if (uuid16 == 0xec88) {
                if (saw_ec88_service) *saw_ec88_service = true;

                const uint8_t* svc_payload = ad_data + 2;
                size_t svc_len = (size_t)ad_len - 3;



                // Direct decode first
                if (govee_decode_adv_data(0xec88, svc_payload, svc_len, device_name,
                                          adv_temp, adv_hum, adv_battery, sensor_type)) {
                    return true;
                }

                // H5075 fallback window decode if service payload has leading bytes
                BLESensorType name_type = govee_detect_type_from_name(device_name);
                if (name_type == BLE_TYPE_GOVEE_H5075 && svc_len >= 6) {
                    for (size_t off = 0; off + 6 <= svc_len && off < 10; off++) {
                        float t = 0, h = 0;
                        uint8_t b = 0;
                        if (govee_decode_h5075(svc_payload + off, 6, &t, &h, &b)) {
                            if (t > -40.0f && t < 85.0f && h >= 0.0f && h <= 100.0f) {
                                *adv_temp = t;
                                *adv_hum = h;
                                *adv_battery = b;
                                if (sensor_type) *sensor_type = BLE_TYPE_GOVEE_H5075;
                                return true;
                            }
                        }
                    }
                }
            }
        }

        idx += (size_t)ad_len + 1;
    }

    return false;
}

static bool ble_is_uuid_ec88_service(const char* uuid_str) {
    if (!uuid_str || !uuid_str[0]) return false;
    uint16_t uuid16 = 0;
    return ble_uuid_extract_16bit(uuid_str, &uuid16) && uuid16 == 0xec88;
}

// ============================================================================
// Device Information Service (DIS, 0x180A) Query
// Reads Manufacturer Name (0x2A29) and Model Number (0x2A24) via GATT
// ============================================================================

// DIS query queue: MAC addresses of devices whose DIS has not yet been read
struct BLEDISQueryItem {
    char mac[18];    // "AA:BB:CC:DD:EE:FF"
    uint32_t queued_at;
};
static std::vector<BLEDISQueryItem> ble_dis_query_queue;
static bool ble_dis_query_pending = false;
static uint32_t ble_dis_query_time = 0;

/**
 * @brief Queue a DIS query for a BLE device
 * @param mac_address MAC address string
 */
static void ble_queue_dis_query(const char* mac_address) {
    if (!mac_address || !mac_address[0]) return;
    // Check if already queued
    for (const auto& item : ble_dis_query_queue) {
        if (strcasecmp(item.mac, mac_address) == 0) return;
    }
    BLEDISQueryItem item;
    memset(&item, 0, sizeof(item));
    strncpy(item.mac, mac_address, sizeof(item.mac) - 1);
    item.queued_at = millis();
    ble_dis_query_queue.push_back(item);
}

/**
 * @brief Connect to a BLE device and read Device Information Service characteristics
 * @param mac_address MAC address of the device
 * @param out_manufacturer Buffer to receive Manufacturer Name (min 32 bytes)
 * @param out_model Buffer to receive Model Number (min 32 bytes)
 * @return true if at least one field was read successfully
 */
static bool ble_read_device_info_service(const char* mac_address, char* out_manufacturer, char* out_model) {
    if (!mac_address || !out_manufacturer || !out_model) return false;
    out_manufacturer[0] = 0;
    out_model[0] = 0;

    if (!ble_ensure_initialized("DIS query")) return false;

    bool lock_acquired = ble_sensor_lock_acquire(1500);
    if (!lock_acquired) {
        DEBUG_PRINTLN(F("[BLE] DIS query skipped: semaphore busy"));
        return false;
    }

    // Pause background scan for GATT connection
    ble_bg_scan_stop();
    bool success = false;

    BLEClient* pClient = ble_get_client();
    if (!pClient) {
        DEBUG_PRINTLN(F("[BLE] DIS: Failed to create client"));
        ble_bg_scan_start();
        ble_sensor_lock_release();
        return false;
    }

    BLEAddress bleAddress(mac_address);
    if (!pClient->connect(bleAddress, 0, BLE_CONNECT_TIMEOUT_MS)) {
        DEBUG_PRINT(F("[BLE] DIS: Failed to connect to "));
        DEBUG_PRINTLN(mac_address);
        ble_bg_scan_start();
        ble_sensor_lock_release();
        return false;
    }

    DEBUG_PRINT(F("[BLE] DIS: Connected to "));
    DEBUG_PRINTLN(mac_address);

    // Get Device Information Service (0x180A)
    BLERemoteService* pDIS = pClient->getService(BLEUUID((uint16_t)0x180A));
    if (pDIS) {
        // Read Manufacturer Name String (0x2A29)
        BLERemoteCharacteristic* pManufacturer = pDIS->getCharacteristic(BLEUUID((uint16_t)0x2A29));
        if (pManufacturer && pManufacturer->canRead()) {
            String val = pManufacturer->readValue().c_str();
            if (val.length() > 0) {
                strncpy(out_manufacturer, val.c_str(), 31);
                out_manufacturer[31] = 0;
                success = true;
                DEBUG_PRINT(F("[BLE] DIS Manufacturer: "));
                DEBUG_PRINTLN(out_manufacturer);
            }
        }

        // Read Model Number String (0x2A24)
        BLERemoteCharacteristic* pModel = pDIS->getCharacteristic(BLEUUID((uint16_t)0x2A24));
        if (pModel && pModel->canRead()) {
            String val = pModel->readValue().c_str();
            if (val.length() > 0) {
                strncpy(out_model, val.c_str(), 31);
                out_model[31] = 0;
                success = true;
                DEBUG_PRINT(F("[BLE] DIS Model: "));
                DEBUG_PRINTLN(out_model);
            }
        }
    } else {
        DEBUG_PRINTLN(F("[BLE] DIS: Service 0x180A not found on device"));
    }

    pClient->disconnect();
    // Resume background scan after DIS query
    ble_bg_scan_start();
    ble_sensor_lock_release();
    return success;
}

/**
 * @brief Update DIS info for all BLE sensors matching a MAC address
 */
void BLESensor::updateDeviceInfo(const char* mac_address, const char* manufacturer, const char* model) {
    if (!mac_address || !mac_address[0]) return;
    bool updated = false;

    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_BLE) continue;
        BLESensor* ble = static_cast<BLESensor*>(sensor);
        // Compare MAC addresses (case-insensitive)
        if (strcasecmp(ble->mac_address_cfg, mac_address) == 0) {
            if (manufacturer && manufacturer[0]) {
                strncpy(ble->ble_manufacturer, manufacturer, sizeof(ble->ble_manufacturer) - 1);
                ble->ble_manufacturer[sizeof(ble->ble_manufacturer) - 1] = 0;
            }
            if (model && model[0]) {
                strncpy(ble->ble_model, model, sizeof(ble->ble_model) - 1);
                ble->ble_model[sizeof(ble->ble_model) - 1] = 0;
            }
            ble->dis_info_queried = true;
            updated = true;
            DEBUG_PRINT(F("[BLE] Updated DIS info for sensor: "));
            DEBUG_PRINTLN(ble->name);
        }
    }

    if (updated) {
        sensor_save();
    }
}

/**
 * @brief Push advertisement data to ALL BLE sensors matching a MAC address.
 * Called from sensor_ble_loop() when new broadcast data arrives.
 * This ensures all logical sensors sharing the same physical device
 * get data_ok=1 and repeat_read=1 simultaneously, regardless of
 * individual read scheduling in the main loop.
 */
static double ble_select_adv_value(const BLEDeviceInfo* cached_dev, unsigned char unitid);

void BLESensor::pushAdvData(const char* mac_address, const BLEDeviceInfo* cached_dev) {
    if (!mac_address || !mac_address[0] || !cached_dev || !cached_dev->has_adv_data) return;

    unsigned long time = os.now_tz();
    if (time < 100) return;  // Not yet initialized

    SensorIterator it = sensors_iterate_begin();
    SensorBase* sensor;
    while ((sensor = sensors_iterate_next(it)) != NULL) {
        if (!sensor || sensor->type != SENSOR_BLE) continue;
        if (!sensor->flags.enable) continue;
        BLESensor* ble = static_cast<BLESensor*>(sensor);
        if (strcasecmp(ble->mac_address_cfg, mac_address) != 0) continue;

        // Only push to broadcast sensors (no GATT characteristic configured,
        // or ec88 service UUID which is advertisement-only)
        bool has_gatt = (ble->characteristic_uuid_cfg[0] != 0) &&
                        !ble_is_uuid_ec88_service(ble->characteristic_uuid_cfg);
        if (has_gatt) continue;

        // Skip if sensor was already updated this second (by direct read path)
        if (ble->last_read == time && ble->flags.data_ok) continue;

        double parsed_value = ble_select_adv_value(cached_dev, ble->assigned_unitid);
        ble->store_result(parsed_value, time);
        ble->last_battery = cached_dev->adv_battery;
        ble->adv_last_success_time = time;
    }
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
    
    // Don't restore adv_last_success_time from persisted state.
    // Each boot gets a fresh start — the 24h auto-disable countdown
    // begins only after the first successful read IN THIS SESSION.
    // This prevents stale timestamps from immediately disabling sensors
    // that have radio coexistence issues on reboot.
    // adv_last_success_time stays 0 until first success.
    
    // Migration: if sensor was auto-disabled by stale adv_last_ok timestamp,
    // re-enable it so it gets a fresh chance at data collection.
    if (obj.containsKey("adv_last_ok") && !flags.enable) {
        DEBUG_PRINTF("[BLE] Re-enabling auto-disabled sensor: %s\n", name);
        flags.enable = true;
    }
    
    // Restore battery level (UINT32_MAX = not yet measured)
    if (obj.containsKey("battery")) {
        last_battery = obj["battery"].as<uint32_t>();
    } else {
        last_battery = UINT32_MAX;
    }
    
    // Restore DIS (Device Information Service) data
    // Accept both "ble_manufacturer" (new) and "ble_mfr" (legacy) for backward compatibility
    if (obj.containsKey("ble_manufacturer")) {
        const char* m = obj["ble_manufacturer"].as<const char*>();
        if (m) {
            strncpy(ble_manufacturer, m, sizeof(ble_manufacturer) - 1);
            ble_manufacturer[sizeof(ble_manufacturer) - 1] = 0;
        }
    } else if (obj.containsKey("ble_mfr")) {
        const char* m = obj["ble_mfr"].as<const char*>();
        if (m) {
            strncpy(ble_manufacturer, m, sizeof(ble_manufacturer) - 1);
            ble_manufacturer[sizeof(ble_manufacturer) - 1] = 0;
        }
    }
    if (obj.containsKey("ble_model")) {
        const char* m = obj["ble_model"].as<const char*>();
        if (m) {
            strncpy(ble_model, m, sizeof(ble_model) - 1);
            ble_model[sizeof(ble_model) - 1] = 0;
        }
    }
    // Mark DIS as queried if we have data from persistence
    if (ble_manufacturer[0] || ble_model[0]) {
        dis_info_queried = true;
    }
}

void BLESensor::toJson(ArduinoJson::JsonObject obj) const {
    SensorBase::toJson(obj);
    if (!obj) return;

    // BLE-specific fields
    if (mac_address_cfg[0]) obj["mac"] = mac_address_cfg;
    if (characteristic_uuid_cfg[0]) obj["char_uuid"] = characteristic_uuid_cfg;
    if (payload_format_cfg != (uint8_t)FORMAT_TEMP_001) obj["format"] = payload_format_cfg;
    // Note: assigned_unitid is handled by SensorBase::toJson
    
    // Persist last successful read time for auto-disable feature
    if (adv_last_success_time > 0) obj["adv_last_ok"] = adv_last_success_time;
    
    // Battery level — only persist when actually measured
    if (last_battery != UINT32_MAX) obj["battery"] = last_battery;
    
    // Persist DIS (Device Information Service) data (consistent with ZigBee naming: zb_manufacturer/zb_model)
    if (ble_manufacturer[0]) obj["ble_manufacturer"] = ble_manufacturer;
    if (ble_model[0]) obj["ble_model"] = ble_model;
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
        // Get device address (format: "aa:bb:cc:dd:ee:ff")
        String addr_str = advertisedDevice.getAddress().toString();
        uint8_t addr_bytes[6];
        
        // Parse address string to bytes
        if (sscanf(addr_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &addr_bytes[0], &addr_bytes[1], &addr_bytes[2],
                   &addr_bytes[3], &addr_bytes[4], &addr_bytes[5]) != 6) {
            return; // Invalid address format
        }

        // Skip logging for devices we have already decided to ignore
        if (ble_ignore_contains(addr_bytes)) {
            return;
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
        bool saw_govee_mfg = false;
        bool saw_ec88_service = false;
        
        // Check for manufacturer data (where Govee sends sensor readings)
        if (advertisedDevice.haveManufacturerData()) {
            String mfg_data = advertisedDevice.getManufacturerData();
            if (mfg_data.length() >= 2) {
                // First 2 bytes are manufacturer ID (little-endian)
                uint16_t mfg_id = (uint8_t)mfg_data[0] | ((uint8_t)mfg_data[1] << 8);
                if (mfg_id == 0xec88 || mfg_id == 0x0001) {
                    saw_govee_mfg = true;
                }
                const uint8_t* payload = (const uint8_t*)mfg_data.c_str() + 2;
                size_t payload_len = mfg_data.length() - 2;
                

                
                has_adv_data = govee_decode_adv_data(mfg_id, payload, payload_len, device_name,
                                                     &adv_temp, &adv_hum, &adv_battery, &sensor_type);
                

            }
        }

        // Some Govee models expose current values in Service Data (UUID 0xEC88)
        // while Manufacturer Data only carries Apple iBeacon (0x004C).
        if (!has_adv_data && advertisedDevice.haveServiceData()) {
            int svc_count = advertisedDevice.getServiceDataCount();
            for (int i = 0; i < svc_count && !has_adv_data; i++) {
                String svc_uuid_str = advertisedDevice.getServiceDataUUID(i).toString().c_str();
                uint16_t svc_uuid16 = 0;
                if (!ble_uuid_extract_16bit(svc_uuid_str.c_str(), &svc_uuid16)) {
                    continue;
                }
                if (svc_uuid16 == 0xec88) {
                    saw_ec88_service = true;
                }

                String svc_data = advertisedDevice.getServiceData(i);
                size_t svc_len = svc_data.length();
                if (svc_len == 0) {
                    continue;
                }

                const uint8_t* svc_payload = (const uint8_t*)svc_data.c_str();

                if (svc_uuid16 == 0xec88) {
                    // 1) Try direct decode first
                    has_adv_data = govee_decode_adv_data(0xec88, svc_payload, svc_len, device_name,
                                                         &adv_temp, &adv_hum, &adv_battery, &sensor_type);

                    // 2) Fallback for H5075-like payloads where service data contains
                    // extra leading bytes before the 6-byte Govee value block.
                    if (!has_adv_data) {
                        BLESensorType name_type = govee_detect_type_from_name(device_name);
                        if (name_type == BLE_TYPE_GOVEE_H5075 && svc_len >= 6) {
                            for (size_t off = 0; off + 6 <= svc_len && off < 10 && !has_adv_data; off++) {
                                float t = 0, h = 0;
                                uint8_t b = 0;
                                if (govee_decode_h5075(svc_payload + off, 6, &t, &h, &b)) {
                                    // Plausibility bounds
                                    if (t > -40.0f && t < 85.0f && h >= 0.0f && h <= 100.0f) {
                                        adv_temp = t;
                                        adv_hum = h;
                                        adv_battery = b;
                                        sensor_type = BLE_TYPE_GOVEE_H5075;
                                        has_adv_data = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Raw AD payload fallback: some stack versions don't always populate
        // service-data vectors for 0x16, but ec88 bytes are still present in payload.
        if (!has_adv_data) {
            has_adv_data = ble_decode_raw_service_data_ec88(advertisedDevice, device_name,
                                                            &adv_temp, &adv_hum, &adv_battery,
                                                            &sensor_type, &saw_ec88_service);
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
                saw_ec88_service = true;
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
            

        }

        // Guard against false positives from corrupted/partial names.
        // A name-only Govee match is accepted only with additional evidence.
        if (!has_adv_data && ble_is_govee_type(sensor_type)) {
            bool has_govee_evidence = saw_ec88_service || saw_govee_mfg || ble_addr_is_known_govee(addr_bytes);
            if (!has_govee_evidence) {
                // Don't trust the Govee type detection, but still keep device in list
                sensor_type = BLE_TYPE_UNKNOWN;
            }
        }
        
        // Classify devices with known sensor patterns for auto-detection,
        // but keep ALL devices in the discovery list so the user can see them.
        if (sensor_type == BLE_TYPE_UNKNOWN && !has_adv_data) {
            // Check if name looks like a sensor — if so, mark for GATT read
            if (strstr(device_name, "GVH") != nullptr ||
                strstr(device_name, "Govee") != nullptr ||
                strstr(device_name, "LYWSD") != nullptr ||
                strstr(device_name, "MJ_HT") != nullptr ||
                strstr(device_name, "ATC_") != nullptr ||
                strstr(device_name, "Temp") != nullptr ||
                strstr(device_name, "Thermo") != nullptr ||
                // BMS patterns
                strstr(device_name, "BMS") != nullptr ||
                strstr(device_name, "xiaoxiang") != nullptr ||
                strstr(device_name, "JBD") != nullptr ||
                strstr(device_name, "DL-") != nullptr ||
                strstr(device_name, "JK-") != nullptr ||
                strstr(device_name, "ANT-") != nullptr ||
                strstr(device_name, "SP0") != nullptr ||
                strstr(device_name, "SP1") != nullptr) {
                sensor_type = BLE_TYPE_GENERIC_GATT; // Mark as generic for later GATT read
            }
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

        // Filter: outside discovery scan, only accept managed (configured) devices
        if (!already_exists && !discovery_scan_active) {
            if (!ble_is_managed_mac(addr_bytes)) {
                ble_ignore_insert(addr_bytes); // suppress future log output
                return; // not a configured sensor – ignore during background scan
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
                existing_device->adv_data_pending_push = true;
            }
            if (sensor_type != BLE_TYPE_UNKNOWN) {
                existing_device->sensor_type = sensor_type;
            }

            if (has_adv_data) {
                DEBUG_PRINTF("[BLE] %s: T=%.1fC H=%.1f%% Bat=%d%%\n",
                             device_name, adv_temp, adv_hum, adv_battery);
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
            new_dev.adv_data_pending_push = has_adv_data;
            
            discovered_ble_devices.push_back(new_dev);

            // Queue DIS query only for likely GATT devices; skip for advertisement-only
            // sensors (Govee/Xiaomi) and unknown devices (phones, headphones, etc.)
            // to avoid unnecessary connect/disconnect churn.
            if (new_dev.sensor_type != BLE_TYPE_UNKNOWN && !sensor_ble_is_adv_sensor(&new_dev)) {
                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         addr_bytes[0], addr_bytes[1], addr_bytes[2],
                         addr_bytes[3], addr_bytes[4], addr_bytes[5]);
                ble_queue_dis_query(mac_str);
            }

            DEBUG_PRINT("New BLE sensor added: ");
            DEBUG_PRINT(new_dev.name);
            DEBUG_PRINT(" type=");
            DEBUG_PRINTLN((int)sensor_type);
        }
    }
};

// NOTE: Callback objects cannot use EXT_RAM_BSS_ATTR - has virtual functions
static MyAdvertisedDeviceCallbacks ble_scan_callbacks;
bool sensor_ble_init()
{
    // When IEEE 802.15.4 mode is Matter, wait for Matter to initialize first.
    // Matter uses the shared NimBLE controller for CHIPoBLE commissioning.
    if (ieee802154_is_matter()) {
#ifdef ENABLE_MATTER
        uint32_t m_init = matter_get_init_time_ms();
        if (m_init == 0 || (millis() - m_init) < 15000) {
            return false; // Matter not ready yet
        }
#endif
    }

    if (!ble_initialized && BLEDevice::getInitialized()) {
        ble_initialized = true;
    }

    if (ble_initialized) {
        ble_semaphore_init();
        if (!pBLEScan) {
            pBLEScan = BLEDevice::getScan();
            DEBUG_PRINTLN("[BLE] Reusing existing NimBLE stack - attaching scan callbacks");
            pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks, true);
        }
        // Start background scan if not already running
        if (!bg_scan_active && !discovery_scan_active) {
            ble_bg_scan_start();
        }
        return true;
    }

    // Backoff if previous init failed
    if (ble_init_failed && (int32_t)(millis() - ble_init_retry_at) < 0) {
        return false;
    }

    DEBUG_PRINTLN("Initializing BLE...");

    ble_initialized = BLEDevice::init("OpenSprinkler");
    if (!ble_initialized && BLEDevice::getInitialized()) {
        ble_initialized = true;
    }
    if (!ble_initialized) {
        DEBUG_PRINTLN("ERROR: BLE initialization failed");
        ble_init_failed = true;
        ble_init_retry_at = millis() + 10000;
        return false;
    }

    DEBUG_PRINTLN("BLE initialized successfully");
    ble_init_failed = false;

    ble_semaphore_init();

    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks, true);

    // Start background passive scan immediately - BLE stays permanently active
    ble_bg_scan_start();
    DEBUG_PRINTLN("[BLE] Background passive scan started after init");

    return true;
}

static bool ble_ensure_initialized(const char* reason) {
    if (ble_initialized) {
        return true;
    }
    if (reason && reason[0]) {
        DEBUG_PRINT(F("[BLE] Init requested by: "));
        DEBUG_PRINTLN(reason);
    }
    return sensor_ble_init();
}

/**
 * @brief Get or create BLE client connection
 */
static BLEClient* ble_get_client()
{
    if (!ble_initialized) return nullptr;
    if (!ble_client) {
        ble_client = BLEDevice::createClient();
    }
    return ble_client;
}

static void sensor_ble_stop_now() {
    // Only stops active scans, BLE stays initialized permanently
    if (!ble_initialized) return;

    if (discovery_scan_active && pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        discovery_scan_active = false;
        discovery_scan_end = 0;
    }
    if (bg_scan_active && pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        bg_scan_active = false;
    }

    if (ble_client) {
        ble_client->disconnect();
        ble_client = nullptr;
    }

    DEBUG_PRINTLN("[BLE] Scans stopped (BLE stays initialized)");
}

/**
 * @brief Stop BLE scanning (BLE stays initialized and bg scan restarts)
 * Called externally (e.g. Matter needs exclusive access).
 * Background scan will auto-restart from sensor_ble_loop().
 */
void sensor_ble_stop() {
    if (!ble_initialized) return;
    sensor_ble_stop_now();
}

/**
 * @brief Start BLE discovery scan (user-requested, active, high duty cycle)
 * Pauses background passive scan during discovery.
 * Background scan auto-restarts after discovery completes.
 */
void sensor_ble_start_scan(uint16_t duration, bool passive) {
    if (!ble_ensure_initialized("scan")) return;

    if (!radio_arbiter_allow_ble_scan()) {
        DEBUG_PRINTLN("[BLE] Scan deferred: web traffic has priority");
        return;
    }

    bool acquired_new = false;
    if (!ble_sensor_lock_acquire(1500, &acquired_new)) {
        DEBUG_PRINTLN("[BLE] Scan skipped: semaphore busy");
        return;
    }

    // Stop background scan for discovery
    ble_bg_scan_stop();

    if (!pBLEScan) {
        pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks, true);
    }

    if (discovery_scan_active) {
        DEBUG_PRINTLN("[BLE] Discovery scan already active");
        ble_sensor_lock_release();
        return;
    }

    // Configure for discovery: active scan, high duty cycle
    if (passive) {
        pBLEScan->setActiveScan(false);
        pBLEScan->setInterval(320);
        pBLEScan->setWindow(160);  // 50% duty cycle
    } else {
        pBLEScan->setActiveScan(true);
        pBLEScan->setInterval(100);
        pBLEScan->setWindow(99);   // 99% duty cycle for discovery
    }

    const uint16_t MAX_DISCOVERY = passive ? 15 : 10;
    uint16_t actual_duration = (duration > MAX_DISCOVERY) ? MAX_DISCOVERY : duration;

    pBLEScan->clearResults();
    radio_arbiter_acquire(RADIO_OWNER_BLE_SCAN, actual_duration * 1000 + 2000);

    // Clear ignore table so discovery scan can see all devices
    memset(ble_ignore_table, 0, sizeof(ble_ignore_table));

    pBLEScan->start(actual_duration, ble_discovery_scan_complete_cb, false);
    discovery_scan_active = true;
    discovery_scan_end = millis() + (actual_duration * 1000);

    ble_sensor_lock_release();

    DEBUG_PRINTF("[BLE] Discovery scan started (duration=%ds, passive=%d)\n", actual_duration, (int)passive);
}

/**
 * @brief BLE maintenance loop - MUST be called regularly from main loop
 * Handles: background scan auto-restart, DIS queries, stale device cleanup
 */
void sensor_ble_loop() {
    if (!ble_initialized) return;

    uint32_t now = millis();

    // Periodically refresh the managed-MAC cache for onResult filtering
    if ((int32_t)(now - managed_ble_mac_refresh_at) >= 0) {
        ble_refresh_managed_macs();
    }

    // Check radio arbiter - pause bg scan if web traffic needs priority
    if (bg_scan_active && !radio_arbiter_allow_ble_scan()) {
        ble_bg_scan_stop();
    }

    // Auto-restart background passive scan when idle
    if (!bg_scan_active && !discovery_scan_active && now >= bg_scan_restart_at) {
        if (radio_arbiter_allow_ble_scan()) {
            ble_bg_scan_start();
        } else {
            bg_scan_restart_at = now + 5000; // Retry in 5s
        }
    }

    // Safety: force-stop discovery scan if overdue
    if (discovery_scan_active && discovery_scan_end > 0 && now > discovery_scan_end + 5000) {
        DEBUG_PRINTLN("[BLE] Discovery scan timeout - forcing stop");
        if (pBLEScan && pBLEScan->isScanning()) {
            pBLEScan->stop();
        }
        discovery_scan_active = false;
        discovery_scan_end = 0;
        radio_arbiter_release(RADIO_OWNER_BLE_SCAN);
    }

    // Process one DIS query at a time (only when not in active GATT operations)
    if (!discovery_scan_active && !ble_dis_query_queue.empty()) {
        if (!ble_dis_query_pending || (now - ble_dis_query_time >= 3000)) {
            BLEDISQueryItem item = ble_dis_query_queue.front();
            ble_dis_query_queue.erase(ble_dis_query_queue.begin());

            bool needs_query = false;
            for (auto& dev : discovered_ble_devices) {
                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         dev.address[0], dev.address[1], dev.address[2],
                         dev.address[3], dev.address[4], dev.address[5]);
                if (strcasecmp(mac_str, item.mac) == 0 && !dev.dis_queried) {
                    needs_query = true;
                    break;
                }
            }

            if (needs_query) {
                ble_dis_query_pending = true;
                ble_dis_query_time = now;

                char manufacturer[32] = {0};
                char model[32] = {0};
                bool success = ble_read_device_info_service(item.mac, manufacturer, model);

                for (auto& dev : discovered_ble_devices) {
                    char mac_str[18];
                    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                             dev.address[0], dev.address[1], dev.address[2],
                             dev.address[3], dev.address[4], dev.address[5]);
                    if (strcasecmp(mac_str, item.mac) == 0) {
                        dev.dis_queried = true;
                        if (success) {
                            strncpy(dev.manufacturer, manufacturer, sizeof(dev.manufacturer) - 1);
                            strncpy(dev.model, model, sizeof(dev.model) - 1);
                        }
                        break;
                    }
                }

                if (success && (manufacturer[0] || model[0])) {
                    BLESensor::updateDeviceInfo(item.mac, manufacturer, model);
                }

                ble_dis_query_pending = false;
                ble_dis_query_time = now;
            }
        }
    }

    // Push new advertisement data to all matching BLE sensors.
    // This runs on the main task, safe to access sensor objects.
    for (auto& dev : discovered_ble_devices) {
        if (dev.adv_data_pending_push && dev.has_adv_data) {
            dev.adv_data_pending_push = false;
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     dev.address[0], dev.address[1], dev.address[2],
                     dev.address[3], dev.address[4], dev.address[5]);
            BLESensor::pushAdvData(mac_str, &dev);
        }
    }

    // Remove stale devices (not seen in 5 minutes)
    discovered_ble_devices.erase(
        std::remove_if(discovered_ble_devices.begin(), discovered_ble_devices.end(),
            [now](const BLEDeviceInfo& dev) {
                return (now - dev.last_seen > 300000);
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
    // Apply user-defined scaling (same behavior as ZigBee/ASB):
    // value = value * factor / divider + offset2/100
    if (factor && divider) {
        value *= (double)factor / (double)divider;
    } else if (divider) {
        value /= (double)divider;
    } else if (factor) {
        value *= (double)factor;
    }
    value += offset2 / 100.0;

    flags.data_ok = 1;
    last_data = value;                      // Store as-is (e.g., 34.59 for 34.59°C)
    last_native_data = (int32_t)(value * 100.0);  // Native: integer representation
    last_read = time;
    repeat_data = last_data;
    repeat_native = last_native_data;
    repeat_read = 1;  // Signal that data is available
    
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
 * 
 * Broadcast sensors (Govee, Xiaomi): Data arrives continuously via background
 * passive scan. read() simply checks the cache - no scan triggering, no
 * Broadcast sensors (Govee, Xiaomi): Read from cache, return SUCCESS directly.
 * No scan triggering, no retry/backoff. Unsolicited data is always accepted
 * via pushAdvData() from sensor_ble_loop().
 * 
 * Poll sensors (BMS, GATT): Pauses background scan, connects via GATT,
 * reads data, disconnects, resumes background scan.
 * Uses two-phase pattern: First call stores data (repeat_read=1),
 * second call returns HTTP_RQT_SUCCESS.
 */
int BLESensor::read(unsigned long time) {
    if (!flags.enable) return HTTP_RQT_NOT_RECEIVED;

    // No BLE in WiFi AP mode (RF conflict), except when using Ethernet
    if (!useEth && os.get_wifi_mode() == WIFI_MODE_AP) {
        flags.data_ok = false;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // Auto-disable after 24h without data
    if (adv_last_success_time > 0 && time > adv_last_success_time) {
        if ((time - adv_last_success_time) > ADV_DISABLE_TIMEOUT) {
            DEBUG_PRINTF("[BLE] Auto-disabled %s (no data for 24h)\n", name);
            flags.enable = false;
            flags.data_ok = false;
            return HTTP_RQT_NOT_RECEIVED;
        }
    }

    if (repeat_read == 1) {
        // Phase 2: Return stored data
        repeat_read = 0;
        last_read = time;
        return flags.data_ok ? HTTP_RQT_SUCCESS : HTTP_RQT_NOT_RECEIVED;
    }

    // Phase 1: Get data
    if (!ble_ensure_initialized("sensor read")) {
        flags.data_ok = false;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // Resolve MAC address
    const char* mac_address = mac_address_cfg;
    if ((!mac_address || !mac_address[0]) && ble_is_mac_string(name)) {
        mac_address = name;
    }
    if (!mac_address || !mac_address[0] || !ble_is_mac_string(mac_address)) {
        DEBUG_PRINTLN(F("[BLE] ERROR: No valid MAC address configured"));
        flags.enable = false;
        flags.data_ok = false;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // Parse GATT characteristic config
    char characteristic_uuid[128] = {0};
    PayloadFormat format = (PayloadFormat)payload_format_cfg;
    if (characteristic_uuid_cfg[0]) {
        ble_copy_stripped(characteristic_uuid, sizeof(characteristic_uuid), characteristic_uuid_cfg);
    } else if (userdef_unit && strlen(userdef_unit) > 0) {
        uint8_t fmt = (uint8_t)FORMAT_TEMP_001;
        ble_parse_uuid_and_format_legacy(userdef_unit, characteristic_uuid, sizeof(characteristic_uuid), &fmt);
        format = (PayloadFormat)fmt;
    }
    bool has_gatt_config = (characteristic_uuid[0] != 0);

    // ec88 service UUIDs are advertisement-only, not readable characteristics
    if (has_gatt_config && ble_is_uuid_ec88_service(characteristic_uuid)) {
        has_gatt_config = false;
        characteristic_uuid[0] = 0;
    }

    // Check device cache
    const BLEDeviceInfo* cached_dev = sensor_ble_find_device(mac_address);
    bool is_adv_sensor = cached_dev && sensor_ble_is_adv_sensor(cached_dev);
    bool is_bms = cached_dev && sensor_ble_is_bms_type(cached_dev->sensor_type);

    // =========================================================================
    // BROADCAST SENSORS: Read directly from cache (background scan fills it)
    // No scan triggering, no retry backoff - data arrives via passive scan.
    // Unsolicited data is accepted immediately regardless of interval.
    // pushAdvData() in sensor_ble_loop() also proactively updates all matching
    // sensors when new advertisement data arrives.
    // =========================================================================
    if (is_adv_sensor && !has_gatt_config) {
        if (cached_dev->has_adv_data && (millis() - cached_dev->last_seen < 300000)) {
            double parsed_value = ble_select_adv_value(cached_dev, assigned_unitid);
            store_result(parsed_value, time);
            repeat_read = 0;  // Direct return, no Phase 2 needed
            last_battery = cached_dev->adv_battery;
            adv_last_success_time = time;
            DEBUG_PRINTF("[BLE] %s: broadcast data T=%.1f H=%.1f B=%d\n",
                         name, cached_dev->adv_temperature, cached_dev->adv_humidity, cached_dev->adv_battery);
            return HTTP_RQT_SUCCESS;
        }
        // No fresh data yet - background scan will pick it up
        DEBUG_PRINTF("[BLE] %s: waiting for broadcast data\n", name);
        flags.data_ok = false;
        last_read = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // =========================================================================
    // BMS SENSORS: GATT bidirectional (connect → write command → read response)
    // =========================================================================
    if (is_bms && cached_dev->sensor_type == BLE_TYPE_BMS_JBD) {
        bool lock_ok = ble_sensor_lock_acquire(500);
        if (!lock_ok) {
            DEBUG_PRINTLN(F("[BLE] BMS read skipped: semaphore busy"));
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        // Pause background scan for GATT connection
        ble_bg_scan_stop();

        BLEClient* pClient = ble_get_client();
        if (!pClient) {
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        BLEAddress bleAddress(mac_address);
        if (!pClient->connect(bleAddress, 0, BLE_CONNECT_TIMEOUT_MS)) {
            DEBUG_PRINTLN(F("[BLE] BMS connect failed"));
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        DEBUG_PRINTLN(F("[BLE] Connected to JBD BMS"));

        BLERemoteService* pService = pClient->getService(BLEUUID(JBD_SERVICE_UUID));
        if (!pService) {
            pClient->disconnect();
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        BLERemoteCharacteristic* pTxChar = pService->getCharacteristic(BLEUUID(JBD_TX_CHAR_UUID));
        BLERemoteCharacteristic* pRxChar = pService->getCharacteristic(BLEUUID(JBD_RX_CHAR_UUID));
        if (!pTxChar || !pTxChar->canWrite() || !pRxChar || !pRxChar->canRead()) {
            pClient->disconnect();
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        // Send read command
        uint8_t cmd_buf[7];
        uint8_t cmd_len = jbd_build_command(JBD_CMD_READ_BASIC, cmd_buf);
        pTxChar->writeValue(cmd_buf, cmd_len);
        delay(100);

        // Read response
        String response = pRxChar->readValue().c_str();
        pClient->disconnect();
        ble_bg_scan_start();
        ble_sensor_lock_release();

        if (response.length() < 7) {
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        const uint8_t* resp_data = (const uint8_t*)response.c_str();
        const uint8_t* payload = nullptr;
        size_t payload_len = 0;
        if (!jbd_validate_response(resp_data, response.length(), &payload, &payload_len)) {
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        float voltage = 0, current = 0, temperature = 0;
        uint8_t soc = 0;
        uint16_t cycles = 0;
        if (!jbd_parse_basic_info(payload, payload_len, &voltage, &current, &soc, &temperature, &cycles)) {
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        DEBUG_PRINTF("[BLE] JBD BMS: V=%.1f I=%.1f SOC=%d T=%.1f Cyc=%d\n",
                     voltage, current, soc, temperature, cycles);

        // Update device cache
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

        last_battery = soc;
        double parsed_value = ble_select_bms_value(voltage, current, soc, temperature, assigned_unitid);
        store_result(parsed_value, time);
        adv_last_success_time = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // =========================================================================
    // GATT POLL SENSORS: Connect → read characteristic → disconnect
    // For sensors that don't broadcast but have readable GATT characteristics
    // =========================================================================
    if (has_gatt_config && strlen(characteristic_uuid) > 0) {
        bool lock_ok = ble_sensor_lock_acquire(500);
        if (!lock_ok) {
            DEBUG_PRINTLN(F("[BLE] GATT read skipped: semaphore busy"));
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        // Pause background scan for GATT
        ble_bg_scan_stop();

        BLEClient* pClient = ble_get_client();
        if (!pClient) {
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        BLEAddress bleAddress(mac_address);
        if (!pClient->connect(bleAddress, 0, BLE_CONNECT_TIMEOUT_MS)) {
            DEBUG_PRINTLN(F("[BLE] GATT connect failed"));
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        DEBUG_PRINTF("[BLE] Connected for GATT read: %s char=%s\n", mac_address, characteristic_uuid);

        // Sanitize and create UUID
        char uuid_clean[128] = {0};
        ble_copy_stripped(uuid_clean, sizeof(uuid_clean), characteristic_uuid);
        BLEUUID targetCharUUID(uuid_clean);

        // Find characteristic - try standard Environmental Sensing (0x181A) first
        BLERemoteCharacteristic* pChar = nullptr;
        BLERemoteService* pSvc = pClient->getService(BLEUUID((uint16_t)0x181A));
        if (pSvc) {
            pChar = pSvc->getCharacteristic(targetCharUUID);
        }

        // Search all services if not found
        if (!pChar) {
#if defined(ESP32C5)
            DEBUG_PRINTLN(F("[BLE] Skipping full service scan on ESP32-C5"));
#else
            std::map<std::string, BLERemoteService*>* services = pClient->getServices();
            if (services) {
                for (auto& svcPair : *services) {
                    if (!svcPair.second) continue;
                    pChar = svcPair.second->getCharacteristic(targetCharUUID);
                    if (pChar) break;
                }
            }
#endif
        }

        if (!pChar || !pChar->canRead()) {
            DEBUG_PRINTLN(F("[BLE] Characteristic not found or not readable"));
            pClient->disconnect();
            ble_bg_scan_start();
            ble_sensor_lock_release();
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        String value_str = pChar->readValue().c_str();
        pClient->disconnect();
        ble_bg_scan_start();
        ble_sensor_lock_release();

        if (value_str.length() == 0) {
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        DEBUG_PRINTF("[BLE] Read %d bytes from GATT\n", value_str.length());

        double parsed_value = 0.0;
        if (!decode_payload((const uint8_t*)value_str.c_str(), value_str.length(), format, &parsed_value)) {
            DEBUG_PRINTLN(F("[BLE] Payload decode failed"));
            flags.data_ok = false;
            last_read = time;
            return HTTP_RQT_NOT_RECEIVED;
        }

        store_result(parsed_value, time);
        adv_last_success_time = time;
        return HTTP_RQT_NOT_RECEIVED;
    }

    // No usable config - ADV sensor not yet seen or unknown device
    if (!cached_dev) {
        DEBUG_PRINTF("[BLE] %s: device not yet discovered (background scan active)\n", name);
    }
    flags.data_ok = false;
    last_read = time;
    return HTTP_RQT_NOT_RECEIVED;
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

bool sensor_ble_reinit_after_matter() {
    if (!ble_initialized) {
        ble_initialized = sensor_ble_init();
        if (!ble_initialized) {
            DEBUG_PRINTLN("[BLE] Failed to initialize BLE after Matter");
            return false;
        }
    }

    ble_semaphore_init();

    pBLEScan = BLEDevice::getScan();
    if (pBLEScan) {
        DEBUG_PRINTLN("[BLE] Reinit after Matter - attaching scan callbacks, starting bg scan");
        pBLEScan->setAdvertisedDeviceCallbacks(&ble_scan_callbacks, true);
        // Start background passive scan
        ble_bg_scan_start();
        return true;
    }

    return false;
}

/**
 * @brief Initialize BLE access semaphore (call during startup)
 * Creates the semaphore that synchronizes access between Matter and Sensors
 */
void ble_semaphore_init() {
    if (!ble_access_semaphore) {
        // Create binary semaphore (1 = BLE available, 0 = BLE locked)
        ble_access_semaphore = xSemaphoreCreateBinary();
        if (ble_access_semaphore) {
            // Initial state: BLE is available (semaphore = 1)
            xSemaphoreGive(ble_access_semaphore);
            DEBUG_PRINTLN("[BLE] Semaphore initialized");
        }
    }
}

/**
 * @brief Try to acquire BLE access with timeout
 * @param timeout_ms Timeout in milliseconds
 * @return true if semaphore acquired, false if timeout
 */
bool sensor_ble_acquire(uint32_t timeout_ms) {
    // Use the same re-entrant lock path as sensors to avoid semaphore state mismatch
    bool acquired = ble_sensor_lock_acquire(timeout_ms);
    if (!acquired) {
        DEBUG_PRINTLN("[BLE] sensor_ble_acquire timeout");
    }
    return acquired;
}

/**
 * @brief Release BLE access (must match a prior acquire)
 */
void sensor_ble_release() {
    ble_sensor_lock_release();
}

#endif // ESP32
