import multiprocessing
import os
import re

Import("env")

# =============================================================================
# Zigbee task-stack patch — prefer internal RAM, PSRAM fallback
# =============================================================================
# The Arduino Zigbee library creates the "Zigbee_main" FreeRTOS task with
# xTaskCreate(), which allocates the 8 KB stack from internal SRAM.
#
# On ESP32-C5 the PSRAM memory-barrier init can fail at boot
#   ("Failed to allocate dummy cacheline for PSRAM memory barrier")
# which makes PSRAM-backed task stacks unreliable for the high-priority
# Zigbee radio task — causing CPU_LOCKUP during esp_zb_start().
#
# This patch uses xTaskCreateWithCaps() to try INTERNAL RAM first (safe for
# radio/DMA adjacent code), then PSRAM as a fallback if internal RAM is too
# fragmented, and finally plain xTaskCreate() as a last resort.
# =============================================================================
def patch_zigbee_psram_task(source, target, env):
    """Patch ZigbeeCore.cpp to allocate Zigbee_main task stack from internal RAM (PSRAM fallback)."""
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if not framework_dir:
        return
    zb_core = os.path.join(framework_dir, "libraries", "Zigbee", "src", "ZigbeeCore.cpp")
    if not os.path.exists(zb_core):
        return

    with open(zb_core, "r") as f:
        content = f.read()

    # Already patched with new version?
    if "MALLOC_CAP_INTERNAL" in content:
        return

    # Remove old PSRAM-first patch if present (re-patch with internal-first)
    if "xTaskCreateWithCaps" in content and "MALLOC_CAP_INTERNAL" not in content:
        # Old patch present — need to replace the entire block
        # Find and replace the old patched block
        old_block_start = content.find("  // Place Zigbee task stack in PSRAM when available (patched by fix_build.py)")
        old_block_end = content.find("#endif", old_block_start)
        if old_block_start >= 0 and old_block_end >= 0:
            old_block_end = content.find("\n", old_block_end) + 1
            content = content[:old_block_start] + "__ZIGBEE_TASK_PLACEHOLDER__" + content[old_block_end:]
        else:
            print("[FIX_BUILD] WARNING: Could not locate old PSRAM patch block for replacement")
            return
    elif "xTaskCreateWithCaps" not in content:
        # Unpatched library — replace original xTaskCreate line
        old = '  xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);'
        if old in content:
            content = content.replace(old, "__ZIGBEE_TASK_PLACEHOLDER__", 1)
        else:
            print("[FIX_BUILD] WARNING: Could not locate xTaskCreate in ZigbeeCore.cpp")
            return

    new = (
        "  // Zigbee task stack: prefer internal RAM for reliability (patched by fix_build.py)\n"
        "  // PSRAM task stacks can cause CPU_LOCKUP if PSRAM memory barrier init failed at boot.\n"
        "#if defined(CONFIG_SPIRAM) || defined(BOARD_HAS_PSRAM)\n"
        "  {\n"
        "    TaskHandle_t zb_task_handle = NULL;\n"
        "    // Try internal RAM first (reliable for radio/DMA-adjacent code)\n"
        '    BaseType_t ret = xTaskCreateWithCaps(esp_zb_task, "Zigbee_main", 8192, NULL, 5,\n'
        "                                         &zb_task_handle, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);\n"
        "    if (ret != pdPASS) {\n"
        '      log_w("Zigbee_main: internal RAM alloc failed, trying PSRAM");\n'
        '      ret = xTaskCreateWithCaps(esp_zb_task, "Zigbee_main", 8192, NULL, 5,\n'
        "                               &zb_task_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n"
        "    }\n"
        "    if (ret != pdPASS) {\n"
        '      log_e("xTaskCreateWithCaps failed, falling back to xTaskCreate");\n'
        '      xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);\n'
        "    }\n"
        "  }\n"
        "#else\n"
        '  xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);\n'
        "#endif"
    )

    content = content.replace("__ZIGBEE_TASK_PLACEHOLDER__", new, 1)
    with open(zb_core, "w") as f:
        f.write(content)
    print("[FIX_BUILD] Patched ZigbeeCore.cpp: Zigbee_main task stack → internal RAM (PSRAM fallback)")

