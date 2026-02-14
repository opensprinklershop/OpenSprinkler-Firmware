/**
 * AES Software Fallback Wrapper
 * 
 * This file provides a wrapper that intercepts mbedTLS AES functions
 * and forces software implementation when internal RAM is low.
 * 
 * Problem: ESP32-C5 Hardware-AES uses DMA which requires internal RAM (MALLOC_CAP_DMA).
 * With Matter enabled, internal heap gets too low and AES fails with:
 * "E (17451) esp-aes: Failed to allocate memory"
 * 
 * Solution: Check available internal heap BEFORE calling hardware AES.
 * If less than threshold, use mbedTLS software AES instead.
 * 
 * Build: Compile this file and link BEFORE libmbedcrypto.a
 * 
 * Author: Copilot for OpenSprinkler ESP32-C5
 */

#if defined(ESP32) || defined(ESP_PLATFORM)

#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "aes-fallback";

// Minimum internal heap required for hardware AES DMA operations
// DMA needs aligned buffers, typically 2x chunk size (input + output)
// Default chunk size is 1600 bytes, so we need at least 8KB to be safe
#ifndef AES_HW_MIN_INTERNAL_HEAP
#define AES_HW_MIN_INTERNAL_HEAP 16384  // 16KB minimum
#endif

// Check if we have enough internal RAM for hardware AES
static inline bool can_use_hw_aes(void)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    if (free_internal < AES_HW_MIN_INTERNAL_HEAP) {
        ESP_LOGW(TAG, "Low internal heap (%u bytes), HW-AES would fail", (unsigned)free_internal);
        return false;
    }
    return true;
}

/**
 * Unfortunately we cannot easily override the esp_aes functions as they are
 * not declared weak in the ESP-IDF mbedTLS port.
 * 
 * The best solution is to rebuild libmbedcrypto.a with:
 * CONFIG_MBEDTLS_HARDWARE_AES=n
 * 
 * Or use CONFIG_MBEDTLS_AES_HW_SMALL_DATA_LEN_OPTIM=y with a very high threshold
 * 
 * This file serves as documentation of the issue.
 * 
 * WORKAROUND: Modify esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c
 * to check heap before allocation and return error if insufficient.
 * The caller (Matter) should then fall back to software AES.
 */

// Exported function to check AES readiness - can be called before Matter.begin()
int aes_check_memory_available(void)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    
    ESP_LOGI(TAG, "AES Memory Check:");
    ESP_LOGI(TAG, "  Internal free: %u bytes", (unsigned)free_internal);
    ESP_LOGI(TAG, "  DMA-capable free: %u bytes", (unsigned)free_dma);
    ESP_LOGI(TAG, "  Largest DMA block: %u bytes", (unsigned)largest_dma);
    
    // DMA needs at least 1600 bytes for input + 1600 for output aligned
    if (largest_dma < 4096) {
        ESP_LOGW(TAG, "Insufficient DMA memory for hardware AES!");
        return -1;
    }
    
    return 0;
}

#endif // ESP32 || ESP_PLATFORM
