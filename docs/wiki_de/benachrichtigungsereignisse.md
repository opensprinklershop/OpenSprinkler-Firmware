# Benachrichtigungsereignisse

OpenSprinkler kann über **MQTT**, **Email Notifications** und **IFTTT Notifications** Benachrichtigungen versenden, wenn bestimmte Ereignisse eintreten – z. B. wenn eine Station startet, ein Sensor auslöst oder ein Rohrbruch erkannt wird.

Welche Ereignisse eine Benachrichtigung auslösen, lässt sich in der App gezielt einstellen.

---

## Wo finde ich die Einstellungen?

Öffne in der OpenSprinkler-App:

**Menü → Integrations**

Dort findest du folgende Bereiche:

| App-Bereich | Funktion |
|---|---|
| **MQTT** | MQTT-Broker konfigurieren |
| **Email Notifications** | E-Mail-Versand einrichten |
| **IFTTT Notifications** | IFTTT-Webhook konfigurieren |
| **Notification Events** → **Configure Events** | Auswählen, welche Ereignisse gemeldet werden |

Die Einstellung **Notification Events** / **Configure Events** gilt gemeinsam für alle aktivierten Benachrichtigungskanäle (MQTT, E-Mail, IFTTT).

---

## Verfügbare Ereignisse

| App-Bezeichnung | Deutsche Bedeutung | Beschreibung |
|---|---|---|
| **Program Start** | Programmstart | Wird ausgelöst, wenn ein Bewässerungsprogramm automatisch oder manuell startet. Kann auch auftreten, wenn ein Programm übersprungen wird (z. B. wegen Wetterbeschränkung oder 0 % Bewässerung). |
| **Station Start** | Stationsstart | Wird ausgelöst, wenn eine Zone / Station startet. |
| **Station Finish** | Stationsende | Wird ausgelöst, wenn eine Zone / Station ihre Laufzeit beendet hat. Die Benachrichtigung enthält die tatsächliche Laufzeit. |
| **Rain Delay Update** | Regenverzögerung aktualisiert | Wird ausgelöst, wenn die Regenverzögerung aktiviert oder deaktiviert wird. |
| **Sensor 1 Update** | Sensor 1 aktualisiert | Wird ausgelöst, wenn sich der Status von Sensor 1 ändert (z. B. Regen- oder Bodensensor). |
| **Sensor 2 Update** | Sensor 2 aktualisiert | Wird ausgelöst, wenn sich der Status von Sensor 2 ändert. |
| **Flow Sensor Update** | Durchflusssensor aktualisiert | Wird am Ende eines Stationslaufs ausgelöst, sofern ein Durchflusssensor eingerichtet ist. Die Benachrichtigung enthält die Impulszahl und das berechnete Wasservolumen. |
| **Flow Alert** | Durchflusswarnung | Wird ausgelöst, wenn der gemessene Durchfluss deutlich von den erwarteten Werten abweicht. Erfordert einen konfigurierten Durchflusssensor. |
| **No Flow Alert** | Kein-Durchfluss-Warnung | Wird ausgelöst, wenn eine Station läuft, aber kein Durchfluss erkannt wird. Mögliche Ursachen: geschlossenes Ventil, leere Wasserquelle oder Leitungsproblem. Erfordert einen konfigurierten Durchflusssensor. |
| **Pipe Burst Alert** | Rohrbruchwarnung | Wird ausgelöst, wenn Durchfluss erkannt wird, obwohl alle Stationen ausgeschaltet sind. Das kann auf ein Leck oder einen Rohrbruch hindeuten. Erfordert einen konfigurierten Durchflusssensor. |
| **Under/Overcurrent Fault** | Unter-/Überstrom-Fehler | Wird ausgelöst, wenn Unterstrom oder Überstrom erkannt wird. Bei Überstrom wird die betroffene Station sofort abgeschaltet. Unterstrom deutet oft auf ein Kabel- oder Magnetventilproblem hin. |
| **Weather Adjustment Update** | Wetteranpassung aktualisiert | Wird ausgelöst, wenn der Wetterdienst den Bewässerungsfaktor oder den Bewässerungswert aktualisiert. |
| **Controller Reboot** | Controller-Neustart | Wird ausgelöst, wenn der Controller neu startet. |
| **Monthly Water Report** | Monatlicher Wasserbericht | Wird zu Beginn eines neuen Monats ausgelöst und enthält die Wassernutzung des Vormonats. Erfordert einen konfigurierten Durchflusssensor. |
| **Monitoring-warnings level low** | Monitoring-Warnung – niedrig | Wird ausgelöst, wenn eine Monitoring-Regel mit niedriger Priorität anspringt. |
| **Monitoring-warnings level medium** | Monitoring-Warnung – mittel | Wird ausgelöst, wenn eine Monitoring-Regel mit mittlerer Priorität anspringt. |
| **Monitoring-warnings level high** | Monitoring-Warnung – hoch | Wird ausgelöst, wenn eine Monitoring-Regel mit hoher Priorität anspringt. |

---

## Hinweise und Tipps

### Durchflusssensor erforderlich

Die folgenden Ereignisse funktionieren nur, wenn ein Durchflusssensor korrekt eingerichtet und an **SN1** angeschlossen ist:

- **Flow Sensor Update**
- **Flow Alert**
- **No Flow Alert**
- **Pipe Burst Alert**
- **Monthly Water Report**

### Durchschnittlicher Durchflusswert (Average flow value)

In den Stationsattributen wird der Wert **Average flow value** angezeigt.  
Dieser Wert ist der **gelernte Durchschnittsdurchfluss** der jeweiligen Station – gemessen über mehrere Stationsläufe.  
Er dient als Referenzwert für die Durchflussüberwachung: Weicht der aktuelle Durchfluss stark davon ab, können **Flow Alert**, **No Flow Alert** oder **Pipe Burst Alert** ausgelöst werden.

### Zu viele Ereignisse vermeiden

> **Achtung:** Wenn zu viele Ereignisse oder mehrere Benachrichtigungskanäle gleichzeitig aktiv sind, kann dies zu Verzögerungen oder ausgelassenen kurzen Bewässerungsläufen führen.

Empfehlung: Aktiviere nur die Ereignisse, die du wirklich benötigst.

---

## Bezug zur App-Oberfläche

Die folgende Tabelle zeigt, wo die relevanten Begriffe in der App-UI zu finden sind:

| App-Begriff | Wo zu finden |
|---|---|
| **Integrations** | Hauptmenü |
| **MQTT** | Integrations → MQTT |
| **Email Notifications** | Integrations → Email Notifications |
| **IFTTT Notifications** | Integrations → IFTTT Notifications |
| **Notification Events** | Integrations → Notification Events |
| **Configure Events** | Integrations → Notification Events → Configure Events |
| **Average flow value** | Stationsattribute (Stationseinstellungen) |
