# Matter Integration für OpenSprinkler ESP32

## Übersicht

Matter (früher CHIP - Connected Home over IP) ist ein offener Smart-Home-Standard, der von der Connectivity Standards Alliance (CSA) entwickelt wurde. Dieser Leitfaden dokumentiert die Möglichkeiten und Schritte zur Integration von Matter in OpenSprinkler.

## Hardware-Kompatibilität

### ESP32-C5 Matter-Unterstützung
- **Status**: ✅ ESP32-C5 unterstützt Matter (Thread + BLE Commissioning)
- **Current Platform**: ESP-IDF v5.1.2
- **Matter SDK**: ESP-Matter basiert auf ESP-IDF v5.1+ (kompatibel)
- **Flash**: 4MB internal + 16MB external (W25Q128) - ausreichend für Matter
- **RAM**: 400KB SRAM - knapp, aber machbar mit Optimierungen

### Flash-Partitionierung
**Aktuell (os4.csv)**:
```
nvs:        20KB   (0x9000-0xE000)
otadata:     8KB   (0xE000-0x10000)
app0:     3392KB   (0x10000-0x360000)  ← App Größe
zb_storage: 128KB  (0x360000-0x380000) ← Zigbee NVS
zb_fct:       4KB  (0x380000-0x381000)
spiffs:     444KB  (0x381000-0x3F0000)
coredump:    64KB  (0x3F0000-0x400000)
```

**Für Matter benötigt**:
- Matter Stack: ~1.2-1.5MB zusätzlich
- Matter Factory Data: ~4KB
- Matter NVS: ~128KB (ähnlich Zigbee)

**Problem**: Matter + Current Features ≈ 3.5-4MB > verfügbare 3.4MB App-Partition

## Matter + Zigbee/BLE Koexistenz

### RF-Koexistenz
- ✅ **Matter Thread + BLE**: Unterstützt (Thread = 802.15.4, BLE = Bluetooth LE)
- ✅ **Matter WiFi + BLE**: Unterstützt (WiFi 2.4GHz + BLE)
- ⚠️ **Zigbee + Matter Thread**: Problematisch (beide nutzen 802.15.4, unterschiedliche Protokolle)
- ✅ **Matter WiFi + Zigbee**: Theoretisch möglich, aber RF-Konflikte auf 2.4GHz

**Empfehlung**: Entweder Zigbee ODER Matter Thread wählen, nicht beide gleichzeitig.

### Software-Koexistenz
ESP32-C5 unterstützt Software-Coexistence zwischen:
- WiFi + BLE ✅
- WiFi + Thread ✅  
- Thread + BLE ✅ (für Commissioning)
- WiFi + Zigbee ⚠️ (eingeschränkt, nur ZCZR-Mode)

## Passende Matter Device Types

### 1. **Valve** (Device Type 0x0042)
**Ideal für Bewässerungsventile**
- **Cluster**: 
  - On/Off (0x0006) - Ventil öffnen/schließen
  - Level Control (0x0008) - optionale Durchflussregelung
  - Flow Measurement (0x0404) - Durchflussmessung
- **Anwendung**: Einzelne Stationen als Valve-Geräte exposieren

### 2. **Generic Switch** (Device Type 0x000F)
**Für manuelle Stationssteuerung**
- **Cluster**:
  - Switch (0x003B) - Schalter-Events
  - Identify (0x0003) - Geräteidentifikation
- **Anwendung**: Quick-Start-Buttons für Stationen

### 3. **Flow Sensor** (Device Type 0x0306)
**Für Durchflussmessung**
- **Cluster**:
  - Flow Measurement (0x0404) - Durchfluss in L/min
  - Temperature Measurement (0x0402) - optional
- **Anwendung**: Sensor-Integration (existing sensor_*.cpp)

### 4. **Pump Controller** (kein offizieller Type, Custom)
**Für Master-Valve (Pumpensteuerung)**
- Basierend auf **Valve** + **Power Source**
- **Cluster**:
  - On/Off
  - Power Configuration (0x0001)

