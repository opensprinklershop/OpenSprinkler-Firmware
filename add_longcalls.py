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


# Dynamic patch for framework library EthernetCompat.h to make DHCP non-blocking
def patch_ethernet_compat():
    import os
    try:
        framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")
        if not framework_dir:
            return
        eth_compat = os.path.join(framework_dir, "libraries", "lwIP_Ethernet", "src", "EthernetCompat.h")
        if not os.path.exists(eth_compat):
            return

        with open(eth_compat, "r", encoding="utf-8") as f:
            content = f.read()

        old_block = (
            "            if (!local_ip.isSet())\n"
            "            {\n"
            "                // Arduino API waits for DHCP answer\n"
            "                while (!LwipIntfDev<RawDev>::connected())\n"
            "                {\n"
            "                    delay(100);\n"
            "                }\n"
            "            }"
        )
        
        new_block = (
            "            // Non-blocking: DHCP is handled asynchronously in the background.\n"
            "            // We do not wait or block here to prevent freezing the application."
        )

        if old_block in content:
            content = content.replace(old_block, new_block, 1)
            with open(eth_compat, "w", encoding="utf-8") as f:
                f.write(content)
            print("[PATCH] Patched EthernetCompat.h: DHCP waits disabled (non-blocking begin)")
    except Exception as e:
        print(f"[PATCH] Error patching EthernetCompat.h: {e}")

patch_ethernet_compat()

