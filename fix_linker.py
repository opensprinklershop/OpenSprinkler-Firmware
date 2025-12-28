"""
PlatformIO build script to fix RISC-V LTO linker issues.
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

# Execute the fix before building
fix_linker_command(env)
