# OSPI Bluetooth LE & Zigbee Sensoren

Implementierung von Bluetooth LE (BLE) und Zigbee Sensoren für Raspberry Pi (OSPI-Build).

## Überblick

Diese Implementierung ermöglicht die Integration von:
- **BLE-Sensoren** über BlueZ (Linux Bluetooth Stack)
- **Zigbee-Sensoren** über Zigbee2MQTT

## Voraussetzungen

### Bluetooth LE (BlueZ)

```bash
# BlueZ-Bibliotheken installieren
sudo apt-get update
sudo apt-get install -y bluetooth bluez libbluetooth-dev

# gatttool für GATT-Zugriff (optional, wird vom Code verwendet)
sudo apt-get install -y bluez-tools

# Bluetooth-Dienst aktivieren
sudo systemctl enable bluetooth
sudo systemctl start bluetooth
```

### Zigbee (Zigbee2MQTT)

```bash
# Node.js installieren (falls noch nicht vorhanden)
curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash -
sudo apt-get install -y nodejs

# MQTT Broker (Mosquitto)
sudo apt-get install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto

# libmosquitto für C++ MQTT Client
sudo apt-get install -y libmosquitto-dev

# Zigbee2MQTT installieren (optional - falls noch nicht vorhanden)
# Siehe: https://www.zigbee2mqtt.io/guide/installation/01_linux.html
```

## Build-Anpassungen

Die neuen Sensor-Typen werden automatisch für OSPI-Builds kompiliert. Stelle sicher, dass in `build.sh` oder `build2.sh` folgende Abhängigkeiten installiert sind:

```bash
# In build.sh bereits vorhanden (erweitern falls nötig):
sudo apt-get install -y \
    libbluetooth-dev \
    libmosquitto-dev
```

## Sensor-Konfiguration

### BLE-Sensoren

Unterstützte Sensor-Typen:
- `SENSOR_BLE_TEMP` (91): Temperatursensor
- `SENSOR_BLE_HUMIDITY` (92): Luftfeuchtigkeitssensor
- `SENSOR_BLE_PRESSURE` (93): Drucksensor

#### Beispiel-Konfiguration (sensors.json):

```json
{
  "sensors": [
    {
      "nr": 1,
      "type": 91,
      "name": "AA:BB:CC:DD:EE:FF",
      "enable": 1,
      "userdef_unit": "00002a1c-0000-1000-8000-00805f9b34fb",
      "read_interval": 300
    },
    {
      "nr": 2,
      "type": 91,
      "name": "11:22:33:44:55:66",
      "enable": 1,
      "userdef_unit": "00002a1c-0000-1000-8000-00805f9b34fb|10",
      "read_interval": 300
    }
  ]
}
```

**Parameter:**
- `type`: Sensor-Typ (91=Temperatur, 92=Luftfeuchtigkeit, 93=Druck)
- `name`: BLE MAC-Adresse des Geräts (z.B. "AA:BB:CC:DD:EE:FF")
- `userdef_unit`: GATT Characteristic UUID, optional mit Format: "UUID|format_id"
  - Nur UUID: Auto-Detection (empfohlen)
  - Mit Format: "00002a1c-...|10" (Format-ID 10 = Temp 0.01°C)
- `read_interval`: Leseintervall in Sekunden

#### Payload-Formate:

Das System unterstützt automatische Erkennung oder manuelle Format-Angabe nach `|`:

| Format-ID | Beschreibung | Beispiel |
|-----------|--------------|----------|
| (ohne \|) | Auto-Detect | Empfohlen für Standard-Sensoren |
| 10 | Temperatur 0.01°C (int16_le) | 2250 = 22.50°C |
| 11 | Luftfeuchtigkeit 0.01% (uint16_le) | 6520 = 65.20% |
| 12 | Druck in Pascal | 101325 Pa |
| 3 | Unsigned 16-bit LE | Allgemein |
| 4 | Signed 16-bit LE | Allgemein |
| 9 | 32-bit Float LE | Manche High-End-Sensoren |
| 20 | Xiaomi Temperatur | Xiaomi MiFlora kompatibel |
| 21 | Xiaomi Luftfeuchtigkeit | Xiaomi MiFlora kompatibel |

**Vollständige Format-Liste** in [sensor_payload_decoder.h](sensor_payload_decoder.h)

#### BLE-Geräte scannen:

```bash
# Bluetooth-Geräte scannen
sudo hcitool lescan

# Oder mit bluetoothctl:
bluetoothctl
> scan on
> devices
> exit
```

#### GATT-Characteristics auslesen:

```bash
# Mit gatttool (MAC-Adresse anpassen):
gatttool -b AA:BB:CC:DD:EE:FF --characteristics

# Wert lesen:
gatttool -b AA:BB:CC:DD:EE:FF --char-read -u 00002a1c-0000-1000-8000-00805f9b34fb
```

### Zigbee-Sensoren

Unterstützte Sensor-Typen:
- `SENSOR_ZIGBEE` (95): Generischer Zigbee-Sensor
- `SENSOR_TUYA_SOIL_MOISTURE` (96): Tuya Bodenfeuchtigkeitssensor
- `SENSOR_TUYA_SOIL_TEMPERATURE` (97): Tuya Bodentemperatursensor

#### Beispiel-Konfiguration (sensors.json):

```json
{
  "sensors": [
    {
      "nr": 2,
      "type": 96,
      "name": "Zigbee Bodenfeuchtigkeit",
      "enable": 1,
      "data1": "sensor_garden_1",
      "data2": "",
      "read_interval": 600
    },
    {
      "nr": 3,
      "type": 95,
      "name": "Zigbee Custom",
      "enable": 1,
      "data1": "0x00158d0001abcdef",
      "data2": "battery",
      "read_interval": 3600
    }
  ]
}
```

