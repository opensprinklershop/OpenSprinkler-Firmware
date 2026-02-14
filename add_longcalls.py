Import("env", "projenv")

# Force -mlongcalls for ALL compilations including library builds
# On ESP8266 (xtensa-lx106), the linker needs longcalls when text 
# sections are spread across large address ranges.

# Apply to project environment (source files)
projenv.Append(CCFLAGS=["-mlongcalls"])
projenv.Append(CXXFLAGS=["-mlongcalls"])

# Apply to the global environment which library builders inherit
env.Append(CCFLAGS=["-mlongcalls"])
env.Append(CXXFLAGS=["-mlongcalls"])

# Hook into each library's build environment
for lb in env.GetLibBuilders():
    lb.env.Append(CCFLAGS=["-mlongcalls"])
    lb.env.Append(CXXFLAGS=["-mlongcalls"])
