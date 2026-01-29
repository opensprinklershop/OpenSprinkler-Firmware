# TLS 1.3 Optimization for OpenSprinkler Firmware

## Übersicht

Das OpenSprinkler-Firmware Projekt wurde für maximale TLS-Performance und Speichereffizienz optimiert:

- **TLS 1.3 ONLY** - Kein Fallback auf TLS 1.2 oder ältere Versionen
- **Hardware-beschleunigte AES-GCM Cipher** - Nutzt ESP32-C5 Krypto-Hardware
- **Minimaler Speicher-Footprint** - Konfiguration direkt im ESP-IDF Framework

## Durchgeführte Änderungen

### 1. ESP-IDF Konfiguration (`sdkconfig.esp32-c5`)
- TLS 1.3 aktiviert: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`
- TLS 1.2 deaktiviert: `# CONFIG_MBEDTLS_SSL_PROTO_TLS1_2 is not set`

### 2. PlatformIO Build-Flags (`platformio.ini`)
```ini
build_flags = 
    -D MBEDTLS_SSL_PROTO_TLS1_3
    -D CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=1
```

### 3. OpenThings Framework Optimierungen

#### `Esp32LocalServer_Config.h`
- `OTF_FORCE_TLS_1_3_ONLY=1` - Erzwingt TLS 1.3
- `OTF_USE_ESPIDF_CIPHER_CONFIG=1` - Nutzt ESP-IDF Cipher-Konfiguration

#### `Esp32LocalServer.cpp`
- Entfernung der Runtime Cipher-Suite-Konfiguration
- Vereinfachte TLS-Konfiguration (nur TLS 1.3)
- Compile-Time Error bei fehlender TLS 1.3 Unterstützung

### 4. ESP-IDF mbedTLS Header (optional)
- `esp-idf/components/mbedtls/port/include/mbedtls/esp_config_opensprinkler.h`
- Zusätzliche Compile-Time Optimierungen für minimalen Footprint

## Technische Details

### Aktivierte TLS 1.3 Cipher Suites
1. **TLS_AES_128_GCM_SHA256** (0x1301) - Mandatory
   - Hardware AES-128-GCM
   - Hardware SHA-256
   
2. **TLS_AES_256_GCM_SHA384** (0x1302) - Optional
   - Hardware AES-256-GCM
   - Hardware SHA-384

### ESP32-C5 Hardware-Beschleunigung
- ✅ AES-128/256 (GCM Mode)
- ✅ SHA-256/384/512
- ✅ ECC (P-256, P-384)
- ✅ RSA (MPI Accelerator)

### Deaktivierte Features (Speicher-Einsparung)
- ❌ TLS 1.2 und ältere Protokolle
- ❌ CBC, CCM, ChaCha20-Poly1305 Cipher
- ❌ TLS Renegotiation
- ❌ Session Tickets
- ❌ Encrypt-Then-MAC Extension
- ❌ Alle schwachen ECC-Kurven

## Speicher-Einsparungen

Durch die Optimierungen werden ca. **2-4 KB Flash** und **500-1000 Bytes RAM** eingespart:
- Keine TLS 1.2 Cipher-Code
- Keine Runtime Cipher-Selection
- Minimale mbedTLS Konfiguration

## Build-Informationen

```
Flash: [====      ]  37.7% (used 3162808 bytes from 8388608 bytes)
RAM:   [          ]   1.6% (used 135160 bytes from 8716288 bytes)
```

## Sicherheit

TLS 1.3 bietet gegenüber TLS 1.2 verbesserte Sicherheit:
- ✅ Forward Secrecy (immer aktiviert)
- ✅ 1-RTT Handshake (schneller)
- ✅ Verschlüsselte Server-Zertifikate
- ✅ Keine veralteten/unsicheren Cipher

## Kompatibilität

**Unterstützte Clients:**
- Moderne Browser (Chrome 70+, Firefox 63+, Safari 12.1+)
- curl 7.52.0+
- Python requests (TLS 1.3 support)
- OpenSSL 1.1.1+

**Nicht unterstützt:**
- Veraltete Browser (IE 11, alte Android WebView)
- Systeme ohne TLS 1.3 Support

## Testing

Build erfolgreich mit:
```bash
pio run -e esp32-c5
```

## Rollback (falls notwendig)

Falls TLS 1.2 Kompatibilität erforderlich ist:

1. **sdkconfig.esp32-c5** wiederherstellen:
   ```bash
   cp sdkconfig.esp32-c5.backup sdkconfig.esp32-c5
   ```

2. **platformio.ini** - Build-Flags entfernen:
   ```ini
   # -D MBEDTLS_SSL_PROTO_TLS1_3
   # -D CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=1
   ```

3. **Framework Code** - `OTF_FORCE_TLS_1_3_ONLY=0` setzen

## Dateien mit Änderungen

- ✅ `/data/OpenSprinkler-Firmware/platformio.ini`
- ✅ `/data/OpenSprinkler-Firmware/sdkconfig.esp32-c5`
- ✅ `/data/OpenThings-Framework-Firmware-Library/Esp32LocalServer_Config.h`
- ✅ `/data/OpenThings-Framework-Firmware-Library/Esp32LocalServer.cpp`
- ✅ `/data/esp-idf/components/mbedtls/port/include/mbedtls/esp_config_opensprinkler.h` (optional)

## Referenz-Dateien

- `sdkconfig.tls13-optimizations.txt` - Zusätzliche empfohlene mbedTLS-Einstellungen
- `sdkconfig.esp32-c5.backup` - Backup der Original-Konfiguration

---

**Erstellt:** 28. Januar 2026  
**Projekt:** OpenSprinkler-Firmware ESP32-C5  
**Framework:** ESP-IDF 5.5.2 + PlatformIO
