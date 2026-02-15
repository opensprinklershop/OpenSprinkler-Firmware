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
#include <esp_idf_version.h>
#include <mbedtls/platform.h>
#include "defines.h"

// heap_caps_malloc_extmem_enable() lowers the ALWAYSINTERNAL threshold at runtime
extern "C" void heap_caps_malloc_extmem_enable(size_t limit);

// ============================================================================
// EARLY PSRAM THRESHOLD — runs before Arduino setup() and FreeRTOS services.
//
// Without the heap_caps_spiram_16byte.patch applied to the framework library,
// the ESP-IDF default is MALLOC_DISABLE_EXTERNAL_ALLOCS (-1), meaning every
// malloc() goes to internal RAM until heap_caps_malloc_extmem_enable() is
// called.  WiFi, FreeRTOS, and framework tasks that start before setup() can
// burn 20-30 KB of internal SRAM that way.
//
// This constructor runs at priority 101 (after static C++ init), sets the
// threshold to 128 bytes immediately, and saves that internal RAM for
// components that truly need it (BLE controller DMA, Zigbee ZBOSS).
// ============================================================================
static void __attribute__((constructor(101))) psram_early_threshold() {
    if (psramFound()) {
        heap_caps_malloc_extmem_enable(128);
    }
}

// DMA+SPIRAM fallback detection: check sdkconfig flags at compile time
#if defined(CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP) || defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)
static bool psram_dma_spiram_fallback_enabled() { return true; }
static const char* psram_dma_spiram_fallback_status() { return "enabled (sdkconfig)"; }
#else
static bool psram_dma_spiram_fallback_enabled() { return false; }
static const char* psram_dma_spiram_fallback_status() { return "disabled"; }
#endif

// Allocation failed callback: log when internal RAM allocation fails
// Rate-limited to avoid flooding the serial log when a component (e.g. ZBOSS
// Zigbee stack) repeatedly requests DMA+INTERNAL buffers in its runtime loop.
static uint32_t _alloc_fail_count = 0;
static uint32_t _alloc_fail_last_log_ms = 0;
static const uint32_t ALLOC_FAIL_LOG_INTERVAL_MS = 10000; // summarize every 10s

static void psram_alloc_failed_callback(size_t size, uint32_t caps, const char *function_name) {
  _alloc_fail_count++;

  uint32_t now = millis();
  // Log the very first failure immediately, then rate-limit.
  if (_alloc_fail_count == 1) {
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    DEBUG_PRINTF("[ALLOC_FAIL] %d bytes (caps=0x%X) in %s - internal RAM exhausted!\n",
                 size, caps, function_name ? function_name : "unknown");
    DEBUG_PRINTF("[ALLOC_FAIL] Internal=%u bytes free, SPIRAM=%u bytes free\n",
                 internal_free, spiram_free);
    if ((caps & MALLOC_CAP_DMA) && (caps & MALLOC_CAP_INTERNAL)) {
      DEBUG_PRINTLN(F("[ALLOC_FAIL] DMA+INTERNAL request cannot use SPIRAM (MALLOC_CAP_INTERNAL flag)"));
    }
    _alloc_fail_last_log_ms = now;
  } else if ((int32_t)(now - _alloc_fail_last_log_ms) >= (int32_t)ALLOC_FAIL_LOG_INTERVAL_MS) {
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    DEBUG_PRINTF("[ALLOC_FAIL] %u failures in last %us (last: %d bytes caps=0x%X, internal free=%u)\n",
                 _alloc_fail_count, ALLOC_FAIL_LOG_INTERVAL_MS / 1000,
                 size, caps, internal_free);
    _alloc_fail_count = 0;
    _alloc_fail_last_log_ms = now;
  }
  // Silently drop intermediate failures to prevent serial flood
}

// PSRAM-allocated buffers
char* ether_buffer = nullptr;
char* tmp_buffer = nullptr;

