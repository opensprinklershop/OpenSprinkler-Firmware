# ESP32-C5 + PSRAM + Matter — Memory Architecture & Configuration

> Last updated: February 2026  
> Applies to: ESP32-C5 Rev 1.0 (eco2), 8 MB PSRAM, WiFi + BLE + Matter + OpenThread

## Overview

The ESP32-C5 (RISC-V RV32IMAC) runs OpenSprinkler with Matter, WiFi, BLE (NimBLE),
and OpenThread simultaneously. With only 320 KB internal SRAM and 8 MB PSRAM, careful
memory partitioning is essential. This document describes the working configuration
and the reasoning behind every setting.

---

## Hardware Constraints

| Resource       | Size      | Notes                                              |
|---------------|-----------|----------------------------------------------------|
| Internal SRAM  | 320,928 B | Shared between .text, .bss, .data, heap            |
| PSRAM          | 8 MB      | Via MSPI, DMA-accessible but with cache caveats     |
| CPU            | RV32IMAC  | No floating-point unit, no RV32D extensions         |

### Rev 1.0 Silicon Errata

1. **Broken PSRAM memory barrier**: `esp_psram_mspi_mb()` is a no-op because the
   dummy cacheline allocation fails. WiFi/DMA structures in PSRAM cause cache
   incoherency → `Load access fault` (MTVAL=0xAAAAAAB2).

2. **No XIP from PSRAM**: `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` causes IRAM cache
   corruption. During context switches, RV32D instructions (e.g. `c.fldsp` = opcode
   `0x2356`) appear in the instruction stream → `Illegal instruction` crash.

---

## Memory Budget (Build12)

### Static Allocation (Linker)

| Section | Size      | % of SRAM |
|---------|-----------|-----------|
| .text   | 121,816 B | 37.96%    |
| .bss    |  90,680 B | 28.26%    |
| .data   |  22,845 B |  7.12%    |
| **Total used** | **235,341 B** | **73.33%** |
| **Free for heap** | **85,587 B** | **26.67%** |

### Runtime Heap Consumption (Expected)

| Phase              | Internal Free | Consumed By                     |
|--------------------|---------------|----------------------------------|
| Boot               | ~130 KB       | —                                |
| After WiFi         | ~60–65 KB     | WiFi buffers (~66 KB)            |
| After Matter.begin | ~20–25 KB     | FreeRTOS tasks, BLE, CHIP pools  |
| Steady state       | ~15–20 KB     | MQTT, sensors, HTTP server       |

---

## ESP-IDF Configuration (defconfig.esp32c5)

