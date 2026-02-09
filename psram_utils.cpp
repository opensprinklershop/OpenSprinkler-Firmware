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
#include "defines.h"

#if defined(SOC_PSRAM_DMA_CAPABLE)
#include <esp_psram.h>
#endif

// ============================================================================
// ESP-IDF mbedTLS Memory Override - Routes ALL TLS allocations to SPIRAM
// ============================================================================
// The precompiled ESP-IDF mbedtls library (libmbedcrypto.a) has esp_mbedtls_mem_calloc
// hardcoded with MALLOC_CAP_INTERNAL (0x804). We override it here to use SPIRAM.
//
// CRITICAL: The ESP-IDF library was compiled with C linkage, so we MUST use
// extern "C" here to match the symbol name. We declare the functions BEFORE
// including mbedtls/platform.h to avoid conflicts with esp_mem.h.

extern "C" {
// Override ESP-IDF's esp_mbedtls_mem_calloc - use PSRAM instead of internal RAM
// This function is called by mbedTLS for ALL allocations (MBEDTLS_PLATFORM_STD_CALLOC)
void* esp_mbedtls_mem_calloc(size_t n, size_t size) {
  size_t total = n * size;
  
  // Use SPIRAM for all mbedTLS allocations
  void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr) {
    return ptr;
  }
  
  // Fallback to any available memory (including internal) if SPIRAM fails
  ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  if (!ptr) {
    DEBUG_PRINTF("[mbedTLS] ALLOC FAILED: %d bytes (SPIRAM and internal exhausted)\n", total);
  }
  return ptr;
}

// Override ESP-IDF's esp_mbedtls_mem_free
void esp_mbedtls_mem_free(void* ptr) {
  if (ptr) {
    heap_caps_free(ptr);
  }
}
}  // extern "C"

// Now include mbedtls/platform.h (which includes esp_mem.h)
// Our extern "C" definitions above will take precedence
#include <mbedtls/platform.h>

// ============================================================================
// mbedTLS PSRAM Allocator (backup for mbedtls_platform_set_calloc_free)
// ============================================================================
// This is kept as a fallback mechanism but the extern "C" override above
// should handle all allocations in the precompiled ESP-IDF mbedtls library

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
// Rate-limited to avoid serial spam from WiFi DMA buffer polling
static uint32_t alloc_fail_count = 0;
static uint32_t alloc_fail_last_log = 0;
static const uint32_t ALLOC_FAIL_LOG_INTERVAL_MS = 30000; // Log summary every 30s

static void psram_alloc_failed_callback(size_t size, uint32_t caps, const char *function_name) {
  alloc_fail_count++;
  
  uint32_t now = millis();
  if (now - alloc_fail_last_log >= ALLOC_FAIL_LOG_INTERVAL_MS) {
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    DEBUG_PRINTF("[ALLOC_FAIL] %d failures in last %ds | Last: %d bytes (caps=0x%X) in %s | Internal=%d, SPIRAM=%d\n", 
                 alloc_fail_count, ALLOC_FAIL_LOG_INTERVAL_MS/1000,
                 size, caps, function_name ? function_name : "unknown",
                 internal_free, spiram_free);
    alloc_fail_count = 0;
    alloc_fail_last_log = now;
  }
}

// PSRAM-allocated buffers
char* ether_buffer = nullptr;
char* tmp_buffer = nullptr;

void init_psram_buffers() {
  // Set PSRAM malloc threshold: allocations >= this size go to PSRAM via malloc().
  // 
  // STRATEGY: Use a LOW threshold to route most malloc() to PSRAM.
  // WiFi DMA allocations are SAFE because they use heap_caps_malloc() with
  // MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL, which always goes to internal RAM
  // regardless of this threshold. The threshold only affects plain malloc()/calloc().
  //
  // WiFi BSS is placed in internal .dram0.bss by linker script (sections.ld),
  // so static WiFi state is also safe.
  //
  // By setting threshold low, Matter/CHIP allocations (many small mallocs for
  // attributes, clusters, event handlers) go to PSRAM, freeing ~40-50 KB of
  // internal RAM for WiFi DMA buffers at runtime.
  //
  // Note: Some FreeRTOS internals and ISR-related allocations need internal RAM
  // and use heap_caps_malloc() with appropriate caps, so they're also unaffected.
  // 
  // IMPORTANT: BLE Controller uses plain malloc() and NEEDS internal RAM for its
  // structures. Threshold too low (e.g., 64) causes BLE controller init to fail
  // with "ble ll env init error code:-21" and crash. Keep at 4096 (default).
  // Start with threshold=SIZE_MAX: ALL plain malloc stays in internal RAM.
  // This protects WiFi/BLE init which uses plain malloc() for structures that
  // MUST be in internal RAM (ESP32-C5 Rev 1.0 PSRAM memory barrier broken).
  // Threshold is lowered to 8 by psram_restore_after_wifi_init() only after
  // WiFi is fully connected. Our own PSRAM buffers use heap_caps_calloc()
  // with MALLOC_CAP_SPIRAM directly, so they are unaffected by the threshold.
  heap_caps_malloc_extmem_enable(SIZE_MAX);
  DEBUG_PRINTLN(F("[PSRAM] malloc threshold=SIZE_MAX (all malloc->internal until WiFi connected)"));

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

// ============================================================================
// WiFi PSRAM Protection
// ============================================================================
// ESP32-C5 Rev 1.0 has a broken PSRAM memory barrier (esp_psram_mspi_mb() is
// a no-op because the dummy cacheline allocation fails). This means the WiFi
// blob's internal structures MUST NOT be allocated in PSRAM, or Load access
// faults will occur when the WiFi hardware/DMA accesses them.
//
// WiFi blob uses plain malloc() internally. The heap_caps routing sends
// allocations >= threshold to PSRAM. Even with threshold=4096, WiFi allocates
// structures >4KB (e.g. NVS buffers, scan lists) that land in PSRAM and crash.
//
// Solution: Temporarily disable PSRAM routing during WiFi.mode()/WiFi.begin()
// by setting threshold to SIZE_MAX (nothing goes to PSRAM). After WiFi is
// initialized, restore the normal threshold so app-level large allocations
// can use PSRAM again.
// ============================================================================

void psram_protect_wifi_init() {
  // Ensure ALL malloc() stays in internal RAM during WiFi.mode()/WiFi.begin().
  // Threshold is SIZE_MAX from boot, but re-assert here in case something
  // changed it (e.g. after a WiFi reconnect cycle).
  heap_caps_malloc_extmem_enable(SIZE_MAX);
  DEBUG_PRINTLN(F("[PSRAM] WiFi init: malloc routed to internal RAM"));
}

void psram_restore_after_wifi_init() {
  // Restore normal PSRAM threshold so app-level allocations use PSRAM again.
  heap_caps_malloc_extmem_enable(8);
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  DEBUG_PRINTF("[PSRAM] WiFi init done, threshold restored to %d. Internal free=%d KB\n",
               8, internal_free/1024);
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

#endif // defined(ESP32) && defined(BOARD_HAS_PSRAM)
