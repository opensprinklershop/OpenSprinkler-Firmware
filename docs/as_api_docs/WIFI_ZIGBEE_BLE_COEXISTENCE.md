# WiFi + Zigbee + BLE Koexistenz auf ESP32-C5

## Übersicht

Der ESP32-C5 verfügt über **ein einziges 2.4 GHz ISM-Band RF-Modul**, das von WiFi, Zigbee (IEEE 802.15.4) und Bluetooth LE gemeinsam genutzt wird. Die Koexistenz wird durch **Time-Division Multiplexing** (Zeitschlitz-Multiplexing) verwaltet.

**Offizielle Dokumentation**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html

## Unterstützte Szenarien für OpenSprinkler (ESP32-C5)

| WiFi-Modus | Zigbee-Rolle | BLE-Status | Unterstützung | Stabilität |
|------------|--------------|------------|---------------|------------|
| STA Connected | End Device | Connected | ✅ Ja | **Stabil (Y)** |
| STA Connected | Router | Connected | ⚠️ Ja | **Instabil (C1)** |
| STA Scan | End Device | Connected | ✅ Ja | **Stabil (Y)** |
| STA Scan | Router | Connected | ⚠️ Ja | **Instabil (C1)** |
| SOFTAP | Router | Any | ❌ Nein | **Nicht unterstützt (X)** |
| SOFTAP | End Device | Connected | ⚠️ Ja | **Instabil (C1)** |

### Legende
- **Y (Stabil)**: Vollständig unterstützt mit stabiler Performance
- **C1 (Instabil)**: Unterstützt, aber Performance kann instabil sein
- **X (Nicht unterstützt)**: Kombination funktioniert nicht
- **S (Bedingt)**: Stabil im STA-Modus, sonst nicht unterstützt

## OpenSprinkler Empfohlene Konfiguration

Für **beste Performance** in OpenSprinkler:

### ✅ Empfohlen (Stabile Konfiguration)
```
WiFi:    STA-Modus (Client, verbunden mit Router)
Zigbee:  End Device
BLE:     Aktiviert (für Sensor-Kommunikation)
```

**Vorteil**: Alle drei Technologien funktionieren stabil und zuverlässig.

### ⚠️ WiFi-Setup-Modus (Automatisch behandelt)
```
WiFi:    SOFTAP-Modus (temporär während Ersteinrichtung)
Zigbee:  AUTOMATISCH DEAKTIVIERT
BLE:     AUTOMATISCH DEAKTIVIERT
```

**Implementierung**: OpenSprinkler deaktiviert **automatisch** Zigbee und BLE beim Wechsel in den SOFTAP-Modus (WiFi-Konfiguration) und reaktiviert sie nach erfolgreicher Verbindung zum Router. Dies verhindert RF-Koexistenz-Konflikte.

**Code-Referenz**:
- `espconnect.cpp`: `disable_zigbee_ble_for_softap()` wird vor `WiFi.softAP()` aufgerufen
- `espconnect.cpp`: `reenable_zigbee_ble_after_softap()` wird bei `WiFi.mode(WIFI_STA)` aufgerufen

### ⚠️ Nicht empfohlen (Instabile Konfiguration)
```
WiFi:    STA-Modus
Zigbee:  Router/Coordinator
BLE:     Aktiviert
```

**Nachteil**: Hohe Paketverlustrate bei Zigbee, da Router ununterbrochen empfangen müssen.

### ❌ Nicht unterstützt
```
WiFi:    SOFTAP-Modus (Access Point)
Zigbee:  Router
BLE:     Beliebig
```

**Grund**: Hardware-Limitation - Zigbee-Router brauchen kontinuierlichen RF-Zugriff, der mit WiFi-AP-Beacon-Übertragungen kollidiert.

## Koexistenz-Mechanismus

### Prioritätsbasierte RF-Ressourcen-Zuteilung

1. **Zeit-Slices**: WiFi und BLE haben feste Zeitschlitze für RF-Zugriff
   - Im **WiFi-Slice**: WiFi hat höhere Priorität
   - Im **BLE-Slice**: BLE hat höhere Priorität
   - **Zigbee**: Empfängt nur in verbleibender Zeit (niedrigste Priorität)

