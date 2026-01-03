# Zigbee Cluster Library (ZCL) Referenz für OpenSprinkler

Diese Datei dokumentiert alle für OpenSprinkler relevanten Zigbee-Cluster basierend auf der **ZCL Specification 07-5123-06**.

## 📚 Übersicht

Die Zigbee Cluster Library definiert standardisierte Cluster für verschiedene Sensor- und Aktor-Typen. Jeder Cluster hat eindeutige IDs für Cluster und Attribute.

## 🌱 Umwelt-/Garten-Sensoren (Measurement & Sensing Clusters)

### 1. Temperature Measurement (0x0402)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | int16 | -27315 - 32767 | 0.01°C | Wert ÷ 100 |
| MinMeasuredValue | 0x0001 | int16 | -27315 - 32767 | 0.01°C | Wert ÷ 100 |
| MaxMeasuredValue | 0x0002 | int16 | -27315 - 32767 | 0.01°C | Wert ÷ 100 |
| Tolerance | 0x0003 | uint16 | 0 - 2048 | 0.01°C | Wert ÷ 100 |

**ZCL Spec Reference**: Section 4.4  
**Beispiel**: Rohwert `2350` = 23.50°C  
**OpenSprinkler Unit**: `UNIT_DEGREE` (2)

---

### 2. Pressure Measurement (0x0403)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | int16 | -32768 - 32767 | 0.1 kPa | Wert ÷ 10 |
| MinMeasuredValue | 0x0001 | int16 | -32768 - 32767 | 0.1 kPa | Wert ÷ 10 |
| MaxMeasuredValue | 0x0002 | int16 | -32768 - 32767 | 0.1 kPa | Wert ÷ 10 |

**ZCL Spec Reference**: Section 4.5  
**Beispiel**: Rohwert `10132` = 1013.2 kPa  
**Note**: 1 kPa = 10 mbar, Atmosphärendruck ≈ 101.3 kPa

---

### 3. Flow Measurement (0x0404)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | uint16 | 0 - 65535 | 0.1 m³/h | Wert ÷ 10 |
| MinMeasuredValue | 0x0001 | uint16 | 0 - 65535 | 0.1 m³/h | Wert ÷ 10 |
| MaxMeasuredValue | 0x0002 | uint16 | 0 - 65535 | 0.1 m³/h | Wert ÷ 10 |

**ZCL Spec Reference**: Section 4.6  
**Anwendung**: Durchflussmessung für Bewässerungssysteme

---

### 4. Relative Humidity Measurement (0x0405)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | uint16 | 0 - 10000 | 0.01% RH | Wert ÷ 100 |
| MinMeasuredValue | 0x0001 | uint16 | 0 - 10000 | 0.01% RH | Wert ÷ 100 |
| MaxMeasuredValue | 0x0002 | uint16 | 0 - 10000 | 0.01% RH | Wert ÷ 100 |

**ZCL Spec Reference**: Section 4.7  
**Beispiel**: Rohwert `6550` = 65.50% RH  
**OpenSprinkler Unit**: `UNIT_PERCENT` (1)

---

### 5. Illuminance Measurement (0x0400)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | uint16 | 0 - 65534 | Lux | 10^((Wert - 1) / 10000) |
| LightSensorType | 0x0004 | enum8 | 0 - 255 | - | - |

**ZCL Spec Reference**: Section 4.2  
**Beispiel**: Rohwert `10000` ≈ 2.72 Lux (logarithmisch!)  
**OpenSprinkler Unit**: `UNIT_LX` (13)  
**Hinweis**: Nicht-lineare Konvertierung!

---

### 6. Soil Moisture Measurement (0x0408)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | uint16 | 0 - 10000 | 0.01% | Wert ÷ 100 |
| MinMeasuredValue | 0x0001 | uint16 | 0 - 10000 | 0.01% | Wert ÷ 100 |
| MaxMeasuredValue | 0x0002 | uint16 | 0 - 10000 | 0.01% | Wert ÷ 100 |

**ZCL Spec Reference**: Section 4.10  
**Beispiel**: Rohwert `2350` = 23.50%  
**OpenSprinkler Unit**: `UNIT_PERCENT` (1)  
**Anwendung**: Bodenfeuchte-Sensoren (z.B. GIEX, Tuya)

---

### 7. Leaf Wetness (0x0407)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | uint16 | 0 - 100 | % | direkt |

**ZCL Spec Reference**: Section 4.9  
**Anwendung**: Blattnässe-Überwachung (Krankheitsprävention)

---

## 🔋 Energie & Batterie

### 8. Power Configuration (0x0001)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MainsVoltage | 0x0000 | uint16 | 0 - 65535 | 0.1V | Wert ÷ 10 |
| BatteryVoltage | 0x0020 | uint8 | 0 - 255 | 0.1V | Wert ÷ 10 |
| BatteryPercentageRemaining | 0x0021 | uint8 | 0 - 200 | 0.5% | Wert ÷ 2 |
| BatteryAlarmState | 0x003E | bitmap32 | - | - | bitweise |

