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

## Table of Contents
- [Sensor Configuration](#sensor-configuration)
- [Sensor Data Retrieval](#sensor-data-retrieval)
- [Sensor Logging](#sensor-logging)
- [Program Adjustments](#program-adjustments)
- [Monitoring](#monitoring)
- [Utility Endpoints](#utility-endpoints)

---

## Sensor Configuration

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
      "type": 5
    },
    {
      "name": "OR",
      "type": 6
    },
    {
      "name": "XOR",
      "type": 7
    },
    {
      "name": "NOT",
      "type": 8
    },
    {
      "name": "TIME",
      "type": 9
    },
    {
      "name": "REMOTE",
      "type": 10
    }
  ]
}
```

---

## Utility Endpoints

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
- `1` - Truebner SMT100 RS485 Modbus (moisture mode)
- `2` - Truebner SMT100 RS485 Modbus (temperature mode)
- `20` - Analog Sensor Board (ASB) - voltage mode 0..5V
- `21` - Analog Sensor Board (ASB) - 0..3.3V to 0..100%
- `29` - Analog Sensor Board (ASB) - user defined sensor
- `40` - MQTT subscription
- `41` - Remote OpenSprinkler sensor
- `50` - Sensor group with min value
- `51` - Sensor group with max value
- `52` - Sensor group with avg value

### Unit IDs
- `1` - Percent (%)
- `2` - Volt (V)
- `3` - Millivolt (mV)
- `4` - Celsius (°C)
- `5` - Fahrenheit (°F)
- `6` - Permittivity (ε)
- `7` - Kilopascal (kPa)
- `8` - Centibar (cbar)

### Adjustment Types
- `0` - No adjustment
- `1` - Linear scaling
- `2` - Digital under min
- `3` - Digital over max
- `4` - Digital under min or over max

### Monitor Types
- `1` - MIN (trigger below minimum)
- `2` - MAX (trigger above maximum)
- `3` - SENSOR12 (compare two sensors)
- `4` - SET_SENSOR12 (set sensor based on monitor state)
- `5` - AND (logical AND of monitors)
- `6` - OR (logical OR of monitors)
- `7` - XOR (logical XOR of monitors)
- `8` - NOT (logical NOT of monitor)
- `9` - TIME (time window condition)
- `10` - REMOTE (remote monitor query)

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
For MQTT sensors (type 40):
1. Configure the sensor with basic parameters using `/sc`
2. Set MQTT broker URL using `/sk` with `type=0`
3. Set MQTT topic using `/sk` with `type=1`
4. Optionally set JSON filter path using `/sk` with `type=2`

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
/sc?pw=<passwordhash>&nr=3&type=40&group=0&name=MQTT%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1
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
