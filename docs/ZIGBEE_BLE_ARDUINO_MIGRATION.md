# ZigBee/BLE Arduino API Migration Analysis

**Erstellt**: 2026-02-21  
**Ziel**: Reduzierung von ESP-IDF direkten Aufrufen zugunsten Arduino-Abstraktion

## Status Quo

### BLE (sensor_ble.cpp)
✅ **Bereits Arduino-kompatibel:**
- Verwendet Arduino BLE Library: `BLEDevice`, `BLEScan`, `BLEClient`, `BLEAdvertisedDevice`
- Keine Nimble oder BlueDroid direkten Aufrufe

⚠️ **Verbesserungspotential:**
- Verwendet FreeRTOS direkt: `xSemaphoreTake`, `xSemaphoreGive`, `SemaphoreHandle_t`
- Könnte durch Arduino Mutex/Lock ersetzt werden (wenn verfügbar)

### ZigBee (sensor_zigbee.cpp, sensor_zigbee_gw.cpp)
✅ **Bereits Arduino-kompatibel:**
- Verwendet `Zigbee.h` Core-Klasse für Initialisierung
- Verwendet `ZigbeeEP` für Endpoint-Management
- Callbacks via Arduino ZigBee Framework

❌ **Noch ESP-IDF abhängig:**

#### 1. ZBOSS Lock Management
```cpp
esp_zb_lock_acquire(portMAX_DELAY);
// ... operations ...
esp_zb_lock_release();
```
**Verwendung:** 28+ Stellen in sensor_zigbee*.cpp  
**Grund:** ZBOSS (Zigbee stack) ist thread-unsafe, alle Zugriffe brauchen Lock  
**Arduino Alternative:** KEINE - Die Arduino Zigbee Library kapselt dies nicht

#### 2. Address Table Lookup
```cpp
uint16_t short_addr = esp_zb_address_short_by_ieee(ieee_le);
```
**Verwendung:** 8 Stellen  
**Grund:** Conversion IEEE (64-bit) → Short Address (16-bit) für remote Geräte  
**Arduino Alternative:** KEINE - Arduino ZigBee EP hat keine Address-Resolution API für remote devices

#### 3. ZCL Read Attribute Commands
```cpp
esp_zb_zcl_read_attr_cmd_t read_req;
// ... setup ...
esp_zb_zcl_read_attr_cmd_req(&read_req);
```
**Verwendung:** 6 Stellen (Client + Gateway mode)  
**Grund:** Lesen von Remote-Sensor-Attributen (z.B. Temperatur von Zigbee Sensor)  
**Arduino Alternative:** 
- `ZigbeeEP::readManufacturer()` / `readModel()` - NUR für Basic Cluster
- KEINE generische Read-API für beliebige Cluster/Attribute

#### 4. Custom Cluster Management
```cpp
esp_zb_zcl_cluster_list_create();
esp_zb_attribute_list_t *temp_cluster = esp_zb_zcl_attr_list_create(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT);
esp_zb_cluster_list_add_temperature_meas_cluster(_cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
```
**Verwendung:** ClientZigbeeReportReceiver Klasse (sensor_zigbee.cpp:314-350)  
**Grund:** Multi-Cluster Client Endpoint (Temp, Humidity, Soil, Light, Pressure, Power)  
**Arduino Alternative:**
- Arduino hat `ZigbeeTempSensor`, `ZigbeeIlluminanceSensor` etc., aber als **separate Endpoints**
- KEINE Möglichkeit, mehrere Client-Cluster in einem Endpoint zu kombinieren

#### 5. Network Management
```cpp
esp_zb_bdb_open_network(duration);
```
**Verwendung:** sensor_zigbee_gw.cpp für Permit-Join  
**Arduino Alternative:** `Zigbee.openNetwork(duration)` - ✅ **BEREITS VERWENDET!**  
**Status:** ✅ Bereits migriert in sensor_zigbee_gw.cpp:1110

#### 6. IEEE 802.15.4 Coexistence
```cpp
#include <esp_coex_i154.h>
esp_coex_ieee802154_ack();
```
**Verwendung:** BEREITS ENTFERNT (war in radio_arbiter)  
**Status:** ✅ Nicht mehr verwendet

#### 7. Partition & NVS
```cpp
#include <esp_partition.h>
esp_partition_find_first(...)
```
**Verwendung:** Minimal (Factory Reset)  
**Arduino Alternative:** KEINE - Low-Level Zugriff nötig für NVRAM-Clear

## Empfehlungen

### Kurzfristig (Quick Wins) ✅

