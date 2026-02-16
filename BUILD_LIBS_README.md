# ESP32-C5 Self-Compiled Arduino Framework Libraries

Automatisiertes Build- und Deploy-System für selbst-kompilierte ESP32-C5 Arduino Framework
Libraries mit optimiertem sdkconfig für OpenSprinkler (WiFi + BLE + Matter + Zigbee + PSRAM).

## Warum selbst kompilieren?

Die offiziellen Arduino-ESP32 Precompiled-Libraries verwenden Standard-sdkconfig-Werte,
die für den OpenSprinkler ESP32-C5 (Matter + BLE + Zigbee + WiFi + 8MB PSRAM) nicht
geeignet sind:

- **PSRAM-Routing fehlt:** BLE, mbedTLS, Matter, mDNS, NVS nutzen internes SRAM statt PSRAM
- **WiFi-Buffer zu groß:** Verbrauchen unnötig viel internes SRAM
- **Zigbee/OpenThread Defaults:** Zu große Message-Buffer und Task-Stacks
- **Kein POOL_USE_HEAP:** Matter-Objekt-Pools blockieren BSS statt PSRAM

Durch selbst-kompilierte Libs mit `defconfig.esp32c5` werden diese Einstellungen
**zur Compile-Zeit** in die Framework-Libraries eingebrannt.

## Architektur

```
esp32-arduino-lib-builder/          <- Build-System (ESP-IDF 5.5.2)
├── configs/defconfig.esp32c5       <- Unsere optimierte Konfiguration (290 Zeilen)
├── esp-idf/                        <- ESP-IDF Sourcen
├── build.sh                        <- Haupt-Build-Script
└── out/tools/esp32-arduino-libs/   <- Build-Output
    └── esp32c5/

framework-arduinoespressif32-libs/  <- Deploy-Ziel (PlatformIO nutzt dieses Verzeichnis)
├── package.json                    <- PlatformIO Paket-Metadaten
├── versions.txt                    <- Build-Versionsinformationen
└── esp32c5/
    ├── bin/                        <- Bootloader ELFs
    ├── lib/                        <- 147 kompilierte IDF/Arduino Libraries
    ├── include/                    <- Header-Dateien
    ├── ld/                         <- Linker-Scripts (mit Fixes)
    ├── sdkconfig                   <- Kompilierte SDK-Konfiguration
    ├── flags/                      <- Compiler-Flags
    └── pioarduino-build.py         <- PlatformIO Build-Integration

OpenSprinkler-Firmware/
├── platformio.ini                  <- Referenziert lokale Libs via file://
├── build_and_deploy_libs.sh        <- Build + Deploy Automation
└── fix_build.py                    <- Pre-Build Patches (Zigbee PSRAM Task)
```

## platformio.ini Konfiguration

Die `platformio.ini` referenziert die selbst-kompilierten Libs über einen lokalen Pfad:

```ini
platform_packages =
    framework-arduinoespressif32@https://github.com/espressif/arduino-esp32/releases/download/3.3.7/esp32-core-3.3.7.tar.xz
    ; Self-compiled libs with optimized sdkconfig for ESP32-C5
    framework-arduinoespressif32-libs@file:///data/Workspace/framework-arduinoespressif32-libs
```

**Wichtig:** Die offizielle GitHub-URL (`esp32-core-3.3.7-libs.tar.xz`) wurde durch
den lokalen `file://`-Pfad ersetzt. Wird die Zeile versehentlich auf die GitHub-URL
zurückgesetzt, werden die selbst-kompilierten Libs überschrieben!

## Schnellstart

### Erstmalige Einrichtung

```bash
# 1. IDF-Environment aktivieren
cd /data/Workspace/esp32-arduino-lib-builder
export IDF_PATH="$PWD/esp-idf"
source "$IDF_PATH/export.sh"

# 2. Libraries bauen (10-30 Min)
./build.sh -t esp32c5 -s

# 3. Deployen und Linker-Fixes anwenden
cd /data/Workspace/OpenSprinkler-Firmware
./build_and_deploy_libs.sh -s

# 4. PlatformIO Package neu installieren
cd /data/Workspace/OpenSprinkler-Firmware
rm -rf .pio/packages/framework-arduinoespressif32-libs
platformio pkg install -e esp32-c5

# 5. Firmware bauen
platformio run -e esp32-c5
```