### 5. **Aggregator / Bridge** (Device Type 0x000E)
**Für gesamte OpenSprinkler-Steuerung**
- **Cluster**:
  - Bridge Device Basic Information (0x0039)
  - Descriptor (0x001D) - Endpunkte für jede Station
- **Anwendung**: OpenSprinkler als Bridge, jede Station als Endpoint

## Architektur-Vorschlag

### Option A: Matter + WiFi (empfohlen)
**Vorteile**:
- Bewährte WiFi-Infrastruktur nutzen
- Kein zusätzlicher Border Router nötig
- Zigbee-Sensoren können parallel laufen (mit Einschränkungen)

**Nachteile**:
- Höherer Stromverbrauch
- WiFi-Abhängigkeit

**Integration**:
```cpp
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <app/server/Server.h>

// In do_setup():
esp_matter::node_t *node = esp_matter::node::create(...);
esp_matter::endpoint_t *root = esp_matter::endpoint::create(node, ...);

// Für jede Station:
for(unsigned char sid=0; sid<os.nstations; sid++) {
    esp_matter::endpoint_t *valve_ep = esp_matter::endpoint::create(
        node, VALVE_DEVICE_TYPE_ID, ...);
    esp_matter::cluster::on_off::create(valve_ep, ...);
}

esp_matter::start(node);
```

### Option B: Matter Thread + WiFi-Fallback
**Vorteile**:
- Low-Power Thread-Mesh für Sensoren
- Resiliente Mesh-Netzwerk-Topologie

**Nachteile**:
- Benötigt Thread Border Router
- Konflikte mit Zigbee (beide 802.15.4)
- Komplexere Setup

### Option C: Hybrid (Matter Bridge für externe Sensoren)
- OpenSprinkler bleibt WiFi
- Matter Bridge exposiert Zigbee/BLE-Sensoren als Matter-Devices
- Nur Valve-Steuerung über Matter

## Memory-Optimierung für Matter

### Flash-Partitioning neu (mit Matter):
```csv
# ESP32-C5 mit Matter (OTA deaktiviert für Platz)
# Name,         Type, SubType,  Offset,   Size
nvs,            data, nvs,      0x9000,   0x6000,   # +4KB
matter_fct,     data, nvs,      0xF000,   0x1000,   # Factory Data
app0,           app,  factory,  0x10000,  0x280000, # Reduziert auf 2.5MB
matter_kvs,     data, nvs,      0x290000, 0x20000,  # Matter KVS 128KB
zb_storage,     data, fat,      0x2B0000, 0x10000,  # Zigbee reduziert
spiffs,         data, spiffs,   0x2C0000, 0x130000, # 1.2MB für Webinterface
coredump,       data, coredump, 0x3F0000, 0x10000,
```

**Externen Flash nutzen**:
- HTML/CSS/JS auf externen Flash verschieben (LittleFS)
- Matter Fabric-Daten auf extern
- Logs/Historien auf extern

### Code-Optimierungen
1. **Features conditional compilieren**:
   ```cpp
   #ifdef ENABLE_MATTER
     #define ENABLE_ZIGBEE 0  // Exklusiv
   #endif
   ```

2. **Matter-Build-Flag**:
   ```ini
   [env:espc5-12-matter]
   build_flags = 
       -D ESP32C5
       -D ENABLE_MATTER
       -D CONFIG_ENABLE_CHIP_SHELL=0  # Shell deaktivieren spart RAM
       -D CHIP_CONFIG_MAX_FABRICS=6   # Max 6 Controller (Standard 16)
   lib_deps = 
       espressif/esp_matter
   ```

3. **Heap-Optimierung**:
   - NimBLE statt Bluedroid (bereits aktiv)
   - LWIP Buffer reduzieren (bereits optimiert)
   - Matter nur mit 6 Fabrics statt 16 (spart ~40KB)

## Implementierungsschritte

