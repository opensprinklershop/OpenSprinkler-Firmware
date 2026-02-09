/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * PSRAM Memory Utilities
 * Jan 2026 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler Firmware
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

#ifndef _PSRAM_UTILS_H
#define _PSRAM_UTILS_H

#if defined(ESP32) && defined(BOARD_HAS_PSRAM)

// Buffer sizes are defined in defines.h, use those directly
// External buffer declarations
extern char* ether_buffer;
extern char* tmp_buffer;

// PSRAM buffer management
void init_psram_buffers();
void print_psram_stats();

// mbedTLS PSRAM allocator - call BEFORE any HTTPS/TLS operations!
void init_mbedtls_psram_allocator();

// WiFi PSRAM protection - temporarily forces ALL malloc to internal RAM
// Call psram_protect_wifi_init() BEFORE WiFi.mode()/WiFi.begin()
// Call psram_restore_after_wifi_init() AFTER WiFi.status()==WL_CONNECTED (or timeout)
// Do NOT call restore immediately after WiFi.begin() — scan/connect is async!
// Required on ESP32-C5 Rev 1.0 where PSRAM memory barrier is broken
void psram_protect_wifi_init();
void psram_restore_after_wifi_init();

// Memory optimization logging (Matter & BLE)
void log_matter_ble_memory_optimization();

#else
// Non-PSRAM platforms
extern char ether_buffer[];
extern char tmp_buffer[];

inline void init_psram_buffers() {}
inline void print_psram_stats() {}
inline void init_mbedtls_psram_allocator() {}
inline void psram_protect_wifi_init() {}
inline void psram_restore_after_wifi_init() {}
inline void log_matter_ble_memory_optimization() {}

#endif

#endif // _PSRAM_UTILS_H
