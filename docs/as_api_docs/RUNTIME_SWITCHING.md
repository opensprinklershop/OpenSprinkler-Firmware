# Runtime Switching: Zigbee ↔ Matter

## Konzept

Wechselseitiger Betrieb von Zigbee und Matter durch Runtime-Umschaltung.

## Technische Limitierungen

### Hardware-Limitierungen (ESP32-C5)
- **Ein 802.15.4 Radio**: Kann nicht gleichzeitig als Zigbee Coordinator UND Matter Thread Router arbeiten
- **WiFi + Zigbee**: Möglich, aber RF-Koexistenz-Probleme auf 2.4GHz
- **WiFi + Matter WiFi**: Funktioniert (Matter nutzt WiFi-Stack)

### Software-Limitierungen
- **Flash-Größe**: Matter Stack (~1.5MB) + Zigbee Stack (~500KB) + App (~3MB) = ~5MB > 4MB verfügbar
- **RAM**: 400KB SRAM muss beide Stacks + Buffers unterstützen
- **Stack-Lifecycle**: Beide Stacks müssen sauber initialisiert/deinitialisiert werden können

## Mögliche Szenarien

### Szenario A: Matter WiFi + Zigbee (Gleichzeitig)
**Status**: ⚠️ Möglich mit Einschränkungen

**Vorteile**:
- Beide Protokolle aktiv
- Zigbee für Sensoren
- Matter für Smart Home Integration

**Nachteile**:
- RF-Interferenz (beide 2.4GHz)
- Erhöhter Stromverbrauch
- Komplexe Koexistenz-Konfiguration

**Implementation**:
```cpp
// platformio.ini
[env:espc5-12-hybrid]
build_flags = 
    -D ESP32C5
    -D ENABLE_MATTER           # Matter WiFi
    -D ZIGBEE_MODE_ZCZR        # Zigbee Coordinator
    -D MATTER_WIFI_ONLY        # Kein Matter Thread!
```

**defines.h**:
```cpp
// Erlaubt Matter WiFi + Zigbee (nicht Thread + Zigbee)
#if defined(ENABLE_MATTER) && defined(ZIGBEE_MODE_ZCZR) && !defined(MATTER_WIFI_ONLY)
  #error "Matter Thread and Zigbee cannot coexist. Use MATTER_WIFI_ONLY for hybrid mode."
#endif
```

### Szenario B: Runtime-Umschaltung Zigbee ↔ Matter Thread
**Status**: 🔄 Komplex, aber machbar

**Use Case**: 
- Tagsüber: Zigbee-Modus (Sensoren lesen)
- Nachts: Matter-Modus (Smart Home Integration)

**Implementation**:

**1. Runtime-Flag System**:
```cpp
// defines.h
#define RF_MODE_ZIGBEE  1
#define RF_MODE_MATTER  2

// Global state
extern uint8_t current_rf_mode;
```

**2. Mode-Switching-Funktion**:
```cpp
// opensprinkler_rf.cpp (neu)
#include "opensprinkler_rf.h"

uint8_t current_rf_mode = RF_MODE_ZIGBEE; // Default

bool switch_rf_mode(uint8_t new_mode) {
    if (current_rf_mode == new_mode) return true;
    
    // Shutdown current mode
    if (current_rf_mode == RF_MODE_ZIGBEE) {
        DEBUG_PRINTLN("RF: Stopping Zigbee...");
        #ifdef ZIGBEE_MODE_ZCZR
        // Zigbee-Stack stoppen (wenn API verfügbar)
        // esp_zb_scheduler_alarm_cancel_all();
        // esp_zb_stop();
        #endif
    } else if (current_rf_mode == RF_MODE_MATTER) {
        DEBUG_PRINTLN("RF: Stopping Matter...");
        #ifdef ENABLE_MATTER
        matter_shutdown();
        #endif
    }
    
    delay(1000); // RF-Hardware abklingen lassen
    
    // Start new mode
    if (new_mode == RF_MODE_ZIGBEE) {
        DEBUG_PRINTLN("RF: Starting Zigbee...");
        #ifdef ZIGBEE_MODE_ZCZR
        // Zigbee-Stack starten
        // esp_zb_platform_config_t config = {...};
        // esp_zb_init(&config);
        // esp_zb_start(false);
        #endif
    } else if (new_mode == RF_MODE_MATTER) {
        DEBUG_PRINTLN("RF: Starting Matter...");
        #ifdef ENABLE_MATTER
        matter_init();
        #endif
    }
    
    current_rf_mode = new_mode;
    return true;
}
```