1. **Network Open Command ersetzen:**
   ```cpp
   // Alt:
   esp_zb_bdb_open_network(duration);
   
   // Neu:
   Zigbee.openNetwork(duration);
   ```
   **Aufwand:** Gering, 2 Stellen betroffen

2. **FreeRTOS Semaphore durch Arduino ersetzen (nur wenn Arduino Lock vorhanden):**
   ```cpp
   // Prüfen ob verfügbar in Arduino ESP32 4.x core
   ```

### Mittelfristig (Refactoring erforderlich) ⚠️

3. **Custom Multi-Cluster Endpoint vereinfachen:**
   - Aktuell: Ein Endpoint mit 7+ Client Clusters
   - Arduino Ansatz: Mehrere Endpoints mit jeweils 1 Cluster
   - **Vorteil:** Vollständig Arduino-kompatibel
   - **Nachteil:** Mehr Endpoints = mehr RAM, komplexere Routing-Tabelle
   - **Entscheidung notwendig:** Performance vs. Abstraktion

### Langfristig (Nicht empfohlen) ❌

4. **ESP-IDF Lock/Address/Read behalten:**
   - **Grund:** Arduino Zigbee Library ist noch jung (2025), keine vollständige Client-Mode API
   - **Diese Aufrufe sind notwendig** für:
     - Remote Attribute Reading (unser Hauptfeature)
     - Address Resolution (ohne geht nichts)
     - Thread Safety (ZBOSS Anforderung)
   - **Alternative:** Warten auf Arduino Zigbee Library Updates, die diese Features kapseln

## Konkrete Handlungsschritte

### Phase 1: Arduino wo möglich ✅ **ERLEDIGT**
- [x] ~~`esp_zb_bdb_open_network` → `Zigbee.openNetwork` in sensor_zigbee_gw.cpp~~ **BEREITS ERLEDIGT**
- [x] Geprüft: Arduino ZigbeeCore hat KEINE `lock()`/`unlock()` Methoden → ESP-IDF Lock bleibt notwendig

### Phase 2: Dokumentation & Isolation 📝 **ERLEDIGT**
- [x] Alle verbleibenden ESP-IDF Aufrufe in Helper-Funktionen gekapselt:
  - `zigbee_lock_acquire()` / `zigbee_lock_release()` → **zigbee_espidf_wrappers.h**
  - `ZigbeeLockGuard` RAII-Klasse → **zigbee_espidf_wrappers.h**
  - `zigbee_ieee_to_short_addr()` → **zigbee_espidf_wrappers.h**
  - `zigbee_read_remote_attribute()` → **zigbee_espidf_wrappers.cpp**
- [x] Ausführliche Kommentare hinzugefügt: "// ESP-IDF direct call - no Arduino equivalent as of 2026"
- [x] Jede Wrapper-Funktion dokumentiert:
  - WARUM sie notwendig ist
  - WARUM Arduino (noch) keine Alternative hat
  - Wann sie durch Arduino ersetzt werden kann

### Phase 3: Optional - Code Migration 🔄
- [ ] Migriere bestehenden Code von direkten `esp_zb_*` Aufrufen zu Wrapper-Funktionen
- [ ] Ersetze manuelle Lock-Paare durch `ZigbeeLockGuard` (RAII)
- [ ] Konsolidiere duplizierte Muster (z.B. IEEE→Short-Address Conversion)

**HINWEIS:** Phase 3 ist optional und bringt hauptsächlich Code-Klarheit, keine funktionalen Änderungen.

### Phase 3: Future-Proofing 🔮
- [ ] Überwache Arduino ESP32 Updates für neue Zigbee API Features
- [ ] Wenn Arduino Library erweitert wird: Migration durchführen

## Fazit

**BLE:** ✅ Bereits weitgehend Arduino-kompatibel, minimales Refactoring

**ZigBee:** ⚠️ Hybrid-Ansatz ist **derzeit optimal**:
- High-Level (Start/Stop/Callbacks): Arduino `Zigbee.h` / `ZigbeeEP`
- Low-Level (Attribute Read/Address Lookup/Lock): ESP-IDF direkt

**Begründung:** Die Arduino Zigbee Library (Stand 2025/2026) ist primär für **End Devices als Sensor-Sender** optimiert, nicht für **Client Mode als Sensor-Empfänger**. Unsere Anwendung (OpenSprinkler liest von entfernten Zigbee Sensoren) benötigt Features, die Arduino noch nicht abstrahiert.

**Empfehlung:** 
1. Quick Win umsetzen (openNetwork)
2. Verbleibende ESP-IDF Calls akzeptieren und dokumentieren
3. Code-Review in 6-12 Monaten, wenn Arduino Zigbee Library reifer ist

---
**Autor:** GitHub Copilot  
**Review:** Stefan Schmaltz
