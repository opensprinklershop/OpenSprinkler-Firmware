/* OpenSprinkler PSRAM Debug Implementation
 * Logs malloc/free calls to track PSRAM allocation patterns
 */

#include "psram_debug.h"

#if defined(ESP32) && defined(BOARD_HAS_PSRAM) && defined(ENABLE_DEBUG)

#include <Arduino.h>
#include <stdio.h>
#include <esp_heap_caps.h>
#include "defines.h"

// Global allocation tracking
uint32_t malloc_count = 0;
uint32_t malloc_spiram_count = 0;
uint32_t total_malloc_bytes = 0;
uint32_t total_spiram_bytes = 0;

void debug_malloc(const char *file, int line, size_t size, void *ptr) {
  if (ptr == NULL) return;
  
  malloc_count++;
  total_malloc_bytes += size;
  
  // Log only large allocations
  if (size > 256) {
    DEBUG_PRINTF("[MALLOC] %d bytes @ %p (line %d:%s)\n", size, ptr, line, file);
  }
}

void debug_free(const char *file, int line, void *ptr) {
  if (ptr == NULL) return;
  
  // Log free operations for large allocations
  size_t block_size = heap_caps_get_allocated_size(ptr);
  if (block_size > 256) {
    DEBUG_PRINTF("[FREE] %d bytes @ %p\n", block_size, ptr);
  }
}

#endif // ENABLE_DEBUG
