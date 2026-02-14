# IEEE 802.15.4 Radio Configuration API

## Overview

The ESP32-C5 has a built-in IEEE 802.15.4 radio that can be used for either **Matter** or **ZigBee** protocols. Since both protocols share the same radio hardware, only one can be active at a time. The IEEE 802.15.4 configuration API allows runtime selection of the radio operating mode.

### Operating Modes

| Mode | Value | Description |
|------|-------|-------------|
| **Disabled** | `0` | IEEE 802.15.4 radio off (default) |
| **Matter** | `1` | Matter protocol (HomeKit, Google Home, Alexa) |
| **ZigBee Gateway** | `2` | ZigBee Coordinator mode — manage devices, receive sensor data |
| **ZigBee Client** | `3` | ZigBee End Device mode — join existing network (e.g., Zigbee2MQTT) |

### Important Notes
- Changing the mode requires a **device reboot**
- The configuration is stored in `/ieee802154.json` on the device's LittleFS filesystem
- Default mode is `0` (disabled)
- ZigBee Gateway mode is not compatible with WiFi AP mode (RF conflict)
- ZigBee Client mode supports WiFi STA coexistence

---

## Authentication

All endpoints require password authentication via the `pw` parameter:
```
# Using MD5 hash (recommended)
/ir?pw=<md5_password_hash>

# Using cleartext password (development only)
/ir?pw=<cleartext_password>
```

---

## Endpoints

> **Platform Availability:** All endpoints in this document are **ESP32-C5 only**.
> The IEEE 802.15.4 radio hardware is exclusive to the ESP32-C5 SoC.
> On other platforms (ESP8266, ESP32, OSPi/Linux), these endpoints return an error or are not registered.

| Endpoint | Platform | Build Guard |
|----------|----------|-------------|
| `/ir` | ESP32-C5 | `#if defined(ESP32C5)` |
| `/iw` | ESP32-C5 | `#if defined(ESP32C5)` |
| `/zj` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/zs` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/zg` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/zd` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/zo` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/zc` | ESP32-C5 + ZigBee | `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)` |
| `/jm` | Any ESP32 + Matter | `#if defined(ESP32) && defined(ENABLE_MATTER)` |
| `/bd` | Any ESP32 + BLE | `#if defined(ESP32) && defined(OS_ENABLE_BLE)` |
| `/bs` | Any ESP32 + BLE | `#if defined(ESP32) && defined(OS_ENABLE_BLE)` |
| `/bc` | Any ESP32 + BLE | `#if defined(ESP32) && defined(OS_ENABLE_BLE)` |

### `/ir` — Get IEEE 802.15.4 Radio Configuration

Returns the current IEEE 802.15.4 radio mode, all available modes, and status flags.

**Platform:** ESP32-C5 only

**Method:** GET

**Parameters:**
| Parameter | Required | Description |
|-----------|----------|-------------|
| `pw` | Yes | Password (MD5 hash or cleartext) |

**Response:**
```json
{
  "activeMode": 1,
  "activeModeName": "matter",
  "modes": [
    {"id": 0, "name": "disabled"},
    {"id": 1, "name": "matter"},
    {"id": 2, "name": "zigbee_gateway"},
    {"id": 3, "name": "zigbee_client"}
  ],
  "enabled": 1,
  "matter": 1,
  "zigbee": 0,
  "zigbee_gw": 0,
  "zigbee_client": 0
}
```

**Response Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `activeMode` | int | Currently active mode value (0–3) |
| `activeModeName` | string | Human-readable name of the active mode |
| `modes` | array | List of all possible IEEE 802.15.4 modes |
| `modes[].id` | int | Numeric mode identifier |
| `modes[].name` | string | Human-readable mode name |
| `enabled` | int | 1 if any mode is active, 0 if disabled |
| `matter` | int | 1 if Matter mode is active |
| `zigbee` | int | 1 if any ZigBee mode is active |
| `zigbee_gw` | int | 1 if ZigBee Gateway mode is active |
| `zigbee_client` | int | 1 if ZigBee Client mode is active |

**Example:**
```
GET /ir?pw=a6d82bced638de3def1e9bbb4983225c
```

---

### `/iw` — Set IEEE 802.15.4 Radio Mode

Sets the IEEE 802.15.4 radio mode and triggers a device reboot to apply the change.

**Platform:** ESP32-C5 only

**Method:** GET

**Parameters:**
| Parameter | Required | Description |
|-----------|----------|-------------|
| `pw` | Yes | Password (MD5 hash or cleartext) |
| `mode` | Yes | New mode value: 0=disabled, 1=matter, 2=zigbee_gw, 3=zigbee_client |

**Response (success):**
```json
{
  "result": 1,
  "mode": 2,
  "mode_name": "zigbee_gateway",
  "reboot": 1
}
```

**Response (error):**
```json
{
  "result": 0,
  "error": "missing mode parameter"
}
```

