# BLE / Zigbee / WiFi Koexistenz – Ist-Zustand und Funktionsumfang (ESP32-C5)

Stand: 2026-02-16

## 1) Geltungsbereich

Diese Dokumentation beschreibt den aktuellen Runtime-Zustand in:
- `sensor_ble.cpp`
- `sensor_zigbee.cpp` (Zigbee Client / End Device)
- `sensor_zigbee_gw.cpp` (Zigbee Gateway / Coordinator)
- `radio_arbiter.cpp`
- `sensors.cpp`
- `main.cpp`

Ziel ist, die aktuelle Funktionsweise transparent zu machen und konkrete Vereinfachungen für den nächsten Refactor vorzubereiten.

---

## 2) Aktuelle Architektur (kurz)

- ESP32-C5 teilt sich 2.4 GHz zwischen WiFi, BLE und IEEE802.15.4 (Zigbee/Matter).
- IEEE802.15.4 Modus wird über `ieee802154_config.*` zur Laufzeit gewählt:
  - `IEEE_MATTER`
  - `IEEE_ZIGBEE_GATEWAY`
  - `IEEE_ZIGBEE_CLIENT`
  - `IEEE_DISABLED`
- In Non-Matter-Modi werden BLE + Zigbee früh gestartet (`sensor_radio_early_init()`), nicht pro Sensor dynamisch de-/re-initialisiert.

---

## 3) BLE – aktueller Funktionsumfang

### 3.1 Initialisierung und Laufzeit

- BLE wird via `sensor_ble_init()` initialisiert und bleibt aktiv.
- `sensor_ble_stop()` stoppt nur Scan + ggf. Client-Verbindung, **kein BLE-Deinit**.
- Scans laufen nicht-blockierend mit Callback (`ble_scan_complete_cb`).

### 3.2 Scan- und Koexistenzlogik

- `radio_arbiter_allow_ble_scan()` priorisiert Webtraffic, erlaubt aber periodische BLE-Fenster.
- Aktueller Scanmodus für Sensordatenerfassung: passiv (`setActiveScan(false)`) mit reduziertem Duty-Cycle.
- Scans werden bei aktiver Web-Priorität verschoben (`retry in 5s`).

### 3.3 Datenpfad

- Advertisements werden in `discovered_ble_devices` gecached.
- Logische BLE-Sensoren lesen primär aus Cache.
- Govee-Dekodierung unterstützt u. a. H5075/H5074/H5179/H5177/Meat.

### 3.4 Mehrere logische Sensoren auf ein physisches BLE-Gerät

- Unterstützt über gemeinsamen Device-Cache:
  - Mehrere Sensoren mit gleicher MAC sehen denselben `BLEDeviceInfo`-Eintrag.
  - Unterschiedliche logische Messgrößen (z. B. Temp/Hum/Battery) werden aus derselben Advertisement-Nutzlast selektiert.

---

## 4) Zigbee Gateway – aktueller Funktionsumfang

### 4.1 Start/Stop

- `sensor_zigbee_gw_start()` startet Coordinator.
- `sensor_zigbee_gw_stop()` ist als No-Op dokumentiert (Zigbee soll dauerhaft laufen).
- Reports laufen asynchron über `GwZigbeeReportReceiver` in einen Pending-Report-Cache.

### 4.2 Passive Reports + aktive Fallback-Reads

- Primär: ZCL-Reports aktualisieren Sensoren asynchron.
- Optional: aktive `Read Attributes` als Fallback.
- `sensor_zigbee_gw_loop()` verarbeitet Pending-Reports ohne Web-Priority-Blockade.

### 4.3 Mehrere logische Sensoren auf ein physisches Zigbee-Gerät

- Explizit unterstützt:
  - In `sensor_zigbee_gw_process_reports()` wird **nicht** beim ersten Match abgebrochen.
  - Alle passenden logischen Sensoren (gleiches IEEE/Cluster/Attr/Ep-Match) werden aktualisiert.

---

## 5) Zigbee Client – aktueller Funktionsumfang

- End-Device-Start über `client_zigbee_start_internal()`.
- Aktives Polling/Read in 2-Phasen-Ablauf in `ZigbeeSensor::read()` (Client-Modus).
- Schutz gegen konkurrierende Reads über `client_active_zigbee_sensor`.

---

## 6) Radio Arbiter – aktueller Funktionsumfang

- Web-Aktivität markiert Prioritätsfenster (`radio_arbiter_mark_web_activity`).
- BLE-Scans dürfen außerhalb Web-Priorität immer laufen, ansonsten nur in periodischen Fenstern.
- Zigbee active ops aktuell freigegeben (`radio_arbiter_allow_zigbee_active_ops() -> true`).

---

## 7) Festgestellte Probleme / technische Schulden

1. **Gateway enthält tote Restart-Guard-Variable**
   - `gw_zigbee_was_stopped` wird geprüft, aber nicht gesetzt.

2. **Gateway enthält ungenutzte Active-Sensor-Variable**
   - `gw_active_zigbee_sensor` ist deklariert, aber unbenutzt.

3. **Gateway-Start enthält weiterhin `Zigbee.stop()`-Pfad**
   - Widerspricht Ziel „nicht mehr aus-/umschalten“.

4. **Koexistenz-Preference wird in Zigbee Startpfaden gesetzt**
   - Derzeit mehrfach in Client/Gateway-Start statt zentral nur einmal.

5. **Client hat analogen ungenutzten Restart-Guard**
   - `client_zigbee_was_stopped` ebenfalls nur geprüft, nicht gesetzt.

---

## 8) Vereinfachungsziel (für Refactor)

1. Kein hartes Start/Stop-Toggling von Zigbee/BLE im Normalbetrieb.
2. BLE: nur Scan an/aus, BLE-Stack bleibt aktiv.
3. Zigbee: nach erfolgreichem Start dauerhaft aktiv.
4. Coex-Mode (`esp_coex_preference_set`) einmalig nach Programmstart auf **balanced** setzen.
5. Tote Felder und Guards entfernen (`gw_zigbee_was_stopped`, `gw_active_zigbee_sensor`, ggf. Client-Pendant).
6. Multi-Logical-Sensor-Mapping weiterhin erhalten (BLE-Cache / Zigbee-Report-Multi-Match).

---

## 9) Abgleich mit den Anforderungen

- „ständiges Ein-/Umschalten entfernen“: technisch sinnvoll und kompatibel mit aktuellem Stand.
- „`gw_zigbee_was_stopped` kann weg“: korrekt, aktuell tote Variable.
- „`gw_active_zigbee_sensor` nicht verwendet“: korrekt, ungenutzt.
- „weitere ungenutzte Felder/Funktionen entfernen“: möglich (mindestens im Zigbee Gateway/Client-Guard-Bereich).
- „mehrere logische Sensoren auf einen physischen“: aktuell bereits unterstützt (BLE + Zigbee GW).
- „coexmode nur 1x auf ausgeglichen“: umsetzbar über zentrale Initialisierung.
- „Zigbee/BLE nicht mehr ausschalten, nur scannen deaktivieren“: BLE bereits weitgehend so; Zigbee-Stop-Pfad im Gateway wird entfernt.
