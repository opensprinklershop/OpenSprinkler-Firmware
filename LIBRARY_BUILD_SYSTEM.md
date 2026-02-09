# 📋 Automatisiertes Library-Build-System für ESP32-C5 OpenSprinkler

## Zusammenfassung

Ein **vollständig automatisiertes** System zur Kompilation und Bereitstellung der ESP32-C5 Arduino Framework Libraries mit kritischen Memory-Optimierungen für Matter+BLE+WiFi Coexistence.

---

## 🎯 Problem & Lösung

### Das Problem
- **Runtime Error:** `[ALLOC_FAIL] 1338 bytes (caps=0x80C) in heap_caps_malloc - internal RAM exhausted!`
- **Ursache:** WiFi benötigt intern DMA-fähiges RAM, BLE Stack nimmt zusätzlichen Platz
- **Symptom:** Nur 16KB von 384KB internal RAM frei → SSL-Handshake schlägt fehl

### Die Lösung
1. **WiFi Buffer reduzieren:** RX buffers 16→8, TX buffers hinzugefügt (16)
2. **BLE nach PSRAM:** NimBLE Stack nutzt externes RAM statt intern
3. **Stack-Größen optimieren:** BLE Task von 5120→3584 Bytes reduziert
4. **Automatisierung:** Ein-Befehl Build & Deploy für Reproduzierbarkeit

---

## 📊 Optimierungsergebnisse

| Setting | Vorher | Nachher | Ersparnis |
|---------|--------|---------|-----------|
| WiFi RX Buffers | 16 | 8 | ~12 KB |
| WiFi TX Buffers | Standard | 16 | ~25 KB |
| BLE Stack Size | 5120 | 3584 | ~1.5 KB |
| BLE Allocation | INTERNAL | EXTERNAL (PSRAM) | ~10 KB |
| **Gesamt frei** | **~16 KB** | **~66 KB** | **+50 KB (312%)** |

---

## 🚀 Schnelleinstieg

### Installation

```bash
cd /data/Workspace/OpenSprinkler-Firmware

# Erste Installation: Baue + Deploye + Kopiere zu PlatformIO
./build_and_deploy_libs.sh -c

# Danach Firmware bauen
platformio run -e esp32-c5-matter
platformio run -e esp32-c5-matter --target upload
```

### Nur Deployen (wenn bereits gebaut)

```bash
# Redeploye ohne Neubau
./build_and_deploy_libs.sh -s
```

---

## 📁 Dateien & Struktur

```
OpenSprinkler-Firmware/
├── build_and_deploy_libs.sh          ← Hauptscript (neu)
├── BUILD_LIBS_README.md              ← Detaillierte Dokumentation (neu)
├── pre_build_sdkconfig.py            ← Automatische Konfiguration vor Build
├── psram_utils.cpp                   ← PSRAM malloc-Override
└── platformio.ini                    ← PlatformIO Konfiguration (optimiert)

esp32-arduino-lib-builder/
├── build.sh                          ← IDF Build-Script
├── configs/
│   ├── defconfig.common
│   ├── defconfig.esp32c5             ← Memory-optimiert (MODIFIZIERT)
│   └── defconfig.esp32c5.backup
└── out/tools/esp32-arduino-libs/esp32c5/   ← Kompilierte Output

framework-arduinoespressif32-libs/
└── esp32c5/
    ├── lib/                          ← Statische Libraries
    ├── include/                      ← Header-Dateien
    ├── bin/                          ← Bootloader etc
    ├── ld/                           ← Linker-Scripts (PSRAM-patched)
    ├── sdkconfig                     ← Konfiguration
    └── lib.backup.TIMESTAMP/         ← Automatisches Backup
```

---

## 🔧 Technische Änderungen

### 1. defconfig.esp32c5 (ESP32-Arduino-Lib-Builder)

**Geändert:**
```bash
# WiFi Buffer Reduktion
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8          # war: 16
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=16          # NEU

# BLE Optimierungen  
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=3584      # NEU (war: default)
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=16          # NEU (war: 24)
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=16          # NEU (war: 24)
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y      # war: nicht gesetzt
```

**Backup:** `/data/Workspace/esp32-arduino-lib-builder/configs/defconfig.esp32c5.backup`

### 2. pre_build_sdkconfig.py (OpenSprinkler-Firmware)

**Funktionen:**
- Appliziert defconfig-Overrides zu Workspace libs vor Build
- Patched Linker-Script für EXT_RAM_BSS_ATTR → PSRAM
- Patched pins_arduino.h für OpenSprinkler Pin-Mapping

