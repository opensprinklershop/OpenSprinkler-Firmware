# Firmware-Größen-Optimierung für OpenSprinkler ESP32-C5

## Aktuelle Größe (vor Optimierung)
```
Flash: 37.7% (3,162,808 bytes von 8,388,608 bytes)
RAM:   79.66% (255,636 bytes von 320,928 bytes SRAM)
ELF:   text=3,124,035 data=512,064 bss=6,210,188
```

## Identifizierte ungenutzte Komponenten

### ✅ Entfernt (sofort)
1. **SD Card Library** (~30 KB Flash)
   - Nur in `EMailSender.cpp` (optional)
   - Nicht für Kernfunktionalität benötigt

2. **SPIFFS Filesystem** (~25 KB Flash)
   - Nur LittleFS wird genutzt
   - SPIFFS ist obsolet

3. **FFat Filesystem** (~35 KB Flash)
   - Nicht verwendet
   - LittleFS ausreichend

### 🔍 Zu prüfen (potenziell entfernbar)

4. **Matter Protocol** (~200-300 KB Flash, ~50 KB RAM)
   - Status: `ENABLE_MATTER` ist aktiviert
   - Verwendung: In main.cpp und opensprinkler_matter.cpp
   - **Frage:** Wird Matter/Thread tatsächlich genutzt?

5. **BLE (Bluetooth Low Energy)** (~150-200 KB Flash, ~30 KB RAM)
   - Status: `OS_ENABLE_BLE` ist aktiviert
   - Verwendung: In opensprinkler_server.cpp für Sensor-Kommunikation
   - **Frage:** Wird BLE für Sensoren benötigt?

6. **Certificate Bundle** (~120 KB Flash)
   - Status: `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`
   - Nutzen: CA Zertifikate für HTTPS Client-Verbindungen
   - **Kann reduziert werden** auf spezifische CAs

7. **Multiple Hash Algorithms** (~20 KB Flash)
   - SHA1, SHA3, MD5 aktiviert
   - Nur SHA-256/384 für TLS 1.3 benötigt

### ⚠️ Beibehalten (in Verwendung)

- **DNSServer**: Für WiFi AP Modus (Captive Portal)
- **Ticker**: Für Reboot-Timer
- **LittleFS**: Für Datenspeicherung
- **WebSockets**: Für Echtzeit-Kommunikation
- **MQTT**: Für Home Automation
- **InfluxDB Client**: Für Datenlogging

## Optimierungsmaßnahmen

### Phase 1: Sichere Entfernungen (✅ implementiert)

```ini
# platformio.ini
lib_ignore =
   Zigbee
   SD          # ← NEU: SD Card Library
   SPIFFS      # ← NEU: SPIFFS Filesystem
   FFat        # ← NEU: FAT Filesystem
```

**Einsparung:** ~90 KB Flash

### Phase 2: Build-Optimierungen

#### 2.1 Compiler-Flags optimieren
```ini
build_flags = 
    -Os                          # Size optimization (bereits aktiv)
    -flto                        # Link Time Optimization (bereits aktiv)
    -Wl,--gc-sections           # Garbage collect sections (bereits aktiv)
    -ffunction-sections         # ← NEU
    -fdata-sections             # ← NEU
    -fno-exceptions             # ← NEU: Keine C++ Exceptions
    -fno-rtti                   # ← NEU: Kein RTTI
    -DCORE_DEBUG_LEVEL=0        # ← NEU: Kein Debug Output
```

**Einsparung:** ~50-100 KB Flash

#### 2.2 mbedTLS optimieren (sdkconfig)
```ini
# Reduziere Certificate Bundle
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y  # Nur häufige CAs
# statt CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y

# Deaktiviere ungenutzte Hash-Algorithmen
# CONFIG_MBEDTLS_SHA1_C is not set     # SHA1 unsicher
# CONFIG_MBEDTLS_SHA3_C is not set     # SHA3 nicht benötigt
# CONFIG_MBEDTLS_ROM_MD5 is not set    # MD5 unsicher

# Reduziere DH Key Sizes
CONFIG_MBEDTLS_MPI_MAX_SIZE=512  # statt 1024
```

