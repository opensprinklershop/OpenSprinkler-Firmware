/**
 * mbedTLS Platform Memory Allocator Override
 * 
 * Provides custom memory allocators for mbedTLS that automatically
 * fall back to SPIRAM when internal RAM is exhausted.
 */

#if defined(ESP32) || defined(ESP_PLATFORM)

#include "mbedtls/platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MBEDTLS_ALLOC";

/**
 * Custom calloc for mbedTLS — ALWAYS prefer SPIRAM
 *
 * ESP32-C5 has DMA-capable PSRAM (CONFIG_SOC_PSRAM_DMA_CAPABLE=y),
 * so there is no reason to waste scarce internal RAM on mbedTLS buffers.
 * Only fall back to internal RAM as an absolute last resort.
 */
static void *mbedtls_calloc_spiram_fallback(size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    void *ptr = NULL;
    
    // 1) Always try SPIRAM first — it's DMA-capable on ESP32-C5 and plentiful
    ptr = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        return ptr;
    }
    
    // 2) SPIRAM exhausted — try internal RAM as fallback
    ptr = heap_caps_calloc(nmemb, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        ESP_LOGW(TAG, "mbedTLS calloc %u bytes fell back to INTERNAL RAM!", (unsigned)total_size);
        return ptr;
    }
    
    // 3) Last resort: try any available memory
    ptr = calloc(nmemb, size);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "mbedTLS calloc %u bytes FAILED — no memory available!", (unsigned)total_size);
    }
    
    return ptr;
}

/**
 * Custom free for mbedTLS
 */
static void mbedtls_free_spiram(void *ptr) {
    if (ptr != NULL) {
        free(ptr);
    }
}

/**
 * Initialize mbedTLS with SPIRAM-aware allocators
 */
void mbedtls_platform_set_spiram_allocators(void) {
    mbedtls_platform_set_calloc_free(mbedtls_calloc_spiram_fallback, mbedtls_free_spiram);
    ESP_LOGI(TAG, "mbedTLS memory allocators set to use SPIRAM fallback");
}

#endif // ESP32 || ESP_PLATFORM