### PSRAM Core Settings

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y
CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set     ← CRITICAL: WiFi must not use PSRAM
# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set              ← CRITICAL: cache corruption on Rev 1.0
```

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 1024 | Allocations ≤1 KB stay internal. Larger go to PSRAM. Reduced from 4096 to move more to PSRAM. |
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | 32768 | Reserve 32 KB internal for DMA/critical. Reduced from 65536. |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | disabled | WiFi DMA buffers MUST be in internal SRAM (broken memory barrier). |
| `SPIRAM_XIP_FROM_PSRAM` | disabled | Causes Illegal instruction crash (cache corruption on Rev 1.0). |

### WiFi Buffers

```ini
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32
CONFIG_ESP_WIFI_RX_BA_WIN=6
CONFIG_ESP_WIFI_TX_BA_WIN=6
```

| Parameter | Default | Optimized | Savings |
|-----------|---------|-----------|---------|
| `STATIC_RX_BUFFER_NUM` | 8 | 6 | ~3.2 KB |
| `STATIC_TX_BUFFER_NUM` | 8 | 6 | ~3.2 KB |
| `DYNAMIC_RX_BUFFER_NUM` | 16 | 10 | ~9.6 KB |

### LWIP Buffers

```ini
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=2880
CONFIG_LWIP_TCP_WND_DEFAULT=2880
CONFIG_LWIP_TCP_RECVMBOX_SIZE=6
CONFIG_LWIP_UDP_RECVMBOX_SIZE=6
```

| Parameter | Default | Optimized | Savings |
|-----------|---------|-----------|---------|
| `TCP_SND_BUF_DEFAULT` | 5744 | 2880 | ~2.8 KB/conn |
| `TCP_WND_DEFAULT` | 5760 | 2880 | ~2.8 KB/conn |

### OpenThread

```ini
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_FTD=y
CONFIG_OPENTHREAD_TASK_SIZE=3072
CONFIG_OPENTHREAD_NUM_MESSAGE_BUFFERS=48
CONFIG_OPENTHREAD_MLE_MAX_CHILDREN=2
CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES=8
CONFIG_OPENTHREAD_SPINEL_RX_FRAME_BUFFER_SIZE=256
CONFIG_OPENTHREAD_UART_BUFFER_SIZE=512
```

| Parameter | Default | Optimized | Savings |
|-----------|---------|-----------|---------|
| `TASK_SIZE` | 4096 | 3072 | 1 KB |
| `NUM_MESSAGE_BUFFERS` | 128 | 48 | ~10 KB |
| `SPINEL_RX_FRAME_BUFFER_SIZE` | 512 | 256 | 0.25 KB |

### BLE (NimBLE)

```ini
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y    ← BLE heap in PSRAM
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3072
CONFIG_BT_NIMBLE_TASK_STACK_SIZE=4096
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=8
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=8
CONFIG_BT_NIMBLE_MAX_BONDS=2
CONFIG_BT_NIMBLE_GATT_MAX_PROCS=2
CONFIG_BT_NIMBLE_MAX_CCCDS=4
```

Unused BLE services (proximity, health, alert, etc.) are disabled to save flash.

### Matter / CHIP

```ini
CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL=y    ← Matter heap in PSRAM
CONFIG_CHIP_TASK_STACK_SIZE=4096               ← Reduced from 6144 (-2 KB)
CONFIG_CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y      ← KEY: pools → malloc() → PSRAM
```

| Parameter | Default | Optimized | Savings |
|-----------|---------|-----------|---------|
| `CHIP_TASK_STACK_SIZE` | 6144 | 4096 | 2 KB |
| `POOL_USE_HEAP` | disabled | **enabled** | **~8–10 KB** (BSS → PSRAM) |

`POOL_USE_HEAP` is the single most impactful optimization. It moves Matter's
session tables, exchange contexts, and subscription handlers from static BSS
(internal SRAM) to `malloc()`, which routes them to PSRAM via the threshold.

### Subsystems Routed to PSRAM

```ini
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y          ← TLS buffers in PSRAM
CONFIG_NVS_ALLOCATE_CACHE_IN_SPIRAM=y        ← NVS cache in PSRAM
CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y         ← mDNS task stack in PSRAM
CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y             ← mDNS data in PSRAM
CONFIG_BLE_MESH_MEM_ALLOC_MODE_EXTERNAL=y     ← BLE Mesh in PSRAM
CONFIG_BLE_MESH_ALLOC_FROM_PSRAM_FIRST=y
```

### Task Stack Sizes

```ini
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096          ← Reduced from 8192 (-4 KB)
CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144
CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=2304
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=4096
```

### LWIP Thread Safety (Matter Compatibility)

```ini
CONFIG_ENABLE_LWIP_THREAD_SAFETY=y            ← TCPIP_CORE_LOCKING enabled
# CONFIG_LWIP_CHECK_THREAD_SAFETY is not set  ← Asserts disabled (Matter issue)
```

Matter calls lwIP without holding the TCPIP lock in some paths. The actual
locking mechanism stays enabled, but the debug asserts are removed via a
Kconfig patch in `build_and_deploy_libs.sh`.

---

## Linker Script Fixes (sections.ld)

Three patches must be applied to `sections.ld` after every library rebuild.
These are automated in `build_and_deploy_libs.sh`.

### Fix 1: WiFi BSS in Internal SRAM

The default linker script places WiFi library BSS (libnet80211, libpp,
liblwip, libwpa_supplicant) in `.ext_ram.bss` (PSRAM). On ESP32-C5 Rev 1.0,
this causes `Load access fault` crashes because the PSRAM memory barrier
is broken.

**Change**: In `.dram0.bss`, modify `EXCLUDE_FILE()` to only exclude BLE
libraries (which are safe in PSRAM), keeping WiFi libraries in internal SRAM.

```
Before: EXCLUDE_FILE(*libble_app.a *libbt.a *liblwip.a *libnet80211.a *libpp.a *libwpa_supplicant.a)
After:  EXCLUDE_FILE(*libble_app.a *libbt.a)
```

### Fix 2: Remove WiFi from ext_ram.bss

Corresponding removal of WiFi libraries from the `.ext_ram.bss` section,
since they now live in internal `.dram0.bss`.

### Fix 3: GDMA IRAM Placement

Add `gdma_hal_enable_access_encrypt_mem` to IRAM alongside
`gdma_hal_set_strategy` to prevent flash cache misses during DMA operations.

---

## Runtime PSRAM Threshold Strategy (psram_utils.cpp)

The firmware uses a two-phase threshold strategy to protect WiFi/BLE init:

1. **Boot → WiFi connected**: `heap_caps_malloc_extmem_enable(SIZE_MAX)`  
   ALL `malloc()` stays in internal RAM. WiFi blob uses plain `malloc()` for
   structures that must be DMA-accessible from internal SRAM.

2. **After WL_CONNECTED**: `heap_caps_malloc_extmem_enable(8)`  
   Allocations ≥8 bytes go to PSRAM. WiFi DMA uses `heap_caps_malloc()` with
   `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`, which bypasses the threshold.

### mbedTLS Override

The precompiled ESP-IDF mbedTLS library hardcodes `MALLOC_CAP_INTERNAL` for
allocations. `psram_utils.cpp` overrides `esp_mbedtls_mem_calloc()` and
`esp_mbedtls_mem_free()` via `extern "C"` to route TLS buffers to PSRAM.

---

## ESP-IDF Fork

Uses Jason2866/esp-idf (release/v5.5, v5.5.2) which includes 145+ commits
ahead of espressif's release/v5.5 with ESP32-C5 fixes.

**Repository**: `https://github.com/opensprinklershop/esp-idf.git`  
**Branch**: `release/v5.5`

