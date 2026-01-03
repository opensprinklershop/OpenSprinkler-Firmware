# ZigBee Cluster ID Database

## Übersicht

Dieses System ermöglicht die zentrale Verwaltung von ZigBee-Sensor-Konfigurationen über eine Online-JSON-Datei. Benutzer können bekannte Sensoren aus einer Dropdown-Liste auswählen, anstatt Cluster-IDs und Attribut-IDs manuell einzugeben.

## Funktionsweise

### 1. Online-Datenquelle

Die ZigBee-Cluster-Daten werden aus einer zentralen JSON-Datei geladen:
```
https://opensprinklershop.de/zigbeeclusterids.json
```

### 2. JSON-Dateiformat

Die JSON-Datei enthält ein Array von Sensor-Definitionen. Beispiel:

```json
[
  {
    "id": "aqara_temp_humidity",
    "name": "Aqara Temperature & Humidity Sensor",
    "description": "Temperature measurement",
    "endpoint": "1",
    "cluster_id": "0x0402",
    "attribute_id": "0x0000",
    "poll_interval": "60000",
    "unitid": "2",
    "unit": "°C",
    "factor": "100",
    "divider": "1",
    "offset": "0"
  },
  {
    "id": "tuya_soil_moisture",
    "name": "Tuya Soil Moisture Sensor",
    "description": "Soil moisture measurement",
    "endpoint": "1",
    "cluster_id": "0x0408",
    "attribute_id": "0x0000",
    "poll_interval": "60000",
    "unitid": "1",
    "unit": "%",
    "factor": "100",
    "divider": "1",
    "offset": "0"
  }
]
```

### 3. Feldbeschreibungen

- **id**: Eindeutige Kennung für den Sensor (wird im UI nicht angezeigt)
- **name**: Sensor-Name (wird in der Dropdown-Liste angezeigt)
- **description**: Messbeschreibung (wird in Klammern angezeigt)
- **endpoint**: ZigBee-Endpoint (typischerweise "1")
- **cluster_id**: Cluster-ID im Hexadezimalformat (z.B. "0x0402" für Temperatur)
- **attribute_id**: Attribut-ID im Hexadezimalformat (z.B. "0x0000")
- **poll_interval**: Abfrageintervall in Millisekunden (optional, Standard: 60000 = 60 Sekunden)
- **unitid**: Chart-Einheit-ID (0=Default, 1=Soil Moisture %, 2=°C, 3=°F, 4=Volt, 5=Air Humidity %, 6=Inch, 7=mm, 8=MPH, 9=KM/H, 10=Level %, 11=DK, 12=Lumen, 13=LUX, 99=Custom)
- **unit**: Benutzerdefinierte Einheit (wird verwendet, wenn unitid=99)
- **factor**: Multiplikationsfaktor für Sensorwert-Konvertierung
- **divider**: Divisionsfaktor für Sensorwert-Konvertierung
- **offset**: Offset in Millivolt für Sensor-Kalibrierung

### 4. UI-Nutzung

Wenn ein Benutzer einen ZigBee-Sensor konfiguriert:

1. Die App lädt automatisch die JSON-Datei von der URL
2. Die Daten werden gecacht, um wiederholte Anfragen zu vermeiden
3. Eine Dropdown-Liste "Bekannte Sensortypen" wird mit Sensor-Namen gefüllt
4. Bei Auswahl eines Sensors werden folgende Felder automatisch ausgefüllt:
   - Endpoint
   - Cluster-ID
   - Attribut-ID
   - Abfrageintervall
   - Einheit-ID
   - Einheit (falls benutzerdefiniert)
   - Faktor
   - Divisor
   - Offset
   - Sensor-Name (falls das Namensfeld leer ist)

### 5. Neue Sensoren melden

Benutzer können auf den Button "Neuen Sensor melden" klicken, um:
- Eine E-Mail an info@opensprinklershop.de mit den aktuellen Sensordaten zu senden
- Diese Daten können dann in die zentrale JSON-Datei aufgenommen werden

## Häufige ZigBee Cluster-IDs

### Temperatur
- **Cluster-ID**: 0x0402
- **Attribut-ID**: 0x0000
- **Wert**: Temperatur in Hundertstel Grad Celsius
- **Faktor**: 100, **Divisor**: 1 (zur Umrechnung in °C)

### Luftfeuchtigkeit
- **Cluster-ID**: 0x0405
- **Attribut-ID**: 0x0000
- **Wert**: Relative Luftfeuchtigkeit in Hundertstel Prozent
- **Faktor**: 100, **Divisor**: 1 (zur Umrechnung in %)

### Bodenfeuchtigkeit
- **Cluster-ID**: 0x0408
- **Attribut-ID**: 0x0000
- **Wert**: Bodenfeuchtigkeit in Prozent
- **Faktor**: 100, **Divisor**: 1

### Beleuchtungsstärke
- **Cluster-ID**: 0x0400
- **Attribut-ID**: 0x0000
- **Wert**: Beleuchtungsstärke in Lux
- **Faktor**: 1, **Divisor**: 1

## Datei-Hosting