**Einsparung:** ~100-150 KB Flash

### Phase 3: Optionale Features deaktivieren

#### 3.1 Matter deaktivieren (falls nicht genutzt)
```ini
# platformio.ini
build_flags = 
    # -D ENABLE_MATTER  # ← Kommentieren falls nicht benötigt

lib_deps = 
    # Matter            # ← Entfernen falls nicht benötigt
```

**Einsparung:** ~250-300 KB Flash, ~50 KB RAM

#### 3.2 BLE reduzieren (falls nur für spezifische Sensoren)
```ini
# sdkconfig.esp32-c5
# Minimal BLE Konfiguration
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n       # Nur Server
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
```

**Einsparung:** ~50-80 KB RAM

#### 3.3 Log Level reduzieren
```ini
# sdkconfig.esp32-c5
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
# statt CONFIG_LOG_DEFAULT_LEVEL_INFO
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
```

**Einsparung:** ~20-30 KB Flash

### Phase 4: ESP-IDF Komponenten optimieren

```ini
# sdkconfig.esp32-c5

# Deaktiviere ungenutzte Protokolle
# CONFIG_LWIP_IPV6 is not set                    # IPv6 falls nicht benötigt
# CONFIG_ESP_NETIF_TCPIP_ADAPTER_COMPATIBLE_LAYER is not set

# Reduziere Thread Stack Sizes
CONFIG_ESP_MAIN_TASK_STACK_SIZE=2048      # statt 3584
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=2048  # statt 3072

# Deaktiviere Debug Features
# CONFIG_FREERTOS_DEBUG_OFLOW is not set
# CONFIG_FREERTOS_ASSERT_ON_UNTESTED_FUNCTION is not set
```

**Einsparung:** ~50-100 KB RAM

## Zusammenfassung der Einsparungen

| Phase | Komponente | Flash | RAM | Status |
|-------|-----------|-------|-----|--------|
| 1 | SD/SPIFFS/FFat | -90 KB | -5 KB | ✅ Implementiert |
| 2.1 | Compiler Flags | -80 KB | -10 KB | 🔄 Empfohlen |
| 2.2 | mbedTLS Certs | -120 KB | - | 🔄 Empfohlen |
| 3.1 | Matter (optional) | -280 KB | -50 KB | ⚠️ Prüfen |
| 3.2 | BLE minimal | - | -60 KB | ⚠️ Prüfen |
| 3.3 | Log Level | -25 KB | -5 KB | 🔄 Empfohlen |
| 4 | ESP-IDF Tuning | -50 KB | -80 KB | 🔄 Empfohlen |
| **Gesamt** | **~645 KB** | **~210 KB** | |

## Empfohlene Vorgehensweise

### Minimal (sicher):
1. ✅ SD/SPIFFS/FFat entfernen (bereits gemacht)
2. Compiler-Flags optimieren
3. mbedTLS Certificate Bundle reduzieren
4. Log Level reduzieren

**Einsparung:** ~315 KB Flash, ~20 KB RAM

### Moderat (wenn Features nicht benötigt):
+ Matter deaktivieren (falls nicht verwendet)
+ BLE minimieren

**Zusätzliche Einsparung:** ~280 KB Flash, ~110 KB RAM

### Aggressiv (maximale Einsparung):
+ ESP-IDF Stack Sizes reduzieren
+ IPv6 deaktivieren
+ Alle Debug-Features entfernen

**Zusätzliche Einsparung:** ~50 KB Flash, ~80 KB RAM

## Nächste Schritte

1. **Build testen** mit aktuellen Änderungen (SD/SPIFFS/FFat entfernt)
2. **Entscheiden:** Matter und BLE benötigt?
3. **Phase 2** implementieren (sichere Optimierungen)
4. **Erneut testen** und Größe messen

---

**Hinweis:** Nach jeder Änderung sollte ein vollständiger Test durchgeführt werden, um sicherzustellen, dass keine benötigten Funktionen beeinträchtigt werden.