2. **Dynamische Priorität**:
   - Zigbee TX/ACK: Höhere Priorität (konkurriert mit WiFi/BLE)
   - Zigbee RX: Niedrigste Priorität (wird von WiFi/BLE überschrieben)
   - BLE Advertising: Alle N Events hat eines hohe Priorität

3. **Koexistenz-Periode**:
   - **WiFi IDLE**: RF von Bluetooth kontrolliert
   - **WiFi CONNECTED**: Periode startet bei TBTT (>100ms), 50/50 WiFi/BLE Split
   - **WiFi SCAN**: WiFi-Slice länger, BLE-Slice angepasst
   - **WiFi CONNECTING**: WiFi-Slice noch länger, BLE-Slice kompensiert

### Beispiel: WiFi CONNECTED + BLE CONNECTED

```
|<------- Koexistenz-Periode (~100ms) ------->|
|                                             |
|  WiFi-Slice (50%)  |   BLE-Slice (50%)     |
|--------------------|-----------------------|
|  WiFi hat Priorität|  BLE hat Priorität    |
|                    |                       |
         ↓ Zigbee kann nur in Lücken empfangen ↓
```

## Konfigurationsoptionen (sdkconfig.defaults)

### Erforderliche Basis-Konfiguration

```ini
# Software-Koexistenz aktivieren
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y

# Bluetooth LE aktivieren
CONFIG_BT_ENABLED=y
CONFIG_BT_BLE_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y

# Dynamischer Speicher für BLE (spart RAM)
CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y
```

### OpenSprinkler Automatische Koexistenz-Verwaltung

OpenSprinkler implementiert **automatisches Zigbee/BLE-Management** während WiFi-Modi-Wechsel:

**Beim Wechsel zu SOFTAP-Modus (WiFi-Setup)**:
```cpp
void start_network_ap() {
    disable_zigbee_ble_for_softap();  // Deaktiviert Zigbee/BLE
    WiFi.softAP(ssid, pass);
    // ... WiFi-Konfiguration läuft ohne RF-Konflikte
}
```

**Beim Wechsel zu STA-Modus (normale Betriebsart)**:
```cpp
void start_network_sta() {
    reenable_zigbee_ble_after_softap();  // Reaktiviert Zigbee/BLE
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    // ... Zigbee/BLE laufen wieder stabil
}
```

**Vorteile**:
- ✅ Keine manuellen Eingriffe erforderlich
- ✅ Verhindert RF-Koexistenz-Konflikte automatisch
- ✅ Maximale Stabilität in allen WiFi-Modi

### Speicher-Optimierung für 400KB SRAM

```ini
# WiFi-Buffer reduzieren
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=8

# WiFi Block Ack Windows
CONFIG_ESP_WIFI_TX_BA_WIN=4
CONFIG_ESP_WIFI_RX_BA_WIN=4

# LWIP TCP/UDP Buffer
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5744
CONFIG_LWIP_TCP_WND_DEFAULT=5744
CONFIG_LWIP_TCP_RECVMBOX_SIZE=4
CONFIG_LWIP_UDP_RECVMBOX_SIZE=4
```

## Performance-Erwartungen

### WiFi STA + Zigbee End Device + BLE (Empfohlen)
- **WiFi**: Normale Performance, minimale Paketverluste
- **Zigbee**: Stabile Kommunikation, 60-120s Polling-Intervalle empfohlen
- **BLE**: Advertising und Connections funktionieren zuverlässig
- **Gesamtsystem**: Produktionsreif ✅

### WiFi STA + Zigbee Router + BLE (Nicht empfohlen)
- **WiFi**: Normale Performance
- **Zigbee**: **Hohe Paketverlustrate** (10-30%), langsame Reaktionszeiten
- **BLE**: Normal
- **Gesamtsystem**: Nur für Tests geeignet ⚠️

## Espressif-Empfehlung für Zigbee Gateway

Für ein produktionsreifes **Zigbee Gateway** empfiehlt Espressif:

**Dual-SoC-Lösung mit separaten Antennen:**
- **ESP32-S3**: WiFi + BLE
- **ESP32-H2**: Zigbee (dedizierter 802.15.4 Radio)
- **Vorteil**: Simultaner WiFi- und Zigbee-Empfang ohne Zeitschlitz-Konflikt

