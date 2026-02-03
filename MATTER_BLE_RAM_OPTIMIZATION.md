# Matter & BLE RAM Optimization Guide

## Overview
This guide documents the RAM optimization strategies for Matter and BLE on the ESP32-C5 with **384KB of internal SRAM** and **8MB PSRAM**.

## Critical: WiFi DMA vs BLE Stack

### The Problem
WiFi requires **DMA-capable internal RAM** for TX/RX buffers. These allocations **cannot** be redirected to PSRAM, even though ESP32-C5 has DMA-capable PSRAM (`SOC_PSRAM_DMA_CAPABLE=1`).

The WiFi library uses hardcoded `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` for critical buffers in `esp_adapter.c`:
```c
// esp-idf/components/esp_wifi/esp32c5/esp_adapter.c
realloc_internal_wrapper()  → heap_caps_realloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
calloc_internal_wrapper()   → heap_caps_calloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
```

### Good News: BLE Already Uses PSRAM!
The sdkconfig has:
```
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
```
This means the NimBLE host stack already allocates its heap from **PSRAM**, not internal RAM.

---

## Current sdkconfig BLE Settings

| Setting | Value | RAM Impact |
|---------|-------|------------|
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | 5120 | 5KB internal (FreeRTOS stack) |
| `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` | y | Heap in PSRAM ✓ |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | 3 | ~450 bytes per bond |
| `CONFIG_BT_NIMBLE_MAX_CCCDS` | 8 | ~20 bytes per CCCD |
| `CONFIG_BT_NIMBLE_GATT_MAX_PROCS` | 4 | ~28 bytes per proc |
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 24 | 3KB (24×128) |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 24 | 7.5KB (24×320) |

---

## How to Reduce BLE Stack Size

To reduce the BLE stack, you must modify the sdkconfig in:
`framework-arduinoespressif32-libs/esp32c5/sdkconfig`

### Reduce Host Task Stack (saves 1.5-2 KB)
```ini
# Default: 5120, Minimum for Matter: ~3072
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3584
```

### Reduce Memory Pools (saves ~5 KB)
```ini
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=12
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=12
```

### Disable Unused GATT Services (saves flash + RAM)
```ini
# CONFIG_BT_NIMBLE_PROX_SERVICE is not set
# CONFIG_BT_NIMBLE_ANS_SERVICE is not set
# CONFIG_BT_NIMBLE_CTS_SERVICE is not set
# CONFIG_BT_NIMBLE_HTP_SERVICE is not set
# CONFIG_BT_NIMBLE_IPSS_SERVICE is not set
# CONFIG_BT_NIMBLE_TPS_SERVICE is not set
# CONFIG_BT_NIMBLE_IAS_SERVICE is not set
# CONFIG_BT_NIMBLE_LLS_SERVICE is not set
# CONFIG_BT_NIMBLE_SPS_SERVICE is not set
# CONFIG_BT_NIMBLE_HR_SERVICE is not set
# CONFIG_BT_NIMBLE_BAS_SERVICE is not set
```

**Note**: These settings cannot be changed via `-D` build flags - they require sdkconfig modification and recompilation of the framework.

---

## PSRAM Memory Management (`psram_utils.*`)

### Implemented Features
- **Early PSRAM redirect**: `heap_caps_malloc_extmem_enable(4)` - allocations ≥4 bytes prefer SPIRAM
- **mbedTLS PSRAM allocator**: TLS/SSL operations use PSRAM for buffers
- **Alloc failure callback**: Logs when internal RAM exhausted (helps debugging)

### Buffer Sizes
```c
ether_buffer: 16 KB in PSRAM
tmp_buffer:   32 KB in PSRAM
```

### Usage
```cpp
#include "psram_utils.h"

void setup() {
    // Call early to reserve internal RAM for WiFi
    reserve_internal_ram_for_wifi();
    
    // Initialize PSRAM buffers
    init_psram_buffers();
    
    // Show optimization status
    log_matter_ble_memory_optimization();
}
```

---

## Estimated Internal RAM Usage

| Component | Internal RAM | Notes |
|-----------|--------------|-------|
| FreeRTOS kernel | ~15 KB | Fixed overhead |
| WiFi static buffers | ~25 KB | Static TX/RX (16 buffers × 1.6KB) |
| WiFi DMA runtime | ~10-20 KB | Dynamic, varies with traffic |
| BLE host task stack | 5 KB | Configurable (min ~3KB) |
| BLE controller | ~10 KB | Fixed (in ROM/IRAM) |
| TCP/IP stack | ~20 KB | LWIP buffers |
| **Total estimate** | **~85-95 KB** | Out of 384 KB |

**Remaining**: ~290 KB for application + heap

---

## WiFi Memory Optimization

WiFi internal RAM usage can be reduced by adjusting buffer counts in sdkconfig:

| Setting | Default | Reduced | Saves |
|---------|---------|---------|-------|
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 16 | 8 | ~12.8 KB |
| `ESP_WIFI_CACHE_TX_BUFFER_NUM` | 32 | 16 | ~25.6 KB |

**Warning**: Reducing buffers may impact WiFi throughput and stability.

---

## Verification

### Check Memory at Boot
```
[RAM] Internal free: 285 KB, DMA-capable: 250 KB
[PSRAM] ether_buffer: 16 KB allocated
[PSRAM] tmp_buffer: 32 KB allocated
```

### Monitor During Operation
Enable `ENABLE_MEMORY_DEBUG` to see periodic reports:
```
[MEM] Heap: 120/384 KB (min: 95 KB) | PSRAM: 7.2/8.0 MB
```

### Test WiFi + BLE Together
1. Start BLE scanning
2. Connect to WiFi
3. Make HTTP requests
4. Monitor for `[ALLOC_FAIL]` messages

---

## Troubleshooting

### ALLOC_FAIL with caps=0x80C
This means WiFi needs DMA+INTERNAL memory but none is available.

**Solutions**:
1. Reduce BLE stack size (modify sdkconfig)
2. Reduce WiFi buffer counts (modify sdkconfig)
3. Defer BLE operations during heavy WiFi traffic
4. Call `reserve_internal_ram_for_wifi()` early

### BLE Scan Fails After WiFi Connect
WiFi may have consumed all DMA-capable RAM.

**Solution**: Initialize WiFi before BLE, or reduce WiFi buffer counts.

---

## Summary of Savings

| Optimization | Savings | Requires |
|--------------|---------|----------|
| Reduce BLE host stack (5120→3584) | 1.5 KB | sdkconfig edit |
| Reduce MSYS blocks (24→12 each) | 5.1 KB | sdkconfig edit |
| Disable unused GATT services | ~2 KB flash | sdkconfig edit |
| Reduce WiFi RX buffers (16→8) | 12.8 KB | sdkconfig edit |
| **Total potential** | **~20 KB** | Framework rebuild |

---

## References
- [ESP-IDF Heap Capabilities](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/mem_alloc.html)
- [NimBLE Configuration](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/nimble/index.html)
- [Matter Memory Guidelines](https://github.com/project-chip/connectedhomeip/blob/master/docs/guides/memory_optimization.md)
- [ESP32-C5 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)
