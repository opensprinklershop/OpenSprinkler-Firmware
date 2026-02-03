/**
 * mbedTLS Platform Memory Allocator Override
 * 
 * Provides custom memory allocators for mbedTLS that automatically
 * fall back to SPIRAM when internal RAM is exhausted.
 */

#include "mbedtls/platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MBEDTLS_ALLOC";

/**
 * Custom calloc for mbedTLS with SPIRAM fallback
 */
static void *mbedtls_calloc_spiram_fallback(size_t nmemb, size_t size) {
    size_t total_size = nmemb * size;
    void *ptr = NULL;
    
    // Try internal RAM first (for DMA-capable allocation if available)
    if (total_size < 512) {  // Small allocations prefer internal RAM
        ptr = heap_caps_calloc(nmemb, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    
    // If failed or large allocation, use SPIRAM
    if (ptr == NULL) {
        ptr = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr != NULL) {
            ESP_LOGD(TAG, "mbedTLS calloc %u bytes from SPIRAM @ %p", total_size, ptr);
        }
    }
    
    // Last resort: try regular malloc
    if (ptr == NULL) {
        ptr = calloc(nmemb, size);
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
