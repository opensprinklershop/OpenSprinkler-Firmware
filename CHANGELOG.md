# Changelog — OpenSprinkler Firmware

Dieses Changelog dokumentiert Änderungen, die **nicht** plattformspezifisch für ESP32 / ESP32-C5 sind — also allgemeine Fixes und Features, die auch ESP8266, Linux/OSPi sowie den gemeinsam genutzten Code betreffen.

---

## [2.3.3 / Build 185] — veröffentlicht 2026-03-07

### Netzwerk

- **Unified Pinger** (2026-02-18): Neue plattformübergreifende `Pinger.h`-Implementierung für ESP8266, ESP32 und Linux/OSPi.
  - ESP32: lwIP-Sockets mit ICMP-Checksumme
  - Linux/OSPi: Raw Socket mit manueller Checksumme
  - Vollständig kompatibel mit der ESP8266-Pinger-API (callback-basiert: `OnReceive`, `OnEnd`)
  - DNS-Hostname-Auflösung, konfigurierbare Ping-Anzahl und Timeout
  - 3-stufige Ping-Strategie: Gateway → google.com → opensprinkler.com
- **IPAddress-Kompatibilität** (2026-02-18): Konfliktierende `IPAddress`-Redefinition für ESP32 entfernt; nutzt jetzt korrekt den Cast-Operator der nativen Arduino-Klasse statt private Member-Zugriff.
- **WiFi-State-Machine-Fix** (2026-02-18): Unmögliche STA-Bedingung innerhalb des AP-Modus-Zweigs entfernt. Diese Code-Logik konnte niemals wahr sein und verhinderte korrekte Reconnect-Logik, was dazu führte, dass das Gerät nach längerem Betrieb mit IP `0.0.0.0` hängen blieb.

### Wetter

- **Wetterkonfiguration persistieren** (2026-01-17–19): Zwei separate Fehler beim Speichern der Wetterkonfiguration behoben (`weather.cpp`, `opensprinkler_server.cpp`).
- **Verbesserte Wetteroptionen-Verarbeitung** (2026-01-18): Robustere Fehlerbehandlung; Hard-Timeout für den Wetter-Abruf implementiert.
- **Wetterabruf-Logik** (2026-02-18): Datenvalidierung vor Verwendung der Wetterdaten eingeführt; verbesserter Retry-Mechanismus bei ungültigen Antworten (`sensor_weather.cpp`).

### MQTT

- **MQTT-Option Fix** (2026-01-16): Korrekte Persistenz der MQTT-Konfigurationsoptionen sichergestellt.
- **Nicht-persistente MQTT-Felder** (2026-01-12): Laufzeit-Felder (z. B. Verbindungsstatus), die nicht gespeichert werden sollen, wurden aus der Serialisierung entfernt.

### RS485 / Modbus / Sensoren

- **RS485-Implementierung abgeschlossen** (2025-12-13): Grundlegende RS485-Kommunikation funktionsfähig.
- **RS485/I2C-Refactoring & neue Sensormodule** (2025-12-23):
  - Gemeinsamer Sensor-Code in eigenständige Dateien ausgelagert (`sensors.cpp` stark reduziert).
  - Neues Modul `sensor_rs485_i2c.cpp` für RS485/I2C-Sensoren.
  - Neues Modul `sensor_modbus_rtu.cpp` — Modbus RTU Protokoll-Unterstützung.
  - Neues Modul `sensor_truebner_rs485.cpp` — Truebner-Bodenfeuchtesensoren über RS485.
  - Neues Modul `sensor_fyta.cpp` — Fyta Pflanzensensoren.
- **RS485/I2C Adressierungs-Fix** (2025-12-28): Fehler bei der I2C-Adresszuweisung behoben.
- **Neue RS485-Boards** (2026-01-16): Unterstützung für weitere RS485-Hardware hinzugefügt.
- **Sensor-Einheiten erweitert** (2026-03-02): Unit-ID-Mapping überarbeitet; neue Einheit „Liter" hinzugefügt; maximale Anzahl der Sensor-Einheitsnamen erhöht.

### Sensor-API

- **JSON-Speicherformat** (2025-12-25): Speicherformat für Sensor- und Programmkonfigurationen von binär auf JSON umgestellt.
- **Analog-Sensor-API Refactoring** (2025-12-25): Analog-Sensor-API grundlegend überarbeitet.
- **Lazy Loading für Sensor-Interface** (2026-01-27): Sensor-Interface nutzt jetzt Lazy Loading, um Speicher nur bei Bedarf zu allozieren — wirkt sich positiv auf alle Plattformen aus.

### InfluxDB

- **tmp_buffer-Deklaration** (2026-01-16): Fehlerhafte Variablendeklaration in `osinfluxdb.cpp` korrigiert.
- **Kompilierfehler** (2026-01-16): Zweiter Syntaxfehler in `osinfluxdb.cpp` behoben.