**Error Messages:**
| Error | Cause |
|-------|-------|
| `missing mode parameter` | No `mode` parameter provided |
| `invalid mode (0-3)` | Mode value out of range |
| `failed to save config` | Filesystem write error |

**Examples:**
```
# Disable IEEE 802.15.4 radio
GET /iw?pw=<hash>&mode=0

# Enable Matter
GET /iw?pw=<hash>&mode=1

# Enable ZigBee Gateway
GET /iw?pw=<hash>&mode=2

# Enable ZigBee Client
GET /iw?pw=<hash>&mode=3
```

> ⚠️ **The device will reboot ~2 seconds after this call to apply the new mode.**

---

### `/zj` — ZigBee Client: Join/Search Network

Triggers a ZigBee network join/search operation. Only available when the mode is **ZigBee Client** (`mode=3`).

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

**Method:** GET

**Parameters:**
| Parameter | Required | Description |
|-----------|----------|-------------|
| `pw` | Yes | Password |
| `duration` | No | Search duration in seconds (1–600, default: 60) |

**Response (success):**
```json
{
  "result": 1,
  "duration": 60,
  "status": "searching",
  "active": 1,
  "connected": 1
}
```

**Response (error — wrong mode):**
```json
{
  "result": 0,
  "error": "not in zigbee_client mode"
}
```

**Example:**
```
# Start network search for 120 seconds
GET /zj?pw=<hash>&duration=120
```

**Notes:**
- The ZigBee End Device will factory-reset its NVRAM and attempt to rejoin the nearest coordinator
- A ZigBee coordinator (e.g., Zigbee2MQTT, deCONZ) must be active and accepting joins
- Use `/zs` to check connection status after initiating the search

---

### `/zs` — ZigBee: Get Connection Status

Returns the current ZigBee connection status. Available in both ZigBee Gateway and ZigBee Client modes (`activeMode` 2 or 3).

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

**Method:** GET

**Parameters:**
| Parameter | Required | Description |
|-----------|----------|-------------|
| `pw` | Yes | Password |

**Response (success):**
```json
{
  "result": 1,
  "active": 1,
  "connected": 1,
  "mode": "client"
}
```

**Response Fields:**
| Field | Type | Description |
|-------|------|-------------|
| `result` | int | 1 on success, 0 on error |
| `active` | int | 1 if ZigBee stack is running |
| `connected` | int | 1 if connected to a network |
| `mode` | string | `"gateway"` or `"client"` depending on the active IEEE 802.15.4 mode |

**Response (error — wrong mode):**
```json
{
  "result": 0,
  "error": "not in zigbee mode"
}
```

---

### `/zg` — ZigBee Gateway: Manage Devices

Manage ZigBee devices when in **ZigBee Gateway** mode (`activeMode=2`). Supports listing devices, permitting network joins, and removing devices.

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

**Method:** GET

**Parameters:**
| Parameter | Required | Description |
|-----------|----------|-------------|
| `pw` | Yes | Password |
| `action` | No | Action to perform: `list` (default), `permit`, `remove` |
| `duration` | No | For `action=permit`: join window duration in seconds (1–600, default: 60) |
| `ieee` | For remove | For `action=remove`: IEEE address of device to remove (e.g., `0x00124B001F8E5678`) |

#### Action: `list` (default)

Returns all known ZigBee devices.

**Response:**
```json
{
  "result": 1,
  "action": "list",
  "devices": [
    {
      "ieee": "0x00124B001F8E5678",
      "short_addr": 1234,
      "model": "TH100",
      "manufacturer": "Tuya",
      "endpoint": 1,
      "device_id": 770,
      "is_new": 0
    }
  ],
  "count": 1
}
```

**Example:**
```
GET /zg?pw=<hash>
GET /zg?pw=<hash>&action=list
```

#### Action: `permit`

Open the ZigBee network for new device pairing.

**Response:**
```json
{
  "result": 1,
  "action": "permit",
  "duration": 60
}
```

**Example:**
```
# Open network for 120 seconds
GET /zg?pw=<hash>&action=permit&duration=120
```

#### Action: `remove`

Remove a device and unbind all sensors associated with it.

**Response:**
```json
{
  "result": 1,
  "action": "remove",
  "ieee": "0x00124B001F8E5678",
  "unbound_sensors": 2
}
```

**Example:**
```
GET /zg?pw=<hash>&action=remove&ieee=0x00124B001F8E5678
```

**Error Responses:**
```json
{"result": 0, "error": "not in zigbee_gateway mode"}
{"result": 0, "error": "missing ieee parameter"}
{"result": 0, "error": "invalid ieee address"}
```

---

### Existing Endpoints (Updated)

#### `/zd` — Get Discovered ZigBee Devices
Returns a list of all ZigBee devices that have been discovered (both modes).

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

#### `/zo` — Open ZigBee Network
Opens the ZigBee network for device pairing (legacy endpoint, equivalent to `/zg?action=permit`).

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