---

## Build System

### Library Build

```bash
cd /data/Workspace/OpenSprinkler-Firmware
./build_and_deploy_libs.sh
```

This script:
1. Patches Matter Kconfig (removes `select LWIP_CHECK_THREAD_SAFETY`)
2. Runs `build.sh -t esp32c5 -s` in esp32-arduino-lib-builder
3. Deploys compiled libraries to `framework-arduinoespressif32-libs/esp32c5/`
4. Applies linker script fixes (WiFi BSS, ext_ram.bss, GDMA)

### Firmware Build

```bash
cd /data/Workspace/OpenSprinkler-Firmware
rm -rf .pio/cache .pio/build
platformio run --environment esp32-c5-matter
```

### Repositories

| Repository | URL | Branch |
|------------|-----|--------|
| esp-idf | `github.com/opensprinklershop/esp-idf` | `release/v5.5` |
| esp32-arduino-lib-builder | `github.com/opensprinklershop/esp32-arduino-lib-builder` | `main` |
| OpenSprinkler-Firmware | `github.com/opensprinklershop/OpenSprinkler-Firmware` | — |

---

## Troubleshooting

### Load access fault (MTVAL=0xAAAAAAxx)

WiFi BSS is in PSRAM. Check that linker fixes are applied to `sections.ld`.

### Illegal instruction (MTVAL=0x2356)

XIP from PSRAM is enabled. Ensure `CONFIG_SPIRAM_XIP_FROM_PSRAM` is **not set**.

### Heap exhaustion after Matter.begin()

Check `CONFIG_CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y` is set. Without it, Matter
pools consume ~10 KB of static BSS in internal SRAM.

### BLE init error code -21

PSRAM threshold is too low during BLE init. Ensure threshold is `SIZE_MAX`
until WiFi is connected (handled by `psram_utils.cpp`).

### LWIP thread safety assert / reboot

Ensure `CONFIG_LWIP_CHECK_THREAD_SAFETY` is **not set** and the Matter Kconfig
patch removing `select LWIP_CHECK_THREAD_SAFETY` is applied.
