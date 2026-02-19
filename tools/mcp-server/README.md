# OpenSprinkler MCP Server

Ein [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) Server, der die HTTP-API des OpenSprinkler Bewässerungscontrollers als MCP-Tools bereitstellt. Damit können KI-Assistenten (z. B. Claude, GitHub Copilot, ChatGPT) den Controller direkt abfragen und steuern.

## Voraussetzungen

- **Node.js** ≥ 18
- Ein erreichbarer **OpenSprinkler Controller** im Netzwerk

## Installation

```bash
cd tools/mcp-server
npm install
npm run build
```

## Konfiguration

Die Verbindung zum Controller wird über Umgebungsvariablen gesteuert:

| Variable | Beschreibung | Standard |
|----------|-------------|----------|
| `OS_BASE_URL` | URL des Controllers | `http://localhost:8080` |
| `OS_PASSWORD` | Gerätepasswort (Klartext) | – |
| `OS_PASSWORD_HASH` | Gerätepasswort (MD5-Hash, alternativ) | – |

Es muss entweder `OS_PASSWORD` oder `OS_PASSWORD_HASH` gesetzt sein.

## Verwendung

### Standalone (stdio-Transport)

```bash
OS_BASE_URL=http://192.168.1.100 OS_PASSWORD=opendoor npm start
```

### In VS Code (Copilot / Claude)

In der Datei `.vscode/mcp.json` (oder den User-Settings unter `mcp.servers`):

```json
{
  "servers": {
    "opensprinkler": {
      "type": "stdio",
      "command": "node",
      "args": ["tools/mcp-server/dist/index.js"],
      "env": {
        "OS_BASE_URL": "http://192.168.1.100",
        "OS_PASSWORD": "opendoor"
      }
    }
  }
}
```

### In Claude Desktop

In `~/.config/claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "opensprinkler": {
      "command": "node",
      "args": ["/pfad/zu/tools/mcp-server/dist/index.js"],
      "env": {
        "OS_BASE_URL": "http://192.168.1.100",
        "OS_PASSWORD": "opendoor"
      }
    }
  }
}
```

## Verfügbare Tools

### Lesen / Abfragen

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `get_all` | Alle Daten auf einmal (Controller, Optionen, Stationen, Status, Programme) | `/ja` |
| `get_controller_variables` | Controller-Variablen (Zeit, Rain Delay, Flow, MQTT, etc.) | `/jc` |
| `get_options` | Einstellungen (Firmware, Netzwerk, Sensoren, Wasserstand, etc.) | `/jo` |
| `get_stations` | Stationsnamen und Attribute | `/jn` |
| `get_station_status` | Aktueller On/Off-Status aller Stationen | `/js` |
| `get_programs` | Alle Programme (Zeitpläne, Dauer, Flags) | `/jp` |
| `get_special_stations` | Spezial-Stationen (RF, Remote, GPIO, HTTP) | `/je` |
| `get_log` | Bewässerungsprotokoll (nach Zeitraum oder Tagen) | `/jl` |
| `get_debug` | Debug-/Diagnoseinformationen | `/db` |

### Steuerung

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `change_controller_variables` | Betrieb ein/aus, Rain Delay setzen, Stationen zurücksetzen, Neustart | `/cv` |
| `change_options` | Einstellungen ändern (Zeitzone, Sensoren, Master, Logging, etc.) | `/co` |
| `manual_station_run` | Station manuell öffnen/schließen | `/cm` |
| `run_once` | Einmal-Programm mit individueller Stationsdauer starten | `/cr` |
| `manual_program_start` | Gespeichertes Programm manuell starten | `/mp` |
| `pause_queue` | Programm-Warteschlange pausieren/fortsetzen | `/pq` |

### Programmverwaltung

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `change_program` | Programm erstellen oder ändern | `/cp` |
| `delete_program` | Programm löschen (einzeln oder alle) | `/dp` |
| `move_program_up` | Programm in der Liste nach oben verschieben | `/up` |

### Stationskonfiguration

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `change_station` | Stationsnamen und Attribute ändern | `/cs` |

