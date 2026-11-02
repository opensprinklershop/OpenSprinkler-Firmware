# Plan: Wasserquellen-Umschaltung Zisterne → Stadtwasser per Monitor

Stand 2026-11-01. **Umgesetzt für 2.4.0(231)** wie unter „Empfohlene Variante“ beschrieben
(`MONITOR_SET_MASTER` = Typ 5, JSON-Felder `monitor`/`master`, `/jc masrd`, Test `test_monitors.py`
Schritt 8). Entscheidungen: Umschaltung sofort (auch mitten im Lauf); kein eigener
Notification-Typ, die Meldung des Quell-Monitors reicht.

## Anforderung

Zisterne als primäre Wasserversorgung (Pumpe = Masterzone). Meldet der Füllstandsensor
< 10 %, soll stattdessen das Stadtwasser-Ventil als Master laufen. Bestehende
Masterzonen-Handhabung anderer Kunden darf sich nicht ändern.

## Ist-Zustand (relevante Stellen)

- Zwei Master-Slots `masters[MASTER_1|MASTER_2][MASOPT_SID|ON_ADJ|OFF_ADJ]`
  (`OpenSprinkler.cpp:4330 populate_master()`, aus `IOPT_MASTER_STATION*`).
- Zuordnung Station→Master über Bitmasken `attrib_mas` / `attrib_mas2`
  (`/jn` `masop`/`masop2`, `/cs` Keys `m`/`n`), persistiert in `stns.dat`.
  Einzige Abfrage: `OpenSprinkler::bound_to_master(sid, mas)` (`OpenSprinkler.cpp:2640`).
  Aufrufer: Master-Schleife `main.cpp:2014ff` (setzt jede Sekunde das Master-Bit aus der
  Queue neu) und `handle_master_adjustments()` `main.cpp:2626`.
- Monitore dürfen Masterstationen nicht schalten (`sensors.cpp:3738`), und selbst wenn:
  die Master-Schleife überschreibt das Bit jede Sekunde. „Monitor schaltet Masterzone“
  geht also heute nicht und ist auch nicht der richtige Ansatz.
- Vorbild: `MONITOR_SET_SENSOR12` setzt in `check_monitors()` Phase 1c
  (`sensors.cpp` ~4130) `os.status.forced_sensor1/2` aus dem Zustand eines anderen
  Monitors. Genau dieses Muster wird wiederverwendet.

## Empfohlene Variante: Master-Umleitung (`MONITOR_SET_MASTER`)

Idee: Master 1 = Zisternenpumpe, Master 2 = Stadtwasserventil, Stationen wie gewohnt an
Master 1 gebunden. Ein neuer Monitor-Typ leitet, solange er aktiv ist, alle Bindungen von
Master 1 auf Master 2 um. Die Stationsattribute werden dabei **nicht** verändert.

### Firmware

1. `sensors.h`: `#define MONITOR_SET_MASTER 5`, Struct
   `Monitor_SET_MASTER_t { uint monitor; uint8_t master; }` in `Monitor_Union_t`
   (`monitor` = Quell-Monitor wie bei SET_SENSOR12, `master` = 1|2 = welcher Master
   umgeleitet wird; Ziel ist implizit der jeweils andere Slot).
2. `sensors.cpp`: `toJson`/`fromJson` (Felder `monitor`, `master`), Phase 1b wie
   SET_SENSOR12 (Zustand = `getEval(monitor)`), Phase 1c setzt
   `os.master_redirect = active ? master : 0` (uint8, volatil, in `OpenSprinkler`).
   Kein Zone/Programm-Output, `start/stop_monitor_action` bleiben unberührt.
3. `OpenSprinkler::bound_to_master(sid, mas)`:
   - `master_redirect == 0` → unverändert (Pfad aller bestehenden Kunden).
   - `master_redirect == 1`: `mas == MASTER_1` → 0; `mas == MASTER_2` →
     `attrib_mas2 | attrib_mas` Bit.
   - `master_redirect == 2` spiegelbildlich.
   Mehr Scheduler-Änderung ist nicht nötig: die Master-Schleife schaltet beim Wechsel
   M1 im nächsten Sekundentakt aus und M2 ein, `handle_master_adjustments()` nimmt die
   On/Off-Adjustments des jeweils wirksamen Masters (Pumpe und Ventil dürfen
   unterschiedliche Vorlaufzeiten haben), die Notif-Schleife `masters_last_on` liefert
   korrekte ON/OFF-Meldungen pro physischem Master. Alle „ist sid ein Master“-Prüfungen
   (`os.status.mas/mas2`, ~12 Stellen) decken beide Slots bereits ab.
