# OpenSprinkler MCP Server — Benutzerhandbuch

Dieses Handbuch zeigt, wie du den MCP Server verwendest, um deinen OpenSprinkler Controller über KI-Assistenten zu steuern.

## Schnellstart

### 1. MCP Server kompilieren

Das MCP Server-Programm läuft auf deinem Entwicklungs-PC und verbindet sich mit der OpenSprinkler-Firmware auf dem ESP32:

```bash
cd tools/mcp-server
npm install
npm run build
```

### 2. In GitHub Copilot verwenden

Kopiere diese Konfiguration in `.vscode/mcp.json`:

```json
{
  "servers": {
    "opensprinkler": {
      "type": "stdio",
      "command": "node",
      "args": ["tools/mcp-server/dist/index.js"],
      "env": {
        "OS_BASE_URL": "http://192.168.0.86",
        "OS_PASSWORD_HASH": "<YOUR_ADMIN_PASSWORD_HASH>"
      }
    }
  }
}
```

⚠️ **Konfiguration erforderlich**:
- Ersetze `192.168.0.86` mit der IP-Adresse deines OpenSprinkler Controllers
- Ersetze `<YOUR_ADMIN_PASSWORD_HASH>` mit dem MD5-Hash deines Admin-Passworts (siehe nächster Abschnitt)

### 3. Mit Claude Desktop verwenden

Bearbeite `~/.config/claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "opensprinkler": {
      "command": "node",
      "args": ["/vollständiger/pfad/zu/tools/mcp-server/dist/index.js"],
      "env": {
        "OS_BASE_URL": "http://192.168.0.86",
        "OS_PASSWORD_HASH": "<YOUR_ADMIN_PASSWORD_HASH>"
      }
    }
  }
}
```

⚠️ **Konfiguration erforderlich**:
- Ersetze `/vollständiger/pfad/zu/` mit dem absoluten Pfad zu deinem Projekt
- Ersetze `192.168.0.86` mit der IP-Adresse deines OpenSprinkler Controllers
- Ersetze `<YOUR_ADMIN_PASSWORD_HASH>` mit dem MD5-Hash deines Admin-Passworts (siehe nächster Abschnitt)

## Admin-Passwort-Hash berechnen

Du benötigst das MD5-Hash deines **Admin-Passworts** (nicht das User-Passwort). Folge diesen Schritten:

### Linux/Mac (Terminal)
```bash
echo -n "dein_admin_passwort" | md5sum
# Ergebnis: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

Ersetze `dein_admin_passwort` mit deinem echten Admin-Passwort.

### Windows (PowerShell)
```powershell
$Text = "dein_admin_passwort"
$Bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
$Hash = [System.Security.Cryptography.MD5]::Create().ComputeHash($Bytes)
$MD5 = ([System.BitConverter]::ToString($Hash) -replace "-","").ToLower()
Write-Host $MD5
```

Ersetze `dein_admin_passwort` mit deinem echten Admin-Passwort und kopiere den berechneten Hash.

## Häufige Aufgaben

### Station manuell öffnen

**Beispiel**: Station 2 (Index 1) für 5 Minuten öffnen

Sage dem KI-Assistenten:
> "Öffne Station 2 für 5 Minuten"

Der Assistant benutzt dann:
- Tool: `manual_station_run`
- Parameter: `sid=1, en=1, t=300`

### Bewässerungsprotokoll anzeigen

> "Zeige die letzten 7 Tage Bewässerungsprotokoll"

Der Assistant nutzt `get_log` mit `hist=7`.

### Programm erstellen

> "Erstelle ein Programm namens 'Morgens' das täglich um 6:00 läuft und Station 1 + 2 je 10 Minuten wässert"

Der Assistant erstellt das Programm mit `change_program`.

### Sensoren prüfen

> "Welche Sensoren sind konfiguriert und welche Werte zeigen sie aktuell?"

Der Assistant ruft `get_sensors` auf und filtert die Daten.

### Rain Delay setzen

> "Setze einen Rain Delay von 24 Stunden"

Der Assistant benutzt `change_controller_variables` mit `rd=24`.

### System-Status prüfen

> "Wie viel RAM und Speicher hat der Controller noch frei?"

Der Assistant nutzt `get_debug` und `get_system_resources`.

## Verfügbare Tools — Übersicht

### 📊 Abfragen (Lesen)
- **get_all** — Alle Daten auf einmal
- **get_controller_variables** — Zeit, Rain Delay, Betriebszustand
- **get_options** — Firmware-Version, Netzwerk-Einstellungen
- **get_stations** — Stationsnamen und -attribute
- **get_station_status** — Aktuelle Schaltzustände
- **get_programs** — Alle Bewässerungsprogramme
- **get_log** — Bewässerungsprotokoll
- **get_debug** — System-Informationen (RAM, Signal, etc.)
- **get_sensors** — Sensorliste und aktuelle Werte
- **get_zigbee_devices** — Zigbee-Geräte (nur ESP32-C5)
- **get_ble_devices** — Bluetooth-Geräte

### ⚙️ Steuerung (Schreiben)
- **manual_station_run** — Station manuell öffnen/schließen
- **run_once** — Einmalige Bewässerung mit eigenen Dauer
- **manual_program_start** — Programm sofort starten
- **pause_queue** — Bewässerung pausieren/fortsetzen
- **change_controller_variables** — Betrieb, Rain Delay, Reboot
- **change_options** — Firmware-Einstellungen ändern
- **change_program** — Programm erstellen/ändern
- **change_station** — Stationsnamen und -attribute ändern
- **delete_program** — Programm löschen
- **configure_sensor** — Sensor hinzufügen/ändern
- **configure_monitor** — Sensor-Trigger konfigurieren

## Ressourcen

Der Assistant kann auch auf spezielle Informationen zugreifen:

- **API-Übersicht** — Fehlercodes, Datenformate, Programmierung
- **Controller-Status** — Live-Zusammenfassung aller Zustände

Frag den Assistant:
> "Zeige mir alle verfügbaren Sensor-Typen"

Der Assistant greift dann auf die API-Übersicht zu.

## Tipps & Tricks

### 1. Mehrere Stationen gleichzeitig steuern

> "Starte Station 1, 2 und 3 je für 15 Minuten"

Der Assistant macht mehrere `manual_station_run` Aufrufe.

### 2. Programmtemplates

> "Erstelle ein Programm 'Vorgarten' — täglich 6:00 und 18:00, Station 1 je 20 Min"

Der Assistant erstellt das Programm mit zwei Zeitpunkten.

### 3. Sensor-Automatisierung

> "Richte einen Sensor-Monitor ein, der bei Bodenfeuchte < 20% automatisch gießt"

Der Assistant nutzt `configure_monitor`.

### 4. Debugging

> "Zeige mir alle Stationen, deren Status-Wert nicht 0 ist"

Der Assistant ruft `get_stations` auf und filtert.

## Troubleshooting

### "Verbindung abgelehnt"
- Controller-IP korrekt?
- Controller im gleichen Netzwerk?
- Firewall blockiert?

Tipp: `ping 192.168.0.86` prüfen.

### "Passwort falsch"
- Nur Admin-Passwort wird akzeptiert, nicht das User-Passwort
- MD5-Hash korrekt berechnet? (siehe oben)
- Hash in Kleinbuchstaben schreiben?

### "Keine Antwort vom Server"
- Ist der MCP Server noch laufen?
- Oder ist der OpenSprinkler Controller offline?
- Firewall-Einstellungen prüfen

## Sicherheit

⚠️ **Wichtig:**
- Speichere den MD5-Hash **sicher** — er authentifiziert dich beim Controller
- Der MCP Server läuft **lokal auf deinem PC** und verbindet sich mit deinem OpenSprinkler über dein Heimnetzwerk
- Das Passwort wird **nicht** ins Internet gesendet (nur lokal)
- `.vscode/mcp.json` und `~/.config/claude/claude_desktop_config.json` enthalten sensible Daten → Datereibeschreibung beachten
- Falls du Remote-Zugriff brauchst (z.B. von außerhalb des Heimnetzes): VPN zur Verbindung mit dem Heimnetzwerk nutzen, nicht den Controller direkt ins Internet freigeben

## Weitere Hilfe

- Siehe [README.md](README.md) für technische Details
- Siehe [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md) für Entwickler
- OpenSprinkler API: [OpenSprinkler.com](https://opensprinkler.com)