**Parameter:**
- `type`: Sensor-Typ (95=generisch, 96=Tuya Feuchtigkeit, 97=Tuya Temperatur)
- `data1`: Zigbee2MQTT Friendly Name oder IEEE-Adresse
- `data2`: JSON-Property-Pfad (z.B. "temperature", "soil_moisture", "battery")
  - Für Tuya-Sensoren (96, 97) wird die Property automatisch gesetzt
  - Für SENSOR_ZIGBEE (95) muss die Property angegeben werden
- `read_interval`: Leseintervall in Sekunden

#### Zigbee2MQTT Setup:

1. **Zigbee2MQTT starten** (falls noch nicht laufend):
   ```bash
   cd /opt/zigbee2mqtt
   npm start
   ```

2. **Geräte pairen**:
   - Web-Interface: http://localhost:8080 (oder IP des Pi)
   - "Permit join" aktivieren
   - Zigbee-Gerät in Pairing-Modus versetzen
   - Gerät erscheint in der Geräteliste

3. **Friendly Names vergeben**:
   - In Zigbee2MQTT Web-UI Gerät umbenennen (z.B. "sensor_garden_1")
   - Oder in `configuration.yaml`:
     ```yaml
     devices:
       '0x00158d0001abcdef':
         friendly_name: sensor_garden_1
     ```

4. **MQTT Topics prüfen**:
   ```bash
   # Alle Zigbee-Nachrichten anzeigen:
   mosquitto_sub -h localhost -t "zigbee2mqtt/#" -v
   
   # Spezielles Gerät:
   mosquitto_sub -h localhost -t "zigbee2mqtt/sensor_garden_1"
   ```

#### MQTT Broker konfigurieren (falls nicht localhost):

Derzeit ist der MQTT-Broker fest auf `localhost:1883` eingestellt. Für entfernte Broker muss der Code angepasst werden, oder die Konfiguration über ein optionales Feld in sensors.json erweitert werden.

## Datenformat

### BLE-Sensoren

Die BLE-Implementierung erwartet folgende Datenformate:
- **Temperatur** (SENSOR_BLE_TEMP): int16 in 0.01°C Schritten (z.B. 2250 = 22.50°C)
- **Luftfeuchtigkeit** (SENSOR_BLE_HUMIDITY): uint16 in 0.01% Schritten (z.B. 6520 = 65.20%)
- **Druck** (SENSOR_BLE_PRESSURE): uint16 in Pa (z.B. 101325 = 101325 Pa = 1013.25 hPa)

Diese entsprechen typischen BLE-SIG-Datenformaten. Für andere Formate muss der Parsing-Code in `sensor_ospi_ble.cpp` angepasst werden.

### Zigbee-Sensoren (Zigbee2MQTT)

Die Zigbee-Implementierung liest JSON-Werte aus MQTT-Topics:
```json
{
  "temperature": 22.5,
  "humidity": 65.2,
  "soil_moisture": 45,
  "battery": 85
}
```

## Debugging

### BLE-Debug:

```bash
# Bluetooth-Status prüfen:
sudo systemctl status bluetooth

# HCI-Adapter anzeigen:
hciconfig

# BLE-Scan manuell:
sudo hcitool lescan

# Logs anzeigen:
sudo journalctl -u bluetooth -f
```

### Zigbee-Debug:

```bash
# Zigbee2MQTT Logs:
journalctl -u zigbee2mqtt -f

# MQTT-Verkehr überwachen:
mosquitto_sub -h localhost -t "zigbee2mqtt/#" -v

# Mosquitto-Status:
sudo systemctl status mosquitto
```

### OpenSprinkler Debug:

Kompiliere mit `-DENABLE_DEBUG` Flag für ausführliche Debug-Ausgaben:
```bash
./build.sh -DENABLE_DEBUG
```

Debug-Ausgaben erscheinen in der Konsole beim Start von OpenSprinkler.

## Bekannte Einschränkungen

1. **BLE-Berechtigungen**: 
   - gatttool benötigt Root-Rechte (wird mit `sudo` ausgeführt)
   - Für produktive Nutzung sollte auf BlueZ D-Bus API umgestellt werden

2. **Zigbee2MQTT Abhängigkeit**:
   - Zigbee2MQTT muss separat installiert und konfiguriert sein
   - MQTT-Broker muss laufen (Standard: localhost:1883)

3. **Keine automatische Geräteerkennung**:
   - BLE- und Zigbee-Geräte müssen manuell konfiguriert werden
   - Pairing erfolgt außerhalb von OpenSprinkler

4. **Performance**:
   - BLE-Abfragen blockieren kurzzeitig (gatttool-Timeout: 10s)
   - Für viele Sensoren empfohlen: read_interval >= 300s

## Weiterentwicklung / TODOs

- [ ] BlueZ D-Bus API für BLE (ohne Root-Rechte, asynchron)
- [ ] Konfigurierbare MQTT-Broker-Adresse in sensors.json
- [ ] Web-UI Integration für Gerätescanning und Pairing
- [ ] Support für BLE-Advertising-basierte Sensoren (passives Listening)
- [ ] Zigbee-Binding-Management über OpenSprinkler API

## Lizenz

Gleiche Lizenz wie OpenSprinkler Firmware (GPLv3).

## Support

Bei Problemen oder Fragen:
- GitHub Issues im OpenSprinkler-Firmware Repository
- OpenSprinkler Forum: https://opensprinkler.com/forums/
