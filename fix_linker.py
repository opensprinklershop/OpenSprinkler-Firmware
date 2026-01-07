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

    # Also set absolute compiler paths so early .ino preprocessing doesn't rely on PATH.
    exe_suffix = ".exe" if os.name == "nt" else ""
    gpp = "riscv32-esp-elf-g++" + exe_suffix
    gcc = "riscv32-esp-elf-gcc" + exe_suffix
    gpp_path = os.path.join(bin_dir, gpp)
    gcc_path = os.path.join(bin_dir, gcc)

    print(f"Using RISC-V toolchain bin: {bin_dir}")
    if os.path.isfile(gpp_path):
        env.Replace(CXX=gpp_path)
    else:
        print(f"Warning: toolchain bin found but {gpp} is missing: {gpp_path}")
    if os.path.isfile(gcc_path):
        env.Replace(CC=gcc_path)

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

# Remove OpenThread library from link command when Zigbee is enabled
def remove_openthread_lib(env):
    # Check if this is a Zigbee build
    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_zigbee = any("ZIGBEE_MODE_ZCZR" in flag for flag in build_flags)
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)

    # NOTE: Zigbee (ZBOSS) and OpenThread both provide IEEE802154 glue on ESP32-C5.
    # If both are linked, the build fails with multiple-definition errors.
    # This project uses Zigbee, and Matter is expected to run over WiFi (not Thread),
    # so we drop OpenThread when Zigbee is enabled.
    if is_zigbee:
        if is_matter:
            print("Zigbee+Matter build detected: removing OpenThread to avoid IEEE802154 symbol conflicts...")
        else:
            print("Removing OpenThread library to prevent conflicts with Zigbee...")
        
        # Filter out OpenThread library from LIBS
        libs = env.get("LIBS", [])
        filtered_libs = [lib for lib in libs if "openthread" not in str(lib).lower()]
        env.Replace(LIBS=filtered_libs)
        
        # Also filter from LIBPATH
        libpath = env.get("LIBPATH", [])
        # Don't remove the path, just the library reference
        
        # Add explicit linker flag to relax duplicate symbols in mixed stacks
        env.Append(LINKFLAGS=[
            "-Wl,--allow-multiple-definition",  # Allow Zigbee to override OpenThread symbols
        ])
        
        print("OpenThread library handling configured for Zigbee build")


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
remove_openthread_lib(env)
fix_zigbee_lib_names(env)
_register_pre_link_fixes(env)
