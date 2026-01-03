"""
PlatformIO build script to fix RISC-V LTO linker issues and remove OpenThread library for Zigbee builds.
Forces the use of GCC wrapper (riscv32-esp-elf-gcc) instead of ld for linking.
This ensures LTO plugins are loaded correctly.
"""

Import("env")

# Replace linker command to use GCC wrapper instead of ld
def fix_linker_command(env):
    # Get the current toolchain prefix
    platform = env.PioPlatform()
    toolchain_prefix = env.get("CC", "").replace("-gcc", "")
    
    if "riscv32" in toolchain_prefix or "esp32c5" in env.get("BOARD", "").lower():
        print("Fixing RISC-V linker to use GCC wrapper for LTO support...")
        
        # Use GCC as linker instead of ld directly
        env.Replace(LINK=toolchain_prefix + "-gcc")
        
        # Ensure LTO flags are passed to linker via GCC
        env.Append(LINKFLAGS=["-flto", "-Oz"])
        
        print(f"Linker set to: {env['LINK']}")

# Remove OpenThread library from link command when Zigbee is enabled
def remove_openthread_lib(env):
    # Check if this is a Zigbee build (not Matter)
    build_flags = env.Flatten(env.get("BUILD_FLAGS", []))
    is_zigbee = any("ZIGBEE_MODE_ZCZR" in flag for flag in build_flags)
    is_matter = any("ENABLE_MATTER" in flag for flag in build_flags)
    
    if is_zigbee and not is_matter:
        print("Removing OpenThread library to prevent conflicts with Zigbee...")
        
        # Filter out OpenThread library from LIBS
        libs = env.get("LIBS", [])
        filtered_libs = [lib for lib in libs if "openthread" not in str(lib).lower()]
        env.Replace(LIBS=filtered_libs)
        
        # Also filter from LIBPATH
        libpath = env.get("LIBPATH", [])
        # Don't remove the path, just the library reference
        
        # Add explicit linker flag to ignore OpenThread symbols
        env.Append(LINKFLAGS=[
            "-Wl,--allow-multiple-definition",  # Allow Zigbee to override OpenThread symbols
        ])
        
        print("OpenThread library handling configured for Zigbee build")

# Execute the fixes before building
fix_linker_command(env)
remove_openthread_lib(env)
