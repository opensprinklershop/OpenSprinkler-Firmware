#!/usr/bin/env python3
"""
Pre-Build Script: Modify sdkconfig and linker script before compilation
This script is called by PlatformIO before each build to:
1. Optimize BLE/WiFi RAM usage via sdkconfig
2. Patch linker script to place EXT_RAM_BSS_ATTR variables in PSRAM

Usage in platformio.ini:
    extra_scripts = 
        pre:pre_build_sdkconfig.py
"""

Import("env")
import os
import re
import shutil

# =============================================================================
# CONFIGURATION: Define sdkconfig parameters to modify
# Format: "CONFIG_NAME": "value" or "CONFIG_NAME": None (to comment out/disable)
# =============================================================================

SDKCONFIG_OVERRIDES = {
    # =====================================================================
    # SPIRAM/PSRAM Configuration - Enable EXT_RAM_BSS_ATTR for static vars
    # =====================================================================
    # Allow placing static variables marked with EXT_RAM_BSS_ATTR in PSRAM
    # This is CRITICAL for the linker script patch to work!
    "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY": "y",
    "CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY": "y",
    
    # =====================================================================
    # mbedTLS PSRAM Allocation - CRITICAL for HTTPS server/client
    # =====================================================================
    # Force mbedTLS to use SPIRAM (PSRAM) for all allocations
    # This fixes ALLOC_FAIL errors for SSL buffers on ESP32-C5 with limited internal RAM
    # See esp-idf/components/mbedtls/port/esp_mem.c for implementation
    "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC": "y",
    "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC": None,  # Disable internal-only allocation
    "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC": None,   # Disable default (malloc) allocation
    "CONFIG_MBEDTLS_IRAM_8BIT_MEM_ALLOC": None, # Disable IRAM allocation fallback
    
    # =====================================================================
    # BLE Stack Size Reduction
    # =====================================================================
    # Reduce NimBLE host task stack: 5120 -> 3584 (saves ~1.5KB internal RAM)
    "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE": "3584",
    "CONFIG_NIMBLE_TASK_STACK_SIZE": "3584",
    "CONFIG_BT_NIMBLE_TASK_STACK_SIZE": "3584",
    
    # ----- BLE Memory Pool Reduction -----
    # Reduce MSYS blocks: 24 -> 16 (saves ~4KB, still enough for Matter)
    "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT": "16",
    "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT": "16",
    
    # ----- BLE Connection/Bond Limits -----
    # Reduce max bonds: 3 -> 2 (saves ~150 bytes per bond)
    "CONFIG_BT_NIMBLE_MAX_BONDS": "2",
    
    # Reduce GATT procedures: 4 -> 2 (saves ~56 bytes)
    "CONFIG_BT_NIMBLE_GATT_MAX_PROCS": "2",
    
    # Reduce CCCDs: 8 -> 4 (saves ~80 bytes)
    "CONFIG_BT_NIMBLE_MAX_CCCDS": "4",
    
    # ----- WiFi Buffer Optimization -----
    # Note: These are already optimized in default sdkconfig for SPIRAM
    # Uncomment to further reduce if needed:
    # "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM": "10",
    # "CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM": "24",
    
    # =====================================================================
    # Disable Unused BLE GATT Services (saves flash and RAM)
    # =====================================================================
    "CONFIG_BT_NIMBLE_PROX_SERVICE": None,  # Proximity
    "CONFIG_BT_NIMBLE_ANS_SERVICE": None,   # Alert Notification
    "CONFIG_BT_NIMBLE_CTS_SERVICE": None,   # Current Time
    "CONFIG_BT_NIMBLE_HTP_SERVICE": None,   # Health Thermometer
    "CONFIG_BT_NIMBLE_IPSS_SERVICE": None,  # Internet Protocol Support
    "CONFIG_BT_NIMBLE_TPS_SERVICE": None,   # TX Power
    "CONFIG_BT_NIMBLE_IAS_SERVICE": None,   # Immediate Alert
    "CONFIG_BT_NIMBLE_LLS_SERVICE": None,   # Link Loss
    "CONFIG_BT_NIMBLE_SPS_SERVICE": None,   # Scan Parameters
    "CONFIG_BT_NIMBLE_HR_SERVICE": None,    # Heart Rate
    "CONFIG_BT_NIMBLE_BAS_SERVICE": None,   # Battery Service
}

