Import("env")

def remove_mlongcalls_flag(node):
    """Remove -mlongcalls from all flag variables in the environment"""
    for flag_var in ["ASFLAGS", "CCFLAGS", "CFLAGS", "CXXFLAGS", "LINKFLAGS"]:
        flags = node.get(flag_var, [])
        if isinstance(flags, list):
            while "-mlongcalls" in flags:
                flags.remove("-mlongcalls")
                print(f"Removed -mlongcalls from {flag_var}")
    return node

# Remove -mlongcalls flag from main environment (not supported by RISC-V ESP32-C5)
remove_mlongcalls_flag(env)

# Hook to remove -mlongcalls from ALL build environments including libraries
env.AddBuildMiddleware(remove_mlongcalls_flag, "*")

print("ESP32-C5 Fix: Configured middleware to remove -mlongcalls flag from all builds")