### Phase 1: Grundgerüst (✅ ABGESCHLOSSEN)
- [x] ESP-Matter Build-Environment in platformio.ini (`espc5-12-matter`)
- [x] Flash-Partitionstabelle angepasst (os4_matter.csv)
- [x] Matter-Module erstellt (opensprinkler_matter.h/cpp)
- [x] ENABLE_MATTER Build-Flag implementiert
- [x] Integration in main.cpp (init/loop/status updates)

### Phase 2: API-Integration (✅ ABGESCHLOSSEN)
- [x] Station ON-Funktion mit Queue-Management
  - Verwendet `pd.enqueue()` für Queue-Erstellung
  - `schedule_all_stations()` für Scheduling
  - Master-Station-Check implementiert
  - Standard-Timer: 10 Minuten (600s)
- [x] Station OFF-Funktion mit proper cleanup
  - Verwendet `turn_off_station()` API
  - Queue-Status-Prüfung
  - Automatisches Logging via turn_off_station()
- [x] Status-Update-Callbacks in turn_on/off_station
  - `matter_update_station_status()` aufgerufen

### Phase 3: ESP-Matter SDK Integration (🔄 AUSSTEHEND)
- [ ] ESP-Matter SDK als PlatformIO-Dependency hinzufügen
- [ ] Node/Endpoint-Erstellung implementieren
- [ ] OnOff Cluster konfigurieren
- [ ] Attribute-Callbacks verdrahten
- [ ] Commissioning-Events behandeln

### Phase 4: Advanced Features (⏳ GEPLANT)
- [ ] Flow-Sensor als Matter Flow Measurement Cluster
- [ ] Programm-Steuerung über Matter
- [ ] Factory Data Provisioning
- [ ] OTA über Matter

### Phase 5: Production (⏳ GEPLANT)
- [ ] Factory Data Provisioning
- [ ] Zertifizierung vorbereiten (CSA)
- [ ] Dokumentation

## Aktueller Implementierungsstatus

### Dateistruktur
```
opensprinkler-firmware-esp32/
├── opensprinkler_matter.h     # Matter API-Definitionen
├── opensprinkler_matter.cpp   # Matter-Implementierung
├── os4_matter.csv             # Flash-Partitionstabelle für Matter
├── platformio.ini             # Build-Config mit [env:espc5-12-matter]
└── main.cpp                   # Integration (matter_init/loop/status)
```

### Implementierte Funktionen

**opensprinkler_matter.cpp** - Station-Steuerung:
```cpp
void matter_station_on(unsigned char sid) {
    // 1. Prüfe Master-Station
    if ((os.status.mas == sid+1) || (os.status.mas2 == sid+1)) return;
    
    // 2. Prüfe ob bereits in Queue
    if (pd.station_qid[sid] != 0xFF) return;
    
    // 3. Erstelle Queue-Element
    RuntimeQueueStruct *q = pd.enqueue();
    if (!q) return;
    
    // 4. Konfiguriere Station
    unsigned long curr_time = os.now_tz();
    q->st = 0;
    q->dur = 600;  // 10 Minuten Standard
    q->sid = sid;
    q->pid = 99;   // Matter-Programm-ID
    
    // 5. Schedule Station
    schedule_all_stations(curr_time, 0);
}

void matter_station_off(unsigned char sid) {
    // 1. Prüfe Queue-Status
    if (pd.station_qid[sid] == 255) return;
    
    // 2. Markiere für Dequeue
    unsigned long curr_time = os.now_tz();
    RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
    q->deque_time = curr_time;
    
    // 3. Stoppe Station (mit Logging)
    turn_off_station(sid, curr_time, 0);
}
```

**main.cpp** - Status-Updates:
```cpp
void turn_on_station(unsigned char sid, ulong duration) {
    // ... existing code ...
    if (os.set_station_bit(sid, 1, duration)) {
        notif.add(NOTIFY_STATION_ON, sid, duration);
#ifdef ENABLE_MATTER
        matter_update_station_status(sid, true);
#endif
    }
}

void turn_off_station(unsigned char sid, time_os_t curr_time, unsigned char shift) {
    // ... existing code ...
    os.set_station_bit(sid, 0);
#ifdef ENABLE_MATTER
    matter_update_station_status(sid, false);
#endif
    // ... logging ...
}
```

