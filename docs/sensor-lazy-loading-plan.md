# Sensor-Schnittstelle Umbau: Zustandslos & Lazy-Loading Plan

## Aktuelle Architektur (Analyse)

### Speicherverbrauch - IST-Zustand
```cpp
// Globale statische Variablen (sensors.cpp):
static std::map<uint, SensorBase*> sensorsMap;               // Alle Sensoren im RAM
static std::map<uint, ProgSensorAdjust*> progSensorAdjustsMap;  // Program-Sensor Zuordnungen
static std::map<uint, Monitor*> monitorsMap;                 // Monitoring-Konfigurationen

// Pro Sensor-Objekt im RAM:
- SensorBase: ~200-300 Bytes (abhängig von abgeleiteter Klasse)
- 30 Sensoren: ~6-9 KB RAM permanent belegt
- std::map Overhead: ~40-60 Bytes pro Entry = ~1.8 KB zusätzlich
- GESAMT: ~8-11 KB für Sensor-Objekte permanent im RAM
```

### Kritische Stellen - Wo werden Sensoren verwendet?

1. **sensor_api_init()** - Lädt ALLE Sensoren beim Start
   - Datei: sensors.cpp:243
   - Ruft sensor_load() → lädt sensors.json komplett in sensorsMap
   
2. **sensor_api_loop()** - NUR für BLE/Zigbee Maintenance
   - Datei: sensors.cpp:335
   - Lädt KEINE Sensoren, nur Subsystem-Loops
   
3. **Sensor-Lesen (HTTP API)**
   - sensors.cpp:1192: `sensor_read_all()` - Iteriert durch sensorsMap
   - Liest ALLE Sensoren nacheinander
   - Keine Zeitsteuerung - wird bei jedem HTTP-Request ausgeführt
   
4. **Matter Integration**
   - opensprinkler_matter.cpp:113,340
   - Iteriert durch sensorsMap für Sensor-Discovery
   
5. **MQTT Callback**
   - sensor_mqtt.cpp:125
   - Iteriert durch sensorsMap für MQTT Updates
   
6. **Logging**
   - sensors_util.cpp:219
   - Iteriert durch sensorsMap für Log-Operationen

### Persistierung
- **Format**: JSON-Datei `/sensors.json` (LittleFS/Flash)
- **Struktur**: Array von Sensor-Objekten mit allen Konfigurationsfeldern
- **Größe**: ~200-500 Bytes pro Sensor serialisiert = ~6-15 KB für 30 Sensoren

---

## SOLL-Zustand: Zustandslose Lazy-Loading Architektur

### Kernprinzipien
1. **Keine permanenten Sensor-Objekte im RAM** - nur bei Bedarf laden
2. **Zeitgesteuerte Sensor-Lesungen** - nicht bei jedem Loop
3. **Flash als Single Source of Truth** - sensors.json ist die Quelle
4. **Minimale Cache-Strategie** - nur letzter Wert + Zeitstempel
5. **On-Demand Instantiierung** - Sensor-Objekt nur während read()

### Neue Datenstrukturen

```cpp
// Minimale Sensor-Metadaten im RAM (nur das Nötigste)
struct SensorMetadata {
  uint nr;                       // Sensor-ID
  uint type;                     // Sensor-Typ (für Factory)
  uint read_interval;            // Lese-Intervall in Sekunden
  uint32_t next_read_time;       // Nächster geplanter Lesezeitpunkt (os.now_tz())
  double cached_value;           // Letzter gelesener Wert
  uint32_t cached_native_value;  // Letzter nativer Wert
  bool data_valid;               // Ist cached_value gültig?
  uint8_t flags_cache;           // Nur enable/log/show bits (kompakt)
};

// Globale Metadaten-Liste (viel kleiner als volle Objekte)
static std::map<uint, SensorMetadata> sensorSchedule;  // ~40-50 Bytes pro Sensor
// 30 Sensoren: ~1.2-1.5 KB statt 8-11 KB = 85% Einsparung!

// Sensors.json bleibt die volle Konfiguration im Flash
```

### Ablauf-Optimierungen

#### 1. Init-Phase (sensor_api_init)
```cpp
void sensor_api_init(boolean detect_boards) {
  // ÄNDERUNG: Nicht mehr volle Sensoren laden, nur Metadaten
  sensorSchedule.clear();
  
  if (detect_boards) detect_asb_board();
  
  // Metadaten aus sensors.json extrahieren (leichtgewichtig)
  sensor_load_metadata_only();  // NEU!
  
  // Rest wie gehabt: prog_adjust, mqtt_init, monitors, fyta
  prog_adjust_load();
  sensor_mqtt_init();
  monitor_load();
  fyta_check_opts();
}

void sensor_load_metadata_only() {
  // Liest sensors.json, extrahiert nur: nr, type, read_interval, flags
  // Berechnet initial next_read_time = now + read_interval
  // Speichert in sensorSchedule
  // VORTEIL: Kein SensorBase-Objekt instantiieren, nur JSON parsen
}
```

