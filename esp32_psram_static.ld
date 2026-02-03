/**
 * @file esp32_psram_static.ld
 * @brief Generic ESP32/ESP32-S3 linker fragment for PSRAM static allocation
 * 
 * This linker script fragment places large static variables into PSRAM
 * (external SPIRAM) to free up internal SRAM for DMA, stack, and WiFi.
 * 
 * ============================================================================
 * PSRAM ZERO-INITIALIZATION METHODS
 * ============================================================================
 * 
 * 1. STATIC ALLOCATION (BSS Segment) - Automatic Zero-Init
 *    -------------------------------------------------------
 *    Requirements:
 *      - Enable in sdkconfig/menuconfig:
 *        CONFIG_SPIRAM=y
 *        CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
 *      - Use EXT_RAM_BSS_ATTR macro before variable declaration
 * 
 *    Example:
 *      #include "esp_attr.h"
 *      EXT_RAM_BSS_ATTR char large_buffer[1024 * 1024];  // 1MB, zero-initialized
 *      EXT_RAM_BSS_ATTR uint8_t frame_buffer[320 * 240]; // 76KB, zero-initialized
 * 
 *    The ESP-IDF bootloader automatically zeros the .ext_ram.bss section.
 * 
 * 2. DYNAMIC ALLOCATION (Heap) - Choose Method
 *    ------------------------------------------
 *    a) heap_caps_calloc() - Allocates AND zeros in one call (preferred):
 *       void* ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
 * 
 *    b) ps_malloc() + memset() - Allocates then zeros:
 *       void* ptr = ps_malloc(size);
 *       if (ptr) memset(ptr, 0, size);
 * 
 *    c) heap_caps_malloc() - Does NOT zero (for performance when not needed):
 *       void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
 * 
 * ============================================================================
 * USAGE IN PLATFORMIO
 * ============================================================================
 * 
 * Add to platformio.ini build_flags:
 *   build_flags = 
 *     -Wl,-T,esp32_psram_static.ld
 * 
 * Or for complete linker script replacement:
 *   board_build.ldscript = esp32_psram_static.ld
 * 
 * ============================================================================
 * CUSTOM ATTRIBUTES FOR EXPLICIT PLACEMENT
 * ============================================================================
 * 
 * Define in your code:
 *   #define EXT_RAM_DATA_ATTR  __attribute__((section(".ext_ram.data")))
 *   #define EXT_RAM_NOINIT_ATTR __attribute__((section(".ext_ram.noinit")))
 * 
 * Usage:
 *   EXT_RAM_BSS_ATTR    static uint8_t zeroed_buffer[64*1024];     // Zero-initialized
 *   EXT_RAM_DATA_ATTR   static uint8_t init_buffer[1024] = {0x55}; // Initialized from flash
 *   EXT_RAM_NOINIT_ATTR static uint8_t persist_buffer[4096];       // Not initialized
 * 
 */

