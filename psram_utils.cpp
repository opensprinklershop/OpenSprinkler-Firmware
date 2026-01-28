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

#include "psram_utils.h"

#if defined(ESP32) && defined(BOARD_HAS_PSRAM)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include "defines.h"

// PSRAM-allocated buffers
char* ether_buffer = nullptr;
char* tmp_buffer = nullptr;

void init_psram_buffers() {
  if (psramFound()) {
    #ifdef ENABLE_MEMORY_DEBUG
    // Detailed memory analysis (only when debug enabled)
    uint32_t psram_size = ESP.getPsramSize();
    uint32_t psram_free = ESP.getFreePsram();
    uint32_t heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t heap_spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    DEBUG_PRINTLN(F("\\n========== MEMORY ANALYSIS =========="));
    DEBUG_PRINTF("[PSRAM] %d MB total, %d MB free\\n", psram_size/1048576, psram_free/1048576);
    DEBUG_PRINTF("[HEAP]  Internal: %d KB total, %d KB free (%.1f%% used)\\n",
                 heap_internal_total/1024, heap_internal_free/1024,
                 100.0*(heap_internal_total-heap_internal_free)/heap_internal_total);
    DEBUG_PRINTF("[HEAP]  SPIRAM: %d KB total, %d KB free\\n",
                 heap_spiram_total/1024, heap_spiram_free/1024);
    DEBUG_PRINTF("[HP SRAM] ESP32-C5: 384 KB total, %d KB heap, ~%d KB code/stack\\n",
                 heap_internal_total/1024, 384-heap_internal_total/1024);
    DEBUG_PRINTLN(F("========================================\\n"));
    #endif
    
    // Allocate buffers directly from PSRAM (now auto-enabled by framework)
    ether_buffer = (char*)heap_caps_malloc(ETHER_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tmp_buffer = (char*)heap_caps_malloc(TMP_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ether_buffer) {
      memset(ether_buffer, 0, ETHER_BUFFER_SIZE_L);
      DEBUG_PRINTF("[PSRAM] ether_buffer: %d bytes allocated in PSRAM\n", ETHER_BUFFER_SIZE_L);
    }
    if (tmp_buffer) {
      memset(tmp_buffer, 0, TMP_BUFFER_SIZE_L);
      DEBUG_PRINTF("[PSRAM] tmp_buffer: %d bytes allocated in PSRAM\n", TMP_BUFFER_SIZE_L);
    }
  } else {
    DEBUG_PRINTLN(F("[PSRAM] WARNING: No PSRAM detected - using internal RAM"));
    DEBUG_PRINTLN(F("[PSRAM] This may cause memory issues with Matter/Zigbee"));
    ether_buffer = (char*)malloc(ETHER_BUFFER_SIZE_L);
    tmp_buffer = (char*)malloc(TMP_BUFFER_SIZE_L);
    if (ether_buffer) memset(ether_buffer, 0, ETHER_BUFFER_SIZE_L);
    if (tmp_buffer) memset(tmp_buffer, 0, TMP_BUFFER_SIZE_L);
  }
}

void print_psram_stats() {
  if (psramFound()) {
    DEBUG_PRINTF("[PSRAM] Free: %d/%d bytes (%.1f%% used)\n",
                 ESP.getFreePsram(), ESP.getPsramSize(),
                 100.0 - (ESP.getFreePsram() * 100.0 / ESP.getPsramSize()));
    DEBUG_PRINTF("[HEAP]  Free: %d bytes\n", ESP.getFreeHeap());
  }
}

// ===== mbedTLS PSRAM Allocator for SSL/TLS =====
// Custom allocator to use PSRAM for mbedTLS memory (for WiFiClientSecure)
// Note: mbedTLS requires calloc(nmemb, size) that zeros memory
static void* mbedtls_calloc_psram(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  void* ptr = nullptr;
  
  if (psramFound() && total > 1024) {  // Only use PSRAM for allocations > 1KB
    ptr = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
      memset(ptr, 0, total);  // calloc must zero memory
      return ptr;
    }
  }
  // Fallback to regular calloc
  return calloc(nmemb, size);
}

static void mbedtls_free_psram(void* ptr) {
  if (ptr) {
    // Use heap_caps_free which works for both PSRAM and DRAM
    heap_caps_free(ptr);
  }
}

void init_mbedtls_psram() {
  // Redirect mbedTLS memory allocation to custom allocator
  #if defined(MBEDTLS_PLATFORM_C) && defined(MBEDTLS_PLATFORM_MEMORY)
  mbedtls_platform_set_calloc_free(mbedtls_calloc_psram, mbedtls_free_psram);
  DEBUG_PRINTLN("[SSL] mbedTLS configured to use PSRAM for allocations > 1KB");
  #else
  DEBUG_PRINTLN("[SSL] WARNING: mbedTLS platform memory functions not available");
  #endif
}

#else
// Non-PSRAM platforms: static allocation
char ether_buffer[ETHER_BUFFER_SIZE_L];
char tmp_buffer[TMP_BUFFER_SIZE_L];
#endif