### OpenThings Framework (OTF)

- **CRLF-Einfügungsfehler** (2026-01-19): Bei langen Nachrichten über den OTF-Kommunikationskanal wurden fälschlicherweise CRLF-Zeichen in den Datenstrom eingefügt, was zu Protokollfehlern führte. Behoben in `opensprinkler_server.cpp`.

### HTTP-Server / Web-API

- **AP-Modus IP-Anzeige** (2026-01-16): IP-Adresse wird jetzt im AP-Modus korrekt auf ESP8266 und ESP32 angezeigt.
- **Arduino OTA** (2026-01-16): OTA-Verarbeitung in den Haupt-Loop integriert.
- **MAC-Adress-Erkennung** (2026-01-16): Verbesserte Logik in `get_ap_ssid()` für den Fall, dass die MAC-Adresse noch alle Nullen enthält.
- **Debug-Ausgabe** (2026-01-16): Fehlerhafter Debug-Output in `OpenSprinkler.cpp` korrigiert.

### ESP8266

- **Kompatibilität** (2025-11-24): Allgemeine ESP8266-Kompatibilität wiederhergestellt.
- **Kompilierfehler behoben** (2025-12-28): Kompilierfehler in `sensor_zigbee.cpp` für ESP8266 behoben.
- **Pin-Definitionen** (2026-03-07): ESP8266 Pin-Definitionen in `defines.cpp`/`defines.h` hinzugefügt.

### Linux / OSPi

- **OSPi-Build Fix** (2025-11-24): Kompilierfehler in `main.cpp` für den OSPi-Build behoben.
- **Pinger auf Linux** (2026-02-18): Pinger funktioniert jetzt auch auf Linux/OSPi (Raw Sockets, kein Extra-Paket erforderlich).

---

## [2.3.4 / Build 184] — 2025-12-28

- Versionsnummer auf 2.3.4 (Build 184) gesetzt; diverse Anpassungen in `defines.h` und `opensprinkler_server.cpp`.

---

## [Merge: OpenSprinkler Master 2.2.1(4)] — 2025-11-24 / 2025-12-29

Upstream-Änderungen aus dem offiziellen OpenSprinkler-Firmware-Repository gemergt:

| Bereich | Änderung |
|---|---|
| **Benachrichtigungen** | `notifier.cpp` stark erweitert; neue Benachrichtigungskanäle und Felder |
| **MQTT** | Allgemeine MQTT-Verbesserungen |
| **Wetter** | Wetter-URL-Handling verbessert; explizite HTTP/HTTPS + Port-Unterstützung |
| **Scheduling** | Preemptive Zone-Reihenfolge: neue Zonen werden vor bestehenden Warteschlangen-Elementen eingefügt |
| **Linux/OSPi GPIO** | `libgpiod` durch `lgpio` ersetzt (einfachere, cleanere GPIO-Schnittstelle) |
| **Linux 32-Bit** | `time_os_t`-Kompatibilitätsproblem auf 32-Bit Linux behoben |
| **USB PD** | PD-Spannungsoptimierung; Standardspannung auf 9 V für Latch-Betrieb gesetzt |
| **Strommessung** | Exponential Moving Average (EMA) für Stromstärke-Messwerte implementiert |
| **Neue API-Endpoints** | Vorab-Unterstützung für `/cr` und `/mp` Endpoints |
| **HTTP/HTTPS** | Explizite HTTP/HTTPS- und Port-Angabe für Wetter-Anfragen (`wsp`) |
| **Dokumentation** | User-Manual für 2.2.1(4) aktualisiert; FAQ und Troubleshooting-Seiten hinzugefügt |

---

## Tools & Entwicklerwerkzeuge

- **MCP-Server** (2026-02-19): Neuer MCP-Server unter `tools/mcp-server/` — stellt die OpenSprinkler REST-API als MCP-Tools für KI-Assistenten (z. B. GitHub Copilot, Claude) bereit.
  - Vollständige TypeScript-Implementierung mit `src/` und `dist/`
  - Deckt alle wesentlichen API-Endpoints ab (Stationen, Programme, Sensoren, Optionen, etc.)
  - Konfigurierbar über Umgebungsvariablen (`OS_BASE_URL`, `OS_PASSWORD_HASH`)
- **API-Dokumentation** (2025-12-31): REST-API-Dokumentation unter `docs/as_api_docs/` hinzugefügt.
- **Dokumentation verschoben** (2026-01-19): Dokumentation nach `docs/as_api_docs` konsolidiert.

---

*Dieses Changelog umfasst ausschließlich Änderungen, die nicht spezifisch für ESP32 / ESP32-C5 sind (kein Zigbee/Matter/BLE-Stack, keine PSRAM/Flash-Optimierungen, keine TLS/mbedTLS-spezifischen Anpassungen). Den vollständigen Git-Verlauf zeigt `git log --oneline`.*