**Für OpenSprinkler mit ESP32-C5:**
- Akzeptabel für **End Device**-Sensoren (stabiler Betrieb)
- Vermeiden Sie Zigbee-**Router**-Rolle bei hohem WiFi-Traffic

## Troubleshooting

### Problem: Zigbee Paketverluste
**Ursachen**:
- Zigbee als Router konfiguriert (statt End Device)
- Hoher WiFi-Datenverkehr (Downloads, Streaming)
- BLE-Verbindungen mit kurzen Intervallen

**Lösungen**:
- Zigbee auf End Device umstellen
- Polling-Intervall auf 60-120s erhöhen
- WiFi-Traffic reduzieren während Zigbee-Polling

### Problem: WiFi-Setup funktioniert nicht (SOFTAP-Modus)
**Symptom**: Kann nicht auf WiFi-Konfigurationsseite zugreifen

**Ursache**: Zigbee/BLE nicht automatisch deaktiviert

**Lösung**: 
1. Überprüfen Sie, ob `disable_zigbee_ble_for_softap()` in `espconnect.cpp` aufgerufen wird
2. Code sollte vor `WiFi.softAP()` ausgeführt werden
3. Nach WiFi-Setup sollte `reenable_zigbee_ble_after_softap()` aufgerufen werden

**Debug-Ausgabe überprüfen**:
```
[COEX] Disabling Zigbee/BLE for WiFi SOFTAP mode
[COEX] Stopping Zigbee
[COEX] Stopping BLE
... WiFi-Setup ...
[COEX] Re-enabling Zigbee/BLE after WiFi setup
[COEX] Restarting Zigbee
[COEX] Restarting BLE
```

### Problem: BLE Verbindungsprobleme
**Ursachen**:
- WiFi in SCAN- oder CONNECTING-Phase
- Zu viele Zigbee-Geräte gleichzeitig aktiv

**Lösungen**:
- BLE-Verbindungen außerhalb WiFi-Scan-Zeiten initiieren
- BLE-Connection-Interval erhöhen (>100ms)

### Problem: Hoher Speicherverbrauch
**Lösung**: Alle Memory-Optimization-Flags in `sdkconfig.defaults` aktivieren:
- `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y`
- WiFi-Buffer reduzieren (siehe Konfiguration oben)
- IRAM-Optimierungen deaktivieren (`CONFIG_ESP_WIFI_RX_IRAM_OPT=n`)

## API-Nutzung (Optional)

Für BLE-MESH-Anwendungen kann die Koexistenz-API verwendet werden:

```cpp
#include "esp_coexist.h"

// BLE MESH Status setzen
esp_coex_status_bit_clear(ESP_COEX_BLE_ST_MESH_CONFIG);
esp_coex_status_bit_set(ESP_COEX_BLE_ST_MESH_TRAFFIC);

// Status:
// - ESP_COEX_BLE_ST_MESH_CONFIG: Netzwerk wird provisioniert
// - ESP_COEX_BLE_ST_MESH_TRAFFIC: Daten werden übertragen
// - ESP_COEX_BLE_ST_MESH_STANDBY: Idle, keine signifikante Datenübertragung
```

**Hinweis**: Für normale WiFi+Zigbee+BLE-Szenarien ist **keine API-Nutzung erforderlich** - der ESP32-C5 wechselt automatisch zwischen Koexistenz-Status.

## Zusammenfassung

✅ **DO (Empfohlen)**:
- WiFi im STA-Modus verwenden
- Zigbee als End Device konfigurieren
- BLE aktivieren für Sensor-Kommunikation
- Polling-Intervalle ≥60s für Zigbee-Sensoren
- Speicher-Optimierungen in sdkconfig.defaults aktivieren

❌ **DON'T (Vermeiden)**:
- WiFi im SOFTAP-Modus + Zigbee Router
- Zigbee Router bei hohem WiFi-Traffic
- Kurze Polling-Intervalle (<10s) bei vielen Geräten
- Gleichzeitiger schwerer WiFi/BLE/Zigbee-Traffic

⚠️ **CONSIDER (Erwägen)**:
- Dual-SoC-Lösung (ESP32-S3 + ESP32-H2) für Zigbee Gateway mit >50 Geräten
- Dediziertes Zigbee-Netzwerk auf separatem ESP32-H2 bei kritischen Anwendungen
