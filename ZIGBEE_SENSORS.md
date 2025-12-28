# Zigbee Sensor Integration für OpenSprinkler (ESP32-C5)

## Übersicht

Die Zigbee-Sensor-Integration nutzt die **native ESP32-C5 Zigbee-Funktionalität** für direkte Kommunikation mit Zigbee-Geräten. Der ESP32-C5 fungiert als Zigbee-Coordinator und empfängt Sensordaten direkt von Zigbee End Devices ohne zusätzliche Hardware oder Software.

## Unterstützte Sensortypen

- **SENSOR_ZIGBEE (95)**: Generischer Zigbee-Sensor
- **SENSOR_TUYA_SOIL_MOISTURE (96)**: Tuya Zigbee Bodenfeuchtesensor
- **SENSOR_TUYA_SOIL_TEMPERATURE (97)**: Tuya Zigbee Bodentemperatursensor

## Hardware-Voraussetzungen

### ESP32-C5 Board

Der ESP32-C5 ist erforderlich, da er einen integrierten Zigbee-Radio-Chip (IEEE 802.15.4) besitzt:

- **Board**: ESP32-C5-DevKitC-1 oder kompatibel
- **Zigbee-Standard**: IEEE 802.15.4 (Zigbee 3.0)
- **Rolle**: Zigbee Coordinator
- **Unterstützte Frequenz**: 2.4 GHz

> **Wichtig**: Nur ESP32-C5 wird unterstützt. ESP32, ESP32-S3 und andere ESP32-Varianten haben **keinen** Zigbee-Support.

## Zigbee-Netzwerk-Architektur

```
ESP32-C5 (Coordinator)
    ↓ Zigbee (IEEE 802.15.4)
Tuya Soil Sensor (End Device)
Andere Zigbee-Sensoren (End Devices)
```

## Tuya Bodenfeuchtesensor Pairing

### 1. ESP32-C5 Zigbee-Coordinator starten

Nach dem Flashen der Firmware startet der ESP32-C5 automatisch als Zigbee-Coordinator. Das Netzwerk ist für 180 Sekunden nach dem ersten Start offen für neue Geräte.

### 2. Tuya-Sensor pairen

1. Halten Sie die Reset-Taste am Tuya-Sensor für 5 Sekunden gedrückt
2. Die LED blinkt schnell - der Sensor sucht nach einem Zigbee-Netzwerk
3. Der ESP32-C5 sollte den Sensor automatisch erkennen und pairen
4. In den Logs erscheint: "New device joined: 0x..."

### 3. IEEE-Adresse notieren

Die IEEE-Adresse wird im Log angezeigt (z.B. `0x00124B001F8E5678`). Diese Adresse wird für die Sensor-Konfiguration benötigt.

## Unterstützte Zigbee-Cluster

| Cluster-ID | Name | Verwendung | Einheit |
|------------|------|------------|---------|
| 0x0408 | Soil Moisture Measurement | Bodenfeuchtigkeit | % |
| 0x0402 | Temperature Measurement | Temperatur | °C |
| 0x0405 | Relative Humidity Measurement | Luftfeuchtigkeit | % |
| 0x0001 | Power Configuration | Batterie | % |

### Tuya Sensor Attribute-Reports

Der Tuya-Sensor sendet automatisch Attribute-Reports an den Coordinator:

- **Cluster**: 0x0408 (Soil Moisture)
- **Attribute**: 0x0000 (MeasuredValue)
- **Format**: Wert in 0.01% (z.B. 2350 = 23.50%)
- **Intervall**: Alle 60-3600 Sekunden (konfigurierbar)

## OpenSprinkler-Konfiguration

### 1. Firmware kompilieren

Stellen Sie sicher, dass Sie die Firmware für ESP32-C5 kompilieren:

```bash
platformio run --environment espc5-12
```

Das `ESP32C5` Define wird automatisch gesetzt.

### 2. Zigbee-Sensor hinzufügen (Web-UI)

1. Gehen Sie zu **Sensors** → **Add Sensor**
2. Wählen Sie Sensor-Typ: **Tuya Soil Moisture** (96) oder **Tuya Soil Temperature** (97)
3. Konfigurieren Sie den Sensor:
   - **Name**: z.B. "Garten Bodenfeuchtigkeit"
   - **Device IEEE**: IEEE-Adresse aus den Logs (z.B. `0x00124B001F8E5678`)
   - **Endpoint**: `1` (Standard für Tuya)
   - **Cluster ID**: `0x0408` für Feuchtigkeit, `0x0402` für Temperatur (wird automatisch gesetzt)
   - **Attribute ID**: `0x0000` (MeasuredValue - Standard)
   - **Read Interval**: z.B. `300` Sekunden (5 Minuten)
   - **Enable Logging**: An (empfohlen)

### 3. Zigbee-Sensor hinzufügen (JSON API)

```json
{
  "nr": 1,
  "name": "Garten Bodenfeuchtigkeit",
  "type": 96,
  "group": 0,
  "enable": 1,
  "log": 1,
  "show": 1,
  "device_ieee": "0x00124B001F8E5678",
  "endpoint": 1,
  "cluster_id": "0x0408",
  "attribute_id": "0x0000",
  "ri": 300
}
```

HTTP POST an: `http://<opensprinkler-ip>/ss`

## Erweiterte Konfiguration

### Generischer Zigbee-Sensor

Für andere Zigbee-Geräte verwenden Sie `SENSOR_ZIGBEE` (Typ 95) und passen Sie Cluster/Attribute an:

```json
{
  "nr": 2,
  "name": "Temperatur-Sensor",
  "type": 95,
  "device_ieee": "0x00158D0001A2B3C4",
  "endpoint": 1,
  "cluster_id": "0x0402",
  "attribute_id": "0x0000",
  "ri": 60
}
```

### Netzwerk erneut öffnen

Um weitere Geräte hinzuzufügen, nachdem das 180-Sekunden-Fenster abgelaufen ist:

1. ESP32-C5 neu starten, oder
2. Spezielle API aufrufen (wird in zukünftigen Versionen hinzugefügt)

## Fehlerbehebung

### Sensor zeigt "No Data"

1. Überprüfen Sie die ESP32-C5 Logs (Serial Monitor):
   ```
   [ZIGBEE_SENSOR] Coordinator started successfully
   [ZIGBEE_SENSOR] New device joined: 0x...
   ```
2. Prüfen Sie, ob die IEEE-Adresse korrekt ist
3. Stellen Sie sicher, dass der Sensor gepaired ist
4. Aktivieren Sie Debug-Logging (`-DENABLE_DEBUG` in `platformio.ini`)

### Sensor-Daten werden nicht aktualisiert

- Überprüfen Sie die Batterie des Sensors
- Prüfen Sie die `lqi` (Link Quality Indicator) im JSON (sollte > 50 sein)
- Verringern Sie den Abstand zwischen Sensor und ESP32-C5
- Stellen Sie sicher, dass Cluster-ID und Attribute-ID korrekt sind

### Sensor lässt sich nicht pairen

- Stellen Sie sicher, dass das Zigbee-Netzwerk offen ist (180s nach Start)
- Reset des Sensors mehrfach versuchen
- Prüfen Sie, ob der Sensor bereits mit einem anderen Coordinator verbunden ist
- ESP32-C5 neu starten

## Technische Details

### Architektur

```
Zigbee End Device (Sensor)
    ↓ (IEEE 802.15.4 / Zigbee)
ESP32-C5 (Zigbee Coordinator)
    ↓ (interne Callbacks)
ZigbeeSensor::zigbee_attribute_callback()
    ↓
OpenSprinkler Sensor-Daten aktualisieren
```

### Datenfluss

1. Zigbee-Sensor sendet Attribute Report
2. ESP32-C5 Zigbee-Stack empfängt Report
3. `esp_zb_action_handler()` verarbeitet Zigbee-Event
4. `ZigbeeSensor::zigbee_attribute_callback()` wird aufgerufen
5. Sensor-Wert wird konvertiert (z.B. 0.01% → %)
6. `SensorBase::last_data` wird aktualisiert
7. Daten werden geloggt

## Quellcode-Referenz

- **Sensor-Typ-Definitionen**: [`sensors.h`](sensors.h ) (Zeile 131-135)
- **ZigbeeSensor-Klasse**: [`sensor_zigbee.h`](sensor_zigbee.h ) / [`sensor_zigbee.cpp`](sensor_zigbee.cpp )
- **Sensor-Factory**: [`sensors.cpp`](sensors.cpp ) (sensor_make_obj)
- **Initialisierung**: [`sensors.cpp`](sensors.cpp ) → `sensor_api_init()` → `sensor_zigbee_init()`
- **ESP32 Zigbee SDK**: `esp_zigbee_core.h`, `esp_zigbee_cluster.h`

## Beispiel-Use-Cases

### Automatische Bewässerung basierend auf Bodenfeuchtigkeit

Erstellen Sie ein Programm, das nur bewässert, wenn die Bodenfeuchtigkeit unter 20% fällt:

1. Sensor hinzufügen (Typ: SENSOR_TUYA_SOIL_MOISTURE)
2. Programm erstellen mit **Sensor Condition**: "Sensor 1 < 20%"
3. OpenSprinkler startet die Bewässerung automatisch bei niedrigem Feuchtigkeitswert

### Multi-Sensor-Setup

Verwenden Sie mehrere Zigbee-Sensoren für verschiedene Gartenbereiche:

- Sensor 1: `0x00124B001F8E5678` → Zone 1 (Rasen)
- Sensor 2: `0x00124B001F8E5679` → Zone 2 (Gemüsebeet)
- Sensor 3: `0x00124B001F8E567A` → Zone 3 (Gewächshaus)

Jeder Sensor sendet automatisch Updates an den ESP32-C5 Coordinator.

## Weitere Informationen

- **Zigbee-Standard**: IEEE 802.15.4 / Zigbee 3.0
- **ESP32-C5 Dokumentation**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/
- **ESP-Zigbee-SDK**: https://github.com/espressif/esp-zigbee-sdk
- **Tuya Zigbee-Geräte**: https://developer.tuya.com/

## Vorteile der nativen Zigbee-Integration

✅ **Keine zusätzliche Hardware**: Kein Zigbee-USB-Stick nötig  
✅ **Kein MQTT**: Direkte Kommunikation ohne Broker  
✅ **Geringere Latenz**: Sensordaten kommen direkt  
✅ **Weniger Komplexität**: Keine Zigbee2MQTT-Installation  
✅ **ESP32-C5 integriert**: Alles in einem Gerät  

---

**Hinweis**: Diese Implementierung nutzt die native ESP32-C5 Zigbee-Funktionalität. Für andere ESP32-Varianten ohne Zigbee-Support ist diese Integration nicht verfügbar.