**3. UI/API für Umschaltung**:
```cpp
// opensprinkler_server.cpp
// Neuer Endpoint: /rf?pw=xxx&mode=zigbee|matter
void server_rf_mode(OTF_PARAMS_DEF) {
    if(!process_password(OTF_PARAMS)) return;
    
    if (findKeyVal(FKV_SOURCE, tmp_buffer, TMP_BUFFER_SIZE, PSTR("mode"), true)) {
        uint8_t new_mode = 0;
        if (strcmp(tmp_buffer, "zigbee") == 0) {
            new_mode = RF_MODE_ZIGBEE;
        } else if (strcmp(tmp_buffer, "matter") == 0) {
            new_mode = RF_MODE_MATTER;
        } else {
            handle_return(HTML_DATA_OUTOFBOUND);
        }
        
        if (switch_rf_mode(new_mode)) {
            handle_return(HTML_SUCCESS);
        } else {
            handle_return(HTML_DATA_MISSING);
        }
    }
}
```

**4. Automatische Umschaltung (z.B. zeitgesteuert)**:
```cpp
// main.cpp in do_loop()
void check_rf_mode_schedule() {
    static unsigned long last_check = 0;
    unsigned long now = millis();
    
    if (now - last_check < 60000) return; // Alle 60s prüfen
    last_check = now;
    
    time_os_t curr_time = os.now_tz();
    int hour = hour_of_day(curr_time);
    
    // 6-22 Uhr: Zigbee (Sensoren aktiv)
    // 22-6 Uhr: Matter (Smart Home nachts)
    if (hour >= 6 && hour < 22) {
        if (current_rf_mode != RF_MODE_ZIGBEE) {
            DEBUG_PRINTLN("Auto-switching to Zigbee mode");
            switch_rf_mode(RF_MODE_ZIGBEE);
        }
    } else {
        if (current_rf_mode != RF_MODE_MATTER) {
            DEBUG_PRINTLN("Auto-switching to Matter mode");
            switch_rf_mode(RF_MODE_MATTER);
        }
    }
}
```

**5. Persistierung der Einstellung**:
```cpp
// OpenSprinkler.h - Neue Option
#define IOPT_RF_MODE  XXX  // Nächste freie IOPT-Nummer

// OpenSprinkler.cpp - Default
case IOPT_RF_MODE:
    iopts[i] = RF_MODE_ZIGBEE;  // Default Zigbee
    break;
```

### Szenario C: Build-Time-Auswahl (Empfohlen)
**Status**: ✅ Aktuell implementiert

**Vorteile**:
- Klare Trennung
- Kein Runtime-Overhead
- Geringerer Memory-Footprint

**Builds**:
- `pio run -e espc5-12` → Zigbee
- `pio run -e espc5-12-matter` → Matter WiFi

## Empfehlung

### Für die meisten Anwendungen:
**Build-Time-Auswahl** (aktuelles System)
- Einfacher
- Stabiler
- Weniger Speicher
- Flash zwei Firmwares je nach Bedarf

### Für fortgeschrittene Nutzer mit spezifischen Anforderungen:
**Szenario A: Matter WiFi + Zigbee gleichzeitig**
- Wenn Zigbee NUR für Sensoren (nicht Steuerung)
- Wenn genug RAM verfügbar
- Wenn RF-Koexistenz akzeptabel

**Warnung**: Erfordert sorgfältige RF-Koexistenz-Konfiguration in `sdkconfig.defaults`:
```ini
# RF Coexistence für WiFi + Zigbee
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n  # Spart RAM
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8
```

### NICHT empfohlen:
**Szenario B: Runtime-Switching**
- Zu komplex
- Sensor-Verbindungen gehen verloren
- Unvorhersehbares Verhalten bei Switching
- Debug-Albtraum

## Implementation-Aufwand

### Szenario A (Gleichzeitig):
- **Aufwand**: Mittel (~2-3 Tage)
- **Risiko**: Mittel (RF-Interferenz)
- **Nutzen**: Hoch (beide Features gleichzeitig)

**Schritte**:
1. Build-Guard in defines.h anpassen (MATTER_WIFI_ONLY erlauben)
2. RF-Koexistenz in sdkconfig.defaults konfigurieren
3. Memory-Profiling durchführen
4. Interferenz-Tests mit Zigbee-Devices

### Szenario B (Runtime-Switching):
- **Aufwand**: Hoch (~1-2 Wochen)
- **Risiko**: Hoch (Stack-Lifecycle, Stabilität)
- **Nutzen**: Fragwürdig

**Schritte**:
1. opensprinkler_rf.h/cpp erstellen
2. Beide Stacks im Flash (>4MB Problem lösen)
3. Stack-Init/Deinit-APIs implementieren
4. UI/API für Mode-Switching
5. Persistierung
6. Extensive Tests

## Fazit

**Praktikabelste Lösung**: 
- **Matter WiFi + Zigbee gleichzeitig** (Szenario A)
- Verzicht auf Matter Thread
- Fokus auf Stabilität

**Nächster Schritt**:
Wenn gewünscht, kann ich Szenario A implementieren (MATTER_WIFI_ONLY Flag + Hybrid-Build).
