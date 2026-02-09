# ESP32-C5 Arduino Libraries Build & Deploy Script

Automatisiertes Script zur Kompilation und Bereitstellung optimierter ESP32-C5 Arduino Framework Libraries für OpenSprinkler mit Memory-Optimierungen für Matter+BLE+WiFi.

## Features

✅ **Vollständig automatisiert** - Ein Befehl zum Bauen und Deployen  
✅ **Memory-Optimiert** - WiFi RX/TX Buffer reduziert, BLE in PSRAM  
✅ **PlatformIO-Integration** - Arbeitet mit der lokalen Framework-Kopie und optional mit PlatformIO packages  
✅ **Backup-Sicherheit** - Automatische Backups der alten Libraries  
✅ **Aussagekräftiger Output** - Farbige Logs mit Fortschrittsanzeige  

## Verwendung

### Basic: Nur Deployen (Libraries bereits gebaut)

```bash
./build_and_deploy_libs.sh -s
```

Dies deployt die bereits kompilierten Libraries aus dem lib-builder zu:
- `/data/Workspace/framework-arduinoespressif32-libs/esp32c5/`

### Full Build & Deploy: Kompilieren + Deployen

```bash
./build_and_deploy_libs.sh
```

Dies:
1. Kompiliert ESP32-C5 Libraries neu (10-30 Min)
2. Deployt zu Workspace-Libs

### Mit PlatformIO Sync

```bash
./build_and_deploy_libs.sh -c
# oder mit Skip-Build:
./build_and_deploy_libs.sh -s -c
```

Dies deployt zusätzlich zu:
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/`

## Optionen

| Option | Beschreibung |
|--------|-------------|
| `-s, --skip-build` | Überspringe Kompilation (Libraries bereits vorhanden) |
| `-c, --copy-pio` | Kopiere auch zu PlatformIO packages Directory |

## Angewendete Optimierungen

Das Script appliziert folgende Memory-Optimierungen automatisch:

### WiFi Buffer Reduktion
```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8        (spart ~12KB)
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=16        (spart ~25KB)
```

### BLE/NimBLE Optimierungen
```
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y    (BLE heap in PSRAM)
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3584    (reduziert von 5120)
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=16        (reduziert von 24)
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=16        (reduziert von 24)
```

### Speicher-Reservierung
```
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536   (64KB für DMA reserviert)
CONFIG_SPIRAM_USE_MALLOC=y                    (malloc nutzt PSRAM)
```

**Ergebnis:** ~50KB internes RAM freigegeben für DMA-Buffer
- Vorher: 16KB frei
- Nachher: ~66KB frei (4x Verbesserung)

## Workflow

### Erste Installation

```bash
# 1. Baue und deploye Libraries
./build_and_deploy_libs.sh -c

# 2. Baue OpenSprinkler Firmware neu
cd /data/Workspace/OpenSprinkler-Firmware
platformio run -e esp32-c5-matter

# 3. Lade auf Device
platformio run -e esp32-c5-matter --target upload
```

### Regelmäßige Updates

```bash
# Wenn nur defconfig.esp32c5 geändert wurde:
./build_and_deploy_libs.sh

# Wenn nur Deployment/Installation nötig ist:
./build_and_deploy_libs.sh -s -c
```

## Backups

Das Script erstellt automatisch Backups:

- **Workspace Backups:** `/data/Workspace/framework-arduinoespressif32-libs/esp32c5/lib.backup.TIMESTAMP/`
- **PlatformIO Backups:** `~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/lib.backup.TIMESTAMP/`

Backups können manuell wiederhergestellt werden:

```bash
# Workspace-Backup wiederherstellen
rm -rf /data/Workspace/framework-arduinoespressif32-libs/esp32c5/lib
cp -r /data/Workspace/framework-arduinoespressif32-libs/esp32c5/lib.backup.TIMESTAMP/lib \
      /data/Workspace/framework-arduinoespressif32-libs/esp32c5/

# Oder ganz zurück zu original:
rm -rf /data/Workspace/framework-arduinoespressif32-libs/esp32c5
cp -r /data/Workspace/framework-arduinoespressif32-libs/esp32c5.original \
      /data/Workspace/framework-arduinoespressif32-libs/esp32c5
```

## Troubleshooting

### Build schlägt fehl: "idf.py: Command not found"

Das IDF-environment ist nicht geladen. Das Script sollte das automatisch machen, aber:

```bash
source /data/Workspace/esp32-arduino-lib-builder/esp-idf/export.sh
./build_and_deploy_libs.sh
```

### Build schlägt fehl: CMake nicht vorhanden

```bash
sudo apt-get install cmake
```

### Biblioteken nicht aktualisiert nach Build

Stelle sicher, dass:
1. Build erfolgreich war (kein Fehler im Output)
2. `-s` Flag nicht gesetzt ist (um wirklich zu bauen)
3. Altes Verzeichnis nicht schreibgeschützt ist

```bash
ls -la /data/Workspace/framework-arduinoespressif32-libs/esp32c5/lib/
```

### PlatformIO verwendet alte Libraries

Versuche Clean-Build:

```bash
cd /data/Workspace/OpenSprinkler-Firmware
platformio run --target clean --environment esp32-c5-matter
platformio run -e esp32-c5-matter
```

## Konfiguration ändern

Die Speicher-Optimierungen sind in defconfig definiert. Um sie anzupassen:

1. Editiere: `/data/Workspace/esp32-arduino-lib-builder/configs/defconfig.esp32c5`

2. Führe Script ohne Skip-Build aus:

```bash
./build_and_deploy_libs.sh -c
```

Wichtige Konfigurationsoptionen:

```bash
# Workflow in defconfig.esp32c5:
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8       # ← Ändern für WiFi Buffer
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=16       # ← Ändern für TX Buffer
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3584   # ← Ändern für BLE Stack
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y   # ← Auf 'n' setzen wenn BLE intern sein soll
```

## Technische Details

### Defconfig Merge-Reihenfolge

Das build.sh Script merged mehrere defconfigs:
1. `defconfig.common` - Allgemeine ESP-IDF Settings
2. `defconfig.esp32c5` - Chip-spezifisch (mit Optimierungen)
3. `defconfig.debug_default` - Debug-Level
4. `defconfig.qio_ram` - QIO RAM-Mode
5. `defconfig.qio` - QIO
6. `defconfig.80m` - 80MHz Takt

Nur `defconfig.esp32c5` ist in diesem Script optimiert.

### Warum ist Framework-Rebuild nötig?

Weil sdkconfig-Werte **Compile-Time** Konfigurationen sind:
- `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8` bestimmt Speichergröße zur Compile-Zeit
- WiFi-Komponente wird mit 8 Buffers gebaut (nicht 16)
- Spart tatsächlich ~12KB internes RAM

Ein reines defconfig-Edit in die bereits kompilierten Libs zu kopieren hätte **keinen Effekt**.

## Support

Falls Probleme auftreten:

1. Prüfe Build-Log: `/tmp/lib_build.log`
2. Verifiziere Pfade: `ls -la /data/Workspace/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs/esp32c5/lib/`
3. Führe manuell Build aus:
   ```bash
   source /data/Workspace/esp32-arduino-lib-builder/esp-idf/export.sh
   cd /data/Workspace/esp32-arduino-lib-builder
   ./build.sh -t esp32c5 -s
   ```

## Lizenz

Dieses Script ist Teil des OpenSprinkler-Firmware Projektes.
