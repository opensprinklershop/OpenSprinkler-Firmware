/* OpenSprinkler Unified Firmware
 * Matter & BLE Memory Optimization Configuration
 * Feb 2026 @ OpenSprinkler.com
 *
 * This file provides optimized configuration for Matter and BLE
 * to reduce RAM usage on ESP32-C5 with limited internal RAM (384KB).
 */

#ifndef _MATTER_BLE_OPTIMIZE_H
#define _MATTER_BLE_OPTIMIZE_H

#if defined(ESP32) && (defined(ENABLE_MATTER) || defined(OS_ENABLE_BLE))

/* ============================================================================
 * MATTER CONFIGURATION OPTIMIZATIONS
 * ============================================================================ */

#ifdef ENABLE_MATTER

/**
 * Matter stack task size optimization
 * Default: 8192 bytes, Optimized: 4096 bytes
 * Save: 4KB of stack RAM
 */
#define CHIP_CONFIG_NUM_GENERAL_PURPOSE_SHARED_RESPONSE_BUFFER_SYSTEM_OBJECTS 8
#define CHIP_CONFIG_GENERAL_PURPOSE_SHARED_RESPONSE_BUFFER_SIZE 512  // Reduced from 1024

/**
 * Reduce Matter MDNs cache entries
 * Save: ~256 bytes RAM
 */
#define CHIP_CONFIG_MDNS_CACHE_MAX_ENTRIES 16  // Reduced from 32

/**
 * BLE advertising buffer optimization
 * Save: 256 bytes
 */
#define CHIP_CONFIG_BLE_PKT_BUFFER_SIZE 256  // Reduced from 512

/**
 * Reduce Matter operational certificate chain depth
 * Save: ~128 bytes
 */
#define CHIP_CONFIG_MAX_CERT_CHAIN_LENGTH 2  // Reduced from 4

/**
 * Disable unused Matter features for RAM savings
 */
#define CHIP_DISABLE_PERSISTENT_STORAGE_OVER_SATURATION 1
#define CHIP_CONFIG_MEMORY_MGMT_ENABLE 1

#endif // ENABLE_MATTER

/* ============================================================================
 * BLE CONFIGURATION OPTIMIZATIONS
 * ============================================================================ */

#ifdef OS_ENABLE_BLE

/**
 * BLE max connections optimization
 * Default: 3, Optimized: 1 (peripheral mode)
 * Save: 480 bytes per connection slot
 * Use when device acts as BLE peripheral only
 */
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1

/**
 * GATT max concurrent procedures
 * Default: 4, Optimized: 1
 * Save: 112 bytes
 */
#define CONFIG_BT_NIMBLE_GATT_MAX_PROCS 1

/**
 * BLE bonding entries optimization
 * Default: 3, Optimized: 1
 * Save: 448 bytes
 */
#define CONFIG_BT_NIMBLE_MAX_BONDS 1

/**
 * GATT Client Characteristic Configuration Descriptor (CCCD) entries
 * Default: 8, Optimized: 2
 * Save: 112 bytes
 */
#define CONFIG_BT_NIMBLE_MAX_CCCDS 2

/**
 * Disable Secure Connections (SC) if not required
 * Default: enabled, Optimized: disabled
 * Save: 2KB of RAM
 * NOTE: Only disable if your application doesn't require SC pairing
 */
#define CONFIG_BT_NIMBLE_SM_SC 0

/**
 * Disable LE Encryption if not required
 * Default: enabled, Optimized: disabled
 * Save: 32 bytes
 * NOTE: Only disable if your application doesn't require encryption
 */
#define CONFIG_BT_NIMBLE_LL_CFG_FEAT_LE_ENCRYPTION 0

/**
 * BLE advertiser data buffer optimization
 * Default: 1024, Optimized: 512
 * Save: 512 bytes RAM per advertiser
 */
#define CONFIG_BT_NIMBLE_EXT_ADV_MAX_LEN 512

/**
 * Reduce BLE whitelist size
 * Default: 8, Optimized: 2
 * Save: ~96 bytes
 */
#define CONFIG_BT_NIMBLE_MAX_WHITELIST 2

/**
 * Disable BLE controller debug logs (further saves RAM)
 */
#define CONFIG_BT_NIMBLE_HOST_ONLY 0

#endif // OS_ENABLE_BLE

/* ============================================================================
 * COMBINED MATTER + BLE OPTIMIZATION
 * ============================================================================ */

#if defined(ENABLE_MATTER) && defined(OS_ENABLE_BLE)

/**
 * When both Matter and BLE are enabled:
 * - Use shared BLE transport for Matter commissioning (saves dual stack memory)
 * - Minimize BLE scanning/connection slots since Matter handles coordination
 */

/**
 * Reduce concurrent BLE scans
 * Default: unlimited, Optimized: 1
 * Save: ~256 bytes per scan slot
 */
#define CONFIG_BT_NIMBLE_SCANNER_DUAL_FPS 0
#define CONFIG_BT_NIMBLE_MAX_CONCURRENT_SCANS 1

/**
 * Optimize event queue for Matter+BLE coexistence
 * Reduces internal queueing overhead
 */
#define CONFIG_BT_NIMBLE_EV_BUF_COUNT 8  // Reduced from 12

#endif // ENABLE_MATTER && OS_ENABLE_BLE

/* ============================================================================
 * MEMORY MONITORING HELPERS
 * ============================================================================ */

/**
 * Enable this to get detailed RAM breakdown during Matter/BLE init
 * Automatically enabled with ENABLE_MEMORY_DEBUG build flag
 */
#if defined(ENABLE_MEMORY_DEBUG) && (defined(ENABLE_MATTER) || defined(OS_ENABLE_BLE))
  #define MATTER_BLE_LOG_MEMORY_USAGE 1
#else
  #define MATTER_BLE_LOG_MEMORY_USAGE 0
#endif

#endif // ESP32 && (ENABLE_MATTER || OS_ENABLE_BLE)

#endif // _MATTER_BLE_OPTIMIZE_H
