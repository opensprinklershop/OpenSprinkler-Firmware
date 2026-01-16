"""
LTO (Link Time Optimization) Script für PlatformIO
Aktiviert LTO für kleinere und schnellere Firmware
"""

Import("env")

# LTO Compiler-Flags hinzufügen
lto_flags = [
    "-flto",
    "-fuse-linker-plugin",
    "-fno-fat-lto-objects",
]

# Linker-Flags für LTO
lto_link_flags = [
    "-flto",
    "-fuse-linker-plugin",
    "-Wl,--gc-sections",
    "-Wl,-O3",
]

# Prüfen ob ESP8266 oder ESP32
if env.get("PIOPLATFORM") == "espressif8266":
    # ESP8266-spezifische LTO-Konfiguration
    env.Append(
        CCFLAGS=lto_flags,
        LINKFLAGS=lto_link_flags,
    )
    
    # AR (Archiver) muss auch LTO-fähig sein
    env.Replace(
        AR="xtensa-lx106-elf-gcc-ar",
        RANLIB="xtensa-lx106-elf-gcc-ranlib",
    )
    
elif env.get("PIOPLATFORM") == "espressif32":
    # ESP32-spezifische LTO-Konfiguration
    env.Append(
        CCFLAGS=lto_flags,
        LINKFLAGS=lto_link_flags,
    )
    
    env.Replace(
        AR="xtensa-esp32-elf-gcc-ar",
        RANLIB="xtensa-esp32-elf-gcc-ranlib",
    )

elif env.get("PIOPLATFORM") == "atmelavr":
    # AVR-spezifische LTO-Konfiguration
    avr_lto_flags = [
        "-flto",
        "-fno-fat-lto-objects",
    ]
    
    avr_link_flags = [
        "-flto",
        "-Wl,--gc-sections",
    ]
    
    env.Append(
        CCFLAGS=avr_lto_flags,
        LINKFLAGS=avr_link_flags,
    )
    
    env.Replace(
        AR="avr-gcc-ar",
        RANLIB="avr-gcc-ranlib",
    )

elif env.get("PIOPLATFORM") == "native":
    # Native/Linux LTO-Konfiguration
    env.Append(
        CCFLAGS=lto_flags,
        LINKFLAGS=lto_link_flags,
    )

# Debug-Ausgabe der Build-Flags
print("=" * 60)
print("LTO aktiviert für Plattform:", env.get("PIOPLATFORM"))
print("CCFLAGS:", env.get("CCFLAGS"))
print("LINKFLAGS:", env.get("LINKFLAGS"))
print("AR:", env.get("AR"))
print("=" * 60)
