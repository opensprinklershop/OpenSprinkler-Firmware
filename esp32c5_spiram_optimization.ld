/**
 * @file esp32c5_spiram_optimization.ld
 * @brief Custom linker script fragment for aggressive SPIRAM usage on ESP32-C5
 * 
 * This fragment extends ESP-IDF linker scripts to place static objects
 * into SPIRAM (PSRAM) for memory optimization.
 * 
 * PSRAM Zero-Initialization:
 * --------------------------
 * 1. Static allocation (EXT_RAM_BSS_ATTR):
 *    - Variables placed in .ext_ram.bss section are automatically zero-initialized
 *    - Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y in sdkconfig
 *    - Example: EXT_RAM_BSS_ATTR char my_buffer[1024*1024];
 * 
 * 2. Dynamic allocation:
 *    - heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM) - zeroed memory
 *    - ps_malloc(size) + memset(ptr, 0, size) - manual zeroing
 * 
 * Usage: Add to platformio.ini:
 *   board_build.ldscript = esp32c5_spiram_optimization.ld
 * 
 * Or include via build_flags:
 *   -Wl,-T,esp32c5_spiram_optimization.ld
 * 
 * Required sdkconfig options:
 *   CONFIG_SPIRAM=y
 *   CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
 *   CONFIG_SPIRAM_MODE_QUAD=y (or OCT for 8-line)
 */

/* 
 * Memory region definition for PSRAM
 * Note: This is typically already defined in the main linker script.
 * Uncomment only if not defined elsewhere.
 */
/*
MEMORY
{
  ext_ram_seg (RWX) : ORIGIN = 0x3C000000, len = 8M
}
*/

/* SPIRAM optimization sections for static data */
SECTIONS
{
  /* =========================================================================
   * .ext_ram.bss - Zero-initialized static data in PSRAM
   * =========================================================================
   * All variables marked with EXT_RAM_BSS_ATTR are placed here.
   * The BSS section is automatically zero-initialized at startup by
   * the ESP-IDF bootloader when CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
   */
  .ext_ram.bss (NOLOAD) :
  {
    . = ALIGN(16);
    _ext_ram_bss_start = ABSOLUTE(.);
    
    /* -----------------------------------------------------------------------
     * Explicitly marked external RAM variables (EXT_RAM_BSS_ATTR)
     * ----------------------------------------------------------------------- */
    *(.ext_ram.bss)
    *(.ext_ram.bss.*)
    
    /* -----------------------------------------------------------------------
     * OpenSprinkler application buffers
     * Large static buffers that benefit from PSRAM placement
     * ----------------------------------------------------------------------- */
    *OpenSprinkler*.*(.bss.buffer* .bss.cache* .bss.ether* .bss.tmp*)
    *opensprinkler_server*.*(.bss.buffer* .bss.html* .bss.json*)
    *mqtt*.*(.bss.buffer* .bss.payload* .bss.topic*)
    *sensors*.*(.bss.buffer* .bss.data* .bss.reading*)
    
    /* -----------------------------------------------------------------------
     * OpenThings Framework buffers
     * ----------------------------------------------------------------------- */
    *OpenThings*.*(.bss.buffer* .bss.cache* .bss.pool* .bss.queue*)
    *Esp32LocalServer*.*(.bss.buffer* .bss.request* .bss.response*)
    
    /* -----------------------------------------------------------------------
     * WebSocket library buffers
     * ----------------------------------------------------------------------- */
    *WebSockets*.*(.bss.buffer* .bss.frame* .bss.message*)
    *libWebSockets*.a:(.bss.buffer* .bss.frame*)
    
    /* -----------------------------------------------------------------------
     * TLS/SSL buffers (mbedTLS)
     * Large TLS buffers (16KB+ per connection)
     * ----------------------------------------------------------------------- */
    *libmbedtls*.a:(.bss.ssl* .bss.tls* .bss.cert* .bss.key*)
    *libmbedcrypto*.a:(.bss.ctx* .bss.buffer*)
    *libmbedx509*.a:(.bss.cert* .bss.crl*)
    
    /* -----------------------------------------------------------------------
     * Network stack buffers
     * ----------------------------------------------------------------------- */
    *liblwip*.a:(.bss.pbuf* .bss.tcp* .bss.udp* .bss.netconn*)
    *libnet80211*.a:(.bss.rx* .bss.tx* .bss.buffer*)
    
    /* -----------------------------------------------------------------------
     * JSON parsing buffers (ArduinoJson)
     * ----------------------------------------------------------------------- */
    *ArduinoJson*.*(.bss.buffer* .bss.pool* .bss.doc*)
    
    /* -----------------------------------------------------------------------
     * InfluxDB client buffers
     * ----------------------------------------------------------------------- */
    *InfluxDB*.*(.bss.buffer* .bss.batch* .bss.point*)
    
    /* -----------------------------------------------------------------------
     * Matter/Zigbee stack (if enabled)
     * ----------------------------------------------------------------------- */
    *libmatter*.a:(.bss.buffer* .bss.msg* .bss.session*)
    *libZigbee*.a:(.bss.buffer* .bss.frame*)
    
    . = ALIGN(16);
    _ext_ram_bss_end = ABSOLUTE(.);
  } > extern_ram_seg
  
  /* =========================================================================
   * .ext_ram.data - Initialized static data in PSRAM
   * =========================================================================
   * For large initialized arrays that should live in PSRAM.
   * These are copied from flash to PSRAM at startup.
   */
  .ext_ram.data :
  {
    . = ALIGN(16);
    _ext_ram_data_start = ABSOLUTE(.);
    
    /* Explicitly marked external RAM data */
    *(.ext_ram.data)
    *(.ext_ram.data.*)
    
    /* Large initialized lookup tables */
    *(.data.large*)
    *(.data.table*)
    *(.data.lookup*)
    
    . = ALIGN(16);
    _ext_ram_data_end = ABSOLUTE(.);
  } > extern_ram_seg
  
  /* =========================================================================
   * .ext_ram.rodata - Read-only data in PSRAM (optional)
   * =========================================================================
   * Large const arrays that can be placed in PSRAM instead of flash.
   * Note: Access may be slower than flash-mapped rodata.
   */
  .ext_ram.rodata :
  {
    . = ALIGN(16);
    _ext_ram_rodata_start = ABSOLUTE(.);
    
    *(.ext_ram.rodata)
    *(.ext_ram.rodata.*)
    
    /* Large const lookup tables */
    *(.rodata.large*)
    *(.rodata.table*)
    
    . = ALIGN(16);
    _ext_ram_rodata_end = ABSOLUTE(.);
  } > extern_ram_seg
  
  /* =========================================================================
   * .ext_ram.noinit - Non-initialized PSRAM (survives soft reset)
   * =========================================================================
   * Data that should NOT be zero-initialized. Use for buffers that
   * need to persist across soft resets.
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
}

/* 
 * Insert after .dram0.bss to ensure PSRAM sections are processed
 * after internal RAM sections
 */
INSERT AFTER .dram0.bss;
