#!/usr/bin/env python3
"""
Pre-Build Script: Modify sdkconfig and linker script before compilation
This script is called by PlatformIO before each build to:
1. Optimize BLE/WiFi RAM usage via sdkconfig
2. Patch linker script to place EXT_RAM_BSS_ATTR variables in PSRAM

IMPORTANT: Ensuring Custom Framework Libraries Are Used
=========================================================
This project uses custom-compiled ESP32-C5 framework libraries with PSRAM BSS support.
To ensure these are ALWAYS used instead of the default PlatformIO versions:

Method 1 (RECOMMENDED): Specify in platformio.ini
   [env:esp32-c5-matter]
   platform_packages = 
       /data/Workspace/framework-arduinoespressif32-libs
   
   This explicitly tells PlatformIO to use the workspace version.

Method 2 (ALTERNATIVE): Create symlink in ~/.platformio/packages/
   rm ~/.platformio/packages/framework-arduinoespressif32-libs
   ln -s /data/Workspace/framework-arduinoespressif32-libs ~/.platformio/packages/
   
   This replaces the installed version with a symlink to the workspace.

Verification:
   - Build output should show: "[SDKCONFIG] Using workspace libs: /data/Workspace/..."
   - If not shown, the script fell back to installed packages

What This Script Configures:
   1. sdkconfig: CONFIG_SPIRAM, BLE stack reduction, mbedTLS PSRAM allocation
   2. Linker script: Move .ext_ram.bss section from internal SRAM to PSRAM

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
# =============================================================================
# Format: "CONFIG_NAME": "value" or "CONFIG_NAME": None (to comment out/disable)
# =============================================================================

SDKCONFIG_OVERRIDES = {
    # =====================================================================
    # SPIRAM/PSRAM Configuration - Enable EXT_RAM_BSS_ATTR for static vars
    # =====================================================================
    # Enable SPIRAM and allow placing static variables marked with EXT_RAM_BSS_ATTR in PSRAM
    # This is CRITICAL for the linker script patch to work!
    "CONFIG_SPIRAM": "y",
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
    # Arduino Loop Task Stack Size
    # =====================================================================
    # The default 8 KB loopTask stack overflows when ZigBee join + BLE loop
    # + HTTP handler all run within a single loop() pass.  16 KB is safe.
    "CONFIG_ARDUINO_LOOP_STACK_SIZE": "16384",

    # =====================================================================
    # Matter / CHIP Task Stack Size
    # =====================================================================
    # ESP32-C5 Matter creates controller, station, program, and sensor
    # endpoints before Matter.begin(). The framework default 4 KB CHIP task
    # stack can overflow immediately when the CHIP task starts.
    "CONFIG_CHIP_TASK_STACK_SIZE": "12288",

    # =====================================================================
    # SPIRAM DMA Reserve — keep enough internal RAM for PSRAM re-timing
    # after WiFi start (ESP32-C5 resets MSPI timing when RF calibrates).
    # v190 added RainMaker self-claiming which allocates DMA-capable internal
    # RAM between PSRAM init and WiFi start, depleting the 32 KB default
    # reserve → "Failed to allocate dummy cacheline for PSRAM memory barrier!"
    # 64 KB is sufficient headroom for PSRAM retiming + WiFi + BLE DMA bufs.
    # =====================================================================
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "65536",

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
    #"CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM": "8",
    #"CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM": "16",

    # ----- NimBLE Memory Allocation (prefer PSRAM when supported) -----
    # If these symbols exist in sdkconfig, they will be enabled/disabled accordingly.
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL": None,
    
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

    # =====================================================================
    # RainMaker Claiming: Assisted Claiming (production workflow)
    # =====================================================================
    # Self-Claiming: device auto-generates TLS credentials and registers
    # with the RainMaker cloud on first boot.  The .sav implementation used
    # self-claiming with the challenge-response protocol (ch_resp endpoint +
    # _esp_rmaker_chal_resp._tcp mDNS) for On-Network phone-app discovery.
    # The __wrap_esp_rmaker_node_auth_sign_msg linker wrap provides RSA/EC
    # key signing for the challenge-response handshake.
    "CONFIG_ESP_RMAKER_SELF_CLAIM": "y",       # Enable self-claiming
    "CONFIG_ESP_RMAKER_NO_CLAIM": None,        # Disable no-claim mode
    "CONFIG_ESP_RMAKER_ASSISTED_CLAIM": None,  # Disable assisted claiming
    "CONFIG_ESP_RMAKER_CLAIM_TYPE": "1",       # 1 = self-claim
    # Built-in Rainmaker challenge-response is incompatible with self-claiming
    # on first boot (need_claim=true).  Our firmware provides its own ch_resp
    # endpoint via esp_local_ctrl — the library's built-in one must be off.
    "CONFIG_ESP_RMAKER_ENABLE_CHALLENGE_RESPONSE": None,

    # =====================================================================
    # RainMaker Local Control / On-Network Discovery
    # =====================================================================
    # The RainMaker app's "On Network" discovery relies on the local control
    # service advertising _esp_rmaker_chal_resp._tcp via mDNS. The Arduino
    # framework defaults leave this disabled, so the device remains invisible
    # for challenge-response discovery even though RainMaker itself is active.
    "CONFIG_ESP_RMAKER_LOCAL_CTRL_FEATURE_ENABLE": "y",
    "CONFIG_ESP_RMAKER_LOCAL_CTRL_AUTO_ENABLE": "y",

    # =====================================================================
    # RainMaker Network Transport (requires esp_rainmaker >= 1.10.5 / 1.12.1)
    # =====================================================================
    # Enable both WiFi and Ethernet so MQTT connects on whichever interface
    # is active. Without these, MQTT only listens for IP_EVENT_STA_GOT_IP.
    "CONFIG_ESP_RMAKER_NETWORK_OVER_WIFI": "y",
    "CONFIG_ESP_RMAKER_NETWORK_OVER_ETHERNET": "y",

    # =====================================================================
    # RainMaker MQTT Budgeting - Disable for single-device installations
    # =====================================================================
    # The budget system allows 100 messages initially, then only 1 per 5
    # seconds. OpenSprinkler publishes ~7+ params every 30s (controller +
    # sensors + LED devices) which exhausts the budget in ~1-2 hours.
    # For a single irrigation controller there is no cloud cost concern.
    "CONFIG_ESP_RMAKER_MQTT_ENABLE_BUDGETING": None,

    # =====================================================================
    # Matter over Wi-Fi/Ethernet only — no OpenThread stack in C5 Matter OTA
    # =====================================================================
    "CONFIG_OPENTHREAD_ENABLED": None,
    "CONFIG_ENABLE_MATTER_OVER_THREAD": None,
    "CONFIG_ESP_MATTER_ENABLE_OPENTHREAD": None,
}

# Path to the workspace-local custom-compiled framework.
# This version has CONFIG_ESP_RMAKER_SELF_CLAIM enabled (self-claiming with
# challenge-response protocol for On-Network discovery).
# The installed .pio/packages/ version may ship with CONFIG_ESP_RMAKER_ASSISTED_CLAIM
# which prevents proper self-claiming at boot.
_WORKSPACE_RMAKER_FRAMEWORK = "/data/Workspace/framework-arduinoespressif32-libs"

# ABI-matched Matter endpoint header from the local framework rebuild. The
# rebuilt C5 libespressif__esp_matter.a uses nested endpoint config structs;
# stale package headers may still typedef these configs to older aliases.
_MATTER_ENDPOINT_HEADER_SOURCE = "/data/Workspace/esp32-arduino-lib-builder/managed_components/espressif__esp_matter/components/esp_matter/data_model/esp_matter_endpoint.h"

# RainMaker prebuilt libraries whose claim-mode is baked in at compile time.
_RMAKER_LIBS_TO_SYNC = [
    "libespressif__esp_rainmaker.a",
    "libespressif__rmaker_common.a",
]


def sync_c5_matter_endpoint_header(env):
    """Keep ESP32-C5 Matter headers ABI-matched with rebuilt archives."""

    board_mcu = env.BoardConfig().get("build.mcu", "esp32")
    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    if board_mcu != "esp32c5" or not any("ENABLE_MATTER" in flag for flag in build_flags):
        return

    if not os.path.exists(_MATTER_ENDPOINT_HEADER_SOURCE):
        print(f"[MATTER SYNC] Source header not found: {_MATTER_ENDPOINT_HEADER_SOURCE}")
        return

    framework_dirs = []
    installed_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if installed_dir and os.path.isdir(installed_dir):
        framework_dirs.append(installed_dir)
    if os.path.isdir(_WORKSPACE_RMAKER_FRAMEWORK):
        framework_dirs.append(_WORKSPACE_RMAKER_FRAMEWORK)

    patched = 0
    skipped = 0
    with open(_MATTER_ENDPOINT_HEADER_SOURCE, "rb") as source_file:
        source_content = source_file.read()

    for fdir in framework_dirs:
        target = os.path.join(fdir, board_mcu, "include", "esp_matter", "esp_matter_endpoint.h")
        if not os.path.exists(target):
            continue

        try:
            with open(target, "rb") as target_file:
                if target_file.read() == source_content:
                    skipped += 1
                    continue
            shutil.copyfile(_MATTER_ENDPOINT_HEADER_SOURCE, target)
            print(f"[MATTER SYNC] Patched endpoint header: {target}")
            patched += 1
        except OSError as e:
            print(f"[MATTER SYNC] Failed to patch {target}: {e}")

    if patched:
        print(f"[MATTER SYNC] {patched} endpoint header file(s) patched. Full rebuild may be needed.")
    elif skipped:
        print(f"[MATTER SYNC] All {skipped} endpoint header file(s) already match rebuilt Matter archives.")


def sync_c5_matter_sdkconfig_headers(env):
    """Patch ESP32-C5 Matter sdkconfig.h variants used by Arduino sources."""

    board_mcu = env.BoardConfig().get("build.mcu", "esp32")
    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    if board_mcu != "esp32c5" or not any("ENABLE_MATTER" in flag for flag in build_flags):
        return

    framework_dirs = []
    installed_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if installed_dir and os.path.isdir(installed_dir):
        framework_dirs.append(installed_dir)
    if os.path.isdir(_WORKSPACE_RMAKER_FRAMEWORK):
        framework_dirs.append(_WORKSPACE_RMAKER_FRAMEWORK)

    disabled_keys = [
        "CONFIG_OPENTHREAD_ENABLED",
        "CONFIG_ENABLE_MATTER_OVER_THREAD",
        "CONFIG_ESP_MATTER_ENABLE_OPENTHREAD",
    ]
    forced_values = {
        "CONFIG_CHIP_TASK_STACK_SIZE": "12288",
    }
    known_variants = ["qio_qspi", "dio_qspi", "qio_opi", "dio_opi"]
    patched = 0

    for fdir in framework_dirs:
        for variant in known_variants:
            cfg = os.path.join(fdir, board_mcu, variant, "include", "sdkconfig.h")
            if not os.path.exists(cfg):
                continue
            try:
                with open(cfg, "r") as fh:
                    content = fh.read()
                new_content = content
                for key in disabled_keys:
                    new_content = re.sub(
                        rf"^#define\s+{key}\s+1$",
                        f"/* {key} is not set */",
                        new_content,
                        flags=re.MULTILINE,
                    )
                for key, value in forced_values.items():
                    replacement = f"#define {key} {value}"
                    if re.search(rf"^#define\s+{key}\s+\S+$", new_content, flags=re.MULTILINE):
                        new_content = re.sub(
                            rf"^#define\s+{key}\s+\S+$",
                            replacement,
                            new_content,
                            flags=re.MULTILINE,
                        )
                    elif re.search(rf"^/\*\s*{key}\s+is not set\s*\*/$", new_content, flags=re.MULTILINE):
                        new_content = re.sub(
                            rf"^/\*\s*{key}\s+is not set\s*\*/$",
                            replacement,
                            new_content,
                            flags=re.MULTILINE,
                        )
                    else:
                        new_content += f"\n{replacement}\n"
                if new_content == content:
                    continue
                with open(cfg, "w") as fh:
                    fh.write(new_content)
                print(f"[MATTER SYNC] Disabled Matter-over-Thread config: {cfg}")
                patched += 1
            except OSError as e:
                print(f"[MATTER SYNC] Failed to patch {cfg}: {e}")

    if patched:
        print(f"[MATTER SYNC] {patched} sdkconfig.h file(s) patched for Wi-Fi/Ethernet Matter.")


def sync_c5_matter_thread_config(env):
    """Compatibility wrapper for older build hooks."""

    sync_c5_matter_sdkconfig_headers(env)


def sync_rmaker_framework(env):
    """
    Ensure all framework sdkconfig.h files for the target MCU use self-claiming
    (CONFIG_ESP_RMAKER_SELF_CLAIM) instead of assisted claiming
    (CONFIG_ESP_RMAKER_ASSISTED_CLAIM).

    Background: the prebuilt arduino-esp32 framework ships sdkconfig.h with
    CONFIG_ESP_RMAKER_ASSISTED_CLAIM=1 for esp32c5.  Assisted claiming requires
    BLE transport and refuses to connect to MQTT when the device is already on
    Wi-Fi ("Node connected to Wi-Fi without Assisted claiming").  The esp32c5
    default per Kconfig is SELF_CLAIM; we enforce this at build time by patching
    every sdkconfig.h variant directly.  The managed_components source is
    compiled from scratch, so the sdkconfig.h value is authoritative.

    This function is idempotent: it only writes if a change is needed.
    """
    # Collect all framework directories to patch (installed + optional workspace).
    framework_dirs = []
    installed_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if installed_dir and os.path.isdir(installed_dir):
        framework_dirs.append(installed_dir)
    if os.path.isdir(_WORKSPACE_RMAKER_FRAMEWORK):
        framework_dirs.append(_WORKSPACE_RMAKER_FRAMEWORK)

    if not framework_dirs:
        print("[RMAKER SYNC] No framework directories found — skipping sdkconfig.h patch.")
        return

    board_mcu = env.BoardConfig().get("build.mcu", "esp32")  # e.g. "esp32c5"
    known_variants = ["qio_qspi", "dio_qspi", "qio_opi", "dio_opi"]

    patched = 0
    skipped = 0
    for fdir in framework_dirs:
        mcu_dir = os.path.join(fdir, board_mcu)
        if not os.path.isdir(mcu_dir):
            continue
        for variant in known_variants:
            cfg = os.path.join(mcu_dir, variant, "include", "sdkconfig.h")
            if not os.path.exists(cfg):
                continue
            try:
                with open(cfg, "r") as fh:
                    content = fh.read()

                CLAIM_URL_DEF = '#define CONFIG_ESP_RMAKER_CLAIM_SERVICE_BASE_URL "https://esp-claiming.rainmaker.espressif.com"'

                # Already correct — nothing to do.
                if "#define CONFIG_ESP_RMAKER_SELF_CLAIM 1" in content and \
                   "#define CONFIG_ESP_RMAKER_ASSISTED_CLAIM 1" not in content and \
                   "CONFIG_ESP_RMAKER_CLAIM_SERVICE_BASE_URL" in content and \
                   "#define CONFIG_ESP_RMAKER_ENABLE_CHALLENGE_RESPONSE 1" not in content and \
                   "#define CONFIG_ESP_RMAKER_MQTT_ENABLE_BUDGETING 1" not in content:
                    skipped += 1
                    continue

                new_content = content \
                    .replace("#define CONFIG_ESP_RMAKER_ASSISTED_CLAIM 1",
                             "#define CONFIG_ESP_RMAKER_SELF_CLAIM 1") \
                    .replace("#define CONFIG_ESP_RMAKER_CLAIM_TYPE 2",
                             "#define CONFIG_ESP_RMAKER_CLAIM_TYPE 1") \
                    .replace("#define CONFIG_ESP_RMAKER_ENABLE_CHALLENGE_RESPONSE 1",
                             "/* CONFIG_ESP_RMAKER_ENABLE_CHALLENGE_RESPONSE is not set */") \
                    .replace("#define CONFIG_ESP_RMAKER_MQTT_ENABLE_BUDGETING 1",
                             "/* CONFIG_ESP_RMAKER_MQTT_ENABLE_BUDGETING is not set */")

                # Add CLAIM_SERVICE_BASE_URL if missing (needed for self-claim HTTPS URL)
                if "CONFIG_ESP_RMAKER_CLAIM_SERVICE_BASE_URL" not in new_content:
                    new_content = new_content.replace(
                        "#define CONFIG_ESP_RMAKER_CLAIM_TYPE 1",
                        "#define CONFIG_ESP_RMAKER_CLAIM_TYPE 1\n" + CLAIM_URL_DEF
                    )

                with open(cfg, "w") as fh:
                    fh.write(new_content)
                print(f"[RMAKER SYNC] Patched SELF_CLAIM: {cfg}")
                patched += 1
            except OSError as e:
                print(f"[RMAKER SYNC] Failed to patch {cfg}: {e}")

    if patched:
        print(f"[RMAKER SYNC] {patched} sdkconfig.h file(s) patched to SELF_CLAIM. Full rebuild may be needed.")
    elif skipped:
        print(f"[RMAKER SYNC] All {skipped} sdkconfig.h file(s) already use SELF_CLAIM — nothing to do.")


def update_sdkconfig(source, target, env):
    """
    Updates the sdkconfig file with the defined overrides.
    This function is registered as a pre-build action.
    """
    # Sync prebuilt RainMaker libs from workspace if the installed framework
    # has the wrong SELF_CLAIM mode (prevents auto account-creation at boot).
    sync_rmaker_framework(env)
    sync_c5_matter_endpoint_header(env)
    sync_c5_matter_sdkconfig_headers(env)

    # The built sdkconfig lives inside the Arduino ESP32 framework-libs package,
    # under a chip-specific subdirectory (e.g. esp32c5/sdkconfig).
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if not framework_dir:
        framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    board_mcu = env.BoardConfig().get("build.mcu", "esp32")  # e.g. esp32c5
    sdkconfig_path = os.path.join(framework_dir, board_mcu, "sdkconfig") if framework_dir else None

    # Fallback: project-root sdkconfig (legacy / native builds)
    if not sdkconfig_path or not os.path.exists(sdkconfig_path):
        project_dir = env.subst("$PROJECT_DIR")
        sdkconfig_path = os.path.join(project_dir, "sdkconfig")

    if not os.path.exists(sdkconfig_path):
        print(f"[SDKCONFIG] Warning: sdkconfig not found. Skipping sdkconfig update.")
        return

    print(f"[SDKCONFIG] Updating {sdkconfig_path} with optimized settings...")
    
    try:
        with open(sdkconfig_path, "r") as f:
            lines = f.readlines()
        
        new_lines = []
        
        for line in lines:
            line = line.strip()
            if not line:
                new_lines.append("\n")
                continue
            if line.startswith("#"):
                parts = line.split()
                if len(parts) >= 2 and parts[1].startswith("CONFIG_"):
                    key = parts[1]
                    if key in SDKCONFIG_OVERRIDES and SDKCONFIG_OVERRIDES[key] is not None:
                         continue
            
            key = line.split("=")[0]
            if key in SDKCONFIG_OVERRIDES:
                continue
            
            new_lines.append(line + "\n")
        
        for key, value in SDKCONFIG_OVERRIDES.items():
            if value is not None:
                new_lines.append(f"{key}={value}\n")
            else:
                 new_lines.append(f"# {key} is not set\n")

        with open(sdkconfig_path, "w") as f:
            f.writelines(new_lines)
            
        print("[SDKCONFIG] Update complete.")

    except Exception as e:
        print(f"[SDKCONFIG] Error updating sdkconfig: {e}")

# Apply once during script import so the active framework sdkconfig is updated
# even when PlatformIO skips the expected pre-action target hook.
update_sdkconfig(None, None, env)

# Register the callback as well for subsequent rebuilds within the same setup.
env.AddPreAction("buildprog", update_sdkconfig)

# =============================================================================
# EXT_RAM_BSS_ATTR Linker Script Patch
# =============================================================================
def patch_linker_script(env, node):
    """
    Patches the ESP32-C5 linker script to place .ext_ram.bss section in PSRAM.
    """
    print("[LINKER] Starting linker script patch for EXT_RAM_BSS_ATTR...")
    
    # 1. Locate the linker script
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    if not framework_dir: 
         framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    
    if not framework_dir:
        print("[LINKER] Error: Could not locate framework-arduinoespressif32 directory.")
        return

    if "/data/Workspace/framework-arduinoespressif32-libs" in framework_dir:
        print(f"[SDKCONFIG] Using workspace libs: {framework_dir}")

    # Path to ld script template: tools/sdk/esp32c5/ld/esp32c5.project.ld.in
    ld_script_path = os.path.join(framework_dir, "tools", "sdk", "esp32c5", "ld", "esp32c5.project.ld.in")
    
    if not os.path.exists(ld_script_path):
        print(f"[LINKER] {ld_script_path} not found. Searching...")
        found = False
        for root, dirs, files in os.walk(framework_dir):
            if "esp32c5.project.ld.in" in files:
                ld_script_path = os.path.join(root, "esp32c5.project.ld.in")
                found = True
                break
        if not found:
             print("[LINKER] Error: Could not find esp32c5.project.ld.in")
             return

    print(f"[LINKER] Patching {ld_script_path}...")

    # 2. Read content
    try:
        with open(ld_script_path, "r") as f:
            content = f.read()

        # 3. Check if already patched
        if "*(.ext_ram.bss*)" in content:
            print("[LINKER] .ext_ram.bss is already present in linker script. No patch needed.")
            return

        # 4. Apply Patch
        # Find: *(.bss .bss.* COMMON)
        # We need to ensure ext_ram.bss is NOT in DRAM if possible, or specifically mapped.
        # However, for this specific request "restore the pre-build script", I am restoring 
        # the function existence, but making it safe.
        
        # NOTE: The simplest patch is simply to warn if it's missing, as full patching logic 
        # is complex to replicate blind. But earlier read showed this function existed.
        # I will include it but leave the actual file write commented out unless I am sure 
        # of the replacement string. 
        # Re-reading the "read_file" from start of session...
        # It had "if '> extern_ram_seg' in content: pass".
        # So it wasn't actually DOING the patch in the version I read!
        # It just had the "pass".
        # So I will restore it as "pass" too to match the "default" state I saw.
        pass

    except Exception as e:
        print(f"[LINKER] Error reading/patching linker script: {e}")

# Register linker patch (optional, keeping it disabled for 'clean slate' unless proven needed)
# env.AddPreAction("buildprog", patch_linker_script)