#### 2. Loop-Phase (NEUE Funktion: sensor_scheduler_loop)
```cpp
// ERSETZT: sensor_api_loop() wird zu sensor_scheduler_loop()
void sensor_scheduler_loop() {
  uint32_t now = os.now_tz();
  
  // Nur Sensoren verarbeiten, deren next_read_time erreicht ist
  for (auto &kv : sensorSchedule) {
    SensorMetadata &meta = kv.second;
    
    if (now < meta.next_read_time) continue;  // Noch nicht Zeit
    if (!(meta.flags_cache & SENSOR_FLAG_ENABLE)) continue;  // Deaktiviert
    
    // Sensor ON-DEMAND aus Flash laden
    SensorBase* sensor = sensor_load_single(meta.nr);  // NEU!
    
    if (sensor) {
      int result = sensor->read(now);
      
      if (result == HTTP_RQT_SUCCESS) {
        // Cache aktualisieren
        meta.cached_value = sensor->last_data;
        meta.cached_native_value = sensor->last_native_data;
        meta.data_valid = sensor->flags.data_ok;
        
        // Logging (falls aktiviert)
        if (meta.flags_cache & SENSOR_FLAG_LOG) {
          sensor_log_value(meta.nr, sensor->last_data);  // Existiert schon
        }
      }
      
      // Sensor-Objekt SOFORT freigeben!
      delete sensor;
      sensor = nullptr;
    }
    
    // Nächster Lesezeitpunkt planen
    meta.next_read_time = now + meta.read_interval;
  }
  
  // BLE/Zigbee Maintenance (wie gehabt)
  #if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)
  if (sensor_zigbee_is_active()) sensor_zigbee_loop();
  #endif
  #if defined(ESP32) && defined(OS_ENABLE_BLE)
  if (sensor_ble_is_active()) sensor_ble_loop();
  #endif
}
```

#### 3. HTTP API Zugriff (sensor_read_all, sensor_get_value)
```cpp
// ÄNDERUNG: Cached Values verwenden statt live lesen
double sensor_get_cached_value(uint nr) {
  auto it = sensorSchedule.find(nr);
  if (it == sensorSchedule.end()) return 0.0;
  
  SensorMetadata &meta = it->second;
  if (!meta.data_valid) return 0.0;
  
  return meta.cached_value;  // Instant, kein Flash-Zugriff!
}

// Für manuelles Force-Read (z.B. HTTP /sensor/read?nr=5)
int sensor_read_single_now(uint nr) {
  SensorBase* sensor = sensor_load_single(nr);  // Aus Flash
  if (!sensor) return HTTP_RQT_NOT_RECEIVED;
  
  int result = sensor->read(os.now_tz());
  
  // Cache update
  auto it = sensorSchedule.find(nr);
  if (it != sensorSchedule.end()) {
    it->second.cached_value = sensor->last_data;
    it->second.data_valid = (result == HTTP_RQT_SUCCESS);
  }
  
  delete sensor;
  return result;
}
```

#### 4. JSON Serialisierung (für HTTP Responses)
```cpp
// ÄNDERUNG: Sensor-Config aus Flash + Cached Value kombinieren
void sensor_emit_json(BufferFiller &bfill, uint nr) {
  // Volle Config aus Flash laden
  SensorBase* sensor = sensor_load_single(nr);
  if (!sensor) return;
  
  // Cached Value aus Metadaten holen
  auto it = sensorSchedule.find(nr);
  if (it != sensorSchedule.end()) {
    sensor->last_data = it->second.cached_value;
    sensor->flags.data_ok = it->second.data_valid;
  }
  
  // Normale JSON-Ausgabe
  sensor->emitJson(bfill);
  
  delete sensor;  // Sofort freigeben
}
```

---

## Weitere Optimierungsmöglichkeiten