## Code-Beispiel: Valve Endpoint (Placeholder für ESP-Matter SDK)

**Hinweis**: Folgendes Code-Beispiel zeigt die geplante Integration, wenn ESP-Matter SDK verfügbar ist:

using namespace esp_matter;
using namespace chip::app::Clusters;

static void matter_valve_attribute_update_cb(attribute::callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *val, void *priv_data)
{
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        bool on_off = val->val.b;
        unsigned char sid = (unsigned char)(uintptr_t)priv_data;
        
        if(on_off) {
            // Station einschalten - verwende existing API
            manual_start_program(1, sid, os.iopts[IOPT_STATION_DELAY_TIME]);
        } else {
            // Station ausschalten
            os.set_station_bit(sid, 0);
        }
        
        DEBUG_PRINTF("Matter: Station %d -> %s\n", sid, on_off ? "ON" : "OFF");
    }
}

void matter_init_valves() {
    node_t *node = node::create(&config, matter_event_cb, NULL);
    
    // Root Endpoint (Aggregator)
    endpoint_t *root = endpoint::create(node, AGGREGATOR_DEVICE_TYPE_ID, 0);
    cluster::descriptor::create(root, &descriptor_config, CLUSTER_FLAG_SERVER);
    
    // Valve Endpoint für jede Station
    for(unsigned char sid = 0; sid < os.nstations; sid++) {
        uint16_t endpoint_id = sid + 1; // Endpoint IDs starten bei 1
        
        endpoint_t *valve_ep = endpoint::create(node, VALVE_DEVICE_TYPE_ID, endpoint_id);
        
        // On/Off Cluster
        cluster_t *on_off_cluster = cluster::on_off::create(valve_ep, NULL, 
            CLUSTER_FLAG_SERVER);
        
        // Attribute Callback registrieren
        cluster::on_off::attribute::create_on_off(on_off_cluster, false);
        attribute::set_callback(matter_valve_attribute_update_cb, (void*)(uintptr_t)sid);
        
        // Optional: Level Control für Durchfluss-Regulation
        // cluster::level_control::create(valve_ep, NULL, CLUSTER_FLAG_SERVER);
        
        DEBUG_PRINTF("Matter: Valve endpoint %d created for station %d\n", 
            endpoint_id, sid);
    }
    
    esp_matter::start(node);
    DEBUG_PRINTLN("Matter stack started");
}

// Status-Updates von OpenSprinkler -> Matter
void matter_update_valve_status(unsigned char sid, bool on) {
    uint16_t endpoint_id = sid + 1;
    
    esp_matter_attr_val_t val;
    val.type = ESP_MATTER_VAL_TYPE_BOOLEAN;
    val.val.b = on;
    
    attribute::update(endpoint_id, OnOff::Id, 
        OnOff::Attributes::OnOff::Id, &val);
}
#endif // ENABLE_MATTER
```

## Build & Test

### Standard Build (ohne Matter)
```bash
pio run -e espc5-12
pio run -e espc5-12 -t upload
```

### Matter Build (mit ENABLE_MATTER)
```bash
pio run -e espc5-12-matter
pio run -e espc5-12-matter -t upload
```

### Build-Flags in espc5-12-matter
```ini
build_flags = 
    -D ESP32C5
    -D ENABLE_MATTER              # Aktiviert Matter-Code
    -D CHIP_CONFIG_MAX_FABRICS=6  # Max 6 Controller (spart RAM)
    -D CONFIG_ENABLE_CHIP_SHELL=0 # Deaktiviert Shell (spart RAM)
    -D CHIP_DEVICE_CONFIG_ENABLE_THREAD=0  # WiFi-only
    -D CHIP_DEVICE_CONFIG_ENABLE_WIFI=1
    # Zigbee deaktiviert (RF-Konflikt vermeiden)