### Sensoren

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `get_sensors` | Alle konfigurierten Sensoren auflisten | `/sl` |
| `get_sensor_values` | Sensorwerte abrufen | `/sg` |
| `get_sensor_log` | Sensor-Logdaten abrufen (JSON/CSV) | `/so` |
| `configure_sensor` | Sensor hinzufügen/ändern/löschen | `/sc` |
| `read_sensor_now` | Sofort-Lesung eines Sensors auslösen | `/sr` |
| `get_sensor_types` | Verfügbare Sensortypen auflisten | `/sf` |
| `list_adjustments` | Sensor-Programmeinstellungen auflisten | `/se` |
| `configure_adjustment` | Sensor-Programmeinstellung konfigurieren | `/sb` |
| `list_monitors` | Sensor-Monitore (Schwellwert-Trigger) auflisten | `/ml` |
| `configure_monitor` | Sensor-Monitor konfigurieren | `/mc` |

### ZigBee / IEEE 802.15.4 (nur ESP32-C5)

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `get_ieee802154_config` | Radio-Konfiguration lesen | `/ir` |
| `set_ieee802154_config` | Radio-Modus setzen (WiFi, Matter, ZigBee GW/Client) | `/iw` |
| `get_zigbee_devices` | ZigBee-Geräteliste | `/zg` |
| `zigbee_join_network` | ZigBee-Netzwerk beitreten / Netzwerk öffnen | `/zj` |
| `get_zigbee_status` | ZigBee-Radio-Status | `/zs` |

### BLE (nur ESP32)

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `get_ble_devices` | BLE-Geräte scannen/auflisten | `/bd` |

### System / Sonstiges

| Tool | Beschreibung | API-Endpunkt |
|------|-------------|--------------|
| `set_password` | Gerätepasswort ändern | `/sp` |
| `change_script_urls` | UI-/Weather-Script-URLs ändern | `/cu` |
| `delete_log` | Logdaten löschen | `/dl` |
| `backup_sensor_config` | Sensor-Konfiguration exportieren | `/sx` |
| `get_system_resources` | Systemressourcen (RAM, Speicher) | `/du` |

## Verfügbare Ressourcen

| Ressource | URI | Beschreibung |
|-----------|-----|-------------|
| API-Übersicht | `opensprinkler://api-overview` | Fehlercodes, Programm-Encoding, Sensortypen |
| Controller-Zusammenfassung | `opensprinkler://controller-summary` | Live-Status des verbundenen Controllers |

## Beispiele

### Alle Stationen abfragen
```
Verwende get_station_status um den aktuellen Status aller Bewässerungszonen zu zeigen.
```

### Station 3 für 10 Minuten öffnen
```
Öffne Station 3 (Index 2) für 10 Minuten mit manual_station_run.
→ sid=2, en=1, t=600
```

### Bewässerungsverlauf der letzten 7 Tage
```
Zeige das Bewässerungslog der letzten 7 Tage mit get_log.
→ hist=7
```

### Rain Delay für 24 Stunden setzen
```
Setze einen Rain Delay von 24 Stunden mit change_controller_variables.
→ rd=24
```

### Neues Programm erstellen
```
Erstelle ein Programm "Morgen-Bewässerung", täglich um 6:00, Station 1 für 15 Min, Station 3 für 20 Min.
→ change_program mit pid=-1, v=[3,127,0,[360,-1,-1,-1],[900,0,1200,0,0,0,0,0]], name=Morgen-Bewässerung
```

### Sensorwerte lesen
```
Zeige alle Sensorwerte mit get_sensors.
```

## Architektur

```
src/
├── index.ts        # Einstiegspunkt – erstellt Client, Server, startet stdio-Transport
├── client.ts       # HTTP-Client für die OpenSprinkler REST-API
├── tools.ts        # Registrierung aller MCP-Tools (Abfragen, Steuerung, Sensoren, etc.)
└── resources.ts    # MCP-Ressourcen (API-Dokumentation, Live-Controller-Status)
```

### Sicherheitshinweise

- Das **Passwort** wird als MD5-Hash an den Controller gesendet (OpenSprinkler-Konvention).
- Der MCP-Server kommuniziert **lokal** über stdio – das Passwort verlässt nicht den Rechner.
- Für Netzwerkzugriff auf den Controller empfiehlt sich ein VPN oder lokales Netzwerk.

## Entwicklung

```bash
# Starten im Dev-Modus (tsx, kein Build nötig)
OS_BASE_URL=http://192.168.1.100 OS_PASSWORD=opendoor npm run dev

# TypeScript kompilieren
npm run build
```

## Lizenz

GPL-3.0 – wie das OpenSprinkler Firmware Projekt.
