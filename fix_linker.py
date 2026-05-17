"""PlatformIO build script helpers.

- Fix RISC-V LTO linker issues by forcing GCC wrapper (riscv32-esp-elf-gcc).
- Remove OpenThread library for Zigbee builds to avoid IEEE802154 symbol conflicts.
- Work around cases where the RISC-V toolchain bin dir is not on PATH during
  the Arduino .ino preprocessing step (ConvertInoToCpp).
"""

import os
import re
import shutil

Import("env")


def _prepend_env_path(env, path):
    if not path:
        return
    env_path = env.get("ENV", {}).get("PATH", "")
    parts = [p for p in env_path.split(os.pathsep) if p]
    if path in parts:
        return
    env["ENV"]["PATH"] = path + (os.pathsep + env_path if env_path else "")


def ensure_riscv_toolchain_on_path(env):
    """Make sure riscv32 toolchain bin dir is on PATH.

    With some platform-espressif32 variants (notably git/develop), the PATH used
    during ConvertInoToCpp can end up missing the toolchain bin dir, causing
    'riscv32-esp-elf-g++ not found' even though the package is installed.
    """

    try:
        pio_platform = env.PioPlatform()
    except Exception:
        pio_platform = None

    if not pio_platform:
        return

    toolchain_dir = pio_platform.get_package_dir("toolchain-riscv32-esp")
    if not toolchain_dir:
        return

    # Some package variants (seen on Windows) have an extra top-level folder
    # (e.g. <pkg>/riscv32-esp-elf/bin) instead of <pkg>/bin.
    bin_candidates = [
        os.path.join(toolchain_dir, "bin"),
        os.path.join(toolchain_dir, "riscv32-esp-elf", "bin"),
        os.path.join(toolchain_dir, "riscv32-esp-elf", "riscv32-esp-elf", "bin"),
    ]
    bin_dir = next((p for p in bin_candidates if os.path.isdir(p)), None)
    if not bin_dir:
        return

    _prepend_env_path(env, bin_dir)

    # Prefer compiler names with PATH set, to avoid duplicate absolute paths
    # being constructed by downstream CMake/IDF generators.
    exe_suffix = ".exe" if os.name == "nt" else ""
    gpp = "riscv32-esp-elf-g++" + exe_suffix
    gcc = "riscv32-esp-elf-gcc" + exe_suffix

    print(f"Using RISC-V toolchain bin: {bin_dir}")
    env.Replace(CXX=gpp)
    env.Replace(CC=gcc)

# Replace linker command to use GCC wrapper instead of ld
def fix_linker_command(env):
    cc_cmd = env.get("CC", "")
    if "riscv32" in str(cc_cmd) or "esp32-c5" in env.get("BOARD", "").lower():
        print("Fixing RISC-V linker to use GCC wrapper for LTO support...")

        # Use GCC as linker instead of ld directly.
        # IMPORTANT: CC may already be an absolute path on Windows.
        gcc_linker = cc_cmd if cc_cmd else "riscv32-esp-elf-gcc"
        env.Replace(LINK=gcc_linker)

        # Remove any -fno-lto from LINKFLAGS (pioarduino-build.py may set it)
        linkflags = env.get("LINKFLAGS", [])
        filtered = [f for f in linkflags if str(f) != "-fno-lto"]
        if len(filtered) != len(linkflags):
            env.Replace(LINKFLAGS=filtered)
            print("Removed -fno-lto from LINKFLAGS")

        # Ensure LTO flags are passed to linker via GCC
        env.Append(LINKFLAGS=["-flto=auto"])

        print(f"Linker set to: {gcc_linker}")

