# Analog Sensor API

## Overview

The Analog Sensor Extension for OpenSprinkler firmware provides comprehensive support for integrating various sensors into your irrigation control system. This extension enables data-driven irrigation decisions based on real-time environmental conditions, helping to optimize water usage and plant health.

### Key Features

**Sensor Support:**
- **RS485/Modbus Sensors:** Professional-grade sensors like Truebner SMT100 (soil moisture, temperature, permittivity modes) and TH100 (humidity, temperature)
- **Analog Sensors:** Support for analog extension boards (ASB) with voltage inputs (0-5V, 0-3.3V) and popular sensors like Vegetronix VH400, SMT50, THERM200
- **Wireless Sensors:** MQTT, BLE, and Zigbee sensor integration
- **Remote Sensors:** Query sensors from other OpenSprinkler devices on your network
- **Weather Data:** Integration of weather service data (temperature, humidity, precipitation, wind, ETO)
- **Virtual Sensors:** Create sensor groups (min, max, average, sum) to combine multiple sensor readings
- **User-Defined Sensors:** Configure custom sensors with flexible conversion formulas

**Data Management:**
- **Historical Logging:** Multi-tier logging system (daily, weekly, monthly) with configurable retention
- **Data Export:** Export sensor data in JSON or CSV format for analysis
- **Filtering & Queries:** Advanced filtering by time range, sensor type, value thresholds
- **Automatic Cleanup:** Remove outliers and old data automatically

**Irrigation Control:**
- **Program Adjustments:** Automatically adjust watering duration based on sensor readings using linear or digital scaling
- **Monitoring Rules:** Create complex conditions to trigger programs (min/max thresholds, time windows, logical operations)
- **Multi-Factor Logic:** Combine multiple sensors and monitors using AND/OR/XOR/NOT logic gates
- **Safety Features:** Maximum runtime limits, cooldown periods, priority-based execution

**Configuration:**
- **Flexible Unit System:** Support for %, V, mV, °C, °F, kPa, cbar, and custom units
- **Calibration Tools:** Offset and scaling factors for accurate measurements
- **Backup & Restore:** Export and import complete sensor configurations
- **Network Integration:** RS485, MQTT, HTTP-based sensor protocols

### Authentication

All API endpoints require password authentication using the `pw` parameter. The password can be provided in two formats:

1. **Cleartext Password:** `pw=your_password` (not recommended for production)
2. **MD5 Hash:** `pw=<passwordhash>` where `<passwordhash>` is the MD5 hash of your password

For security reasons, all examples in this documentation use the placeholder `<passwordhash>`. Replace this with your actual password or its MD5 hash when making API calls.

**Example:**
```
# Using cleartext password (development only)
/sl?pw=mypassword

# Using MD5 hash (recommended)
/sl?pw=a94a8fe5ccb19ba61c4c0873d391e987
```

---

## Recent Updates

### February 2026

**Unified IEEE 802.15.4 Support (ESP32-C5):**
- Native ZigBee and Matter support merged into a single firmware binary (`esp32-c5` PlatformIO environment)
- Runtime-selectable radio mode via `/ir` and `/iw` endpoints (see [IEEE802154_API.md](IEEE802154_API.md))
- ZigBee Gateway mode (`activeMode=2`): ESP32-C5 as ZigBee Coordinator with device management (`/zg`)
- ZigBee Client mode (`activeMode=3`): ESP32-C5 joins existing ZigBee networks with network search (`/zj`)
- New endpoints: `/ir`, `/iw`, `/zj`, `/zs`, `/zg` for IEEE 802.15.4 radio configuration and ZigBee management
- Legacy ZigBee endpoints (`/zd`, `/zo`, `/zc`) remain available for backward compatibility
- Matter pairing endpoint (`/jm`) updated with `matter_enabled` field based on runtime mode
- Updated ZigBee sensor documentation (Native ZigBee and Zigbee2MQTT modes)

### January 2025

**Sensor Type IDs:**
- Added missing sensor types: Zigbee (95), BLE (96), FYTA sensors (60-61)
- Corrected ASB sensor type IDs (10, 11, 15-18, 30-32, 49)
- Added diagnostic sensor types (10000-10001)
- Updated sensor group types (1000-1003)

**Unit IDs:**
- Added new unit types: km/h (9), Level (10), Permittivity/DK (11), Lumen (12), Lux (13)
- Corrected unit ID numbering scheme (0-13, 99)

**Monitor Types:**
- Corrected monitor type IDs: AND (10), OR (11), XOR (12), NOT (13), TIME (14), REMOTE (100)
- Updated monitor type descriptions

**Additional Enhancements:**
- Added RS485 flags configuration details with bit-level documentation
- Added sensor group usage examples and explanations
- Improved MQTT sensor configuration examples
- Added data type support for RS485 sensors (uint16, int16, uint32, int32, float, double)
- Added detailed configuration guides for all sensor types:
  - BLE sensors (ESP32 and OSPI/BlueZ)
  - Zigbee sensors (via Zigbee2MQTT)
  - FYTA cloud sensors
  - Remote OpenSprinkler sensors
  - Weather service sensors
  - Internal/diagnostic sensors
- Added GATT characteristic UUID configuration for BLE
- Added RF coexistence notes for ESP32 BLE/WiFi
- Added FYTA API integration details
- Added weather sensor usage guidelines

---

