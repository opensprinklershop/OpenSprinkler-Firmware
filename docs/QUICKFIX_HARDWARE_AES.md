# Schnellste Lösung: mbedTLS mit Software-AES bauen

## Problem
`E (17451) esp-aes: Failed to allocate memory` - Hardware-AES braucht DMA-fähigen internen RAM der bei Matter knapp ist.

## Schnellste Lösung: Docker + lib-builder

**Einmaliger Aufwand ~30min, dann hast du eine funktionierende Library!**

### 1. Docker Desktop für Windows installieren (falls nicht vorhanden)
https://www.docker.com/products/docker-desktop/

### 2. Terminal öffnen und ausführen:

```powershell
# Verzeichnis erstellen
cd D:\Projekte
mkdir lib-builder-temp
cd lib-builder-temp

# Docker Container starten (lädt ~2GB Image beim ersten Mal)
docker run --rm -it -v ${PWD}:/output espressif/esp32-arduino-lib-builder:release-v5.5 /bin/bash
```

### 3. Im Docker Container:

```bash
# In den lib-builder wechseln  
cd /esp32-arduino-lib-builder

# Konfiguration für ESP32-C5 ohne Hardware-AES erstellen
cat >> configs/defconfig.esp32c5 << 'EOF'
# CUSTOM: Disable Hardware AES for Matter memory fix
CONFIG_MBEDTLS_HARDWARE_AES=n
EOF

# Nur die IDF Libraries für ESP32-C5 bauen (dauert ~15-20min)
./build.sh -t esp32c5 -b idf_libs

# Fertige Library kopieren
cp out/esp32c5/arduino_idf_libs/lib/libmbedcrypto.a /output/
```

### 4. Zurück in PowerShell - Library installieren:

```powershell
# Backup der originalen Library
$libPath = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32-libs\esp32c5\lib"
Copy-Item "$libPath\libmbedcrypto.a" "$libPath\libmbedcrypto.a.backup"

# Neue Library kopieren
Copy-Item "D:\Projekte\lib-builder-temp\libmbedcrypto.a" "$libPath\libmbedcrypto.a"

# Cleanup
cd D:\Projekte
Remove-Item -Recurse lib-builder-temp
```

### 5. Projekt neu bauen:

```powershell
cd D:\Projekte\openSprinkler-firmware-esp32
pio run -t clean
pio run
```

## Verifizierung

Nach dem Upload sollte:
- `esp-aes: Failed to allocate memory` NICHT mehr erscheinen
- Matter.begin() erfolgreich durchlaufen
- Kommissionierung funktionieren

## Rollback

Falls Probleme auftreten:
```powershell
$libPath = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32-libs\esp32c5\lib"
Copy-Item "$libPath\libmbedcrypto.a.backup" "$libPath\libmbedcrypto.a" -Force
pio run -t clean
pio run
```

## Alternative: Warten auf ESP-IDF Update

Espressif wird voraussichtlich in zukünftigen Versionen eine Option für PSRAM-fähiges Crypto einführen.
ESP32-C5 ist noch relativ neu, daher gibt es hier noch Optimierungspotential.

## Technische Details

- Pre-compiled libs: `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32-libs\esp32c5\lib\`
- Problematische Library: `libmbedcrypto.a` (7.5MB)
- Problematische Funktion: `esp_aes_process_dma_ext_ram()` in `esp_aes_dma_core.c`
- Root cause: `heap_caps_aligned_alloc()` mit `MALLOC_CAP_DMA` schlägt fehl wenn <16KB interner Heap frei
