#!/usr/bin/env python3
"""
Pre-Build Script: Modify sdkconfig before compilation
This script is called by PlatformIO before each build to optimize BLE/WiFi RAM usage.

Usage in platformio.ini:
    extra_scripts = 
        pre:pre_build_sdkconfig.py
"""

Import("env")
import os
import re

# =============================================================================
# CONFIGURATION: Define sdkconfig parameters to modify
# Format: "CONFIG_NAME": "value" or "CONFIG_NAME": None (to comment out/disable)
# =============================================================================

SDKCONFIG_OVERRIDES = {
    # ----- BLE Stack Size Reduction -----
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
    
    # ----- Disable Unused BLE GATT Services -----
    # These save flash and a bit of RAM
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

def pre_build_callback(source, target, env):
    """Callback executed before build"""
    print("\n" + "="*60)
    print("[SDKCONFIG] Pre-build: Applying BLE/WiFi RAM optimizations")
    print("="*60)
    
    sdkconfig_path = find_sdkconfig()
    if sdkconfig_path:
        modify_sdkconfig(sdkconfig_path)
    else:
        print("[SDKCONFIG] WARNING: Could not find sdkconfig file")
        print("[SDKCONFIG] Searched in framework-arduinoespressif32-libs/esp32c5/")
    
    print("="*60 + "\n")

# Register the pre-build callback
env.AddPreAction("buildprog", pre_build_callback)

# Also run on first import (for clean builds)
print("\n[SDKCONFIG] Script loaded - will optimize sdkconfig before build")
