"""PlatformIO build script helpers.

- Fix RISC-V LTO linker issues by forcing GCC wrapper (riscv32-esp-elf-gcc).
- Remove OpenThread library for Zigbee builds to avoid IEEE802154 symbol conflicts.
- Work around cases where the RISC-V toolchain bin dir is not on PATH during
  the Arduino .ino preprocessing step (ConvertInoToCpp).
"""

import os

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

        # Ensure LTO flags are passed to linker via GCC
        env.Append(LINKFLAGS=["-flto"])

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
        "espressif__esp-zigbee-lib",   # Zigbee helper library
    ]
    
    # Use zczr stack if ZCZR mode is enabled (WARNING: breaks WiFi!)
    if is_zigbee_zczr:
        zigbee_libs[0] = "esp_zb_api.zczr"
        zigbee_libs[1] = "zboss_stack.zczr"
        print("WARNING: Using ZBOSS Coordinator/Router stack - WiFi will NOT work!")
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


def _register_pre_link_fixes(outer_env):
    """Ensure late-added libs/flags get normalized right before link."""

    def _pre_link_action(target, source, env=None, **kwargs):
        # SCons calls actions as execfunction(target=..., source=..., env=...)
        # so accept the keyword name exactly.
        fix_zigbee_lib_names(env or outer_env)

    # Link target for PlatformIO firmware image.
    outer_env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", _pre_link_action)

# Execute the fixes before building
ensure_riscv_toolchain_on_path(env)
fix_linker_command(env)
configure_zigbee_libs(env)
fix_zigbee_lib_names(env)
_register_pre_link_fixes(env)