env.AddPreAction("buildprog", patch_zigbee_psram_task)
patch_zigbee_psram_task(None, None, env)


def patch_zigbee_application_controlled_steering(source, target, env):
    """Patch ZigbeeCore.cpp so End Device network steering is started only by the app (/zj)."""
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if not framework_dir:
        return
    zb_core = os.path.join(framework_dir, "libraries", "Zigbee", "src", "ZigbeeCore.cpp")
    if not os.path.exists(zb_core):
        return

    with open(zb_core, "r") as f:
        content = f.read()

    changed = False

    old_auto = (
        "          } else {\n"
        "            log_i(\"Start network steering\");\n"
        "            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);\n"
        "            Zigbee._started = true;\n"
        "            xSemaphoreGive(Zigbee.lock);\n"
        "          }"
    )
    new_auto = (
        "          } else {\n"
        "            log_i(\"Factory-new end device ready; network steering is application-controlled\");\n"
        "            Zigbee._started = true;\n"
        "            xSemaphoreGive(Zigbee.lock);\n"
        "          }"
    )
    if old_auto in content:
        content = content.replace(old_auto, new_auto, 1)
        changed = True

    old_retry = (
        "        } else {\n"
        "          log_i(\"Network steering was not successful (status: %s)\", esp_err_to_name(err_status));\n"
        "          esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);\n"
        "        }"
    )
    new_retry = (
        "        } else {\n"
        "          log_i(\"Network steering was not successful (status: %s); waiting for application retry\", esp_err_to_name(err_status));\n"
        "        }"
    )
    if old_retry in content:
        content = content.replace(old_retry, new_retry, 1)
        changed = True

    if changed:
        with open(zb_core, "w") as f:
            f.write(content)
        print("[FIX_BUILD] Patched ZigbeeCore.cpp: End Device steering is application-controlled (no auto/retry loop)")


env.AddPreAction("buildprog", patch_zigbee_application_controlled_steering)
patch_zigbee_application_controlled_steering(None, None, env)

# --- Parallel build: use all available CPU cores ---
num_cores = multiprocessing.cpu_count()
env.SetOption("num_jobs", num_cores)
print(f"ESP32-C5 Fix: Parallel build enabled ({num_cores} jobs)")

def remove_mlongcalls_flag(node):
    """Remove unsupported flags and enforce LTO for ESP32-C5 builds."""
    for flag_var in ["ASFLAGS", "CCFLAGS", "CFLAGS", "CXXFLAGS", "LINKFLAGS"]:
        flags = node.get(flag_var, [])
        if isinstance(flags, list):
            while "-mlongcalls" in flags:
                flags.remove("-mlongcalls")
                print(f"Removed -mlongcalls from {flag_var}")

            # Some framework scripts inject -fno-lto for selected targets.
            # Remove it and force -flto so code-size optimization remains active.
            while "-fno-lto" in flags:
                flags.remove("-fno-lto")
                print(f"Removed -fno-lto from {flag_var}")

            if flag_var in ["CCFLAGS", "CFLAGS", "CXXFLAGS", "LINKFLAGS"] and "-flto=auto" not in flags:
                flags.append("-flto=auto")
                print(f"Added -flto=auto to {flag_var}")
    return node

# Remove -mlongcalls flag from main environment (not supported by RISC-V ESP32-C5)
remove_mlongcalls_flag(env)

# Hook to remove -mlongcalls from ALL build environments including libraries
env.AddBuildMiddleware(remove_mlongcalls_flag, "*")

print("ESP32-C5 Fix: Configured middleware to remove -mlongcalls flag from all builds")
