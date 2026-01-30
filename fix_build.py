Import("env")

# Remove -mlongcalls flag which is not supported by RISC-V (ESP32-C5)
if "-mlongcalls" in env.get("CCFLAGS", []):
    env["CCFLAGS"].remove("-mlongcalls")

if "-mlongcalls" in env.get("ASFLAGS", []):
    env["ASFLAGS"].remove("-mlongcalls")

# Also checking CFLAGS and CXXFLAGS just in case
if "-mlongcalls" in env.get("CFLAGS", []):
    env["CFLAGS"].remove("-mlongcalls")

if "-mlongcalls" in env.get("CXXFLAGS", []):
    env["CXXFLAGS"].remove("-mlongcalls")