4. `/jc`: neues Feld `masrd` (0/1/2), damit App, MQTT-Status und Zigbee/Matter-Expose
   „Stadtwasser aktiv“ anzeigen können. Optional später: `/cv?masrd=` als manueller
   Override (Pumpenwartung) mit Timeout.
5. Boot/Failsafe: vor dem ersten Sensorwert ist `master_redirect = 0` (Zisterne).
   Für toten Sensor existieren bereits `stale_timeout`/`failsafe_active` am Monitor;
   für die Zisterne wird in der Doku empfohlen `failsafe_active = 1` (Stadtwasser).
6. Keine Binärformat-Änderung: Monitore liegen in `monitors.json`. Downgrade: unbekannter
   Typ 5 ist ein wirkungsloser Leaf-Monitor.

### Konfiguration durch den Kunden

- Monitor A: `MONITOR_MIN` auf Füllstandsensor, `value1 = 10` (aktiv), `value2 = 30`
  (inaktiv) als Hysterese gegen Flattern. Kombinierbar per AND/OR, z. B. zusätzlich
  Pumpenstörung.
- Monitor B: `MONITOR_SET_MASTER`, `monitor = A`, `master = 1`.
- Stationen: wie bisher nur „Master 1 verwenden“. Stationen, die explizit an Master 2
  hängen, laufen unverändert über Master 2.

### App (`/srv/www/htdocs/ui`)

- Monitor-Editor: neuer Typ „Master umleiten“ mit Quell-Monitor und Master-Auswahl.
- Statusanzeige: Hinweis „Stadtwasser aktiv“ aus `/jc masrd`.
- `station-attributes.js` unverändert.

### Tests / Doku

- `OpenSprinkler-Test/test_monitors.py`: Fall Umleitung an/aus mit laufender Station,
  Prüfung der Master-Bits über `/js` und der Off-Adjustments.
- `docs/docs/analog-sensor-config*.md` (Abschnitt Monitore), `pro-api-endpoints.md`
  (`/mc` Typ 5, `/jc masrd`), Platform-Matrix: alle Plattformen (plattformneutraler Code).

### Aufwand (Schätzung)

Firmware ca. 80–120 Zeilen, App ein Nachmittag, Tests/Doku ein weiterer. Risiko für
Bestandskunden: nur der Zweig `master_redirect != 0`, der ohne SET_MASTER-Monitor nie
erreicht wird.

## Verworfene Varianten

- **Attribute umschreiben** (`attrib_mas` ↔ `attrib_mas2` per Monitor kopieren): schreibt
  bei jedem Wechsel `stns.dat`, zerstört die Kundenkonfiguration, UI zeigt gewechselte
  Bindungen. Nein.
- **Alternativ-Station je Master-Slot** (Override der Master-SID): allgemeiner, braucht
  aber eine Refaktorierung aller `os.status.mas/mas2`-Prüfungen in einen Helfer und eine
  dritte Adjustment-Konfiguration. Nur sinnvoll, wenn beide Master-Slots für etwas
  anderes gebraucht werden.
- **Monitor schaltet Masterzone direkt**: wird von der Master-Schleife jede Sekunde
  überschrieben; Monitore lehnen Masterstationen bewusst ab.

## Entschieden (01.11.2026)

- Umschalten sofort, auch mitten im Lauf; die Hysterese am MIN-Monitor verhindert Flattern.
- Kein eigener Notification-Typ; die Meldung des Quell-Monitors (`NOTIFY_MONITOR_*`) reicht.
- Hinweis: `handle_master_adjustments()` berechnet `deque_time` beim Einplanen mit dem damals
  wirksamen Master; wechselt die Quelle mitten im Lauf, gilt für die laufende Station noch der
  Nachlauf des alten Masters. Bewusst so belassen.