#### `/zc` — Clear New Device Flags
Clears the "new device" flags after the user has been notified.

**Platform:** ESP32-C5 (requires `OS_ENABLE_ZIGBEE` build flag)

#### `/jm` — Get Matter Pairing Information
Returns Matter commissioning status and pairing codes. The `matter_enabled` field reflects the runtime IEEE 802.15.4 mode.

**Platform:** ESP32-C5 (requires `ENABLE_MATTER` build flag)

**Response (Matter active, not yet commissioned):**
```json
{
  "matter_enabled": 1,
  "commissioned": 0,
  "qr_url": "https://project-chip.github.io/connectedhomeip/qrcode.html?data=...",
  "pairing_code": "12345678"
}
```

**Response (Matter active, already commissioned):**
```json
{
  "matter_enabled": 1,
  "commissioned": 1
}
```

**Response (Matter mode not active — `activeMode` ≠ 1):**
```json
{
  "matter_enabled": 0
}
```

> **Note:** `qr_url` and `pairing_code` are only included when `commissioned` is `0`.

---

## Configuration File Format

The mode is persisted in `/ieee802154.json` on LittleFS:

```json
{
  "mode": 0
}
```

| Value | Mode |
|-------|------|
| `0` | Disabled |
| `1` | Matter |
| `2` | ZigBee Gateway |
| `3` | ZigBee Client |

---

## Typical Workflows

### Enable Matter Integration
```bash
# 1. Set mode to Matter
curl "http://192.168.1.100/iw?pw=<hash>&mode=1"
# Device reboots automatically

# 2. After reboot, check Matter pairing info
curl "http://192.168.1.100/jm?pw=<hash>"
```

### Set Up ZigBee Gateway
```bash
# 1. Enable ZigBee Gateway mode
curl "http://192.168.1.100/iw?pw=<hash>&mode=2"
# Device reboots automatically

# 2. After reboot, open network for pairing
curl "http://192.168.1.100/zg?pw=<hash>&action=permit&duration=120"

# 3. Power on ZigBee sensors...

# 4. Check discovered devices
curl "http://192.168.1.100/zg?pw=<hash>&action=list"

# 5. Create sensor for the discovered device
curl "http://192.168.1.100/sc?pw=<hash>&nr=5&type=95&group=0&name=ZB%20Temp&ip=0&port=0&id=0&ri=60&enable=1&log=1"
```

### Set Up ZigBee Client (Join Existing Network)
```bash
# 1. Enable ZigBee Client mode
curl "http://192.168.1.100/iw?pw=<hash>&mode=3"
# Device reboots automatically

# 2. Start network search (ensure Zigbee2MQTT allows joining)
curl "http://192.168.1.100/zj?pw=<hash>&duration=120"

# 3. Check connection status
curl "http://192.168.1.100/zs?pw=<hash>"
```

### Disable IEEE 802.15.4 Radio
```bash
curl "http://192.168.1.100/iw?pw=<hash>&mode=0"
# Device reboots automatically — radio is off
```

---

## Architecture

### Runtime vs. Compile-Time
Both Matter and ZigBee code are **compiled into the unified firmware** (PlatformIO environment `esp32-c5`). The operating mode is selected at **runtime** via the `/ieee802154.json` configuration file. This means:

- No need for separate firmware binaries
- Mode switching via the web API with a simple reboot
- The radio hardware is only initialized for the selected protocol

### Radio Exclusivity
The ESP32-C5 has a single IEEE 802.15.4 radio transceiver. Matter and ZigBee both use this radio with incompatible protocol stacks:
- **Matter** uses the CHIP/OpenThread stack
- **ZigBee** uses the ZBOSS stack (Coordinator or End Device)

Only one stack can control the radio at a time. Switching requires a full device reboot to reinitialize the radio with the correct protocol stack.

### ZigBee Mode Differences
| Feature | ZigBee Gateway (mode=2) | ZigBee Client (mode=3) |
|---------|------------------------|----------------------|
| Role | Coordinator (ZCZR) | End Device |
| WiFi coexistence | Limited | Full support |
| Network | Creates own network | Joins existing network |
| Device management | Yes (`/zg` endpoint) | No |
| Requires coordinator | No (IS the coordinator) | Yes (e.g., Zigbee2MQTT) |
| Implementation | `sensor_zigbee_gw.cpp` | `sensor_zigbee.cpp` |

---

## Version Information
This API documentation corresponds to OpenSprinkler firmware with unified ESP32-C5 IEEE 802.15.4 support.

**Last updated:** February 2026

**Changes:**
- `/ir` response now includes `activeMode`, `activeModeName`, and `modes` array listing all possible radio modes
- Unified PlatformIO environment `esp32-c5` replaces separate `esp32-c5-matter` / `esp32-c5-zigbee` environments
- Runtime mode selection via `/ieee802154.json` — no separate firmware binaries needed
