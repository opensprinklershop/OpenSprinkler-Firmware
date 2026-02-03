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
#include <esp_heap_caps_init.h>
#include <mbedtls/platform.h>
#include "defines.h"

#if defined(SOC_PSRAM_DMA_CAPABLE)
#include <esp_psram.h>
#endif

// ============================================================================
// mbedTLS PSRAM Allocator - Routes SSL/TLS allocations to SPIRAM
// ============================================================================
// This is critical for HTTPS connections on memory-constrained ESP32-C5
// The default mbedTLS allocator uses internal RAM which is limited to 384KB

static void* mbedtls_psram_calloc(size_t n, size_t size) {
  size_t total = n * size;
  
  // Try SPIRAM first (preferred for large TLS buffers)
  void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr) {
    return ptr;
  }
  
  // Fallback to any available memory
  ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  if (ptr) {
    DEBUG_PRINTF("[mbedTLS] SPIRAM fallback: %d bytes from internal\n", total);
  } else {
    DEBUG_PRINTF("[mbedTLS] ALLOC FAILED: %d bytes\n", total);
  }
  return ptr;
}

static void mbedtls_psram_free(void* ptr) {
  if (ptr) {
    heap_caps_free(ptr);
  }
}

// Initialize mbedTLS to use PSRAM allocator
// MUST be called before any HTTPS/TLS operations!
void init_mbedtls_psram_allocator() {
  int ret = mbedtls_platform_set_calloc_free(mbedtls_psram_calloc, mbedtls_psram_free);
  if (ret == 0) {
    DEBUG_PRINTLN(F("[mbedTLS] PSRAM allocator installed successfully"));
  } else {
    DEBUG_PRINTF("[mbedTLS] WARNING: Failed to set allocator (ret=%d)\n", ret);
  }
}

// Allocation failed callback: log when internal RAM allocation fails
// Note: ESP32 API only allows logging, not replacement allocation
static void psram_alloc_failed_callback(size_t size, uint32_t caps, const char *function_name) {
  DEBUG_PRINTF("[ALLOC_FAIL] %d bytes (caps=0x%X) in %s - internal RAM exhausted!\n", 
               size, caps, function_name ? function_name : "unknown");
  
  // Log available memory
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  DEBUG_PRINTF("[ALLOC_FAIL] Available: Internal=%d bytes, SPIRAM=%d bytes\n", 
               internal_free, spiram_free);
  
  if ((caps & MALLOC_CAP_DMA) && (caps & MALLOC_CAP_INTERNAL)) {
    DEBUG_PRINTLN(F("[ALLOC_FAIL] This was a DMA+INTERNAL allocation - caller should retry with SPIRAM!"));
  }
}

// PSRAM-allocated buffers
char* ether_buffer = nullptr;
char* tmp_buffer = nullptr;

void init_psram_buffers() {
  heap_caps_malloc_extmem_enable(4);

  if (!psramFound()) {
    DEBUG_PRINTLN(F("[PSRAM] WARNING: No PSRAM - using internal RAM (may cause issues)"));
    // Fallback: allocate from internal RAM
    ether_buffer = (char*)calloc(1, ETHER_BUFFER_SIZE_L);
    tmp_buffer = (char*)calloc(1, TMP_BUFFER_SIZE_L);
    return;
  }

  esp_err_t err = heap_caps_register_failed_alloc_callback(psram_alloc_failed_callback);
  if (err != ESP_OK) {
    DEBUG_PRINTF("[PSRAM] WARNING: Failed to register callback (err=%d)\n", err);
  }
  
  #if defined(ENABLE_DEBUG)
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  DEBUG_PRINTF("[HEAP] INTERNAL: %d KB | SPIRAM: %d KB\n", 
               internal_free/1024, spiram_free/1024);
  #endif

  // Allocate buffers from PSRAM using calloc (automatic zero-init)
  ether_buffer = (char*)heap_caps_calloc(1, ETHER_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  tmp_buffer = (char*)heap_caps_calloc(1, TMP_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  
  if (ether_buffer) {
    DEBUG_PRINTF("[PSRAM] ether_buffer: %d KB allocated\n", ETHER_BUFFER_SIZE_L/1024);
  } else {
    DEBUG_PRINTLN(F("[PSRAM] ERROR: ether_buffer allocation FAILED"));
  }
  if (tmp_buffer) {
    DEBUG_PRINTF("[PSRAM] tmp_buffer: %d KB allocated\n", TMP_BUFFER_SIZE_L/1024);
  } else {
    DEBUG_PRINTLN(F("[PSRAM] ERROR: tmp_buffer allocation FAILED"));
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

// Matter & BLE RAM optimization summary
void log_matter_ble_memory_optimization() {
  #if defined(ENABLE_MATTER) || defined(OS_ENABLE_BLE)
  DEBUG_PRINTLN(F("\n[OPTIMIZATION] Matter & BLE Memory Configuration:"));
  
  #ifdef ENABLE_MATTER
  DEBUG_PRINTLN(F("  Matter: Enabled"));
  #endif
  
  #ifdef OS_ENABLE_BLE
  DEBUG_PRINTLN(F("  BLE: NimBLE with SPIRAM allocation"));
  DEBUG_PRINTLN(F("    - MEM_ALLOC_MODE_EXTERNAL=y (heap in PSRAM)"));
  DEBUG_PRINTLN(F("    - Host task stack: 5120 bytes"));
  #endif
  
  // Show current RAM status
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t internal_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  
  DEBUG_PRINTF("  RAM Status:\n");
  DEBUG_PRINTF("    - Internal: %d KB free\n", internal_free/1024);
  DEBUG_PRINTF("    - DMA-capable: %d KB free\n", internal_dma/1024);
  DEBUG_PRINTF("    - SPIRAM: %d KB free\n", spiram_free/1024);
  
  // Warn if internal RAM is low (WiFi needs ~20KB DMA)
  if (internal_dma < 25600) {
    DEBUG_PRINTLN(F("  WARNING: Low internal DMA RAM! WiFi may fail."));
    DEBUG_PRINTLN(F("           Consider reducing BLE/Matter features."));
  }
  
  DEBUG_PRINTLN(F(""));
  #endif // ENABLE_MATTER || OS_ENABLE_BLE
}

#else
// Non-PSRAM platforms: static allocation
char ether_buffer[ETHER_BUFFER_SIZE_L];
char tmp_buffer[TMP_BUFFER_SIZE_L];
#endif