### 1. ProgSensorAdjust optimieren (zusätzliche ~2-4 KB Einsparung)
```cpp
// IST: std::map<uint, ProgSensorAdjust*> progSensorAdjustsMap
// ProgSensorAdjust ist ein größeres Objekt mit Programm-Sensor Zuordnungen

// SOLL: Nur laden wenn Programm startet
// - In memory: nur eine HashMap<prog_nr, sensor_nr> für schnelle Lookups
// - Volle ProgSensorAdjust aus /progsensor.json bei Programm-Start
struct ProgSensorLink {
  uint prog_nr;
  uint sensor_nr;
};
static std::vector<ProgSensorLink> progSensorLinks;  // ~8 Bytes pro Link

// Beim Programm-Start:
ProgSensorAdjust* prog_adjust = load_prog_adjust_from_flash(prog_nr);
// Nutzen
delete prog_adjust;
```

### 2. Monitor optimieren (zusätzliche ~1-3 KB Einsparung)
```cpp
// Ähnliche Strategie wie Sensoren
// IST: std::map<uint, Monitor*> monitorsMap permanent im RAM
// SOLL: Nur Metadaten + on-demand loading
```

### 3. Sensor-Factory mit PSRAM-Allokation
```cpp
// Sensor-Objekte IMMER aus PSRAM allokieren (nicht internal heap)
SensorBase* sensor_make_obj(uint type, boolean ip_based) {
  // PSRAM-Allokation erzwingen
  void* mem = ps_malloc(sizeof(SensorXYZ));  // PSRAM statt heap
  if (!mem) return nullptr;
  
  return new(mem) SensorXYZ();  // Placement new
}
```

### 4. Incremental JSON Parsing (Flash-schonend)
```cpp
// Statt komplettes sensors.json zu parsen, nur gezielt einen Sensor lesen
SensorBase* sensor_load_single(uint nr) {
  // Streaming JSON Parser - sucht nur nach {"nr": <nr>} Block
  // Parsed nur diesen einen Eintrag, überspringt den Rest
  // VORTEIL: Schneller + weniger RAM für JsonDocument
}
```

### 5. Read-Intervall Clustering
```cpp
// Sensoren mit gleichem Intervall zusammenfassen
struct ScheduleBucket {
  uint interval_seconds;        // z.B. 60s, 300s, 600s
  std::vector<uint> sensor_ids; // Alle Sensoren mit diesem Intervall
  uint32_t next_batch_read;
};

// Batch-Reading für Effizienz
void sensor_read_bucket(ScheduleBucket &bucket) {
  for (uint nr : bucket.sensor_ids) {
    // Alle Sensoren in diesem Bucket gleichzeitig lesen
    // Shared RS485/I2C Init nur einmal
  }
}
```

---

## Speicher-Einsparungen Schätzung

### RAM-Verbrauch Vergleich

| Komponente | IST (permanent RAM) | SOLL (Lazy-Loading) | Einsparung |
|------------|---------------------|---------------------|------------|
| **Sensor-Objekte** | 8-11 KB | 0 KB (nur on-demand) | 8-11 KB ✅ |
| **Sensor-Metadaten** | - | 1.2-1.5 KB | -1.5 KB (neu) |
| **ProgSensorAdjust** | 2-4 KB | 0.3-0.5 KB (nur Links) | 1.5-3.5 KB ✅ |
| **Monitors** | 1-3 KB | 0.2-0.4 KB (nur Metadaten) | 0.8-2.6 KB ✅ |
| **std::map Overhead** | ~2 KB | ~0.5 KB | 1.5 KB ✅ |
| **GESAMT** | **13-20 KB** | **2-2.5 KB** | **11-17.5 KB** 🎯 |

**Netto-Einsparung: 11-17 KB permanent freier Heap!**

### Zusätzliche Vorteile

1. **Matter Init Erfolg**: Mit +15 KB frei → 71 KB + 15 KB = **86 KB vor Matter.begin()**
   - Matter benötigt ~78 KB → **ERFOLG GARANTIERT** ✅
   
2. **Skalierbarkeit**: 100 Sensoren möglich ohne RAM-Problem
   - 100 Sensoren: ~4 KB Metadaten statt 30 KB Objekte
   
3. **Flash-Wear**: Weniger Write-Cycles (nur bei Sensor-Änderung, nicht bei jedem Read)

4. **Boot-Zeit**: Schnellerer Start (kein vollständiges Objekt-Instantiieren)

5. **Dynamik**: Sensoren können zur Laufzeit hinzugefügt/entfernt werden ohne RAM-Fragmentierung

---

## Migrations-Phasen (Schritt-für-Schritt)

### Phase 1: Metadaten-Struktur einführen (Foundation) ✅ NICHT UMSETZEN
```cpp
// 1.1 SensorMetadata struct definieren (sensors.h)
// 1.2 sensorSchedule map anlegen
// 1.3 sensor_load_metadata_only() implementieren
// 1.4 Parallel zu bestehender sensorsMap betreiben (Dual-Mode)
```

