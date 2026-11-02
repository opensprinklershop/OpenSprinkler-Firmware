# Analog Sensor Konfiguration

Die **Analog Sensor Konfiguration** ist der OpenSprinklerPro-UI-Bereich zum Definieren von Sensorquellen und zum Verwenden ihrer Werte in der Bewässerungsautomation. Öffnen Sie ihn über das Fußmenü mit **Analog Sensor Konfiguration**.

![Analog Sensor Konfiguration](assets/screenshots/pro/de/sensor-config.png){ .mobile-screenshot }

Verwandte Seiten: [Sensor-Automation und Monitore](sensor-automation-de.md), [FYTA-Sensoren](fyta-sensors.md), [OpenSprinklerPro-Erweiterung](opensprinklerpro_de.md#monitore-und-lokale-warnungen), [API- und Plattform-Ergänzung](pro-api-endpoints.md).

## Sensordefinitionen

Der erste Abschnitt definiert die Sensorliste. Jede Zeile enthält Sensornummer, Name, aktuellen Wert und Aktivstatus. **Sensor hinzufügen** öffnet den Editor für einen neuen Sensor; bestehende Sensoren werden über den Sensornamen bearbeitet.

![Neuer Sensor](assets/screenshots/pro/de/analog-sensor-editor.png){ .mobile-screenshot }

### Editor-Felder

| Feld | Bedeutung |
|---|---|
| Nummer, Name und Gruppe | Identifizieren den Sensor in Listen, Diagrammen, Programm-Anpassungen und Monitoren. Gruppen dienen zum Sortieren und Filtern zusammengehöriger Sensoren. |
| Typ | Wählt Treiber oder virtuelle Quelle aus. Der Typ bestimmt, welche Zusatzfelder angezeigt werden. |
| Einheit und Format | Legt fest, wie Werte angezeigt, protokolliert und verglichen werden. Die Einheit sollte zum physikalischen Wert oder zur externen Datenquelle passen. |
| Intervall, Log und Anzeigen | Das Intervall steuert die Aktualisierung. Log aktiviert die Speicherung für Diagramme; Anzeigen steuert die Sichtbarkeit in Übersichten. |
| Bus, Adresse und Kanal | Für RS485/Modbus, ASB und OSPi-Analogeingänge zur Auswahl von Hardwarebus, Geräteadresse oder Analogkanal. |
| Skalierung für benutzerdefinierte Sensoren | Für eigene Analogsensoren wird hier die Umrechnung von Rohspannung/ADC-Wert in den endgültigen Messwert eingetragen. |
| MQTT-Topic und Filter | MQTT-Sensoren abonnieren ein Topic und extrahieren optional einen Wert aus JSON oder Text. |
| Quellsensorliste | Gruppensensoren berechnen einen neuen Wert aus ausgewählten Quellsensoren. Die Quellen sollten kompatible Einheiten verwenden. |

### RS485 / Modbus

Diese Typen sind für kabelgebundene Sensoren am RS485-Bus oder generische Modbus-RTU-Geräte. Stellen Sie Bus, Slave-Adresse, Register/Kanal und Leseintervall entsprechend dem angeschlossenen Gerät ein.

| Sensortyp | Erklärung |
|---|---|
| Truebner SMT100 RS485 Modbus, Feuchtemodus | Liest die Bodenfeuchte eines Truebner SMT100 über RS485/Modbus. Geeignet für Bewässerungsentscheidungen anhand des volumetrischen Wassergehalts. |
| Truebner SMT100 RS485 Modbus, Temperaturmodus | Liest den Temperaturkanal des SMT100. Nutzen Sie ihn, wenn Bodentemperatur protokolliert oder in Monitorregeln verwendet werden soll. |
| Truebner SMT100 RS485 Modbus, Permittivitätsmodus | Liest die dielektrische Permittivität des SMT100 für Diagnose oder erweiterte Bodenmodelle. |
| Truebner TH100 RS485 Modbus, Feuchtemodus | Liest die relative Luftfeuchte eines Truebner TH100. Nützlich für Gewächshaus- oder Umgebungskontrolle. |
| Truebner TH100 RS485 Modbus, Temperaturmodus | Liest die Temperatur des TH100 für Umgebung oder Einbauort. |
| RS485/MODBUS RTU generischer Sensor | Liest einen konfigurierbaren Modbus-RTU-Wert aus Fremdgeräten. Adresse, Register, Datenformat und Skalierung müssen zum Gerätehandbuch passen. |

### Analog Sensor Board (ASB)

ASB-Typen lesen Spannungseingänge des OpenSprinkler-Analogboards und wandeln sie in physikalische Werte um. Wählen Sie den richtigen Kanal und nur Sensormodi, die zur Verdrahtung und Versorgungsspannung passen.

| Sensortyp | Erklärung |
|---|---|
| ASB - Spannungsmodus 0..5V | Gibt die gemessene ASB-Eingangsspannung direkt im Bereich 0 bis 5 V aus. Geeignet für Kalibrierung oder Sensoren mit eigener Umrechnung. |
| ASB - 0..3,3V zu 0..100% | Wandelt eine Eingangsspannung von 0 bis 3,3 V in Prozent um. Nützlich für generische Pegel- oder Feuchtesignale. |
| ASB - SMT50 Feuchtemodus | Wandelt den Analogausgang eines Truebner SMT50 in Bodenfeuchte um. |
| ASB - SMT50 Temperaturmodus | Wandelt den Temperaturausgang des SMT50 in einen Temperaturwert um. |
| ASB - SMT100-analog Feuchtemodus | Wandelt den analogen Feuchteausgang eines Truebner SMT100 um. |
| ASB - SMT100-analog Temperaturmodus | Wandelt den analogen Temperaturausgang eines Truebner SMT100 um. |
| ASB - Vegetronix VH400 | Wandelt den Ausgang des Vegetronix VH400 in Bodenfeuchte um. |
| ASB - Vegetronix THERM200 | Wandelt den Ausgang des Vegetronix THERM200 in Temperatur um. |
| ASB - Vegetronix AquaPlumb | Wandelt den Ausgang des Vegetronix AquaPlumb für Wassererkennung oder Füllstands-Messungen um. |
| ASB - benutzerdefinierter Sensor | Nutzt eigene Skalierung für Sensoren ohne eingebaute Umrechnung. Einheit, Anzeigeformat und Umrechnung sorgfältig festlegen, bevor der Wert automatisiert wird. |

### OSPi-Analogeingänge

Diese Typen sind für Analogeingänge auf OSPi-/Raspberry-Pi-Installationen. Auf reinen ESP-Controllern ohne passende Analoghardware werden sie nicht verwendet.

| Sensortyp | Erklärung |
|---|---|
| OSPi-Analogeingang - Spannungsmodus 0..3,3V | Gibt die gemessene Eingangsspannung direkt im Bereich 0 bis 3,3 V aus. |
| OSPi-Analogeingang - 0..3,3V zu 0..100% | Wandelt einen 0- bis 3,3-V-Eingang in Prozent für generische Analogsensoren um. |
| OSPi-Analogeingang - SMT50 Feuchtemodus | Wandelt einen an OSPi angeschlossenen SMT50-Feuchteausgang um. |
| OSPi-Analogeingang - SMT50 Temperaturmodus | Wandelt einen an OSPi angeschlossenen SMT50-Temperaturausgang um. |
| Interne Raspberry-Pi-Temperatur | Liest die CPU-Temperatur des Raspberry Pi. Als Diagnosesensor für OSPi-Gehäuse oder Temperaturprüfung geeignet. |
| Interne ESP32-Temperatur | Liest den internen ESP32-Temperatursensor, sofern unterstützt. Als Controllerdiagnose verwenden, nicht als kalibrierte Umgebungstemperatur. |

### Funk-, Cloud- und Remote-Quellen

Diese Typen empfangen Werte aus Funkintegrationen, Cloud-Diensten, MQTT oder einem anderen OpenSprinkler-Controller. Prüfen Sie die Plattformverfügbarkeit: Zigbee und BLE benötigen passende ESP32-Builds, FYTA benötigt Cloud-Zugangsdaten, MQTT eine Broker-Konfiguration.

| Sensortyp | Erklärung |
|---|---|
| Zigbee-Sensor | Ordnet einen Wert eines gekoppelten Zigbee-Geräts zu, z. B. Temperatur, Luftfeuchte oder Bodenfeuchte. Gerät und Messwert auswählen. |
| BLE-Sensor | Liest einen unterstützten Bluetooth-Low-Energy-Sensor. Scanintervall und Gerät so wählen, dass Aktualität und Funklast passen. |
| FYTA-Feuchtesensor | Importiert Bodenfeuchte eines FYTA-Pflanzensensors nach der FYTA-Einrichtung. |
| FYTA-Temperatursensor | Importiert Temperatur eines FYTA-Pflanzensensors nach der FYTA-Einrichtung. |
| Gardena-Feuchtesensor | Importiert Bodenfeuchte eines Gardena smart Sensors über die Gardena Cloud-API. |
| Gardena-Temperatursensor | Importiert Temperatur eines Gardena smart Sensors über die Gardena Cloud-API. |
| MQTT-Abonnement | Abonniert ein MQTT-Topic und parst den empfangenen Wert. Topic, optionalen JSON-/Textfilter, Einheit und Timeout-Verhalten konfigurieren. |
| Remote-JSON-Sensor | Fragt einen beliebigen externen HTTP/HTTPS-JSON-Endpunkt ab und extrahiert den Wert über einen Suchkanal. |
| Remote-OpenSprinkler-Sensor | Liest einen Sensor von einem anderen OpenSprinkler-Controller. Remote-URL/Controller und Quellsensornummer konfigurieren. |

### Wetterdaten

Wettersensoren stellen Werte bereit, die dem Controller bereits aus dem konfigurierten Wetterdienst bekannt sind. Wählen Sie den Einheitstyp passend zu Regeln und Diagrammen.

| Sensortyp | Erklärung |
|---|---|
| Wetterdaten - Temperatur (°F) | Wetterdienst-Temperatur in Grad Fahrenheit. |
| Wetterdaten - Temperatur (°C) | Wetterdienst-Temperatur in Grad Celsius. |
| Wetterdaten - Luftfeuchte (%) | Relative Luftfeuchte des Wetterdiensts in Prozent. |
| Wetterdaten - Niederschlag (inch) | Niederschlag des Wetterdiensts in Zoll. |
| Wetterdaten - Niederschlag (mm) | Niederschlag des Wetterdiensts in Millimetern. |
| Wetterdaten - Wind (mph) | Windgeschwindigkeit des Wetterdiensts in Meilen pro Stunde. |
| Wetterdaten - Wind (kmh) | Windgeschwindigkeit des Wetterdiensts in Kilometern pro Stunde. |
| Wetterdaten - ETO | Referenz-Evapotranspiration als Kontextwert für Bewässerung. |
| Wetterdaten - Strahlung | Solarstrahlung aus der Wetterquelle, wenn verfügbar. |

### Gruppensensoren

Gruppensensoren erzeugen einen aggregierten Wert aus anderen Sensoren. Wählen Sie Quellsensoren mit vergleichbaren Einheiten und Log-Intervallen, damit das Ergebnis sinnvoll ist.

| Sensortyp | Erklärung |
|---|---|
| Sensorgruppe mit Minimalwert | Meldet den niedrigsten aktuellen Wert der ausgewählten Quellsensoren. |
| Sensorgruppe mit Maximalwert | Meldet den höchsten aktuellen Wert der ausgewählten Quellsensoren. |
| Sensorgruppe mit Durchschnittswert | Meldet den arithmetischen Durchschnitt der ausgewählten Quellsensoren. |
| Sensorgruppe mit Summenwert | Meldet die Summe der ausgewählten Quellsensoren, z. B. kombinierte Regen- oder durchflussähnliche Werte. |

### Diagnose

Diagnosesensoren stellen Controller-Zustandsdaten im gleichen Diagramm- und Monitorsystem wie Umweltsensoren bereit.

| Sensortyp | Erklärung |
|---|---|
| Freier Speicher | Meldet aktuell freien RAM. Monitore können Speicherdruck oder Lecks in Langzeittests erkennen. |
| Freier Speicherplatz | Meldet verbleibenden Speicherplatz, sofern unterstützt. Nützlich für Warnungen, bevor Logs oder Konfiguration das Dateisystem füllen. |

## Programm-Anpassungen

**Programm-Anpassungen** skalieren die Bewässerungsdauer eines Programms anhand eines Sensorwerts. Typische Beispiele sind weniger Bewässerung bei hoher Bodenfeuchte oder mehr Bewässerung bei hoher Temperatur.

![Programm-Anpassungen](assets/screenshots/pro/de/analog-program-adjustments.png){ .mobile-screenshot }

![Neue Programm-Anpassung](assets/screenshots/pro/de/analog-program-adjustment-editor.png){ .mobile-screenshot }

### Anpassungsmodell

Jede Anpassung verbindet einen Sensor mit einem Bewässerungsprogramm und berechnet einen Prozentfaktor. Aktivieren Sie zunächst das Logging des Sensors und prüfen Sie den Wertebereich, bevor reale Bewässerung beeinflusst wird.

### Anpassungstypen

| Typ | Verwendung |
|---|---|
| `LINEAR` | Skaliert kontinuierlich zwischen konfiguriertem unteren und oberen Punkt. |
| `DIGITAL_MIN` | Löst eine binäre Reduktion oder Sperre aus, wenn der Sensor unter einem Schwellwert liegt. |
| `DIGITAL_MAX` | Löst eine binäre Reduktion oder Sperre aus, wenn der Sensor über einem Schwellwert liegt. |
| `DIGITAL_MINMAX` | Wendet binäres Verhalten innerhalb oder außerhalb eines konfigurierten Bereichs an. |

Funktionsmodell und Formeln stehen unter [Sensor-Automation und Monitore](sensor-automation-de.md#sensorbasierte-programm-anpassungen). MCP/API-Automation kann `configure_adjustment` verwenden; REST-Details stehen in der [API- und Plattform-Ergänzung](pro-api-endpoints.md).

## Monitore und lokale Warnungen

**Überwachung und Kontrolle** erstellt Schwellwert- und Logikregeln aus Sensoren. Monitore können lokale Warnungen erzeugen, Benachrichtigungsereignisse speisen und Bedingungen wie „Boden ist trocken“ und „Zeitfenster ist morgens“ kombinieren.

![Überwachung und Kontrolle](assets/screenshots/pro/de/monitors.png){ .mobile-screenshot }

### Editor-Felder (alle Monitortypen)

Jeder Monitor besitzt unabhängig vom Typ die folgenden Felder. Über die Auswahl des **Typs** erscheinen die typspezifischen Felder, die weiter unten beschrieben sind.

| Feld | Bedeutung |
| --- | --- |
| **Monitor-Nr / Monitor-Name** | Eindeutige Nummer und ein sprechender Name, der in Listen und Warnungen angezeigt wird. |
| **Typ** | Wählt den Regeltyp. Ein Wechsel blendet die typspezifischen Felder um. |
| **Zu startendes Programm / Zu startende Zone** | Aktion, solange der Monitor aktiv ist: ein Programm oder eine einzelne Zone starten. Für reine Warn- oder Logikmonitore auf *Ausgeschaltet* lassen. |
| **Max. Laufzeit (s)** | Maximale Laufzeit der gestarteten Zone pro Aktivierung. |
| **Priorität** | Dringlichkeit der erzeugten Benachrichtigung (Niedrig / Mittel / Hoch). |
| **Zurücksetzen nach (s)** | Größer als 0: Der Aktiv-Zustand wird nach dieser Zeit automatisch zurückgesetzt (periodisches/pulsierendes Verhalten). 0 hält den Zustand, bis die Bedingung selbst wegfällt. |
| **Ausgangsmodus** | *Starten und Stoppen* steuert den Ausgang in beide Richtungen. *Nur ausschalten* lässt den Monitor die Zone/das Programm nur abschalten (z. B. als Not-Aus) und niemals starten. |
| **Failsafe nach (s)** | Liefert der referenzierte Sensor so lange keine gültigen Daten, wird der Failsafe-Zustand angewendet. 0 deaktiviert das Failsafe, der letzte Zustand bleibt erhalten. |
| **Failsafe-Zustand** | Ausgangszustand (Ein/Aus), der erzwungen wird, sobald die Sensordaten als veraltet gelten. |
| **Auf der Hauptseite zeigen** | Zeigt den Monitor und seinen aktuellen Zustand auf der App-Hauptseite. |

### Monitortypen

#### Mindestwert (MIN)

Aktiviert, wenn ein Sensorwert auf oder unter einen Schwellwert fällt – z. B. um eine Zisterne bei niedrigem Füllstand nachzufüllen. *Wert für Aktivierung* und *Wert für Deaktivierung* bilden ein Hysteresefenster, damit der Ausgang nicht ständig um einen einzelnen Wert herum schaltet.

![Mindestwert-Monitor](assets/screenshots/pro/de/monitor-min.png){ .mobile-screenshot }

#### Maximalwert (MAX)

Aktiviert, wenn ein Sensorwert auf oder über einen Schwellwert steigt – z. B. ein Übertemperatur- oder Überlaufalarm. Nutzt dasselbe Hysteresefenster (*Wert für Aktivierung* / *Wert für Deaktivierung*) wie der MIN-Typ.

![Maximalwert-Monitor](assets/screenshots/pro/de/monitor-max.png){ .mobile-screenshot }

#### Digitaler Sensor 1/2 (SN 1/2)

Spiegelt den Zustand des eingebauten digitalen Regen-/Bodensensor-Eingangs (Sensor 1 oder Sensor 2). *inverse* aktiviert die Reaktion auf den umgekehrten Zustand.

![Digitalsensor-Monitor](assets/screenshots/pro/de/monitor-sensor12.png){ .mobile-screenshot }

#### Digitalen Sensor setzen (SET SN 1/2)

Steuert den virtuellen Zustand des digitalen Sensors 1 oder 2 aus einem anderen Monitor. So kann eine berechnete Bedingung für die Steuerung wie ein physischer Regen-/Bodensensor wirken.

![Digitalsensor-setzen-Monitor](assets/screenshots/pro/de/monitor-set-sensor12.png){ .mobile-screenshot }

#### Master umleiten (SET MASTER)

Wasserquellen-Umschaltung: Solange der gewählte Quell-Monitor aktiv ist, laufen alle Stationen, die an den gewählten Master gebunden sind, stattdessen mit dem anderen Master (mit dessen eigenen Vor- und Nachlaufzeiten). Die Stationseinstellungen werden dabei nicht verändert, und ohne einen solchen Monitor verhält sich die Master-Steuerung wie bisher.

Typischer Aufbau *Zisterne mit Stadtwasser-Nachspeisung*: Master 1 = Zisternenpumpe, Master 2 = Stadtwasserventil, alle Zonen nur an Master 1 gebunden. Ein *Mindestwert*-Monitor auf dem Füllstandsensor (z. B. aktiv ab 10 %, inaktiv ab 30 % als Hysterese) dient als Quelle; der SET-MASTER-Monitor leitet Master 1 → Master 2 um. Fällt der Füllstand unter 10 %, wird sofort, auch mitten im Lauf, auf das Stadtwasserventil gewechselt. Die Monitor-Meldung des Quell-Monitors informiert über den Wechsel. Für einen ausgefallenen Füllstandsensor empfiehlt sich am Quell-Monitor *Failsafe* mit aktivem Zustand, damit im Zweifel Stadtwasser verwendet wird. Der aktive Zustand erscheint in der Statuszeile der App und in `/jc` als `masrd` (0 = keine Umleitung, 1 = Master 1 → 2, 2 = Master 2 → 1).

#### UND (AND)

Nur aktiv, wenn **alle** ausgewählten Quellmonitore aktiv sind. Jeder der bis zu vier Eingänge kann über seine *inverse*-Checkbox einzeln invertiert werden.

![UND-Monitor](assets/screenshots/pro/de/monitor-and.png){ .mobile-screenshot }

#### ODER (OR)

Aktiv, sobald **einer** der ausgewählten Quellmonitore aktiv ist. Eingänge lassen sich einzeln invertieren.

![ODER-Monitor](assets/screenshots/pro/de/monitor-or.png){ .mobile-screenshot }

#### Exklusiv-ODER (XOR)

Aktiv, wenn eine **ungerade** Anzahl der ausgewählten Eingänge aktiv ist (Exklusiv-ODER). Nützlich, um eine Abweichung zwischen zwei Bedingungen zu erkennen.

![XOR-Monitor](assets/screenshots/pro/de/monitor-xor.png){ .mobile-screenshot }

#### NICHT (NOT)

Invertiert den Zustand eines einzelnen Quellmonitors.

![NICHT-Monitor](assets/screenshots/pro/de/monitor-not.png){ .mobile-screenshot }

#### Zeitfenster (TIME)

Aktiv während eines täglichen Zeitfensters an den ausgewählten Wochentagen. *Von* und *Bis* festlegen (ein Fenster über Mitternacht wird unterstützt) und die Wochentage anhaken.

![Zeitfenster-Monitor](assets/screenshots/pro/de/monitor-time.png){ .mobile-screenshot }

#### Remote (REMOTE)

Spiegelt den Aktiv-Zustand eines Monitors auf einem anderen OpenSprinkler-Controller. *Remote Monitor Nr*, *IP* und *Port* des Controllers eintragen.

![Remote-Monitor](assets/screenshots/pro/de/monitor-remote.png){ .mobile-screenshot }

Siehe [Sensor-Automation und Monitore](sensor-automation-de.md#monitore) für das Funktionsmodell.

## Sensor-Diagramm und Daten

Das **Sensor Diagramm** zeigt protokollierte Sensorwerte. Nutzen Sie es, um Messwerte zu prüfen, bevor diese für Programm-Anpassungen oder Monitore verwendet werden. Die UI kann außerdem Logs herunterladen oder die Datentabelle öffnen.

![Sensor Diagramm](assets/screenshots/pro/de/analog-sensor-chart.png){ .mobile-screenshot }

### Diagramme lesen

Achten Sie auf fehlende Messpunkte, unerwartete Spitzen und falsch gewählte Einheiten. Vergleichen Sie verwandte Sensoren in derselben Gruppe, bevor sie automatische Entscheidungen steuern.

### Daten herunterladen

Protokollierte Daten können heruntergeladen oder als Tabelle geprüft werden. Das hilft bei Kalibrierung, Fehlersuche und Weitergabe von Sensorkurven.

## FYTA-Einrichtung

**FYTA Einrichtung** speichert FYTA-API-Token oder Login-Daten und ordnet FYTA-Pflanzensensoren OpenSprinkler-Sensoren zu. Details: [FYTA-Sensoren](fyta-sensors.md).

![FYTA Einrichtung](assets/screenshots/pro/de/fyta-setup.png){ .mobile-screenshot }

![FYTA-Zugangsdaten](assets/screenshots/pro/de/analog-fyta-credentials.png){ .mobile-screenshot }

### Zugangsdaten

Tragen Sie API-Token oder Login-Daten ein, die Ihre Firmware-Version benötigt. Halten Sie Zugangsdaten privat und aktualisieren Sie sie, wenn sich der FYTA-Zugriff ändert.

### Pflanzen zuordnen

Nach der Anmeldung werden FYTA-Pflanzenwerte für Feuchte und Temperatur OpenSprinkler-Sensoren zugeordnet, damit sie protokolliert, überwacht und für Anpassungen genutzt werden können.

## Gardena-Einrichtung

Die **Gardena-Einrichtung** ermöglicht die Verbindung mit der offiziellen Gardena-Cloud-API. Damit können Gardena smart Bodenfeuchte- und Temperatursensoren direkt importiert und in Ihren Ablaufregeln eingebunden werden.

### Gardena OAuth-Assistent

1. Registrieren Sie sich bei Bedarf im Gardena Developer Portal und erstellen Sie eine App, um Client-ID und Client-Secret zu erhalten.
2. Öffnen Sie im OpenSprinkler-Menü **Optionen -> Integrationen** oder direkt den **Gardena OAuth-Assistenten** in den Analogsensor-Optionen.
3. Geben Sie Ihre Anmeldedaten ein. Der Assistent führt Sie durch den OAuth 2.0 Login und fordert das Refresh-Token automatisch an.
4. Nach erfolgreichem Login werden verbundene Gardena-Geräte automatisch erkannt.

### Zuordnung von Gardena-Sensoren

Wählen Sie in den Sensordefinitionen den Typ `Gardena-Feuchtesensor` oder `Gardena-Temperatursensor` aus. Im Editor können Sie danach das gewünschte Gerät und den spezifischen Datenkanal auswählen.

---

## Remote-JSON-Sensor einrichten

Der **Remote-JSON-Sensor** ist ein extrem flexibler Typ zum Abfragen beliebiger REST-APIs, die Daten im JSON-Format zurückliefern. Er arbeitet ressourcenschonend direkt über einen Streaming-Parser.

### Editor-Felder

* **URL**: Die vollständige HTTP- oder HTTPS-Adresse der API (z. B. `http://api.weather.com/v1/sensor`). Max. 200 Zeichen. Sie können benutzerdefinierte Query-Parameter (wie Token, Passwörter, IDs) direkt in der URL mitsenden (z. B. `http://192.168.1.1/api?pw=123&js=1`).
* **JSON-Suchpfad (Filter)**: Der Pfad zum gewünschten Zahlenwert im JSON-Dokument. Verschachtelte Objekte werden mit einem senkrechten Strich (`|`) getrennt (z. B. `main|temp` oder `root|sensor_1|humidity`).

### Technische Details

* **Anfragemethode**: Es wird ausschließlich ein einfaches **HTTP-GET** ausgeführt.
* **Abfragetyp (Single Data Poll)**: Bei jedem Erreichen des Abfrageintervalls wird ein einzelner Messwert synchron aus der JSON-Antwort extrahiert (keine Abfrage historischer Logdateien oder Arrays).
* **Timeout**: Das Zeitfenster für den Verbindungsaufbau und Datenstrom beträgt fest **3000 Millisekunden**. Wenn der Endpunkt nicht innerhalb von 3 Sekunden antwortet, wird die Messung verworfenf.
* **Synchronisation (Empfehlenswertes Design)**:
  Um Timeouts und übermäßige externe API-Anfragen (wie z. B. an OpenMeteo) zu vermeiden, wird empfohlen, auf einem lokalen Server oder Router (wie OpenWrt) ein separates Skript (z. B. Cronjob) laufen zu lassen. Dieses Skript holt die externen Vorhersagen, dämpft sie (Anti-Aliasing) und speichert das Ergebnis als statisches JSON auf dem Router ab. OpenSprinkler pollt dann nur die lokale statische Datei in wenigen Millisekunden ab.

## Datensicherung und Wiederherstellung

**Datensicherung** exportiert oder stellt Sensor-Konfiguration, Programm-Anpassungen und Monitore getrennt vom allgemeinen Controller-Backup wieder her. Nutzen Sie dies vor größeren Sensoränderungen oder Firmwaretests.

![Datensicherung](assets/screenshots/pro/de/analog-backup-restore.png){ .mobile-screenshot }

### Enthaltene Daten

Backups enthalten Sensordefinitionen, Anpassungsregeln und Monitorregeln. Sie ersetzen kein vollständiges Controller-Backup für andere Einstellungen.

### Sicherer Wiederherstellungsablauf

Exportieren Sie vor vielen Änderungen ein Backup, stellen Sie nur Dateien aus vertrauenswürdigen Quellen wieder her und prüfen Sie Sensorwerte nach dem Restore, bevor automatische Anpassungen erneut aktiv genutzt werden.