# Add Zigbee libraries and remove OpenThread when Zigbee is enabled
def configure_zigbee_libs(env):
    """Configure Zigbee libraries for End Device mode with WiFi coexistence."""
    
    # Check if this is a Zigbee build
    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_zigbee = any("OS_ENABLE_ZIGBEE" in flag for flag in build_flags)
    is_zigbee_zczr = any("ZIGBEE_MODE_ZCZR" in flag for flag in build_flags)
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)

    if not is_zigbee:
        return

    print("Zigbee build detected - configuring ESP-Zigbee libraries...")

    # Get the ESP32 framework library path
    lib_path_found = None
    try:
        pio_platform = env.PioPlatform()
        framework_dir = pio_platform.get_package_dir("framework-arduinoespressif32-libs")
        if framework_dir:
            # Find the correct library path for ESP32-C5
            import os
            mcu = env.BoardConfig().get("build.mcu", "esp32c5")
            # Try common library paths in order of preference
            lib_paths = [
                os.path.join(framework_dir, mcu, "lib"),  # Primary location
                os.path.join(framework_dir, mcu, "qio_qspi", "lib"),  # Official release layout
                os.path.join(framework_dir, mcu, "dio_qspi", "lib"),
                os.path.join(framework_dir, mcu, "dio", "lib"),
            ]
            for lib_path in lib_paths:
                if os.path.isdir(lib_path):
                    lib_path_found = lib_path
                    # Check if Zigbee libs exist
                    zigbee_lib = os.path.join(lib_path, "libespressif__esp-zigbee-lib.a")
                    if os.path.exists(zigbee_lib):
                        print(f"Found Zigbee libraries in: {lib_path}")
                        env.Append(LIBPATH=[lib_path])
                        break
                    else:
                        print(f"Path exists but missing Zigbee lib: {lib_path}")
    except Exception as e:
        print(f"Warning: Could not determine framework lib path: {e}")

    # Add the Zigbee stack libraries in correct order (dependencies last)
    # Library names without 'lib' prefix and without '.a' suffix
    # 
    # IMPORTANT: The ESP-Zigbee SDK has these layers:
    # 1. esp_zb_api.ed/zczr - High-level ESP-IDF Zigbee API (esp_zb_* functions)
    # 2. zboss_stack.ed/zczr - ZBOSS protocol stack (zb_* functions)
    # 3. zboss_port.native - Platform abstraction layer
    # 4. ieee802154 - IEEE 802.15.4 MAC layer
    # 5. espressif__esp-zigbee-lib - Additional helper functions (small stub)
    
    zigbee_libs = [
        "esp_zb_api.ed",               # ESP-IDF Zigbee API wrapper (esp_zb_* functions)
        "zboss_stack.ed",              # ZBOSS End Device Stack (WiFi compatible!)
        "zboss_port.native",           # ZBOSS Port Layer (native, not remote)
        "ieee802154",                  # IEEE 802.15.4 MAC layer
        "esp_hal_ieee802154",          # IEEE 802.15.4 HAL peripheral defs (IDF 5.5.2+)
        "espressif__esp-zigbee-lib",   # Zigbee helper library
    ]
    
    # Use zczr stack if ZCZR mode is enabled (WARNING: breaks WiFi!)
    if is_zigbee_zczr:
        zigbee_libs[0] = "esp_zb_api.zczr"
        zigbee_libs[1] = "zboss_stack.zczr"
        print("Using ZBOSS Coordinator/Router stack")
    else:
        print("Using ZBOSS End Device stack (WiFi coexistence compatible)")
    
    # Add libraries
    for lib in zigbee_libs:
        env.Append(LIBS=[lib])
    print(f"Added Zigbee libs: {', '.join(zigbee_libs)}")

    # NOTE: With the unified build (runtime-selectable IEEE 802.15.4 mode),
    # both Matter (OpenThread) and Zigbee (ZBOSS) libraries must be linked.
    # The radio mode is selected at runtime via ieee802154.json config —
    # only one stack will actually be initialized at boot.
    # The --allow-multiple-definition flag handles any rare overlapping symbols.
    if is_matter:
        print("Unified Zigbee+Matter build: keeping OpenThread libs for Matter support")
    else:
        # Pure Zigbee build (no Matter) — OpenThread is not needed
        print("Removing OpenThread library to prevent conflicts with Zigbee...")
        libs = env.get("LIBS", [])
        filtered_libs = [lib for lib in libs if "openthread" not in str(lib).lower()]
        env.Replace(LIBS=filtered_libs)
    
    # Add explicit linker flag to relax duplicate symbols in mixed stacks
    env.Append(LINKFLAGS=[
        "-Wl,--allow-multiple-definition",  # Allow Zigbee to override OpenThread symbols
    ])
    
    print("Zigbee library configuration complete")


# Legacy alias for compatibility
def remove_openthread_lib(env):
    """Legacy function - now handled by configure_zigbee_libs."""
    pass


def fix_zigbee_lib_names(env):
    """Normalize Zigbee library names for GNU ld.

    Some toolchains/framework combinations may accidentally push full library
    names with a leading 'lib' into the LIBS list (e.g. 'libzboss_port.remote').
    SCons turns that into '-l<name>' which becomes '-llibzboss_port.remote'
    and fails to resolve. The correct entry should be 'zboss_port.remote'.
    """

    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_zigbee = any("ZIGBEE_MODE_ZCZR" in flag for flag in build_flags)
    if not is_zigbee:
        return

    libs = env.get("LIBS", [])
    # LIBS can be empty early; still normalize LINKFLAGS.

    def _fix_one(value):
        value_str = str(value)
        if value_str == "-libzboss_port.remote":
            return "-lzboss_port.remote", True
        if value_str == "libzboss_port.remote":
            return "zboss_port.remote", True
        if value_str == "ibzboss_port.remote":
            return "zboss_port.remote", True
        return value, False

    fixed_libs = []
    libs_changed = False
    for lib in libs:
        new_lib, did_change = _fix_one(lib)
        fixed_libs.append(new_lib)
        libs_changed |= did_change

    if libs_changed:
        env.Replace(LIBS=fixed_libs)
        print("Normalized Zigbee lib name for zboss_port.remote")

    linkflags = env.get("LINKFLAGS", [])
    if linkflags:
        fixed_flags = []
        flags_changed = False
        for flag in linkflags:
            new_flag, did_change = _fix_one(flag)
            fixed_flags.append(new_flag)
            flags_changed |= did_change
        if flags_changed:
            env.Replace(LINKFLAGS=fixed_flags)
            print("Normalized Zigbee linker flag for zboss_port.remote")


