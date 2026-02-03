/* OpenSprinkler PSRAM Debug Wrapper
 * Patches malloc/free to log allocations and track PSRAM usage
 */

#ifndef PSRAM_DEBUG_H
#define PSRAM_DEBUG_H

#if defined(ESP32) && defined(BOARD_HAS_PSRAM) && defined(ENABLE_DEBUG)

#include <esp_heap_caps.h>

// Global stats
extern uint32_t malloc_count;
extern uint32_t malloc_spiram_count;
extern uint32_t total_malloc_bytes;
extern uint32_t total_spiram_bytes;

void debug_malloc(const char *file, int line, size_t size, void *ptr);
void debug_free(const char *file, int line, void *ptr);

#endif // ENABLE_DEBUG
#endif // PSRAM_DEBUG_H
