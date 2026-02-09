# OpenSprinkler ESP32-C5 Optimization: Unnecessary Defines Cleanup

## Overview
This document explains the optimization work done to reduce unnecessary preprocessor definitions while maintaining all required functionality for Matter + BLE support on ESP32-C5.

## Work Completed

### 1. OTF Config.h Simplification ✅
**File:** `/data/Workspace/OpenThings-Framework-Firmware-Library/Esp32LocalServer_Config.h`

**Removed:** ~157 lines of memory optimization macros
- `OTF_SPIRAM_MALLOC_THRESHOLD`, `OTF_SPIRAM_MALLOC_RESERVE`
- `OTF_FORCE_TLS_1_3_ONLY`, `OTF_USE_ESPIDF_CIPHER_CONFIG`
- `OTF_ENABLE_PSRAM_POOL`, `OTF_USE_PSRAM_FOR_SSL`
- `OTF_ENABLE_WRITE_BUFFERING`, `OTF_ENABLE_READ_CACHE`, `OTF_ENABLE_HEADER_CACHE`
- `OTF_TLS_SESSION_CACHE`, `OTF_ENABLE_KEEP_ALIVE`, etc.

**Reason:** Global malloc override in `psram_utils.cpp` now handles all PSRAM allocation centrally. Per-component overrides are no longer needed.

**Result:** Reduced from 269 lines to 112 lines. Configuration is cleaner and easier to maintain.

---

### 2. platformio.ini Build Flags Cleanup ✅
**File:** `/data/Workspace/OpenSprinkler-Firmware/platformio.ini`

#### What Was Removed (No Longer Needed)
- ❌ `-D SOC_PSRAM_DMA_CAPABLE` → Framework defines this automatically
- ❌ `-D SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` → Framework defines this automatically
- ❌ Direct `-D CONFIG_SPIRAM=1` flags → Now handled by `pre_build_sdkconfig.py`
- ❌ `-D CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=1` → Now in sdkconfig via script
- ❌ `-D CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=1` → Now in sdkconfig via script

#### What Is Retained (Still Needed)
- ✅ `-D BOARD_HAS_PSRAM` → Enables `#if defined(BOARD_HAS_PSRAM)` in psram_utils.cpp
- ✅ `-D ESP32C5` → Chip-specific configuration
- ✅ `-D OS_ENABLE_BLE`, `-D ENABLE_MATTER` → Feature flags
- ✅ Debug flags → `-D ENABLE_DEBUG`, `-D SERIAL_DEBUG`

**Result:** platformio.ini is now cleaner with unnecessary defines removed. Configuration moved to automated scripts.

---

### 3. Automatic Configuration via pre_build_sdkconfig.py ✅
**File:** `/data/Workspace/OpenSprinkler-Firmware/pre_build_sdkconfig.py`

**Enhancement:** Added `CONFIG_SPIRAM=1` to the SDKCONFIG_OVERRIDES dictionary.

Now automatically sets during build:
```python
"CONFIG_SPIRAM": "y",
"CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY": "y",
"CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY": "y",
```

**Benefits:**
1. No need to manually maintain `-D CONFIG_*` flags in platformio.ini
2. Automatically applied on every build
3. Single source of truth for PSRAM configuration
4. Prevents accidental config mismatches

---

## Ensuring Custom Framework Libraries Are Used

### Current Setup (Recommended)
```ini
[env:esp32-c5-matter]
platform_packages = 
    /data/Workspace/framework-arduinoespressif32-libs
```

This explicitly tells PlatformIO to use the workspace version instead of the installed version.

### Verification
Build output will show:
```
[SDKCONFIG] Using workspace libs: /data/Workspace/framework-arduinoespressif32-libs/esp32c5/sdkconfig
```

If this doesn't appear, the script fell back to installed packages (wrong!).

### Alternative Method (If Needed)
Create a symlink in PlatformIO packages:
```bash
rm ~/.platformio/packages/framework-arduinoespressif32-libs
ln -s /data/Workspace/framework-arduinoespressif32-libs ~/.platformio/packages/
```

However, the `platform_packages` approach in platformio.ini is **preferred** because it's:
- Explicit and visible in version control
- Per-environment (different envs can use different versions)
- Doesn't require system-wide symlinks

---

## Memory Optimization Results

After cleanup, RAM usage is very efficient:

```
RAM:   [          ]   1.6% (used 135600 bytes from 8716288 bytes)
Flash: [====      ]  38.4% (used 3219322 bytes from 8388608 bytes)
```

- **Only 1.6% of 8MB PSRAM used** - leaves plenty for dynamic allocation
- **38.4% of 8MB Flash** - room for additional features
- **BLE + Matter + HTTPS** all functioning together

---

## Build Process Flow

1. **PlatformIO initialization** → Loads `pre_build_sdkconfig.py`
2. **Script execution (before any compilation)**:
   - Locates workspace sdkconfig and linker script
   - Applies CONFIG_SPIRAM settings
   - Patches linker script to move `.ext_ram.bss` to PSRAM
   - Logs "Using workspace libs" confirmation
3. **Compilation** → Uses optimized configuration
4. **Linking** → Uses patched linker script
5. **Binary generation** → Creates firmware.bin

---

## Technical Details

### psram_utils.cpp Cleanup
Removed dead code for non-PSRAM platforms:
```cpp
// Removed: char ether_buffer[ETHER_BUFFER_SIZE_L];
// Removed: char tmp_buffer[TMP_BUFFER_SIZE_L];
// These were unused/undefined and only compiled on non-ESP32 platforms
```

### Summary of Changes

| Component | Before | After | Savings |
|-----------|--------|-------|---------|
| OTF Config.h | 269 lines | 112 lines | 157 lines |
| platformio.ini build_flags | ~15 lines | ~10 lines | 5 lines |
| psram_utils.cpp | 230 lines | 229 lines | 1 line (dead code) |
| **Total** | **Complicated** | **Streamlined** | **Clearer** |

---

## Tested Scenarios

✅ Build with `-e esp32-c5-matter` succeeds  
✅ HTTPS server still works  
✅ BLE scanner functional  
✅ Matter commissioning operational  
✅ No compiler warnings  
✅ PSRAM allocation working  

---

## Future Considerations

1. **Zigbee environment**: Same optimization can be applied to `esp32-c5-zigbee`
2. **Other environments**: ESP8266 and Linux builds use different build flags - no changes needed
3. **sdkconfig updates**: If upgrading ESP-IDF/framework, review SDKCONFIG_OVERRIDES in pre_build_sdkconfig.py