def ensure_schedule_daylight_lib(env):
    """Ensure esp_daylight is linked after esp_schedule.

    The rebuilt ESP32-C5 framework contains esp_schedule built against the
    esp_daylight component. PlatformIO's ComponentManager can rewrite the
    package build file during a build, so fix the effective SCons LIBS list
    right before linking as well.
    """

    libs = list(env.get("LIBS", []))
    if not libs:
        return

    def lib_name(value):
        value_str = str(value)
        if value_str.startswith("-l"):
            value_str = value_str[2:]
        base = os.path.basename(value_str)
        if base.startswith("lib") and base.endswith(".a"):
            base = base[3:-2]
        return base

    fixed_libs = []
    changed = False
    for index, lib in enumerate(libs):
        fixed_libs.append(lib)
        if lib_name(lib) != "espressif__esp_schedule":
            continue

        next_lib = libs[index + 1] if index + 1 < len(libs) else None
        if next_lib is not None and lib_name(next_lib) == "espressif__esp_daylight":
            continue

        fixed_libs.append("espressif__esp_daylight")
        changed = True

    if changed:
        env.Replace(LIBS=fixed_libs)
        print("Added esp_daylight after esp_schedule for framework schedule support")


def restore_c5_matter_archive(env):
    """Restore the OpenThread-free ESP32-C5 Matter archive after package rewrites."""

    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)
    is_esp32c5 = any("ESP32C5" in flag for flag in build_flags)
    if not (is_matter and is_esp32c5):
        return

    try:
        framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    except Exception:
        framework_dir = None
    if not framework_dir:
        return

    target_archive = os.path.join(framework_dir, "esp32c5", "lib", "libespressif__esp_matter.a")
    clean_candidates = []
    c5_root = os.path.join(framework_dir, "esp32c5")
    if os.path.isdir(c5_root):
        for entry in sorted(os.listdir(c5_root), reverse=True):
            if not entry.startswith("lib.bootloop"):
                continue
            candidate = os.path.join(c5_root, entry, "libespressif__esp_matter.a")
            if os.path.exists(candidate):
                clean_candidates.append(candidate)

    def has_openthread(archive):
        if not archive or not os.path.exists(archive):
            return False
        with open(archive, "rb") as archive_file:
            content = archive_file.read()
        return any(symbol in content for symbol in (
            b"esp_openthread_init",
            b"otInstanceInitSingle",
            b"OpenthreadLauncher",
        ))

    if not has_openthread(target_archive):
        return

    clean_archive = next((candidate for candidate in clean_candidates
                          if os.path.exists(candidate) and not has_openthread(candidate)), None)
    if not clean_archive:
        print("Warning: ESP32-C5 Matter archive still contains OpenThread symbols; no clean backup found")
        return

    shutil.copyfile(clean_archive, target_archive)
    print(f"Restored OpenThread-free ESP32-C5 Matter archive from: {clean_archive}")