def find_sdkconfig():
    """Find the sdkconfig file in the build directory or framework"""
    # Check framework libs directory
    framework_paths = [
        os.path.join(env.get("PROJECT_PACKAGES_DIR", ""), "framework-arduinoespressif32-libs", "esp32c5", "sdkconfig"),
        os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/sdkconfig"),
        "/data/Workspace/framework-arduinoespressif32-libs/esp32c5/sdkconfig",
    ]
    
    for path in framework_paths:
        if os.path.exists(path):
            return path
    
    # Check build directory
    build_dir = env.get("BUILD_DIR", "")
    if build_dir:
        sdkconfig_path = os.path.join(build_dir, "sdkconfig")
        if os.path.exists(sdkconfig_path):
            return sdkconfig_path
    
    return None

def modify_sdkconfig(sdkconfig_path):
    """Modify sdkconfig with our overrides"""
    if not os.path.exists(sdkconfig_path):
        print(f"[SDKCONFIG] ERROR: File not found: {sdkconfig_path}")
        return False
    
    print(f"[SDKCONFIG] Modifying: {sdkconfig_path}")
    
    with open(sdkconfig_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original_content = content
    changes_made = []
    
    for config_name, new_value in SDKCONFIG_OVERRIDES.items():
        # Pattern to match: CONFIG_NAME=value or # CONFIG_NAME is not set
        pattern_set = rf'^{re.escape(config_name)}=.*$'
        pattern_unset = rf'^# {re.escape(config_name)} is not set$'
        
        if new_value is None:
            # Disable/comment out this config
            replacement = f"# {config_name} is not set"
            
            # Check if it's currently set
            if re.search(pattern_set, content, re.MULTILINE):
                content = re.sub(pattern_set, replacement, content, flags=re.MULTILINE)
                changes_made.append(f"  Disabled: {config_name}")
        else:
            # Set to new value
            replacement = f"{config_name}={new_value}"
            
            # Check if it's currently set to a different value
            match = re.search(pattern_set, content, re.MULTILINE)
            if match:
                old_line = match.group(0)
                if old_line != replacement:
                    content = re.sub(pattern_set, replacement, content, flags=re.MULTILINE)
                    changes_made.append(f"  Changed: {config_name} -> {new_value}")
            # Check if it's commented out
            elif re.search(pattern_unset, content, re.MULTILINE):
                content = re.sub(pattern_unset, replacement, content, flags=re.MULTILINE)
                changes_made.append(f"  Enabled: {config_name}={new_value}")
    
    if content != original_content:
        with open(sdkconfig_path, 'w', encoding='utf-8') as f:
            f.write(content)
        
        print(f"[SDKCONFIG] {len(changes_made)} changes applied:")
        for change in changes_made:
            print(change)
        return True
    else:
        print("[SDKCONFIG] No changes needed (already optimized)")
        return False

# =============================================================================
# LINKER SCRIPT PATCHING: Move .ext_ram.bss from internal SRAM to PSRAM
# =============================================================================

def find_framework_linker_script():
    """Find the sections.ld linker script in the framework"""
    framework_paths = [
        # Check installed package first (this is what PlatformIO uses!)
        os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/ld/sections.ld"),
        # Then local workspace copies
        "/data/Workspace/framework-arduinoespressif32-libs/esp32c5/ld/sections.ld",
        os.path.join(env.get("PROJECT_PACKAGES_DIR", ""), "framework-arduinoespressif32-libs", "esp32c5", "ld", "sections.ld"),
    ]
    
    found_paths = []
    for path in framework_paths:
        if os.path.exists(path):
            found_paths.append(path)
    
    return found_paths if found_paths else None

def patch_linker_script(linker_path):
    """
    Patch the framework linker script to move .ext_ram.bss from internal SRAM to PSRAM.
    
    The default ESP-IDF linker script places .ext_ram.bss inside .dram0.bss (internal SRAM).
    We need to:
    1. Remove *(.ext_ram.bss .ext_ram.bss.*) from .dram0.bss section
    2. Create a new .ext_ram.bss section that maps to extern_ram_seg (PSRAM)
    
    This enables zero-initialized static variables with EXT_RAM_BSS_ATTR to be placed in PSRAM.
    """
    if not os.path.exists(linker_path):
        print(f"[LINKER] ERROR: File not found: {linker_path}")
        return False
    
    print(f"[LINKER] Patching: {linker_path}")
    
    with open(linker_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    original_content = content
    changes_made = []
    
    # Check if already patched (look for our marker comment or section)
    if "/* PSRAM_PATCH:" in content or "_ext_ram_bss_start = ABSOLUTE" in content:
        print("[LINKER] Already patched for PSRAM")
        return False
    
    # Step 1: Comment out .ext_ram.bss from .dram0.bss section
    # Find the line: *(.ext_ram.bss .ext_ram.bss.*)
    old_pattern = r'(\s+)\*\(\.ext_ram\.bss \.ext_ram\.bss\.\*\)'
    if re.search(old_pattern, content):
        # Replace with commented version
        content = re.sub(
            old_pattern,
            r'\1/* PSRAM_PATCH: Moved to extern_ram_seg */\n\1/* *(.ext_ram.bss .ext_ram.bss.*) */',
            content
        )
        changes_made.append("  - Removed .ext_ram.bss from .dram0.bss (internal SRAM)")
    
    # Step 2: Add new .ext_ram.bss section after .ext_ram.dummy
    ext_ram_section = '''
  /**
   * PSRAM_PATCH: External RAM BSS Section for EXT_RAM_BSS_ATTR variables
   * Variables marked with EXT_RAM_BSS_ATTR are placed here and zero-initialized.
   * This frees up internal SRAM for DMA, WiFi, and stack.
   */
  .ext_ram.bss (NOLOAD) :
  {
    . = ALIGN(16);
    _ext_ram_bss_start = ABSOLUTE(.);
    *(.ext_ram.bss .ext_ram.bss.*)
    . = ALIGN(16);
    _ext_ram_bss_end = ABSOLUTE(.);
  } > extern_ram_seg

'''
    
    # Find the .ext_ram.dummy section: "} > extern_ram_seg" followed by "  /**"
    # Use DOTALL to match across newlines
    dummy_pattern = r'(\.ext_ram\.dummy \(NOLOAD\):.*?\} > extern_ram_seg\n)(\s*/\*\*)'
    match = re.search(dummy_pattern, content, re.DOTALL)
    if match:
        # Insert our section after .ext_ram.dummy
        content = content[:match.end(1)] + ext_ram_section + content[match.start(2):]
        changes_made.append("  - Added .ext_ram.bss section in extern_ram_seg (PSRAM)")
    
    if content != original_content:
        # Create backup
        backup_path = linker_path + ".orig"
        if not os.path.exists(backup_path):
            shutil.copy2(linker_path, backup_path)
            print(f"[LINKER] Backup created: {backup_path}")
        
        with open(linker_path, 'w', encoding='utf-8') as f:
            f.write(content)
        
        print(f"[LINKER] {len(changes_made)} changes applied:")
        for change in changes_made:
            print(change)
        return True
    else:
        print("[LINKER] No changes needed")
        return False

def pre_build_callback(source, target, env):
    """Callback executed before build"""
    print("\n" + "="*60)
    print("[PRE-BUILD] Applying RAM optimizations for ESP32-C5")
    print("           - mbedTLS PSRAM allocation (fixes HTTPS ALLOC_FAIL)")
    print("           - BLE stack size reduction")
    print("           - Linker script patch for EXT_RAM_BSS_ATTR -> PSRAM")
    print("="*60)
    
    # Patch sdkconfig
    sdkconfig_path = find_sdkconfig()
    if sdkconfig_path:
        modify_sdkconfig(sdkconfig_path)
    else:
        print("[SDKCONFIG] WARNING: Could not find sdkconfig file")
        print("[SDKCONFIG] Searched in framework-arduinoespressif32-libs/esp32c5/")
    
    # Patch linker script to move .ext_ram.bss to PSRAM
    linker_paths = find_framework_linker_script()
    if linker_paths:
        for linker_path in linker_paths:
            patch_linker_script(linker_path)
    else:
        print("[LINKER] WARNING: Could not find sections.ld linker script")
        print("[LINKER] Searched in framework-arduinoespressif32-libs/esp32c5/ld/")
    
    print("="*60 + "\n")

# Register the pre-build callback
env.AddPreAction("buildprog", pre_build_callback)

# Also run on first import (for clean builds)
print("\n[PRE-BUILD] Script loaded - will optimize sdkconfig and linker script before build")