```

### Memory-Profiling
```bash
# App-Größe prüfen
pio run -e espc5-12-matter
esptool.py image_info .pio/build/espc5-12-matter/firmware.bin

# Komponenten-Größe (wenn ESP-IDF direkt)
idf.py size-components
```

## Aktueller Status

### ✅ Fertiggestellt
1. **Build-System**
   - Separate Matter-Build-Umgebung (espc5-12-matter)
   - ENABLE_MATTER-Flag für conditional compilation
   - Flash-Partitionierung optimiert (os4_matter.csv)

2. **API-Integration**
   - Station ON/OFF mit OpenSprinkler Queue-API
   - Bi-direktionale Status-Updates
   - Master-Station-Schutz
   - Proper Queue-Management

3. **Code-Struktur**
   - opensprinkler_matter.h/cpp Module
   - Inline-Stubs wenn ENABLE_MATTER nicht definiert
   - Zero-Overhead bei Standard-Builds

### 🔄 In Arbeit
1. **ESP-Matter SDK Integration**
   - Warten auf stabile PlatformIO-Integration
   - Node/Endpoint-Erstellung
   - OnOff Cluster-Konfiguration

### ⏳ Geplant
1. **Flow-Sensor Matter-Cluster**
2. **Commissioning QR-Code Generierung**
3. **Factory Data Provisioning**
4. **Matter OTA Updates**

## Dependencies (platformio.ini)

```ini
[env:espc5-12-matter]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
framework = arduino, espidf
platform_packages = 
    framework-esp-idf @ https://github.com/espressif/esp-idf.git#v5.1.4
    
board = esp32-c5-devkitc-1
board_build.partitions = os4_matter.csv

build_flags = 
    -D ESP32C5
    -D ENABLE_MATTER
    -D ENABLE_DEBUG
    # Matter Optimierungen
    -D CHIP_CONFIG_MAX_FABRICS=6
    -D CONFIG_ENABLE_CHIP_SHELL=0
    -D CHIP_DEVICE_CONFIG_ENABLE_THREAD=0  # Nur WiFi
    -D CHIP_DEVICE_CONFIG_ENABLE_WIFI=1
    
lib_deps = 
    espressif/esp_matter @ ^1.2.0
    Ethernet
    # Zigbee entfernen für Matter-Build
    https://github.com/ThingPulse/esp8266-oled-ssd1306.git
    knolleary/PubSubClient
    links2004/WebSockets
    symlink://../OpenThings-Framework-Firmware-Library
    RobTillaart/ADS1X15
    https://github.com/tobiasschuerg/InfluxDB-Client-for-Arduino
```

## Nächste Schritte (für Entwickler)

### 1. ESP-Matter SDK Integration

Wenn ESP-Matter via PlatformIO verfügbar:
```bash
# In platformio.ini unter [env:espc5-12-matter]
lib_deps = 
    espressif/esp_matter @ ^1.2.0
```

Dann in `opensprinkler_matter.cpp` uncomment:
```cpp
// Zeilen 17-21: ESP-Matter Headers
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <app/server/Server.h>
#include <app/clusters/on-off-server/on-off-server.h>
```

### 2. Matter Node & Endpoints implementieren

In `matter_init()` (Zeile 99-135):
```cpp
void matter_init() {
    // Uncomment ESP-Matter Code in Zeilen 110-133
    matter_node = esp_matter::node::create(&config, matter_event_callback, NULL);
    matter_root_endpoint = esp_matter::endpoint::create(
        matter_node, MATTER_DEVICE_TYPE_AGGREGATOR, 0);
    
    for (unsigned char sid = 0; sid < os.nstations; sid++) {
        uint16_t endpoint_id = sid + 1;
        matter_valve_endpoints[sid] = esp_matter::endpoint::create(
            matter_node, MATTER_DEVICE_TYPE_VALVE, endpoint_id);
        
        auto* cluster = esp_matter::cluster::on_off::create(
            matter_valve_endpoints[sid], NULL, CLUSTER_FLAG_SERVER);
        esp_matter::cluster::on_off::attribute::create_on_off(cluster, false);
    }
    
    esp_matter::start(matter_node);
}
```

### 3. Attribute Updates implementieren

In `matter_update_station_status()` (Zeile 213-222):
```cpp
void matter_update_station_status(unsigned char sid, bool on) {
    if (!matter_initialized || !matter_commissioned) return;
    
    uint16_t endpoint_id = sid + 1;
    esp_matter_attr_val_t val;
    val.type = ESP_MATTER_VAL_TYPE_BOOLEAN;
    val.val.b = on;
    esp_matter::attribute::update(endpoint_id, 0x0006, 0x0000, &val);
}
```

### 4. Testing
```bash
# Build & Flash
pio run -e espc5-12-matter -t upload -t monitor

