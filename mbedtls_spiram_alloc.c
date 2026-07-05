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
#include "esp_rom_sys.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "MBEDTLS_ALLOC";
static uint32_t s_heap_caps_fallback_logs = 0;
static volatile bool s_allow_internal_reroute = false;
static uint32_t s_alloc_trace_logs = 0;
static const uint32_t ALLOC_TRACE_LOG_LIMIT = 2000;

#if defined(ENABLE_DEBUG_ALLOC)
static inline void trace_alloc_event(const char *fn, const char *phase, size_t size, uint32_t caps, const void *ptr) {
    if (s_alloc_trace_logs < ALLOC_TRACE_LOG_LIMIT) {
        esp_rom_printf("[ALLOCDBG] %s %s size=%u caps=0x%X -> %p\n",
                       fn, phase, (unsigned)size, (unsigned)caps, ptr);
        s_alloc_trace_logs++;
    } else if (s_alloc_trace_logs == ALLOC_TRACE_LOG_LIMIT) {
        esp_rom_printf("[ALLOCDBG] alloc trace suppressed after %u entries\n", (unsigned)ALLOC_TRACE_LOG_LIMIT);
        s_alloc_trace_logs++;
    }
}

static inline void trace_realloc_event(const char *phase, const void *old_ptr, size_t size, uint32_t caps, const void *new_ptr) {
    if (s_alloc_trace_logs < ALLOC_TRACE_LOG_LIMIT) {
        esp_rom_printf("[ALLOCDBG] heap_caps_realloc %s old=%p size=%u caps=0x%X -> %p\n",
                       phase, old_ptr, (unsigned)size, (unsigned)caps, new_ptr);
        s_alloc_trace_logs++;
    } else if (s_alloc_trace_logs == ALLOC_TRACE_LOG_LIMIT) {
        esp_rom_printf("[ALLOCDBG] alloc trace suppressed after %u entries\n", (unsigned)ALLOC_TRACE_LOG_LIMIT);
        s_alloc_trace_logs++;
    }
}
#else
static inline void trace_alloc_event(const char *fn, const char *phase, size_t size, uint32_t caps, const void *ptr) {
    (void)fn; (void)phase; (void)size; (void)caps; (void)ptr;
}
static inline void trace_realloc_event(const char *phase, const void *old_ptr, size_t size, uint32_t caps, const void *new_ptr) {
    (void)phase; (void)old_ptr; (void)size; (void)caps; (void)new_ptr;
}
#endif

static inline bool internal_reroute_locked(uint32_t caps) {
    // FreeRTOS/IDF internal objects (e.g., TCBs for static task creation) must
    // stay in valid internal RAM. Rerouting MALLOC_CAP_INTERNAL to PSRAM can
    // trigger xPortCheckValidTCBMem() assertions.
    (void)s_allow_internal_reroute;
    return (caps & MALLOC_CAP_INTERNAL) != 0;
}

static inline bool can_try_spiram_fallback(uint32_t caps) {
    // Never rewrite requests that already target SPIRAM.
    if ((caps & MALLOC_CAP_SPIRAM) != 0) {
        return false;
    }

    // Keep strict/low-level capability paths untouched.
    if ((caps & (MALLOC_CAP_EXEC | MALLOC_CAP_CACHE_ALIGNED)) != 0) {
        return false;
    }

    // Never reroute explicit internal-memory requests.
    return false;
}

static inline bool can_try_spiram_relaxed(uint32_t caps) {
    if ((caps & MALLOC_CAP_SPIRAM) != 0) {
        return false;
    }
    if ((caps & (MALLOC_CAP_EXEC | MALLOC_CAP_CACHE_ALIGNED)) != 0) {
        return false;
    }
    return true;
}

static inline uint32_t to_spiram_fallback_caps(uint32_t caps) {
    return (caps & ~MALLOC_CAP_INTERNAL) | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
}

static inline void log_caps_fallback_once(const char *fn, size_t size, uint32_t from_caps, uint32_t to_caps) {
    if (s_heap_caps_fallback_logs < 8) {
        s_heap_caps_fallback_logs++;
        ESP_EARLY_LOGW(TAG,
                       "%s %u caps=0x%X -> SPIRAM caps=0x%X",
                       fn,
                       (unsigned)size,
                       (unsigned)from_caps,
                       (unsigned)to_caps);
    }
}

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
 * Linker wrap for heap_caps_aligned_alloc:
 * When internal RAM (including DMA-capable RAM) is exhausted,
 * fall back to PSRAM (SPIRAM) which is DMA-capable on ESP32-C5.
 * This prevents SSL/TLS/AES failures under high memory pressure.
 */
extern void* __real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
extern void* __real_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);
extern void* __real_heap_caps_malloc(size_t size, uint32_t caps);
extern void* __real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);
extern void* __real_heap_caps_realloc(void *ptr, size_t size, uint32_t caps);