### Nach defconfig-Aenderungen

```bash
# IDF-Environment muss aktiv sein!
cd /data/Workspace/esp32-arduino-lib-builder
export IDF_PATH="$PWD/esp-idf"
source "$IDF_PATH/export.sh"

# Neu bauen + deployen
cd /data/Workspace/OpenSprinkler-Firmware
./build_and_deploy_libs.sh        # Baut und deployt

# PlatformIO bekommt es ueber den file://-Pfad automatisch
rm -rf .pio/packages/framework-arduinoespressif32-libs
platformio pkg install -e esp32-c5
platformio run -e esp32-c5
```

### Nur Deployment (Libraries bereits gebaut)

```bash
./build_and_deploy_libs.sh -s     # Skip Build, nur Deploy
```

## Optionen

| Option | Beschreibung |
|--------|-------------|
| `-s, --skip-build` | Ueberspringe Kompilation (Libraries bereits vorhanden) |
| `-c, --copy-pio` | Kopiere auch zu `~/.platformio/packages/` (optional) |

## Angewendete Optimierungen (defconfig.esp32c5)

### PSRAM-Routing (wichtigste Optimierung)

| Subsystem | Setting | Effekt |
|-----------|---------|--------|
| SPIRAM Core | `SPIRAM_USE_MALLOC=y`, `ALWAYSINTERNAL=1024` | malloc() > 1KB -> PSRAM |
| BLE/NimBLE | `NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` | BLE Heap -> PSRAM |
| mbedTLS | `MBEDTLS_EXTERNAL_MEM_ALLOC=y` | TLS Buffers -> PSRAM |
| Matter/CHIP | `ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL=y` | Matter Allocs -> PSRAM |
| Matter Pools | `CHIP_SYSTEM_CONFIG_POOL_USE_HEAP=y` | Object Pools -> Heap/PSRAM |
| NVS | `NVS_ALLOCATE_CACHE_IN_SPIRAM=y` | NVS Cache -> PSRAM |
| mDNS | `MDNS_TASK_CREATE_FROM_SPIRAM=y` | mDNS Stack -> PSRAM |
| BLE Mesh | `BLE_MESH_MEM_ALLOC_MODE_EXTERNAL=y` | Mesh Allocs -> PSRAM |
| FreeRTOS | `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` | Task Stacks -> PSRAM moeglich |

### WiFi Buffer Reduktion

```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6     (Default: 10)
CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y         (Statt dynamisch - vermeidet PSRAM-Allocs)
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6     (Default: 16)
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=10   (Default: 32)
```

### Task Stack Reduktion

```
CONFIG_CHIP_TASK_STACK_SIZE=4096           (Default: 6144, spart 2KB)
CONFIG_OPENTHREAD_TASK_SIZE=3072           (Default: 4096, spart 1KB)
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096      (Default: 8192, spart 4KB)
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3072 (Default: 5120, spart 2KB)
```

### OpenThread Reduktion

```
CONFIG_OPENTHREAD_NUM_MESSAGE_BUFFERS=48   (Default: 128, spart ~10KB)
CONFIG_OPENTHREAD_MLE_MAX_CHILDREN=2       (Default: 10)
CONFIG_OPENTHREAD_TMF_ADDR_CACHE_ENTRIES=8 (Default: 32)
```

### TLS Optimierung

- TLS 1.3 mit HW-beschleunigten Ciphern (AES-GCM, AES-CCM)
- Deaktiviert: ChaCha20, Camellia (Software-only)
- Nur HW-ECC Curves: P256, P384, Curve25519
- Certificate Bundle: Common statt Full (spart ~50KB Flash)

## Linker-Fixes (ESP32-C5 Rev 1.0)