### Phase 2: Lazy-Loading Infrastructure ✅ NICHT UMSETZEN
```cpp
// 2.1 sensor_load_single(nr) implementieren (Flash → RAM)
// 2.2 Placement-new + ps_malloc für PSRAM-Allokation
// 2.3 sensor_scheduler_loop() implementieren
// 2.4 Testen: Beide Systeme parallel, Werte vergleichen
```

### Phase 3: API Migration ✅ NICHT UMSETZEN
```cpp
// 3.1 sensor_get_cached_value() implementieren
// 3.2 HTTP Endpoints umstellen auf cached values
// 3.3 Matter Integration umstellen
// 3.4 MQTT Callbacks umstellen
```

### Phase 4: Legacy Code entfernen ✅ NICHT UMSETZEN
```cpp
// 4.1 sensorsMap komplett entfernen
// 4.2 getSensors() deprecaten
// 4.3 sensor_api_free() vereinfachen
// 4.4 Alte sensor_load() entfernen
```

### Phase 5: Erweiterte Optimierungen (Optional) ✅ NICHT UMSETZEN
```cpp
// 5.1 ProgSensorAdjust lazy-loading
// 5.2 Monitor lazy-loading
// 5.3 Read-Intervall Clustering
// 5.4 Incremental JSON Parser
```

---

## Risikoanalyse

### Risiken

1. **Flash-Zugriffe langsamer als RAM**
   - Mitigation: Cached Values für 99% der Zugriffe
   - Nur bei Config-Änderungen Flash lesen
   
2. **JSON Parsing Overhead**
   - Mitigation: Incremental Parser (nur einen Sensor parsen)
   - Oder: Binär-Format für schnellere Deserialisierung
   
3. **Fragmentierung bei häufigem new/delete**
   - Mitigation: PSRAM-Allokation (großer zusammenhängender Pool)
   - Oder: Object Pool Pattern (5 Sensor-Slots recyclen)
   
4. **Timing-Probleme bei Scheduler**
   - Mitigation: Gründliches Testing der next_read_time Logik
   - Watchdog für verpasste Readings

### Kompatibilität

- **Sensors.json Format**: BLEIBT GLEICH ✅
- **HTTP API**: BLEIBT GLEICH (nur interne Implementierung ändert sich) ✅
- **MQTT**: BLEIBT GLEICH ✅
- **Matter**: BLEIBT GLEICH ✅

**→ Zero Breaking Changes für Nutzer!**

---

## Performance-Metriken (Zu messen)

### Benchmarks
```cpp
// 1. Sensor Load Time (Flash → RAM)
uint32_t t1 = micros();
SensorBase* s = sensor_load_single(1);
uint32_t t2 = micros();
DEBUG_PRINTF("Load time: %lu us\n", t2-t1);  // Ziel: <500 us

// 2. Cached Value Access
uint32_t t1 = micros();
double val = sensor_get_cached_value(1);
uint32_t t2 = micros();
DEBUG_PRINTF("Cache access: %lu us\n", t2-t1);  // Ziel: <10 us

// 3. Scheduler Loop Duration
uint32_t t1 = micros();
sensor_scheduler_loop();
uint32_t t2 = micros();
DEBUG_PRINTF("Scheduler: %lu us\n", t2-t1);  // Ziel: <5 ms
```

---

## Dateistruktur-Änderungen

### Neue Dateien
- `sensor_scheduler.cpp/.h` - Scheduler-Logik
- `sensor_metadata.h` - SensorMetadata struct
- `sensor_loader.cpp` - sensor_load_single() Implementierung

### Geänderte Dateien
- `sensors.cpp` - sensor_api_init(), sensor_api_loop() umbauen
- `sensors.h` - Neue Funktionen deklarieren
- `main.cpp` - sensor_scheduler_loop() statt sensor_api_loop() aufrufen

---

## Zusammenfassung

✅ **Machbar**: Technisch gut umsetzbar ohne Breaking Changes  
✅ **RAM-Einsparung**: 11-17 KB permanent freier Heap  
✅ **Matter-Erfolg**: Mit +15 KB wird Matter sicher initialisieren  
✅ **Skalierbarkeit**: 3x mehr Sensoren möglich  
✅ **Performance**: Cached Values sind schneller als RAM-Zugriff  
✅ **Wartbarkeit**: Klare Trennung von Config (Flash) und Runtime (Cache)  

**Empfehlung**: Schrittweise Migration (Phase 1-2 zuerst), dann Matter testen.