static size_t collect_prefer_caps(size_t num, uint32_t *caps_list, size_t caps_list_len, va_list ap) {
    size_t count = (num < caps_list_len) ? num : caps_list_len;
    for (size_t i = 0; i < count; i++) {
        caps_list[i] = va_arg(ap, uint32_t);
    }

    // Consume any remaining varargs to keep va_list handling correct for callers.
    for (size_t i = count; i < num; i++) {
        (void)va_arg(ap, uint32_t);
    }

    return count;
}

void* __wrap_heap_caps_malloc(size_t size, uint32_t caps) {
    void *ptr = NULL;
    trace_alloc_event("heap_caps_malloc", "request", size, caps, NULL);

    if (internal_reroute_locked(caps)) {
        ptr = __real_heap_caps_malloc(size, caps);
        trace_alloc_event("heap_caps_malloc", "locked-real", size, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_fallback(caps)) {
        uint32_t fallback_caps = to_spiram_fallback_caps(caps);
        ptr = __real_heap_caps_malloc(size, fallback_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_malloc(spiram-first)", size, caps, fallback_caps);
            trace_alloc_event("heap_caps_malloc", "spiram-first", size, fallback_caps, ptr);
            return ptr;
        }
    }

    ptr = __real_heap_caps_malloc(size, caps);
    if (ptr != NULL) {
        trace_alloc_event("heap_caps_malloc", "real", size, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_relaxed(caps)) {
        uint32_t relaxed_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ptr = __real_heap_caps_malloc(size, relaxed_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_malloc(relaxed)", size, caps, relaxed_caps);
            trace_alloc_event("heap_caps_malloc", "relaxed", size, relaxed_caps, ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_malloc", "fail", size, caps, NULL);

    return NULL;
}

void* __wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    void *ptr = NULL;
    size_t total = n * size;
    trace_alloc_event("heap_caps_calloc", "request", total, caps, NULL);

    if (internal_reroute_locked(caps)) {
        ptr = __real_heap_caps_calloc(n, size, caps);
        trace_alloc_event("heap_caps_calloc", "locked-real", total, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_fallback(caps)) {
        uint32_t fallback_caps = to_spiram_fallback_caps(caps);
        ptr = __real_heap_caps_calloc(n, size, fallback_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_calloc(spiram-first)", n * size, caps, fallback_caps);
            trace_alloc_event("heap_caps_calloc", "spiram-first", total, fallback_caps, ptr);
            return ptr;
        }
    }

    ptr = __real_heap_caps_calloc(n, size, caps);
    if (ptr != NULL) {
        trace_alloc_event("heap_caps_calloc", "real", total, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_relaxed(caps)) {
        uint32_t relaxed_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ptr = __real_heap_caps_calloc(n, size, relaxed_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_calloc(relaxed)", n * size, caps, relaxed_caps);
            trace_alloc_event("heap_caps_calloc", "relaxed", total, relaxed_caps, ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_calloc", "fail", total, caps, NULL);

    return NULL;
}

void* __wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    void *ptr = NULL;
    trace_alloc_event("heap_caps_aligned_alloc", "request", size, caps, NULL);

    if (internal_reroute_locked(caps)) {
        ptr = __real_heap_caps_aligned_alloc(alignment, size, caps);
        trace_alloc_event("heap_caps_aligned_alloc", "locked-real", size, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_fallback(caps)) {
        uint32_t fallback_caps = to_spiram_fallback_caps(caps);
        ptr = __real_heap_caps_aligned_alloc(alignment, size, fallback_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_aligned_alloc(spiram-first)", size, caps, fallback_caps);
            trace_alloc_event("heap_caps_aligned_alloc", "spiram-first", size, fallback_caps, ptr);
            return ptr;
        }
    }

    ptr = __real_heap_caps_aligned_alloc(alignment, size, caps);
    if (ptr != NULL) {
        trace_alloc_event("heap_caps_aligned_alloc", "real", size, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_relaxed(caps)) {
        uint32_t relaxed_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ptr = __real_heap_caps_aligned_alloc(alignment, size, relaxed_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_aligned_alloc(relaxed)", size, caps, relaxed_caps);
            trace_alloc_event("heap_caps_aligned_alloc", "relaxed", size, relaxed_caps, ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_aligned_alloc", "fail", size, caps, NULL);
    
    return NULL;
}

void* __wrap_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps) {
    void *ptr = NULL;
    size_t total = n * size;
    trace_alloc_event("heap_caps_aligned_calloc", "request", total, caps, NULL);

    if (internal_reroute_locked(caps)) {
        ptr = __real_heap_caps_aligned_calloc(alignment, n, size, caps);
        trace_alloc_event("heap_caps_aligned_calloc", "locked-real", total, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_fallback(caps)) {
        uint32_t fallback_caps = to_spiram_fallback_caps(caps);
        ptr = __real_heap_caps_aligned_calloc(alignment, n, size, fallback_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_aligned_calloc(spiram-first)", total, caps, fallback_caps);
            trace_alloc_event("heap_caps_aligned_calloc", "spiram-first", total, fallback_caps, ptr);
            return ptr;
        }
    }

    ptr = __real_heap_caps_aligned_calloc(alignment, n, size, caps);
    if (ptr != NULL) {
        trace_alloc_event("heap_caps_aligned_calloc", "real", total, caps, ptr);
        return ptr;
    }

    if (can_try_spiram_relaxed(caps)) {
        uint32_t relaxed_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ptr = __real_heap_caps_aligned_calloc(alignment, n, size, relaxed_caps);
        if (ptr != NULL) {
            log_caps_fallback_once("heap_caps_aligned_calloc(relaxed)", total, caps, relaxed_caps);
            trace_alloc_event("heap_caps_aligned_calloc", "relaxed", total, relaxed_caps, ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_aligned_calloc", "fail", total, caps, NULL);
    return NULL;
}

void* __wrap_heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    void *out = NULL;
    trace_realloc_event("request", ptr, size, caps, NULL);

    if (internal_reroute_locked(caps)) {
        out = __real_heap_caps_realloc(ptr, size, caps);
        trace_realloc_event("locked-real", ptr, size, caps, out);
        return out;
    }

    if (can_try_spiram_fallback(caps)) {
        uint32_t fallback_caps = to_spiram_fallback_caps(caps);
        out = __real_heap_caps_realloc(ptr, size, fallback_caps);
        if (out != NULL) {
            log_caps_fallback_once("heap_caps_realloc(spiram-first)", size, caps, fallback_caps);
            trace_realloc_event("spiram-first", ptr, size, fallback_caps, out);
            return out;
        }
    }

    out = __real_heap_caps_realloc(ptr, size, caps);
    if (out != NULL) {
        trace_realloc_event("real", ptr, size, caps, out);
        return out;
    }

    if (can_try_spiram_relaxed(caps)) {
        uint32_t relaxed_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        out = __real_heap_caps_realloc(ptr, size, relaxed_caps);
        if (out != NULL) {
            log_caps_fallback_once("heap_caps_realloc(relaxed)", size, caps, relaxed_caps);
            trace_realloc_event("relaxed", ptr, size, relaxed_caps, out);
            return out;
        }
    }

    trace_realloc_event("fail", ptr, size, caps, NULL);

    return NULL;
}

void* __wrap_heap_caps_malloc_prefer(size_t size, size_t num, ...) {
    uint32_t caps_list[8];
    va_list ap;
    va_start(ap, num);
    size_t count = collect_prefer_caps(num, caps_list, 8, ap);
    va_end(ap);

    if (count == 0) {
        trace_alloc_event("heap_caps_malloc_prefer", "empty", size, 0, NULL);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        void *ptr = __wrap_heap_caps_malloc(size, caps_list[i]);
        if (ptr != NULL) {
            trace_alloc_event("heap_caps_malloc_prefer", "selected", size, caps_list[i], ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_malloc_prefer", "fail", size, caps_list[0], NULL);
    return NULL;
}

void* __wrap_heap_caps_realloc_prefer(void *ptr, size_t size, size_t num, ...) {
    uint32_t caps_list[8];
    va_list ap;
    va_start(ap, num);
    size_t count = collect_prefer_caps(num, caps_list, 8, ap);
    va_end(ap);

    if (count == 0) {
        trace_realloc_event("empty", ptr, size, 0, NULL);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        void *out = __wrap_heap_caps_realloc(ptr, size, caps_list[i]);
        if (out != NULL) {
            trace_realloc_event("selected", ptr, size, caps_list[i], out);
            return out;
        }
    }

    trace_realloc_event("fail", ptr, size, caps_list[0], NULL);
    return NULL;
}

void* __wrap_heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...) {
    uint32_t caps_list[8];
    size_t total = n * size;
    va_list ap;
    va_start(ap, num);
    size_t count = collect_prefer_caps(num, caps_list, 8, ap);
    va_end(ap);

    if (count == 0) {
        trace_alloc_event("heap_caps_calloc_prefer", "empty", total, 0, NULL);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        void *ptr = __wrap_heap_caps_calloc(n, size, caps_list[i]);
        if (ptr != NULL) {
            trace_alloc_event("heap_caps_calloc_prefer", "selected", total, caps_list[i], ptr);
            return ptr;
        }
    }

    trace_alloc_event("heap_caps_calloc_prefer", "fail", total, caps_list[0], NULL);
    return NULL;
}

/**
 * Initialize mbedTLS with SPIRAM-aware allocators
 */
void mbedtls_platform_set_spiram_allocators(void) {
    mbedtls_platform_set_calloc_free(mbedtls_calloc_spiram_fallback, mbedtls_free_spiram);
    ESP_LOGI(TAG, "mbedTLS memory allocators set to use SPIRAM fallback");
}

void mbedtls_spiram_allow_internal_reroute(bool enable) {
    s_allow_internal_reroute = enable;
    ESP_LOGI(TAG, "internal reroute %s", enable ? "enabled" : "disabled");
}

#endif // ESP32 || ESP_PLATFORM