## Table of Contents
- [Platform Availability](#platform-availability)
- [Sensor Configuration](#sensor-configuration)
- [Sensor Data Retrieval](#sensor-data-retrieval)
- [Sensor Logging](#sensor-logging)
- [Program Adjustments](#program-adjustments)
- [Monitoring](#monitoring)
- [Utility Endpoints](#utility-endpoints)

---

## Platform Availability

Not all endpoints and sensor types are available on every hardware platform. The table below summarises platform support for each endpoint group.

### Endpoint Availability

| Endpoint | Description | ESP32 | ESP32-C5 | ESP8266 | OSPi/Linux |
|----------|-------------|:-----:|:--------:|:-------:|:----------:|
| `/si` | Configure user-defined sensor params | ✅ | ✅ | ✅ | ✅ |
| `/sc` | Configure sensor | ✅ | ✅ | ✅ | ✅ |
| `/sj` | Get MQTT/URL config | ✅ | ✅ | ✅ | ✅ |
| `/sk` | Set MQTT/URL config | ✅ | ✅ | ✅ | ✅ |
| `/sa` | Set RS485 address | ✅ | ✅ | ✅ | ✅ |
| `/sl` | List all sensors | ✅ | ✅ | ✅ | ✅ |
| `/sg` | Get sensor values | ✅ | ✅ | ✅ | ✅ |
| `/sr` | Read sensor now | ✅ | ✅ | ✅ | ✅ |
| `/sf` | Get sensor types | ✅ | ✅ | ✅ | ✅ |
| `/so` | Get sensor log | ✅ | ✅ | ✅ | ✅ |
| `/sn` | Clear sensor log | ✅ | ✅ | ✅ | ✅ |
| `/sb` | Configure adjustment | ✅ | ✅ | ✅ | ✅ |
| `/se` | List adjustments | ✅ | ✅ | ✅ | ✅ |
| `/sd` | Calculate adjustment | ✅ | ✅ | ✅ | ✅ |
| `/sh` | Get adjustment types | ✅ | ✅ | ✅ | ✅ |
| `/mc` | Configure monitor | ✅ | ✅ | ✅ | ✅ |
| `/ml` | List monitors | ✅ | ✅ | ✅ | ✅ |
| `/mt` | Get monitor types | ✅ | ✅ | ✅ | ✅ |
| `/sx` | Backup config | ✅ | ✅ | ✅ | ✅ |
| `/du` | System resources | ✅ | ✅ | ✅ | ✅ |
| `/ir` | IEEE 802.15.4 mode read | ❌ | ✅ | ❌ | ❌ |
| `/iw` | IEEE 802.15.4 mode write | ❌ | ✅ | ❌ | ❌ |
| `/zj` | ZigBee join network | ❌ | ✅¹ | ❌ | ❌ |
| `/zs` | ZigBee status | ❌ | ✅¹ | ❌ | ❌ |
| `/zg` | ZigBee device management | ❌ | ✅¹ | ❌ | ❌ |
| `/zd` | ZigBee discovered devices | ❌ | ✅¹ | ❌ | ❌ |
| `/zo` | ZigBee open network | ❌ | ✅¹ | ❌ | ❌ |
| `/zc` | ZigBee clear flags | ❌ | ✅¹ | ❌ | ❌ |
| `/jm` | Matter pairing info | ✅² | ✅² | ❌ | ❌ |
| `/bd` | BLE device scan | ✅ | ❌ | ❌ | ❌ |
| `/bs` | BLE device status | ✅ | ❌ | ❌ | ❌ |
| `/bc` | BLE device clear | ✅ | ❌ | ❌ | ❌ |

> ¹ Requires `OS_ENABLE_ZIGBEE` build flag  
> ² Requires `ENABLE_MATTER` build flag

### Sensor Type Availability

| Sensor Type | Description | ESP32 | ESP32-C5 | ESP8266 | OSPi/Linux |
|-------------|-------------|:-----:|:--------:|:-------:|:----------:|
| 1–9 | RS485/Modbus | ✅ | ✅ | ✅ | ✅ |
| 10–49 | ASB (Analog Sensor Board) | ✅ | ✅ | ✅ | ❌ |
| 50–54 | OSPI analog inputs | ❌ | ❌ | ❌ | ✅ |
| 60–61 | FYTA cloud sensors | ✅ | ✅ | ✅ | ✅ |
| 90 | MQTT subscription | ✅ | ✅ | ✅ | ✅ |
| 95 | ZigBee (native) | ❌ | ✅ | ❌ | ❌ |
| 95 | ZigBee (via Zigbee2MQTT/MQTT) | ✅ | ✅ | ✅ | ✅ |
| 96 | BLE sensor | ✅ | ❌ | ❌ | ✅³ |
| 100 | Remote OS sensor | ✅ | ✅ | ✅ | ✅ |
| 101–110 | Weather service | ✅ | ✅ | ✅ | ✅ |
| 1000–1003 | Sensor groups | ✅ | ✅ | ✅ | ✅ |
| 10000–10001 | Diagnostic (free mem/storage) | ✅ | ✅ | ✅ | ✅ |

> ³ OSPi/Linux uses BlueZ D-Bus API instead of native ESP32 BLE

---

## Sensor Configuration

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### Configure User-Defined Sensor Parameters
**Endpoint:** `/si`  
**Command:** `si`  
**HTTP Method:** GET  
**Description:** Configure user-defined sensor parameters including conversion factors, dividers, units, and offsets.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Sensor number (must be > 0) |
| `fac` | integer | No | Multiplication factor for sensor value conversion |
| `div` | integer | No | Division factor for sensor value conversion |
| `unit` | string | No | Custom unit name (URL-encoded, max 7 characters) |
| `unitid` | integer | No | Assigned unit ID from predefined units |
| `offset` | integer | No | Offset in millivolts for analog sensors |
| `offset2` | integer | No | Offset in unit values applied after conversion |

#### Response
```json
{
  "result": 1
}
```

#### Example
```
/si?pw=<passwordhash>&nr=5&fac=100&div=1&unit=cbar&offset=0&offset2=-50
```

---

### Configure Sensor
**Endpoint:** `/sc`  
**Command:** `sc`  
**HTTP Method:** GET  
**Description:** Configure a sensor with all parameters including type, network settings, and reading intervals.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Sensor number (must be > 0) |
| `type` | integer | Yes | Sensor type (0 = delete sensor, see `/sf` for types) |
| `group` | integer | Yes* | Sensor group number |
| `name` | string | Yes* | Sensor name (URL-encoded, max 29 characters) |
| `ip` | integer | Yes* | IP address as unsigned 32-bit integer |
| `port` | integer | Yes* | Network port number |
| `id` | integer | Yes* | Modbus ID for RS485 sensors |
| `ri` | integer | Yes* | Read interval in seconds |
| `enable` | integer | No | 1 = enabled, 0 = disabled (default: 1) |
| `log` | integer | No | 1 = logging enabled, 0 = disabled (default: 1) |
| `show` | integer | No | 1 = show in UI, 0 = hidden (default: 0) |
| `fac` | integer | No | Multiplication factor |
| `div` | integer | No | Division factor |
| `unit` | string | No | Custom unit (URL-encoded) |
| `unitid` | integer | No | Unit ID |
| `offset` | integer | No | Offset in millivolts |
| `offset2` | integer | No | Unit value offset |
| `rs485flags` | integer | No | RS485 configuration flags |
| `rs485code` | integer | No | RS485 function code |
| `rs485reg` | integer | No | RS485 register address |
| `url` | string | No | MQTT broker URL (URL-encoded) |
| `topic` | string | No | MQTT topic (URL-encoded) |
| `filter` | string | No | JSON filter path (URL-encoded) |

*Required unless `type=0` (delete)

#### Response
```json
{
  "result": 1
}
```

#### Example - Add Modbus Sensor
```
/sc?pw=<passwordhash>&nr=1&type=1&group=0&name=Moisture%20Sensor&ip=3232235777&port=502&id=1&ri=300&enable=1&log=1
```

#### Example - Delete Sensor
```
/sc?pw=<passwordhash>&nr=5&type=0
```

---

### Get MQTT/URL Configuration
**Endpoint:** `/sj`  
**Command:** `sj`  
**HTTP Method:** GET  
**Description:** Retrieve MQTT or URL configuration for a sensor.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Sensor number |
| `type` | integer | Yes | 0 = URL, 1 = MQTT topic, 2 = JSON filter |

#### Response
```json
{
  "value": "mqtt://broker.example.com:1883"
}
```

---

### Set MQTT/URL Configuration
**Endpoint:** `/sk`  
**Command:** `sk`  
**HTTP Method:** GET  
**Description:** Configure MQTT or URL settings for a sensor.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Sensor number |
| `type` | integer | Yes | 0 = URL, 1 = MQTT topic, 2 = JSON filter |
| `value` | string | Yes | Configuration value (URL-encoded) |

#### Response
```json
{
  "result": 1
}
```

#### Example
```
/sk?pw=<passwordhash>&nr=3&type=1&value=home%2Fgarden%2Fmoisture
```

---

### Set RS485 Sensor Address
**Endpoint:** `/sa`  
**Command:** `sa`  
**HTTP Method:** GET  
**Description:** Helper function to set the Modbus address of an RS485 sensor.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Sensor number |
| `id` | integer | Yes | New Modbus ID to assign |

#### Response
```json
{
  "result": 1
}
```

---

## Sensor Data Retrieval

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### List All Sensors
**Endpoint:** `/sl`  
**Command:** `sl`  
**HTTP Method:** GET  
**Description:** Retrieve a list of all configured sensors with their full configuration.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `test` | integer | No | If provided, returns only test status |

#### Response
```json
{
  "count": 5,
  "detected": 2,
  "sensors": [
    {
      "nr": 1,
      "type": 1,
      "group": 0,
      "name": "Soil Moisture 1",
      "ip": 3232235777,
      "port": 502,
      "id": 1,
      "ri": 300,
      "enable": 1,
      "log": 1,
      "show": 1,
      "fac": 0,
      "div": 0,
      "unit": "",
      "unitid": 1,
      "offset": 0,
      "offset2": 0,
      "rs485flags": 0,
      "rs485code": 3,
      "rs485reg": 0
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `count` | Total number of configured sensors |
| `detected` | Number of detected analog sensor boards |
| `sensors` | Array of sensor configurations |

---

### Get Sensor Values
**Endpoint:** `/sg`  
**Command:** `sg`  
**HTTP Method:** GET  
**Description:** Retrieve the last read values from sensors.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | No | Specific sensor number (omit for all sensors) |

#### Response
```json
{
  "datas": [
    {
      "nr": 1,
      "nativedata": 2457,
      "data": 24.57,
      "unit": "%",
      "unitid": 1,
      "last": 1735689600
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `nr` | Sensor number |
| `nativedata` | Raw sensor value before conversion |
| `data` | Converted sensor value |
| `unit` | Unit of measurement |
| `unitid` | Unit ID |
| `last` | Unix timestamp of last reading |

---

### Read Sensor Now
**Endpoint:** `/sr`  
**Command:** `sr`  
**HTTP Method:** GET  
**Description:** Force an immediate sensor reading and return the result with status.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | No | Specific sensor number (omit for all sensors) |

#### Response
```json
{
  "datas": [
    {
      "nr": 1,
      "status": 200,
      "nativedata": 2457,
      "data": 24.57,
      "unit": "%",
      "unitid": 1
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `status` | HTTP-style status code (200 = success) |

---

### Get Supported Sensor Types
**Endpoint:** `/sf`  
**Command:** `sf`  
**HTTP Method:** GET  
**Description:** List all supported sensor types for the current platform.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |

#### Response
```json
{
  "count": 25,
  "detected": 2,
  "sensorTypes": [
    {
      "type": 1,
      "name": "Truebner SMT100 RS485 Modbus, moisture mode",
      "unit": "%",
      "unitid": 1
    },
    {
      "type": 20,
      "name": "ASB - voltage mode 0..5V",
      "unit": "V",
      "unitid": 2
    }
  ]
}
```

---

## Sensor Logging

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### Get Sensor Log
**Endpoint:** `/so`  
**Command:** `so`  
**HTTP Method:** GET  
**Description:** Retrieve historical sensor data from logs.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `log` | integer | No | Log type: 0 = daily (default), 1 = weekly, 2 = monthly |
| `start` | integer | No | Starting index (default: 0) |
| `max` | integer | No | Maximum number of results (default: all) |
| `nr` | integer | No | Filter by sensor number |
| `type` | integer | No | Filter by sensor type |
| `after` | integer | No | Filter entries after Unix timestamp |
| `before` | integer | No | Filter entries before Unix timestamp |
| `lasthours` | integer | No | Filter last N hours |
| `lastdays` | integer | No | Filter last N days |
| `csv` | integer | No | 0 = JSON (default), 1 = CSV, 2 = short CSV |

#### Response (JSON)
```json
{
  "logtype": 0,
  "logsize": 1500,
  "filesize": 12000,
  "log": [
    {
      "nr": 1,
      "type": 1,
      "time": 1735689600,
      "nativedata": 2457,
      "data": 24.57,
      "unit": "%",
      "unitid": 1
    }
  ]
}
```

#### Response (CSV)
```csv
nr;type;time;nativedata;data;unit;unitid
1;1;1735689600;2457;24.57;%;1
```

#### Response (Short CSV)
```csv
nr;time;data
1;1735689600;24.57
```

---

### Clear Sensor Log
**Endpoint:** `/sn`  
**Command:** `sn`  
**HTTP Method:** GET  
**Description:** Delete sensor log entries based on filters.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `log` | integer | No | Log type: 0 = daily, 1 = weekly, 2 = monthly, -1 = all (default: -1) |
| `nr` | integer | No | Delete only entries for this sensor number |
| `under` | float | No | Delete entries with values under this threshold |
| `over` | float | No | Delete entries with values over this threshold |
| `before` | integer | No | Delete entries before this Unix timestamp |
| `after` | integer | No | Delete entries after this Unix timestamp |

#### Response
```json
{
  "deleted": 150
}
```

Or for all logs:
```json
{
  "deleted": 1200,
  "deleted_week": 350,
  "deleted_month": 120
}
```

#### Example - Clear All Logs
```
/sn?pw=<passwordhash>
```

#### Example - Clear Old Data
```
/sn?pw=<passwordhash>&before=1704067200
```

#### Example - Clear Outliers
```
/sn?pw=<passwordhash>&nr=1&under=0&over=100
```

---

## Program Adjustments

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### Configure Program Adjustment
**Endpoint:** `/sb`  
**Command:** `sb`  
**HTTP Method:** GET  
**Description:** Configure automatic watering program adjustments based on sensor values.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Adjustment number |
| `type` | integer | Yes | Adjustment type (0 = delete, see `/sh` for types) |
| `sensor` | integer | Yes* | Sensor number to use for adjustment |
| `prog` | integer | Yes* | Program number to adjust |
| `factor1` | float | Yes* | First scaling factor |
| `factor2` | float | Yes* | Second scaling factor |
| `min` | float | Yes* | Minimum sensor value for adjustment |
| `max` | float | Yes* | Maximum sensor value for adjustment |
| `name` | string | No | Adjustment name (URL-encoded) |

*Required unless `type=0` (delete)

#### Response
```json
{
  "result": 1
}
```

#### Example
```
/sb?pw=<passwordhash>&nr=1&type=1&sensor=1&prog=1&factor1=0.5&factor2=1.5&min=20&max=60&name=Moisture%20Adjust
```

---

### List Program Adjustments
**Endpoint:** `/se`  
**Command:** `se`  
**HTTP Method:** GET  
**Description:** List all configured program adjustments.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | No | Filter by adjustment number |
| `prog` | integer | No | Filter by program number |
| `sensor` | integer | No | Filter by sensor number |

#### Response
```json
{
  "count": 2,
  "progAdjust": [
    {
      "nr": 1,
      "type": 1,
      "sensor": 1,
      "prog": 1,
      "factor1": 0.5,
      "factor2": 1.5,
      "min": 20.0,
      "max": 60.0,
      "name": "Moisture Adjust",
      "current": 0.85
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `current` | Current calculated adjustment factor |

---

### Calculate Adjustment
**Endpoint:** `/sd`  
**Command:** `sd`  
**HTTP Method:** GET  
**Description:** Calculate adjustment values or preview adjustment curves.

#### Request Parameters (By Number)
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes* | Adjustment number to calculate |

#### Request Parameters (By Program)
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `prog` | integer | Yes* | Program number to calculate |

#### Request Parameters (Visual Calculation)
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `type` | integer | Yes | Adjustment type |
| `sensor` | integer | Yes | Sensor number |
| `factor1` | float | Yes | First scaling factor |
| `factor2` | float | Yes | Second scaling factor |
| `min` | float | Yes | Minimum sensor value |
| `max` | float | Yes | Maximum sensor value |

#### Response (Simple)
```json
{
  "adjustment": 0.85
}
```

#### Response (Visual)
```json
{
  "adjustment": {
    "min": 0,
    "max": 100,
    "current": 24.57,
    "adjust": 0.85,
    "unit": "%",
    "inval": [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100],
    "outval": [0.5, 0.6, 0.7, 0.85, 1.0, 1.2, 1.35, 1.45, 1.5, 1.5, 1.5]
  }
}
```

---

### Get Adjustment Types
**Endpoint:** `/sh`  
**Command:** `sh`  
**HTTP Method:** GET  
**Description:** List supported program adjustment types.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |

#### Response
```json
{
  "count": 5,
  "progTypes": [
    {
      "type": 0,
      "name": "No Adjustment"
    },
    {
      "type": 1,
      "name": "Linear scaling"
    },
    {
      "type": 2,
      "name": "Digital under min"
    },
    {
      "type": 3,
      "name": "Digital over max"
    },
    {
      "type": 4,
      "name": "Digital under min or over max"
    }
  ]
}
```

---

## Monitoring

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### Configure Monitor
**Endpoint:** `/mc`  
**Command:** `mc`  
**HTTP Method:** GET  
**Description:** Configure monitoring rules that trigger programs based on sensor conditions.

#### Request Parameters (Common)
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | Yes | Monitor number |
| `type` | integer | Yes | Monitor type (0 = delete, see `/mt` for types) |
| `sensor` | integer | No | Sensor number to monitor |
| `prog` | integer | Yes* | Program to trigger |
| `zone` | integer | Yes* | Zone/station to control |
| `name` | string | No | Monitor name (URL-encoded) |
| `maxrun` | integer | Yes* | Maximum runtime in seconds |
| `prio` | integer | No | Priority (default: 0) |
| `rs` | integer | No | Reset seconds - cooldown after trigger |

#### Additional Parameters by Type

**MIN/MAX Types:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `value1` | float | Minimum threshold |
| `value2` | float | Maximum threshold |

**SENSOR12 Type:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `sensor12` | integer | Secondary sensor number |
| `invers` | integer | 1 = invert logic, 0 = normal |

**SET_SENSOR12 Type:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `monitor` | integer | Monitor number to read state from |
| `sensor12` | integer | Sensor to set |

**AND/OR/XOR Types:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `monitor1` | integer | First monitor number |
| `monitor2` | integer | Second monitor number |
| `monitor3` | integer | Third monitor number |
| `monitor4` | integer | Fourth monitor number |
| `invers1` | integer | Invert first monitor |
| `invers2` | integer | Invert second monitor |
| `invers3` | integer | Invert third monitor |
| `invers4` | integer | Invert fourth monitor |

**NOT Type:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `monitor` | integer | Monitor number to invert |

**TIME Type:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `from` | integer | Start time (HHMM format, e.g., 0600) |
| `to` | integer | End time (HHMM format, e.g., 1800) |
| `wdays` | integer | Weekday bitmask (0-127, Monday=bit 0) |

**REMOTE Type:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `rmonitor` | integer | Remote monitor number |
| `ip` | integer | Remote IP as 32-bit integer |
| `port` | integer | Remote port |

#### Response
```json
{
  "result": 1
}
```

#### Example - Temperature MIN Monitor
```
/mc?pw=<passwordhash>&nr=1&type=1&sensor=2&prog=1&zone=0&maxrun=3600&value1=15&value2=0&name=Low%20Temp
```

#### Example - Time Window Monitor
```
/mc?pw=<passwordhash>&nr=2&type=8&prog=2&zone=0&maxrun=1800&from=0600&to=1800&wdays=127
```

---

### List Monitors
**Endpoint:** `/ml`  
**Command:** `ml`  
**HTTP Method:** GET  
**Description:** List configured monitors.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `nr` | integer | No | Filter by monitor number |
| `prog` | integer | No | Filter by program number |
| `sensor` | integer | No | Filter by sensor number |

#### Response
```json
{
  "monitors": [
    {
      "nr": 1,
      "type": 1,
      "sensor": 2,
      "prog": 1,
      "zone": 0,
      "name": "Low Temp",
      "maxrun": 3600,
      "prio": 0,
      "active": 0,
      "time": 0,
      "rs": 0,
      "ts": 0,
      "value1": 15.0,
      "value2": 0.0
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `active` | Current activation state (1 = active, 0 = inactive) |
| `time` | Activation timestamp |
| `rs` | Reset seconds (cooldown period) |
| `ts` | Time remaining until reset (seconds) |

---

### Get Monitor Types
**Endpoint:** `/mt`  
**Command:** `mt`  
**HTTP Method:** GET  
**Description:** List supported monitor types.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |

#### Response
```json
{
  "monitortypes": [
    {
      "name": "Min",
      "type": 1
    },
    {
      "name": "Max",
      "type": 2
    },
    {
      "name": "SN 1/2",
      "type": 3
    },
    {
      "name": "SET SN 1/2",
      "type": 4
    },
    {
      "name": "AND",
      "type": 10
    },
    {
      "name": "OR",
      "type": 11
    },
    {
      "name": "XOR",
      "type": 12
    },
    {
      "name": "NOT",
      "type": 13
    },
    {
      "name": "TIME",
      "type": 14
    },
    {
      "name": "REMOTE",
      "type": 100
    }
  ]
}
```

---

## Utility Endpoints

> **Platform:** All endpoints in this section are available on **ESP32, ESP32-C5, ESP8266, and OSPi/Linux**.

### Backup Sensor Configuration
**Endpoint:** `/sx`  
**Command:** `sx`  
**HTTP Method:** GET  
**Description:** Export sensor configuration, adjustments, and monitors for backup.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |
| `backup` | integer | No | Backup type bitmask: 1=sensors, 2=adjustments, 4=monitors (default: 7=all) |

#### Response
```json
{
  "backup": 7,
  "time": 1735689600,
  "os-version": 220,
  "minor": 1,
  "sensors": [...],
  "progadjust": [...],
  "monitors": [...]
}
```

#### Example - Backup Only Sensors
```
/sx?pw=<passwordhash>&backup=1
```

---

### System Resource Usage
**Endpoint:** `/du`  
**Command:** `du`  
**HTTP Method:** GET  
**Description:** Get system resource usage including memory and log file statistics.

#### Request Parameters
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pw` | string | Yes | Password |

#### Response (ESP32)
```json
{
  "status": 1,
  "freeMemory": 125000,
  "totalBytes": 1474560,
  "usedBytes": 245000,
  "freeBytes": 1229560,
  "pingok": 1,
  "mqtt": 1,
  "ifttt": 1,
  "logfiles": {
    "l01": 1500,
    "l02": 0,
    "l11": 450,
    "l12": 0,
    "l21": 120,
    "l22": 0
  }
}
```

| Field | Description |
|-------|-------------|
| `freeMemory` | Available RAM in bytes |
| `totalBytes` | Total filesystem size |
| `usedBytes` | Used filesystem space |
| `freeBytes` | Free filesystem space |
| `pingok` | Network ping status (1 = OK) |
| `mqtt` | MQTT connection status |
| `ifttt` | IFTTT notification status |
| `logfiles` | Log file entry counts (l01=daily A, l02=daily B, l11=weekly A, l12=weekly B, l21=monthly A, l22=monthly B) |

---

## Data Types and Constants

### Sensor Types
Common sensor type constants (use `/sf` endpoint to get complete list for your platform):

**RS485 Sensors (1-9):**
- `1` - Truebner SMT100 RS485 Modbus (moisture mode)
- `2` - Truebner SMT100 RS485 Modbus (temperature mode)
- `3` - Truebner SMT100 RS485 Modbus (permittivity mode)
- `4` - Truebner TH100 RS485 (humidity mode)
- `5` - Truebner TH100 RS485 (temperature mode)
- `9` - RS485 generic sensor

**Analog Sensor Board (ASB) Sensors (10-49):** *(ESP32, ESP32-C5, ESP8266 only)*
- `10` - ASB - voltage mode 0..4V
- `11` - ASB - percent 0..3.3V to 0..100%
- `15` - ASB - SMT50 moisture sensor
- `16` - ASB - SMT50 temperature sensor
- `17` - ASB - SMT100 analog moisture sensor
- `18` - ASB - SMT100 analog temperature sensor
- `30` - ASB - Vegetronix VH400 moisture sensor
- `31` - ASB - Vegetronix THERM200 temperature sensor
- `32` - ASB - Vegetronix Aquaplumb sensor
- `49` - ASB - user defined sensor

**OSPI Sensors (50-59):** *(OSPi/Linux only)*
- `50` - OSPI analog input - voltage mode 0..3.3V
- `51` - OSPI analog input - percent 0..3.3V to 0..100%
- `52` - OSPI analog input - SMT50 moisture
- `53` - OSPI analog input - SMT50 temperature
- `54` - OSPI internal temperature sensor

**Independent Sensors:**
- `60` - FYTA moisture sensor
- `61` - FYTA temperature sensor
- `90` - MQTT subscription
- `95` - Zigbee sensor (via Zigbee2MQTT)
- `96` - BLE (Bluetooth Low Energy) sensor

**Network & Virtual Sensors:**
- `100` - Remote OpenSprinkler sensor
- `101` - Weather service - temperature (Fahrenheit)
- `102` - Weather service - temperature (Celsius)
- `103` - Weather service - humidity (%)
- `105` - Weather service - precipitation (inch)
- `106` - Weather service - precipitation (mm)
- `107` - Weather service - wind (mph)
- `108` - Weather service - wind (km/h)
- `109` - Weather service - ETO
- `110` - Weather service - radiation

**Sensor Groups (1000-1003):**
- `1000` - Sensor group with min value
- `1001` - Sensor group with max value
- `1002` - Sensor group with avg value
- `1003` - Sensor group with sum value

**Diagnostic Sensors:**
- `10000` - Free memory
- `10001` - Free storage

### Unit IDs
- `0` - None
- `1` - Percent (%)
- `2` - Degree Celsius (°C)
- `3` - Fahrenheit (°F)
- `4` - Volt (V)
- `5` - Humidity Percent (%rH)
- `6` - Inch (in)
- `7` - Millimeter (mm)
- `8` - Miles per hour (mph)
- `9` - Kilometers per hour (km/h)
- `10` - Level
- `11` - Permittivity (DK/ε)
- `12` - Lumen (lm)
- `13` - Lux (lx)
- `99` - User-defined unit

### Adjustment Types
- `0` - No adjustment
- `1` - Linear scaling
- `2` - Digital under min
- `3` - Digital over max
- `4` - Digital under min or over max

### Monitor Types
- `1` - MIN (trigger below minimum)
- `2` - MAX (trigger above maximum)
- `3` - SENSOR12 (read digital OS sensors rain/soil moisture)
- `4` - SET_SENSOR12 (write digital OS sensors rain/soil moisture)
- `10` - AND (logical AND of monitors)
- `11` - OR (logical OR of monitors)
- `12` - XOR (logical XOR of monitors)
- `13` - NOT (logical NOT of monitor)
- `14` - TIME (time window condition)
- `100` - REMOTE (remote monitor query)

---

## Error Codes

Standard OpenSprinkler error responses:
- `1` - Success
- `2` - Unauthorized (invalid password)
- `16` - Data missing (required parameter not provided)
- `17` - Out of range
- `18` - Data format error
- `32` - Page not found

---

## Notes

### Sensor Number Assignment
- Sensor numbers (`nr`) should be unique positive integers
- It's recommended to use sequential numbering starting from 1
- Deleting a sensor does not automatically renumber other sensors

### IP Address Encoding
IP addresses are encoded as 32-bit unsigned integers in network byte order:
```
192.168.1.100 = (192 << 24) | (168 << 16) | (1 << 8) | 100 = 3232235876
```

### RS485 Flags Configuration
The `rs485flags` parameter is a bitmask that configures RS485 communication settings:

**Bit Structure:**
- **Bits 0-1:** Parity (0=none, 1=even, 2=odd)
- **Bit 2:** Stop bits (0=1 stop bit, 1=2 stop bits)
- **Bits 3-5:** Speed (0=9600, 1=19200, 2=38400, 3=57600, 4=115200)
- **Bit 6:** Byte order (0=big endian, 1=little endian/swapped)
- **Bits 7-9:** Data type (0=uint16, 1=int16, 2=uint32, 3=int32, 4=float, 5=double)

**Example Configurations:**
```
// 9600 baud, no parity, 1 stop bit, big endian, uint16
rs485flags = 0

// 9600 baud, even parity, 1 stop bit, big endian, uint16
rs485flags = 1

// 19200 baud, no parity, 1 stop bit, big endian, uint16
rs485flags = 8  // (1 << 3)

// 9600 baud, no parity, 1 stop bit, little endian, uint16
rs485flags = 64  // (1 << 6)

// 9600 baud, no parity, 1 stop bit, big endian, float
rs485flags = 512  // (4 << 7)
```

### URL Encoding
String parameters (names, units, topics, URLs) must be URL-encoded:
- Spaces: `%20`
- Forward slash: `%2F`
- Special characters: Use standard URL encoding

### Time Windows (Monitor Type 9)
The `wdays` parameter is a bitmask where:
- Monday = bit 0 (value 1)
- Tuesday = bit 1 (value 2)
- Wednesday = bit 2 (value 4)
- Thursday = bit 3 (value 8)
- Friday = bit 4 (value 16)
- Saturday = bit 5 (value 32)
- Sunday = bit 6 (value 64)

Example: Monday-Friday = 1+2+4+8+16 = 31

### MQTT Configuration
For MQTT sensors (type 90):
1. Configure the sensor with basic parameters using `/sc`
2. Set MQTT broker URL using `/sk` with `type=0`
3. Set MQTT topic using `/sk` with `type=1`
4. Optionally set JSON filter path using `/sk` with `type=2`

**Example:**
```
# Step 1: Create MQTT sensor
/sc?pw=<passwordhash>&nr=3&type=90&group=0&name=MQTT%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1

# Step 2: Set broker URL
/sk?pw=<passwordhash>&nr=3&type=0&value=mqtt%3A%2F%2F192.168.1.50%3A1883

# Step 3: Set topic
/sk?pw=<passwordhash>&nr=3&type=1&value=home%2Fgarden%2Ftemperature

# Step 4: Set JSON filter (if the MQTT message is JSON)
/sk?pw=<passwordhash>&nr=3&type=2&value=temperature
```

### MQTT Configuration
For MQTT sensors (type 90):
1. Configure the sensor with basic parameters using `/sc`
2. Set MQTT broker URL using `/sk` with `type=0`
3. Set MQTT topic using `/sk` with `type=1`
4. Optionally set JSON filter path using `/sk` with `type=2`

**Example:**
```
# Step 1: Create MQTT sensor
/sc?pw=<passwordhash>&nr=3&type=90&group=0&name=MQTT%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1

# Step 2: Set broker URL
/sk?pw=<passwordhash>&nr=3&type=0&value=mqtt%3A%2F%2F192.168.1.50%3A1883

# Step 3: Set topic
/sk?pw=<passwordhash>&nr=3&type=1&value=home%2Fgarden%2Ftemperature

# Step 4: Set JSON filter (if the MQTT message is JSON)
/sk?pw=<passwordhash>&nr=3&type=2&value=temperature
```

**MQTT Message Formats:**
- **Plain value:** `24.5` (directly parsed as sensor value)
- **JSON with filter:** `{"temperature":24.5,"humidity":65}` (use filter: `temperature`)
- **JSON nested:** `{"sensor":{"temp":24.5}}` (use filter: `sensor.temp`)

### Remote Sensors
Remote sensors (type 100) allow reading sensor values from another OpenSprinkler device on your network:
- Query sensors from remote OpenSprinkler devices via HTTP
- Useful for distributed sensor networks
- Supports all sensor types available on the remote device

**Configuration:**
```
# Remote sensor configuration
# ip = IP address of remote OpenSprinkler (as 32-bit integer)
# port = HTTP port of remote device (default: 80)
# id = Sensor number on remote device
/sc?pw=<passwordhash>&nr=7&type=100&group=0&name=Remote%20Moisture&ip=3232235876&port=80&id=1&ri=300&enable=1&log=1
```

**Example:**
```
# Read sensor #5 from OpenSprinkler at 192.168.1.200
# IP encoding: (192<<24)|(168<<16)|(1<<8)|200 = 3232235976
/sc?pw=<passwordhash>&nr=8&type=100&group=0&name=Remote%20Sensor&ip=3232235976&port=80&id=5&ri=300
```

**Notes:**
- Remote device must be accessible via network
- Both devices should use the same password for security
- Network latency affects read intervals
- Use for centralized monitoring of distributed sensors

### FYTA Sensors

**Platform:** ESP32, ESP32-C5, ESP8266, OSPi/Linux

FYTA sensors (types 60-61) connect to FYTA plant monitoring devices via cloud API:
- **Type 60:** FYTA moisture sensor
- **Type 61:** FYTA temperature sensor
- Requires FYTA account and plant configuration in FYTA app
- Cloud-based data retrieval

**Prerequisites:**
1. FYTA plant sensor device
2. FYTA account (email/password)
3. Plant configured in FYTA mobile app

**Configuration:**
First, configure FYTA credentials in OpenSprinkler system options:
```
# Set FYTA email and password in system settings
# (via web interface or options API)
```

Then create FYTA sensors:
```
# FYTA moisture sensor
# id = FYTA plant ID from FYTA app
/sc?pw=<passwordhash>&nr=9&type=60&group=0&name=FYTA%20Moisture&ip=0&port=0&id=12345&ri=600&enable=1&log=1

# FYTA temperature sensor for same plant
/sc?pw=<passwordhash>&nr=10&type=61&group=0&name=FYTA%20Temp&ip=0&port=0&id=12345&ri=600&enable=1&log=1
```

**Notes:**
- Requires internet connection
- Read intervals should be >= 300 seconds (API rate limits)
- Plant ID can be found in FYTA app or via FYTA API
- ESP8266 uses HTTP, ESP32/OSPI use HTTPS for FYTA API

### Weather Sensors
Weather sensors (types 101-110) integrate weather service data:
- Temperature (Fahrenheit/Celsius)
- Humidity
- Precipitation (inch/mm)
- Wind speed (mph/km/h)
- ETO (Evapotranspiration)
- Solar radiation

**Configuration:**
Weather sensors use the configured weather service in OpenSprinkler system options.

```
# Weather temperature sensor (Celsius)
/sc?pw=<passwordhash>&nr=11&type=102&group=0&name=Weather%20Temp&ip=0&port=0&id=0&ri=3600&enable=1&log=1

# Weather humidity sensor
/sc?pw=<passwordhash>&nr=12&type=103&group=0&name=Weather%20Humidity&ip=0&port=0&id=0&ri=3600&enable=1&log=1

# Weather precipitation (mm)
/sc?pw=<passwordhash>&nr=13&type=106&group=0&name=Weather%20Rain&ip=0&port=0&id=0&ri=3600&enable=1&log=1
```

**Notes:**
- Requires weather service configuration in OpenSprinkler
- Read intervals should be >= 3600 seconds (1 hour)
- Weather data updates based on service provider limits
- Use for watering adjustments based on weather conditions

### Internal/Diagnostic Sensors
Internal sensors (types 10000-10001) provide system health monitoring:
- **Type 10000:** Free memory (RAM) in bytes
- **Type 10001:** Free storage (filesystem) in bytes

**Configuration:**
```
# Free memory sensor
/sc?pw=<passwordhash>&nr=20&type=10000&group=0&name=Free%20RAM&ip=0&port=0&id=0&ri=60&enable=1&log=1

# Free storage sensor
/sc?pw=<passwordhash>&nr=21&type=10001&group=0&name=Free%20Storage&ip=0&port=0&id=0&ri=60&enable=1&log=1
```

**Use Cases:**
- Monitor system health
- Debug memory leaks
- Track storage usage
- Trigger alerts on low resources

### Zigbee Sensors

**Platform:** Native ZigBee — ESP32-C5 only · Zigbee2MQTT mode — all platforms

Zigbee sensors (type 95) can be used in two ways on ESP32-C5:

#### Native ZigBee (ESP32-C5 only)
The ESP32-C5 has a built-in IEEE 802.15.4 radio for native ZigBee support. The radio mode is selected at runtime via the **IEEE 802.15.4 API** (see [IEEE802154_API.md](IEEE802154_API.md)).

Query the current mode and all available modes with `/ir`:
```json
{
  "activeMode": 2,
  "activeModeName": "zigbee_gateway",
  "modes": [
    {"id": 0, "name": "disabled"},
    {"id": 1, "name": "matter"},
    {"id": 2, "name": "zigbee_gateway"},
    {"id": 3, "name": "zigbee_client"}
  ],
  "enabled": 1, "matter": 0, "zigbee": 1, "zigbee_gw": 1, "zigbee_client": 0
}
```

Set the mode with `/iw?mode=N` (device reboots to apply):
- **ZigBee Gateway** (`/iw?mode=2`): ESP32-C5 acts as ZigBee Coordinator, directly receives sensor data
- **ZigBee Client** (`/iw?mode=3`): ESP32-C5 joins an existing ZigBee network (e.g., Zigbee2MQTT)

**Native ZigBee Endpoints:**
| Endpoint | Description | Modes |
|----------|-------------|-------|
| `/ir` | Get current radio mode and list all available modes | All |
| `/iw` | Set radio mode (requires reboot) | All |
| `/zj` | Join/search ZigBee network | Client (`activeMode=3`) |
| `/zs` | Get ZigBee connection status | Gateway & Client |
| `/zg` | Manage ZigBee devices (list, permit, remove) | Gateway (`activeMode=2`) |
| `/zd` | List discovered ZigBee devices | Gateway & Client |
| `/zo` | Open network for pairing (legacy, use `/zg?action=permit`) | Gateway |
| `/zc` | Clear new device flags | Gateway & Client |

#### Via Zigbee2MQTT
Zigbee sensors can also connect via Zigbee2MQTT using MQTT as transport:
- Requires a Zigbee coordinator (e.g., CC2652, ConBee II) connected to your network
- Uses MQTT as transport protocol
- Supports various Zigbee sensors (temperature, humidity, moisture, etc.)

**Configuration (MQTT mode):**
1. Configure the Zigbee sensor using `/sc` with type 95
2. Set MQTT broker URL using `/sk` with `type=0`
3. Set Zigbee2MQTT topic using `/sk` with `type=1` (typically `zigbee2mqtt/[device_name]`)
4. Set JSON filter path using `/sk` with `type=2` to extract the desired value

**Example:**
```
# Step 1: Create Zigbee temperature sensor
/sc?pw=<passwordhash>&nr=5&type=95&group=0&name=Zigbee%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1

# Step 2: Set MQTT broker
/sk?pw=<passwordhash>&nr=5&type=0&value=mqtt%3A%2F%2F192.168.1.50%3A1883

# Step 3: Set Zigbee2MQTT topic
/sk?pw=<passwordhash>&nr=5&type=1&value=zigbee2mqtt%2Fgarden_sensor

# Step 4: Set JSON filter (e.g., for temperature field)
/sk?pw=<passwordhash>&nr=5&type=2&value=temperature
```

**Supported Zigbee Devices:**
- Temperature/Humidity sensors (Xiaomi Aqara, Sonoff)
- Soil moisture sensors
- Contact sensors
- Motion sensors with temperature readings
- Custom Zigbee devices reporting numeric values

### BLE Sensors

**Platform:** ESP32 (native BLE) · OSPi/Linux (BlueZ D-Bus)

BLE (Bluetooth Low Energy) sensors (type 96) provide wireless sensor connectivity:
- **ESP32:** Native BLE support for scanning and reading BLE advertisements and GATT characteristics
- **OSPI:** Requires BlueZ stack on Linux (Raspberry Pi)
- Automatically discovers and connects to nearby BLE sensors
- Low power consumption for battery-operated sensors
- **Single type (96)** with flexible payload decoding (similar to Zigbee sensors)

**Supported BLE Sensors:**
- Xiaomi Mi Flora (plant sensors) - moisture, temperature, light, fertility
- Xiaomi Mi Temperature & Humidity sensors (LYWSD03MMC, MHO-C401)
- RuuviTag environmental sensors
- Generic BLE GATT sensors broadcasting standard characteristics:
  - Temperature (0x2A1C) 
  - Humidity (0x2A6F)
  - Pressure (0x2A6D)
  - Battery Level (0x2A19)

**Configuration for ESP32:**
```
# Step 1: Create BLE sensor
# The 'name' field contains the BLE MAC address
# The 'unitid' defines the measurement unit
/sc?pw=<passwordhash>&nr=6&type=96&group=0&name=AA:BB:CC:DD:EE:FF&ip=0&port=0&id=0&ri=300&enable=1&log=1&unitid=2

# Step 2: Set GATT characteristic UUID
# The 'unit' field in /si contains the UUID (optionally with format specifier)
# Format IDs: 10=temperature, 11=humidity, 12=pressure (see sensor_payload_decoder.h)
/si?pw=<passwordhash>&nr=6&unit=00002a1c-0000-1000-8000-00805f9b34fb|10
```

**Configuration for OSPI (Raspberry Pi):**
```
# Step 1: Create BLE sensor using BlueZ D-Bus API
# MAC address in 'name' field, unitid for measurement unit
/sc?pw=<passwordhash>&nr=6&type=96&group=0&name=AA:BB:CC:DD:EE:FF&ip=0&port=0&id=0&ri=300&enable=1&log=1&unitid=2

# Step 2: Set GATT service UUID (optional, auto-discover if not set)
/sk?pw=<passwordhash>&nr=6&type=0&value=0000180a-0000-1000-8000-00805f9b34fb

# Step 3: Set GATT characteristic UUID to read
/sk?pw=<passwordhash>&nr=6&type=1&value=00002a1c-0000-1000-8000-00805f9b34fb

# Step 4: Set data parsing mode (raw, int16, uint16, float, temperature, humidity, pressure)
/sk?pw=<passwordhash>&nr=6&type=2&value=temperature
```

**Common GATT Characteristics:**
- **Temperature:** `00002a1c-0000-1000-8000-00805f9b34fb` (Unit: °C, unitid=2)
- **Humidity:** `00002a6f-0000-1000-8000-00805f9b34fb` (Unit: %, unitid=1)
- **Pressure:** `00002a6d-0000-1000-8000-00805f9b34fb` (Unit: Pa, unitid depends on sensor)
- **Battery Level:** `00002a19-0000-1000-8000-00805f9b34fb` (Unit: %, unitid=1)

**Payload Decoding:**
The sensor automatically decodes BLE GATT characteristic values using:
- Auto-detection for standard formats (default)
- Manual format specification via format ID (|10, |11, etc.)
- Custom parsing modes (raw, int16, uint16, float)

**BLE Scanning API (ESP32 only):**
The firmware can scan for nearby BLE devices. Use the web interface or query endpoints to discover BLE MAC addresses for configuration.

**Notes:**
- BLE sensors require close proximity (typically < 10 meters)
- ESP32 can handle multiple BLE sensors simultaneously
- Read intervals should be >= 60 seconds to conserve battery
- Some BLE sensors require pairing or encryption keys (currently not supported)
- **RF Coexistence:** BLE cannot be used when ESP32 is in WiFi AP mode (RF conflict)
- Use Ethernet mode or WiFi Station mode for BLE functionality
- **Power Saving:** ESP32 BLE is turned on only during sensor reads, then off to free RF resources

### Sensor Groups
Sensor groups (types 1000-1003) allow you to combine multiple sensors:
- **Type 1000 (MIN):** Returns the minimum value from all sensors in the group
- **Type 1001 (MAX):** Returns the maximum value from all sensors in the group
- **Type 1002 (AVG):** Returns the average value from all sensors in the group
- **Type 1003 (SUM):** Returns the sum of all sensor values in the group

**Creating a Sensor Group:**
The `group` parameter in `/sc` specifies which sensor group a sensor belongs to. Sensors with the same `group` value (> 0) are automatically grouped together.

**Example:**
```
# Create three moisture sensors in group 1
/sc?pw=<passwordhash>&nr=1&type=1&group=1&name=Bed%201&ip=3232235876&port=502&id=1&ri=300
/sc?pw=<passwordhash>&nr=2&type=1&group=1&name=Bed%202&ip=3232235876&port=502&id=2&ri=300
/sc?pw=<passwordhash>&nr=3&type=1&group=1&name=Bed%203&ip=3232235876&port=502&id=3&ri=300

# Create average sensor for group 1
/sc?pw=<passwordhash>&nr=10&type=1002&group=1&name=Average%20Moisture&ip=0&port=0&id=0&ri=60
```

### Data Conversion Formula
Sensor values are converted using:
```
converted_value = ((raw_value + offset_mv) * factor / divider) + offset2
```

Where:
- `raw_value` = reading from sensor hardware
- `offset_mv` = voltage offset in millivolts (analog sensors)
- `factor` = multiplication factor
- `divider` = division factor
- `offset2` = final value offset in target units

---

## Examples

### Complete Sensor Setup Example

#### 1. Add an SMT100 Moisture Sensor
```
/sc?pw=<passwordhash>&nr=1&type=1&group=0&name=Garden%20Bed%201&ip=3232235876&port=502&id=1&ri=300&enable=1&log=1
```

#### 2. Add a User-Defined Analog Sensor
```
/sc?pw=<passwordhash>&nr=2&type=29&group=0&name=Custom%20Sensor&ip=0&port=0&id=0&ri=60&enable=1&log=1
/si?pw=<passwordhash>&nr=2&fac=100&div=33&unit=cbar&offset=0&offset2=-10
```

#### 3. Configure MQTT Sensor
```
/sc?pw=<passwordhash>&nr=3&type=90&group=0&name=MQTT%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1
/sk?pw=<passwordhash>&nr=3&type=0&value=mqtt%3A%2F%2F192.168.1.50%3A1883
/sk?pw=<passwordhash>&nr=3&type=1&value=home%2Fgarden%2Ftemperature
/sk?pw=<passwordhash>&nr=3&type=2&value=temp
```

#### 4. Create Linear Program Adjustment
```
/sb?pw=<passwordhash>&nr=1&type=1&sensor=1&prog=1&factor1=0.5&factor2=1.5&min=30&max=70
```

#### 5. Create Temperature-Based Monitor
```
/mc?pw=<passwordhash>&nr=1&type=1&sensor=3&prog=2&zone=0&maxrun=1800&value1=10&value2=0&name=Freeze%20Protection&rs=3600
```

#### 6. Query Current Values
```
/sg?pw=<passwordhash>
```

#### 7. Get Last 24 Hours of Data
```
/so?pw=<passwordhash>&lasthours=24&csv=0
```

#### 8. Backup All Configuration
```
/sx?pw=<passwordhash>&backup=7
```

---

## Version Information
This API documentation corresponds to OpenSprinkler firmware version 2.2.0 and later with analog sensor support.

**Last updated:** February 2026 — Added unified IEEE 802.15.4 / ZigBee / Matter documentation for ESP32-C5.