**ZCL Spec Reference**: Section 3.3  
**Beispiel**: BatteryPercentageRemaining `180` = 90%  
**OpenSprinkler Unit**: `UNIT_PERCENT` (1)

---

## 🌡️ Erweiterte Sensoren

### 9. Occupancy Sensing (0x0406)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| Occupancy | 0x0000 | bitmap8 | 0-1 | bool | Bit 0 |
| OccupancySensorType | 0x0001 | enum8 | 0-3 | - | direkt |

**Anwendung**: Bewegungsmelder für Wildtier-Abschreckung  
**OpenSprinkler Unit**: `UNIT_LEVEL` (10)

---

### 10. Carbon Dioxide (CO₂) Measurement (0x040D)

| Attribut | ID | Typ | Bereich | Einheit | Konvertierung |
|----------|------|------|---------|---------|---------------|
| MeasuredValue | 0x0000 | float | 0 - max | ppm | direkt (float) |
| MinMeasuredValue | 0x0001 | float | - | ppm | direkt |
| MaxMeasuredValue | 0x0002 | float | - | ppm | direkt |

**Anwendung**: Gewächshaus-Überwachung

---

## 📊 Konvertierungs-Zusammenfassung

### Einfache Skalierung (÷100):
- **0x0402** Temperature
- **0x0405** Humidity
- **0x0408** Soil Moisture

### Dezimale Skalierung (÷10):
- **0x0403** Pressure (kPa)
- **0x0404** Flow (m³/h)

### Batterie-Skalierung (÷2):
- **0x0001:0x0021** Battery Percentage

### Spezielle Konvertierung:
- **0x0400** Illuminance: logarithmisch `10^((value - 1) / 10000)`

---

## 🔧 OpenSprinkler-Implementierung

### Cluster-Definitionen in `sensor_zigbee.cpp`:

```cpp
// Zigbee Cluster IDs (ZCL spec)
#define ZB_ZCL_CLUSTER_ID_BASIC                     0x0000
#define ZB_ZCL_CLUSTER_ID_POWER_CONFIG              0x0001
#define ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT          0x0402
#define ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT      0x0403
#define ZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT          0x0404
#define ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT  0x0405
#define ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING         0x0406
#define ZB_ZCL_CLUSTER_ID_LEAF_WETNESS              0x0407
#define ZB_ZCL_CLUSTER_ID_SOIL_MOISTURE             0x0408
#define ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT   0x0400
#define ZB_ZCL_CLUSTER_ID_CO2_MEASUREMENT           0x040D
```

### Automatische Konvertierung:

```cpp
void ZigbeeSensor::zigbee_attribute_callback(...) {
    double converted_value = (double)value;
    
    switch(cluster_id) {
        case 0x0408: // Soil Moisture
        case 0x0405: // Humidity
        case 0x0402: // Temperature
            converted_value = value / 100.0;
            break;
            
        case 0x0403: // Pressure
        case 0x0404: // Flow
            converted_value = value / 10.0;
            break;
            
        case 0x0001: // Battery
            if (attr_id == 0x0021)
                converted_value = value / 2.0;
            break;
            
        case 0x0400: // Illuminance (logarithmic!)
            converted_value = pow(10, (value - 1) / 10000.0);
            break;
    }
    
    // Apply user-defined factor/divider/offset
    // ...
}
```

---

## 🌐 Online Cluster-Datenbank

Für dynamische Cluster-Definitionen wird empfohlen:

**`https://opensprinklershop.de/zigbee/clusters.json`**

```json
{
  "version": "1.0",
  "updated": "2026-01-01",
  "clusters": [
    {
      "id": "0x0408",
      "name": "Soil Moisture Measurement",
      "attributes": [
        {
          "id": "0x0000",
          "name": "MeasuredValue",
          "type": "uint16",
          "unit": "%",
          "conversion": "÷100",
          "range": "0-100%"
        }
      ]
    }
  ]
}
```

### Vorschlag: Email-Melde-Funktion

```http
POST https://opensprinklershop.de/zigbee/report-sensor
Content-Type: application/json

{
  "manufacturer": "GIEX",
  "model": "ZG-100",
  "cluster_id": "0x0408",
  "attribute_id": "0x0000",
  "description": "Soil moisture & temperature sensor",
  "user_email": "user@example.com",
  "firmware_version": "4.0.5"
}
```

---

## 📖 Quellen

- **ZCL Specification**: `07-5123-06-zigbee-cluster-library-specification.pdf`
- **Zigbee Alliance**: https://zigbeealliance.org/
- **ESP-Zigbee SDK**: https://github.com/espressif/esp-zigbee-sdk

---

**Hinweis**: Diese Referenz wird kontinuierlich erweitert, sobald neue Sensoren getestet werden.