Die Datei `zigbeeclusterids.json` sollte auf einem Webserver zugänglich sein unter:
```
https://opensprinklershop.de/zigbeeclusterids.json
```

### Apache/nginx-Konfiguration

Stellen Sie sicher, dass:
1. Die Datei über HTTPS zugänglich ist
2. CORS-Header gesetzt sind, um Cross-Origin-Anfragen zu ermöglichen:
   ```
   Access-Control-Allow-Origin: *
   Access-Control-Allow-Methods: GET
   Content-Type: application/json
   ```

## Wartung

### Neue Sensoren hinzufügen

1. Bearbeiten Sie die JSON-Datei auf dem Server
2. Fügen Sie einen neuen Eintrag mit allen erforderlichen Feldern hinzu
3. Speichern und hochladen
4. Änderungen sind sofort für alle Benutzer verfügbar (beim nächsten Laden)

### Cache

Die App speichert JSON-Daten während der Sitzung im Cache. Um den Cache zu aktualisieren, muss die Seite neu geladen werden.

## Sicherheit

- JSON-Datei wird über HTTPS geladen
- Es erfolgt nur Lesezugriff
- Bei Ladefehlern wird ein leeres Array zurückgegeben
- Fehler werden in der Browser-Konsole protokolliert

---

## Hardware-Voraussetzungen

### ESP32-C5 Board

Der ESP32-C5 ist erforderlich, da er einen integrierten Zigbee-Radio-Chip (IEEE 802.15.4) besitzt:

- **Board**: ESP32-C5-DevKitC-1 oder kompatibel
- **Zigbee-Standard**: IEEE 802.15.4 (Zigbee 3.0)
- **Rolle**: Zigbee Coordinator
- **Unterstützte Frequenz**: 2.4 GHz

> **Wichtig**: Nur ESP32-C5 wird unterstützt. ESP32, ESP32-S3 und andere ESP32-Varianten haben **keinen** Zigbee-Support.

## ZigBee-Netzwerk-Architektur

```
ESP32-C5 (Coordinator)
    ↓ Zigbee (IEEE 802.15.4)
Tuya Soil Sensor (End Device)
Andere Zigbee-Sensoren (End Devices)
```

## Sensor-Pairing

### 1. ESP32-C5 Zigbee-Coordinator starten

Nach dem Flashen der Firmware startet der ESP32-C5 automatisch als Zigbee-Coordinator. Das Netzwerk ist für 180 Sekunden nach dem ersten Start offen für neue Geräte.

### 2. Sensor pairen

1. Halten Sie die Reset-Taste am Sensor für 5 Sekunden gedrückt
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
| 0x0400 | Illuminance Measurement | Beleuchtungsstärke | Lux |
| 0x0403 | Pressure Measurement | Luftdruck | kPa |
| 0x0404 | Flow Measurement | Durchfluss | m³/h |
| 0x0407 | Leaf Wetness | Blattnässe | % |
| 0x0406 | Occupancy Sensing | Bewegung/Anwesenheit | bool |
| 0x0001 | Power Configuration | Batterie | % |

📖 **Vollständige Cluster-Referenz**: Siehe [ZIGBEE_CLUSTER_REFERENCE.md](ZIGBEE_CLUSTER_REFERENCE.md) für detaillierte Datentypen, Konvertierungsformeln und ZCL-Spec-Referenzen.

### Automatische ZCL-Wert-Konvertierung

Der ESP32-C5 führt **automatisch** die korrekte ZCL-Standard-Konvertierung durch:

| Cluster | Name | Rohwert | Konvertierung | Ergebnis |
|---------|------|---------|---------------|----------|
| 0x0408 | Soil Moisture | 0-10000 | ÷ 100 | 0-100% |
| 0x0402 | Temperature | int16 | ÷ 100 | °C |
| 0x0405 | Humidity | 0-10000 | ÷ 100 | 0-100% |
| 0x0001 | Battery | 0-200 | ÷ 2 | 0-100% |

**Beispiel**: Zigbee-Sensor meldet `2350` für Cluster 0x0408 → automatisch konvertiert zu `23.50%`

### Benutzerdefinierte Konvertierung (Optional)

Für spezielle Sensoren können Sie zusätzliche Konvertierungsparameter definieren:

```json
{
  "factor": 100,      // Multiplikationsfaktor
  "divider": 1,       // Divisionsfaktor
  "offset_mv": 0,     // Null-Punkt-Offset in Millivolt (vor factor/divider)
  "offset2": -50      // Offset in 0.01 Einheit (nach factor/divider)
}
```

**Reihenfolge der Konvertierung:**
1. ZCL-Standard-Konvertierung (z.B. ÷100 für Cluster 0x0408)
2. `offset_mv`: Abzug in Millivolt
3. `factor` / `divider`: Skalierung
4. `offset2`: Offset in 0.01 Einheit

**Beispiel**: Sensor mit Kalibrierung
```json
{
  "cluster_id": "0x0408",
  "factor": 95,       // Sensor liefert 5% zu wenig
  "divider": 100,     // → Multiplikation mit 0.95
  "offset2": 300      // + 3% Offset
}
```
Rohwert `5000` → ZCL: `50%` → factor/divider: `47.5%` → offset2: `50.5%`

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