Das Deploy-Script wendet automatisch 3 Linker-Fixes an:

1. **WiFi BSS -> Internal SRAM:** PSRAM Memory Barrier Bug verhindert WiFi in PSRAM
2. **WiFi aus .ext_ram.bss entfernt:** Korrespondierend zu Fix 1
3. **GDMA -> IRAM:** DMA-Handler muessen in IRAM liegen

Diese Fixes **muessen nach jedem Library-Rebuild** erneut angewendet werden.
Das `build_and_deploy_libs.sh` Script macht das automatisch.

## Versionen (Stand 2026-02-15)

| Komponente | Version |
|-----------|---------|
| esp32-arduino-lib-builder | main / 36f7c5d |
| ESP-IDF | 5.5.2 (release/v5.5 / dac6094f34) |
| Arduino ESP32 | 3.3.7 |
| pioarduino Platform | 55.03.37 |
| esp-zboss-lib | 1.6.4 |
| esp-zigbee-lib | 1.6.8 |
| esp_matter | 1.4.1 |

## Build-Ergebnis (2026-02-15)

```
RAM:   [          ]   1.5% (used 128768 bytes from 8716288 bytes)
Flash: [====      ]  39.8% (used 3338517 bytes from 8388608 bytes)
147 Libraries kompiliert
3 Linker-Fixes angewendet
Build: SUCCESS in 93s
```

## Troubleshooting

### "idf.py: Command not found"

IDF-Environment nicht geladen. Vor dem Build:

```bash
cd /data/Workspace/esp32-arduino-lib-builder
export IDF_PATH="$PWD/esp-idf"
source "$IDF_PATH/export.sh"
```

### Bootloader nicht gefunden

```
Source 'esp32c5/bin/bootloader_qio_80m.elf' not found
```

Die `bin/` Dateien wurden nicht nach `esp32c5/bin/` kopiert. Manuell:

```bash
cp -r /data/Workspace/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs/esp32c5/bin \
      /data/Workspace/framework-arduinoespressif32-libs/esp32c5/bin
```

### PlatformIO verwendet alte Libraries

```bash
rm -rf .pio/packages/framework-arduinoespressif32-libs
platformio pkg install -e esp32-c5
platformio run -e esp32-c5
```

### Build-Ergebnis ueberpruefen

```bash
# Pruefe ob PSRAM-Settings eingebrannt sind:
grep -E "SPIRAM=|NIMBLE_MEM.*EXTERNAL|MATTER_MEM.*EXTERNAL|POOL_USE_HEAP" \
  /data/Workspace/framework-arduinoespressif32-libs/esp32c5/sdkconfig

# Pruefe Lib-Anzahl (sollte ~147 sein):
ls /data/Workspace/framework-arduinoespressif32-libs/esp32c5/lib/ | wc -l

# Pruefe Linker-Fixes:
grep 'EXCLUDE_FILE(\*libble_app.a \*libbt.a)' \
  /data/Workspace/framework-arduinoespressif32-libs/esp32c5/ld/sections.ld
```

## Konfiguration aendern

1. Editiere: `/data/Workspace/esp32-arduino-lib-builder/configs/defconfig.esp32c5`
2. Baue neu: `./build_and_deploy_libs.sh`
3. Re-install: `rm -rf .pio/packages/framework-arduinoespressif32-libs && pio pkg install -e esp32-c5`
4. Firmware bauen: `platformio run -e esp32-c5`

### Defconfig Merge-Reihenfolge

Das `build.sh` Script merged mehrere defconfigs:

1. `defconfig.common` — Allgemeine ESP-IDF Settings
2. `defconfig.esp32c5` — **Unsere optimierte Konfiguration**
3. `defconfig.debug_default` — Debug-Level
4. `defconfig.qio_ram` — QIO RAM-Mode
5. `defconfig.qio` — QIO Flash-Mode
6. `defconfig.80m` — 80MHz Flash-Takt

## Lizenz

Teil des OpenSprinkler-Firmware Projektes.