void init_psram_buffers() {
  psramAddToHeap();

  // Lower the SPIRAM_MALLOC_ALWAYSINTERNAL threshold from 4096 → 128 bytes
  // ESP32-C5 PSRAM is DMA-capable — no reason to burn internal RAM for most allocs.
  // Only tiny allocations (<= 128 bytes) will still prefer internal RAM for speed.
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(128);
    DEBUG_PRINTLN(F("[PSRAM] Lowered ALWAYSINTERNAL threshold: 4096 → 128 bytes"));
  }

  // Register allocation failed callback for PSRAM fallback
  if (psramFound()) {
    esp_err_t err = heap_caps_register_failed_alloc_callback(psram_alloc_failed_callback);
    if (err == ESP_OK) {
      DEBUG_PRINTLN(F("[PSRAM] Registered alloc_failed_callback for PSRAM fallback"));
    } else {
      DEBUG_PRINTF("[PSRAM] WARNING: Failed to register callback (err=%d)\n", err);
    }
  }
  
  // Debug: Check PSRAM initialization
  DEBUG_PRINTLN(F("\n====== PSRAM INITIALIZATION DEBUG ======"));
  
  // Show DMA+SPIRAM fallback status
  if (psram_dma_spiram_fallback_enabled()) {
    DEBUG_PRINTLN(F("[PSRAM] DMA+SPIRAM Fallback: ACTIVE"));
    DEBUG_PRINTLN(F("[PSRAM]   Hardware crypto/DMA can use PSRAM when internal RAM exhausted"));
  } else {
    DEBUG_PRINTF("[PSRAM] DMA+SPIRAM Fallback: %s\n", psram_dma_spiram_fallback_status());
  }
  
  DEBUG_PRINTF("[PSRAM] psramFound() = %d\n", psramFound());
  DEBUG_PRINTF("[PSRAM] ESP.getPsramSize() = %d bytes\n", ESP.getPsramSize());
  DEBUG_PRINTF("[PSRAM] ESP.getFreePsram() = %d bytes\n", ESP.getFreePsram());
  
  // Check heap capabilities
  uint32_t spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  
  DEBUG_PRINTF("[HEAP] INTERNAL: %d/%d KB (%.1f%% free)\n", 
               internal_free/1024, internal_total/1024, 100.0*internal_free/internal_total);
  DEBUG_PRINTF("[HEAP] SPIRAM: %d/%d KB (%.1f%% free)\n", 
               spiram_free/1024, spiram_total/1024, 
               spiram_total > 0 ? 100.0*spiram_free/spiram_total : 0);
  
  if (psramFound()) {
    #ifdef ENABLE_MEMORY_DEBUG
    // Detailed memory analysis (only when debug enabled)
    uint32_t psram_size = ESP.getPsramSize();
    uint32_t psram_free = ESP.getFreePsram();
    uint32_t heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t heap_spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    DEBUG_PRINTLN(F("\n========== DETAILED MEMORY ANALYSIS =========="));
    DEBUG_PRINTF("[PSRAM] %d MB total, %d MB free\n", psram_size/1048576, psram_free/1048576);
    DEBUG_PRINTF("[HEAP]  Internal: %d KB total, %d KB free (%.1f%% used)\n",
                 heap_internal_total/1024, heap_internal_free/1024,
                 100.0*(heap_internal_total-heap_internal_free)/heap_internal_total);
    DEBUG_PRINTF("[HEAP]  SPIRAM: %d KB total, %d KB free\n",
                 heap_spiram_total/1024, heap_spiram_free/1024);
    DEBUG_PRINTF("[HP SRAM] ESP32-C5: 384 KB total, %d KB heap, ~%d KB code/stack\n",
                 heap_internal_total/1024, 384-heap_internal_total/1024);
    DEBUG_PRINTLN(F("========================================\n"));
    #endif
    
    // Test allocation to verify PSRAM works
    DEBUG_PRINTLN(F("[TEST] Attempting test allocation from PSRAM..."));
    void *test_ptr = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (test_ptr) {
      DEBUG_PRINTF("[TEST] Allocation successful @ %p\n", test_ptr);
      heap_caps_free(test_ptr);
    } else {
      DEBUG_PRINTLN(F("[TEST] FAILED - Could not allocate from PSRAM!"));
    }
    
    // Allocate buffers directly from PSRAM (now auto-enabled by framework)
    ether_buffer = (char*)heap_caps_malloc(ETHER_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tmp_buffer = (char*)heap_caps_malloc(TMP_BUFFER_SIZE_L, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ether_buffer) {
      memset(ether_buffer, 0, ETHER_BUFFER_SIZE_L);
      DEBUG_PRINTF("[PSRAM] ether_buffer: %d bytes @ %p\n", 
                   ETHER_BUFFER_SIZE_L, ether_buffer);
    } else {
      DEBUG_PRINTLN(F("[PSRAM] ERROR: ether_buffer allocation FAILED"));
    }
    if (tmp_buffer) {
      memset(tmp_buffer, 0, TMP_BUFFER_SIZE_L);
      DEBUG_PRINTF("[PSRAM] tmp_buffer: %d bytes @ %p\n", 
                   TMP_BUFFER_SIZE_L, tmp_buffer);
    } else {
      DEBUG_PRINTLN(F("[PSRAM] ERROR: tmp_buffer allocation FAILED"));
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

// Forward declaration for mbedTLS SPIRAM allocator (defined in mbedtls_spiram_alloc.c)
extern "C" void mbedtls_platform_set_spiram_allocators(void);

void init_mbedtls_psram_allocator() {
  if (psramFound()) {
    mbedtls_platform_set_spiram_allocators();
    DEBUG_PRINTLN(F("[PSRAM] mbedTLS allocators set to SPIRAM fallback"));
  } else {
    DEBUG_PRINTLN(F("[PSRAM] No PSRAM — mbedTLS using default allocators"));
  }
}

// WiFi PSRAM protection: temporarily force all malloc to internal RAM
// ESP32-C5 Rev 1.0 has a broken PSRAM memory barrier → cache incoherency
// WiFi driver buffers MUST be in internal SRAM during init/scan/connect

static bool _wifi_psram_protected = false;

void psram_protect_wifi_init() {
  if (!psramFound()) return;
  _wifi_psram_protected = true;
  DEBUG_PRINTLN(F("[PSRAM] WiFi protection: forcing malloc to internal RAM"));
}

void psram_restore_after_wifi_init() {
  if (!_wifi_psram_protected) return;
  _wifi_psram_protected = false;
  DEBUG_PRINTLN(F("[PSRAM] WiFi protection lifted: PSRAM malloc restored"));
}

void log_matter_ble_memory_optimization() {
  DEBUG_PRINTLN(F("\n====== MATTER/BLE MEMORY OPTIMIZATION ======"));
  
  uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  
  DEBUG_PRINTF("[MEM] Internal free: %d KB, SPIRAM free: %d KB\n",
               internal_free / 1024, spiram_free / 1024);
  
#ifdef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
  DEBUG_PRINTLN(F("[BLE] NimBLE heap: PSRAM (external)"));
#elif defined(CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_IRAM_8BIT)
  DEBUG_PRINTLN(F("[BLE] NimBLE heap: IRAM 8-bit"));
#else
  DEBUG_PRINTLN(F("[BLE] NimBLE heap: internal (default)"));
#endif

#ifdef CONFIG_CHIP_PLATFORM_ESP32
  DEBUG_PRINTLN(F("[MATTER] ESP Matter platform active"));
#endif

  DEBUG_PRINTLN(F("============================================\n"));
}

#else
// Non-PSRAM platforms: static allocation
char ether_buffer[ETHER_BUFFER_SIZE_L];
char tmp_buffer[TMP_BUFFER_SIZE_L];
#endif