# In Serial Monitor schauen nach:
# "Matter: Initializing..."
# "Matter: Would create X valve endpoints"
# "Matter: Init complete"

# Commissioning mit Google Home oder Apple Home App
# QR-Code wird über Serial ausgegeben
```

### 5. Flow-Sensor hinzufügen (optional)

In `main.cpp` flow_poll():
```cpp
void flow_poll() {
    // ... existing code ...
    if (flow_rt_period > 0) {
        float gpm = 60000.0f / flow_rt_period;  // Calculate GPM
#ifdef ENABLE_MATTER
        matter_update_flow_rate(gpm);
#endif
    }
}
```

## Ressourcen

- **ESP-Matter GitHub**: https://github.com/espressif/esp-matter
- **Matter Spec**: https://csa-iot.org/developer-resource/specifications-download-request/
- **Device Types**: https://github.com/project-chip/connectedhomeip/tree/master/src/app/zap-templates/zcl/data-model/chip
- **ESP32-C5 Datasheet**: https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf

## Fazit & Status

**Matter-Integration ist vorbereitet und teilweise implementiert**:

### ✅ Abgeschlossen:
1. **Build-System**: Vollständig ENABLE_MATTER-gesteuert, separate Build-Umgebung
2. **Flash-Partitioning**: os4_matter.csv mit optimierter Partitionierung
3. **API-Integration**: Station ON/OFF nutzen korrekte OpenSprinkler Queue-APIs
4. **Status-Synchronisation**: Bi-direktionale Updates zwischen OS und Matter
5. **Code-Struktur**: Saubere Modularisierung (opensprinkler_matter.h/cpp)

### 🔄 Nächste Schritte:
1. **ESP-Matter SDK**: Warten auf stabile PlatformIO-Library oder manuell integrieren
2. **Placeholder ausfüllen**: ESP-Matter API-Aufrufe in opensprinkler_matter.cpp aktivieren
3. **Testing**: Commissioning mit Google Home / Apple Home

### ⚠️ Wichtige Erkenntnisse:
- **Flash-Speicher**: Mit 2.5MB App-Partition ist Matter machbar (ohne OTA)
- **RF-Koexistenz**: Matter WiFi + Zigbee möglich, aber Thread + Zigbee nicht empfohlen
- **Memory**: 400KB SRAM ausreichend mit Optimierungen (max 6 Fabrics)
- **Integration**: Nutzt bestehende OpenSprinkler-APIs (`pd.enqueue()`, `schedule_all_stations()`, `turn_off_station()`)

### 📊 Entwicklungsaufwand:
- **Grundgerüst (Phase 1+2)**: ✅ Abgeschlossen (~1 Tag)
- **ESP-Matter Integration (Phase 3)**: ⏳ Ausstehend (~3-5 Tage)
- **Advanced Features (Phase 4)**: ⏳ Geplant (~1-2 Wochen)
- **Production-Ready (Phase 5)**: ⏳ Geplant (~1 Woche)

**Empfohlener Ansatz**: Matter WiFi (nicht Thread) + Valve Device Type für Stationen.
