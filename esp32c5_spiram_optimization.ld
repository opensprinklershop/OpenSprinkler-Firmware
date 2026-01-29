/**
 * @file esp32c5_spiram_optimization.ld
 * @brief Custom linker script fragment for aggressive SPIRAM usage
 * 
 * This fragment extends ESP-IDF linker scripts to place static objects
 * >= 16 bytes into SPIRAM automatically.
 * 
 * Usage: Add to platformio.ini:
 *   board_build.ldscript = esp32c5_spiram_optimization.ld
 * 
 * Or include via build_flags:
 *   -Wl,-T,esp32c5_spiram_optimization.ld
 */

/* SPIRAM optimization sections for static data >= 16 bytes */
SECTIONS
{
  /* 
   * Place large static buffers and objects into external RAM (SPIRAM)
   * Anything marked with EXT_RAM_BSS_ATTR or >= 16 bytes
   */
  .ext_ram.bss (NOLOAD) :
  {
    _ext_ram_bss_start = ABSOLUTE(.);
    
    /* OpenSprinkler specific large buffers */
    *(.ext_ram.bss)
    *(.ext_ram.bss.*)
    
    /* Place any uninitialized data >= 16 bytes into SPIRAM */
    /* This requires custom toolchain support or explicit attributes */
    
    /* Common large buffers (examples) */
    *libOpenSprinkler*.a:(.bss.buffer* .bss.cache* .bss.queue*)
    *libOpenThings*.a:(.bss.buffer* .bss.cache* .bss.pool*)
    *libWebSockets*.a:(.bss.buffer*)
    *libmbedtls*.a:(.bss.ssl* .bss.tls*)
    
    _ext_ram_bss_end = ABSOLUTE(.);
  } > ext_ram_seg
  
  /* 
   * SPIRAM for initialized data >= 16 bytes 
   */
  .ext_ram.data :
  {
    _ext_ram_data_start = ABSOLUTE(.);
    
    *(.ext_ram.data)
    *(.ext_ram.data.*)
    
    /* Place large initialized arrays/structs */
    *(.data.large*)
    *(.rodata.large*)
    
    _ext_ram_data_end = ABSOLUTE(.);
  } > ext_ram_seg
}

/* 
 * Insert before output section .flash.rodata 
 * to intercept large const data 
 */
INSERT AFTER .iram0.text;
