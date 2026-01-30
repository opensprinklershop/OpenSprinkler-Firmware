# LÖSUNG: mbedTLS mit Software-AES neu bauen

## Problem
ESP32-C5 mit Matter hat zu wenig internes RAM für Hardware-AES DMA:
```
E (17451) esp-aes: Failed to allocate memory
```

Die pre-compiled `libmbedcrypto.a` hat Hardware-AES aktiviert, sdkconfig-Änderungen haben keinen Effekt.

## Lösung 1: esp32-arduino-lib-builder verwenden (EMPFOHLEN)

### Schritt 1: Lib-Builder klonen (Linux/WSL erforderlich)
```bash
git clone https://github.com/espressif/esp32-arduino-lib-builder
cd esp32-arduino-lib-builder
```

### Schritt 2: Konfiguration anpassen
Erstelle `configs/defconfig.esp32c5.custom`:
```
# Disable Hardware AES (uses DMA, requires internal RAM)
CONFIG_MBEDTLS_HARDWARE_AES=n

# Enable software fallback
CONFIG_MBEDTLS_AES_USE_ROM_TABLES=y

# Optional: Aktiviere Small Data Optimization (nutzt non-DMA für kleine Daten)
# CONFIG_MBEDTLS_AES_HW_SMALL_DATA_LEN_OPTIM=y
```

### Schritt 3: Nur libmbedcrypto für ESP32-C5 bauen
```bash
./build.sh -t esp32c5 -b idf_libs
```

### Schritt 4: Fertige Library kopieren
Die gebaute `libmbedcrypto.a` befindet sich in:
`out/esp32c5/arduino_idf_libs/lib/`

Kopiere sie nach:
```
~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/lib/libmbedcrypto.a
```

**WICHTIG: Backup erstellen!**
```bash
cp ~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/lib/libmbedcrypto.a \
   ~/.platformio/packages/framework-arduinoespressif32-libs/esp32c5/lib/libmbedcrypto.a.backup
```

## Lösung 2: Docker-basierter Build (einfacher)

```bash
# Arduino-ESP32 Repo klonen (oder dein lokales verwenden)
cd /mnt/d/Projekte
git clone https://github.com/espressif/arduino-esp32

# Docker Container starten
docker run --rm -it -v $PWD/arduino-esp32:/arduino-esp32 \
  espressif/esp32-arduino-lib-builder:release-v5.5 /bin/bash

# Im Container: Konfiguration anpassen und bauen
cd /esp32-arduino-lib-builder
./build.sh -t esp32c5 menuconfig
# → Component config → mbedTLS → AES → Hardware AES acceleration DEAKTIVIEREN
./build.sh -t esp32c5 -b idf_libs
```

## Lösung 3: Minimaler Patch (Quick Fix)

Falls die komplette Neukompilierung zu aufwändig ist:

### A) Matter-Initialisierung verzögern bis mehr Speicher frei ist
In `main.cpp` vor `Matter.begin()`:
```cpp
// Warte bis genug interner Speicher frei ist
while (heap_caps_get_free_size(MALLOC_CAP_DMA) < 32768) {
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

### B) Speicher freigeben vor Matter-Init
Temporär WiFi disconnecten, Matter starten, WiFi reconnecten.

### C) PSRAM-fähige Crypto prüfen
Für ESP32-C5: Prüfen ob neuere ESP-IDF Version PSRAM-DMA für Crypto unterstützt.

## Verifikation

Nach dem Ersetzen der Library:
1. PlatformIO Clean durchführen: `pio run -t clean`
2. Neu bauen: `pio run`
3. Upload und Serial Monitor
4. `esp-aes: Failed to allocate memory` sollte nicht mehr erscheinen

## Dateien

- `libmbedcrypto.a` - Die problematische Library (7.5MB, enthält Hardware-AES)
- `libmbedtls.a` - TLS Layer (146KB)
- `libmbedtls_2.a` - TLS Layer Part 2 (2MB)

Pfad der pre-compiled libs:
```
%USERPROFILE%\.platformio\packages\framework-arduinoespressif32-libs\esp32c5\lib\
```

## Referenz

- ESP-IDF mbedTLS Port: `esp-idf/components/mbedtls/port/aes/`
- Fehlerquelle: `esp_aes_dma_core.c` Zeilen 269-281 (`heap_caps_aligned_alloc` mit `MALLOC_CAP_DMA`)
- Arduino Lib Builder: https://github.com/espressif/esp32-arduino-lib-builder
- Docker Image: `espressif/esp32-arduino-lib-builder:release-v5.5`