def patch_c5_matter_sdkconfig_headers(env):
    """Patch ESP32-C5 Matter sdkconfig.h macros before Arduino Matter compiles."""

    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)
    is_esp32c5 = any("ESP32C5" in flag for flag in build_flags)
    if not (is_matter and is_esp32c5):
        return

    try:
        framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
    except Exception:
        framework_dir = None
    if not framework_dir:
        return

    disabled_keys = [
        "CONFIG_OPENTHREAD_ENABLED",
        "CONFIG_ENABLE_MATTER_OVER_THREAD",
        "CONFIG_ESP_MATTER_ENABLE_OPENTHREAD",
        "CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER",
        "CONFIG_THREAD_NETWORK_ENDPOINT_ID",
        "CONFIG_WIFI_NETWORK_COMMISSIONING_DRIVER",
    ]
    forced_values = {
        "CONFIG_CHIP_TASK_STACK_SIZE": "12288",
        "CONFIG_ENABLE_ETHERNET_TELEMETRY": "1",
        "CONFIG_ETHERNET_NETWORK_COMMISSIONING_DRIVER": "1",
        "CONFIG_ETHERNET_NETWORK_ENDPOINT_ID": "0",
    }
    patched = 0
    for variant in ("qio_qspi", "dio_qspi", "qio_opi", "dio_opi"):
        sdkconfig_header = os.path.join(framework_dir, "esp32c5", variant, "include", "sdkconfig.h")
        if not os.path.exists(sdkconfig_header):
            continue
        with open(sdkconfig_header, "r") as config_file:
            content = config_file.read()
        new_content = content
        for key in disabled_keys:
            new_content = re.sub(
                rf"^#define\s+{key}\s+\S+$",
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
        with open(sdkconfig_header, "w") as config_file:
            config_file.write(new_content)
        patched += 1

    if patched:
        print(f"Patched ESP32-C5 Matter sdkconfig.h macros in {patched} variant(s)")


def disable_c5_matter_over_thread_config(env):
    """Compatibility wrapper for older build hooks."""

    patch_c5_matter_sdkconfig_headers(env)


def ensure_c5_matter_radio_libs(env):
    """Ensure ESP32-C5 Matter builds link late radio/Matter archives.

    The local C5 framework can still pull libieee802154.a for Matter even when
    OpenThread is disabled. That archive needs esp_hal_ieee802154 for the
    ieee802154_periph definition. With RISC-V LTO, endpoint references may also
    appear after the first esp_matter archive scan, so repeat esp_matter late.
    """

    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)
    is_esp32c5 = any("ESP32C5" in flag for flag in build_flags)
    if not (is_matter and is_esp32c5):
        return

    libs = list(env.get("LIBS", []))

    def lib_name(value):
        value_str = str(value)
        if value_str.startswith("-l"):
            value_str = value_str[2:]
        base = os.path.basename(value_str)
        if base.startswith("lib") and base.endswith(".a"):
            base = base[3:-2]
        return base

    names = [lib_name(lib) for lib in libs]
    changed = False

    if "esp_hal_ieee802154" not in names:
        libs.append("esp_hal_ieee802154")
        changed = True

    if "espressif__esp_matter" in names:
        libs.append("espressif__esp_matter")
        changed = True

        dnssd_compat = os.path.join(
            env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"),
            "esp32c5", "lib", "libmatter_dnssd_compat.a")
        if os.path.exists(dnssd_compat):
            libs.append(env.File(dnssd_compat))
            changed = True

    if changed:
        env.Replace(LIBS=libs)
        print("Added late ESP32-C5 Matter radio/link libraries")


def remove_c5_openthread_libs(env):
    """Remove OpenThread libraries for ESP32-C5 Matter builds.

    The local OpenSprinkler ESP32-C5 framework-libs package is rebuilt for
    Wi-Fi/Ethernet Matter without OpenThread. Newer pioarduino platform files
    can still inject -lopenthread late in LIBS/LINKFLAGS, so strip it from the
    effective SCons environment right before linking.
    """

    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)
    is_esp32c5 = any("ESP32C5" in flag for flag in build_flags)
    if not (is_matter and is_esp32c5):
        return

    def keep(value):
        return "openthread" not in str(value).lower()

    libs = list(env.get("LIBS", []))
    filtered_libs = [lib for lib in libs if keep(lib)]
    if len(filtered_libs) != len(libs):
        env.Replace(LIBS=filtered_libs)
        print("Removed OpenThread libs for ESP32-C5 WiFi/Ethernet Matter build")

    linkflags = list(env.get("LINKFLAGS", []))
    filtered_flags = [flag for flag in linkflags if keep(flag)]
    if len(filtered_flags) != len(linkflags):
        env.Replace(LINKFLAGS=filtered_flags)
        print("Removed OpenThread linker flags for ESP32-C5 WiFi/Ethernet Matter build")


def _register_pre_link_fixes(outer_env):
    """Ensure late-added libs/flags get normalized right before link."""

    def _pre_link_action(target, source, env=None, **kwargs):
        # SCons calls actions as execfunction(target=..., source=..., env=...)
        # so accept the keyword name exactly.
        active_env = env or outer_env
        ensure_schedule_daylight_lib(active_env)
        patch_c5_matter_sdkconfig_headers(active_env)
        restore_c5_matter_archive(active_env)
        ensure_c5_matter_radio_libs(active_env)
        remove_c5_openthread_libs(active_env)
        fix_zigbee_lib_names(active_env)

    # Link target for PlatformIO firmware image.
    outer_env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", _pre_link_action)

# Execute the fixes before building
ensure_riscv_toolchain_on_path(env)
fix_linker_command(env)
configure_zigbee_libs(env)
ensure_schedule_daylight_lib(env)
patch_c5_matter_sdkconfig_headers(env)
restore_c5_matter_archive(env)
ensure_c5_matter_radio_libs(env)
remove_c5_openthread_libs(env)
fix_zigbee_lib_names(env)
_register_pre_link_fixes(env)