SECTIONS
{
  /* =========================================================================
   * .ext_ram.bss - Zero-Initialized Static Data in PSRAM
   * =========================================================================
   * BSS = Block Started by Symbol (uninitialized data)
   * All data here is guaranteed to be zeroed at boot by ESP-IDF.
   */
  .ext_ram.bss (NOLOAD) :
  {
    . = ALIGN(16);
    _ext_ram_bss_start = ABSOLUTE(.);
    
    /* ------ Explicitly marked variables (EXT_RAM_BSS_ATTR) ------ */
    *(.ext_ram.bss)
    *(.ext_ram.bss.*)
    
    /* ------ OpenSprinkler Core Buffers ------ */
    *OpenSprinkler.*(.bss.buffer* .bss.cache* .bss.queue* .bss.ether* .bss.tmp*)
    *opensprinkler_server.*(.bss.buffer* .bss.html* .bss.json* .bss.response*)
    *program_data.*(.bss.buffer* .bss.schedule*)
    
    /* ------ MQTT/PubSubClient Buffers ------ */
    *mqtt.*(.bss.buffer* .bss.payload* .bss.topic*)
    *PubSubClient.*(.bss.buffer*)
    
    /* ------ Sensor Buffers ------ */
    *sensor*.*(.bss.buffer* .bss.data* .bss.reading* .bss.history*)
    
    /* ------ OpenThings Framework ------ */
    *OpenThings*.*(.bss.buffer* .bss.cache* .bss.pool*)
    *Esp32LocalServer.*(.bss.buffer* .bss.request* .bss.response*)
    *etherport.*(.bss.buffer*)
    
    /* ------ WebSockets ------ */
    *WebSockets*.*(.bss.buffer* .bss.frame* .bss.message* .bss.queue*)
    
    /* ------ TLS/mbedTLS (16KB+ per connection) ------ */
    *libmbedtls.a:(.bss.ssl_context* .bss.ssl_session*)
    *libmbedcrypto.a:(.bss.ctx* .bss.aes* .bss.sha*)
    *libmbedx509.a:(.bss.cert* .bss.crl*)
    
    /* ------ JSON Parsing (ArduinoJson) ------ */
    *ArduinoJson*.*(.bss.buffer* .bss.pool* .bss.doc*)
    
    /* ------ InfluxDB Client ------ */
    *InfluxDB*.*(.bss.buffer* .bss.batch* .bss.point* .bss.line*)
    
    /* ------ Network/lwIP Buffers ------ */
    *liblwip.a:(.bss.pbuf_pool* .bss.memp_* .bss.netconn*)
    
    /* ------ Display Framebuffers ------ */
    *SSD1306*.*(.bss.buffer* .bss.frame*)
    *display*.*(.bss.framebuffer* .bss.buffer*)
    
    /* ------ Matter/Thread/Zigbee (if enabled) ------ */
    *libmatter.a:(.bss.buffer* .bss.msg* .bss.session* .bss.fabric*)
    *libZigbee.a:(.bss.buffer* .bss.frame* .bss.cluster*)
    *libopenthread.a:(.bss.buffer* .bss.msg*)
    
    /* ------ Email Sender ------ */
    *EMailSender.*(.bss.buffer* .bss.body* .bss.attachment*)
    
    . = ALIGN(16);
    _ext_ram_bss_end = ABSOLUTE(.);
  } > extern_ram_seg

  /* =========================================================================
   * .ext_ram.data - Initialized Static Data in PSRAM  
   * =========================================================================
   * Data here is copied from flash to PSRAM at boot.
   * Use for large initialized arrays (lookup tables, default configs).
   */
  .ext_ram.data :
  {
    . = ALIGN(16);
    _ext_ram_data_start = ABSOLUTE(.);
    
    *(.ext_ram.data)
    *(.ext_ram.data.*)
    
    /* Large initialized lookup tables */
    *(.data.large*)
    *(.data.table*)
    *(.data.lookup*)
    *(.data.font*)
    
    . = ALIGN(16);
    _ext_ram_data_end = ABSOLUTE(.);
  } > extern_ram_seg AT > default_rodata_seg
  
  /* Load address for .ext_ram.data (in flash) */
  _ext_ram_data_flash_start = LOADADDR(.ext_ram.data);

  /* =========================================================================
   * .ext_ram.rodata - Read-Only Data in PSRAM (optional)
   * =========================================================================
   * For very large const data that doesn't fit in flash cache window.
   * Access is slower than flash-XIP, use only for huge tables.
   */
  .ext_ram.rodata :
  {
    . = ALIGN(16);
    _ext_ram_rodata_start = ABSOLUTE(.);
    
    *(.ext_ram.rodata)
    *(.ext_ram.rodata.*)
    
    *(.rodata.large*)
    *(.rodata.huge*)
    
    . = ALIGN(16);
    _ext_ram_rodata_end = ABSOLUTE(.);
  } > extern_ram_seg AT > default_rodata_seg

  /* =========================================================================
   * .ext_ram.noinit - Non-Initialized PSRAM (survives soft reset)
   * =========================================================================
   * Data here is NOT zeroed at boot. Useful for:
   * - Crash dumps that survive reset
   * - Buffers that should persist across OTA updates
   * - Performance-critical buffers that don't need zeroing
   */
  .ext_ram.noinit (NOLOAD) :
  {
    . = ALIGN(16);
    _ext_ram_noinit_start = ABSOLUTE(.);
    
    *(.ext_ram.noinit)
    *(.ext_ram.noinit.*)
    
    . = ALIGN(16);
    _ext_ram_noinit_end = ABSOLUTE(.);
  } > extern_ram_seg

  /* =========================================================================
   * .ext_ram.heap - PSRAM Heap Region
   * =========================================================================
   * The remaining PSRAM after static allocations is used for heap.
   * heap_caps_malloc(MALLOC_CAP_SPIRAM) allocates from here.
   */
  .ext_ram.heap (NOLOAD) :
  {
    . = ALIGN(16);
    _ext_ram_heap_start = ABSOLUTE(.);
    /* Heap extends to end of extern_ram_seg */
  } > extern_ram_seg
}

/* Insert after internal BSS to ensure correct section ordering */
INSERT AFTER .dram0.bss;

/* ============================================================================
 * ASSERTIONS - Catch configuration errors at link time
 * ============================================================================ */

/* Ensure external RAM sections fit */
ASSERT((_ext_ram_bss_end - _ext_ram_bss_start) < 4M, 
       "ERROR: .ext_ram.bss exceeds 4MB - check PSRAM size!")

ASSERT((_ext_ram_data_end - _ext_ram_data_start) < 1M,
       "ERROR: .ext_ram.data exceeds 1MB - reduce initialized data in PSRAM!")