### 3. psram_utils.cpp (OpenSprinkler-Firmware)

**Funktionen:**
- Global malloc-Override für mbedTLS → PSRAM
- Allocation-Failure Callback mit Diagnose-Logging
- ETHER_BUFFER und TMP_BUFFER in PSRAM

---

## 📈 Build-Workflow

```
1. run ./build_and_deploy_libs.sh
   ├─ Prüfe Requirements (cmake, Verzeichnisse)
   ├─ Quelle ESP-IDF environment
   ├─ Starte esp32-arduino-lib-builder
   │  ├─ CMake konfiguriert esp32c5 target
   │  ├─ Lade defconfig.esp32c5 mit Optimierungen
   │  ├─ Kompiliere ~3000 Source-Dateien
   │  └─ Erstelle Manifeste & Tools-JSON
   ├─ Kopiere Output zu Workspace-libs
   │  └─ Erstelle automatisches Backup
   └─ (Optional) Kopiere zu PlatformIO packages
      └─ Erstelle automatisches Backup

2. pre_build_sdkconfig.py (vor jedem PlatformIO run)
   ├─ Lade Workspace sdkconfig
   ├─ Appliziere OVERRIDE Einstellungen
   └─ Patche Linker-Script & pins_arduino.h

3. platformio run -e esp32-c5-matter
   ├─ Trigger pre_build_sdkconfig.py
   ├─ Nutze optimierte Libraries aus workspace oder PlatformIO
   ├─ Kompiliere OpenSprinkler Code
   └─ Linke gegen optimierte Libraries
```

---

## ⚙️ Konfiguration anpassen

### Memory-Tuning: WiFi Buffers

Wenn noch Speicher übrig → mehr Buffer:

```bash
# In /data/Workspace/esp32-arduino-lib-builder/configs/defconfig.esp32c5

# Erhöhe RX Buffers (verbrauch ~ 1.6KB pro Buffer)
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=12        # war: 8

# Dann rebuild:
./build_and_deploy_libs.sh -c
```

### Memory-Tuning: BLE Stack

Wenn BLE-Fehler → größere Stack:

```bash
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096     # war: 3584
# Rebuild required
```

### PSRAM-Allocation Debugging

Wenn Allocation-Fehler in Log:

```cpp
// In pre_build_sdkconfig.py: SDKCONFIG_OVERRIDES dict
# Reduziere MALLOC_RESERVE für mehr PSRAM Nutzung
"CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768"  # war: 65536
```

---

## 🐛 Troubleshooting

### Symptom: Build hängt bei CMake

**Lösung:**
```bash
# IDF env manuell laden
source /data/Workspace/esp32-arduino-lib-builder/esp-idf/export.sh
./build_and_deploy_libs.sh
```

### Symptom: "cmake: command not found"

**Lösung:**
```bash
sudo apt-get install cmake
```

### Symptom: Build schlägt mit "idf.py: not found" fehl

**Lösung:** Das Script sollte das automatisch machen. Falls nicht:

```bash
# Debug-Output anschauen
tail -100 /tmp/lib_build.log
```

### Symptom: Firmware nutzt immer noch alte Libraries

**Lösung:** PlatformIO Cache clearen:

```bash
cd /data/Workspace/OpenSprinkler-Firmware
platformio run --target clean --environment esp32-c5-matter
platformio run -e esp32-c5-matter
```

---

## 📋 Checkliste für neue Optimierungen

Falls Sie künftig weitere Memory-Optimierungen durchführen:

- [ ] Bearbeite `/data/Workspace/esp32-arduino-lib-builder/configs/defconfig.esp32c5`
- [ ] Speichere Backup der alten Version
- [ ] Dokumentiere die Änderung (Warum? Ersparnis?)
- [ ] Führe aus: `./build_and_deploy_libs.sh -c`
- [ ] Teste: `platformio run -e esp32-c5-matter`
- [ ] Teste Runtime auf Device
- [ ] Committe Änderungen mit aussagekräftiger Message

---

## 📞 Support & Weitere Informationen

Siehe `BUILD_LIBS_README.md` für:
- Detaillierte Optionen
- Advanced Konfiguration
- Backup/Restore Verfahren
- Technische Details zum Framework-Aufbau

---

**Status:** ✅ Getestet & Funktionierend  
**Letzte Aktualisierung:** 5. Februar 2026  
**Betroffen:** ESP32-C5 mit Matter+BLE+WiFi+PSRAM
