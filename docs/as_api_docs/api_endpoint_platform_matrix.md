# OpenSprinkler API Endpoint — Platform Availability Matrix

> Auto-generated research document.  
> Source: `opensprinkler_server.cpp` — `_url_keys[]` PROGMEM array (line 4732) and `urls[]` handler table (line 4810).

## How it works

Every API endpoint is a **2-character URL key** (e.g. `jc` → `GET /jc?pw=…`).  
The `_url_keys[]` array and the `urls[]` function-pointer array are kept in the same order; entries wrapped in `#if defined(…)` are compiled in only on the matching platform/feature set.

---

## Platform legend

| Token | Meaning |
|-------|---------|
| **All** | No platform guard — compiled on ESP8266, all ESP32 variants, and OSPi/Linux |
| **ESP32** | `#if defined(ESP32)` — all ESP32 family chips (includes ESP32-C5) |
| **ESP32-C5** | `#if defined(ESP32C5)` — ESP32-C5 only |
| **ESP8266 ∪ ESP32 ∪ OSPi** | `#if defined(ESP8266) \|\| defined(ESP32) \|\| defined(OSPI)` |
| **+ZIGBEE** | additionally requires `OS_ENABLE_ZIGBEE` build flag |
| **+BLE** | additionally requires `OS_ENABLE_BLE` build flag |
| **+MATTER** | additionally requires `ENABLE_MATTER` build flag |

---

## Complete endpoint table

### Core endpoints (no platform guard — all platforms)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 1 | `cv` | `server_change_values` | All | Change controller values (manual start/stop, rain delay, etc.) |
| 2 | `jc` | `server_json_controller` | All | Get controller variables (JSON) |
| 3 | `dp` | `server_delete_program` | All | Delete a program |
| 4 | `cp` | `server_change_program` | All | Create / change a program |
| 5 | `cr` | `server_change_runonce` | All | Run-once program |
| 6 | `mp` | `server_manual_program` | All | Manual program (start a single station) |
| 7 | `up` | `server_moveup_program` | All | Move a program up in the list |
| 8 | `jp` | `server_json_programs` | All | Get all programs (JSON) |
| 9 | `co` | `server_change_options` | All | Change controller options |
| 10 | `jo` | `server_json_options` | All | Get controller options (JSON) |
| 11 | `sp` | `server_change_password` | All | Change device password |
| 12 | `js` | `server_json_status` | All | Get station status (JSON) |
| 13 | `cm` | `server_change_manual` | All | Manual station on/off |
| 14 | `cs` | `server_change_stations` | All | Change station attributes |
| 15 | `jn` | `server_json_stations` | All | Get station names & attributes (JSON) |
| 16 | `je` | `server_json_station_special` | All | Get special station data (JSON) |
| 17 | `jl` | `server_json_log` | All | Get watering log (JSON) |
| 18 | `dl` | `server_delete_log` | All | Delete watering log |
| 19 | `su` | `server_view_scripturl` | All | View JavaScript URL |
| 20 | `cu` | `server_change_scripturl` | All | Change JavaScript URL |
| 21 | `ja` | `server_json_all` | All | Get ALL data in one call (JSON, combines jc+jo+jp+jn+js) |
| 22 | `pq` | `server_pause_queue` | All | Pause / resume the run queue |

### Sensor endpoints (no platform guard — all platforms)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 23 | `sc` | `server_sensor_config` | All | Configure a sensor (add/edit/delete) |
| 24 | `sl` | `server_sensor_list` | All | List all configured sensors (JSON) |
| 25 | `sg` | `server_sensor_get` | All | Get a single sensor's current data (JSON) |
| 26 | `sr` | `server_sensor_readnow` | All | Trigger an immediate sensor read |
| 27 | `sa` | `server_set_sensor_address` | All | Set sensor I²C / RS-485 address |
| 28 | `so` | `server_sensorlog_list` | All | List sensor log entries (JSON) |
| 29 | `sn` | `server_sensorlog_clear` | All | Clear sensor log |
| 30 | `sb` | `server_sensorprog_config` | All | Configure a sensor-program binding |
| 31 | `sd` | `server_sensorprog_calc` | All | Calculate / preview sensor-program adjustment |
| 32 | `se` | `server_sensorprog_list` | All | List sensor-program bindings (JSON) |
| 33 | `sf` | `server_sensor_types` | All | List supported sensor types (JSON) |
| 34 | `du` | `server_usage` | All | Get per-station water usage data |
| 35 | `sh` | `server_sensorprog_types` | All | List sensor-program adjustment types |
| 36 | `sx` | `server_sensorconfig_backup` | All | Export / import sensor configuration backup |

### Debug endpoint (no platform guard)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 37 | `db` | `server_json_debug` | All | Get debug / diagnostic info (free heap, etc.) |

### InfluxDB endpoints (no platform guard — all platforms)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 38 | `is` | `server_influx_set` | All | Set InfluxDB configuration |
| 39 | `ig` | `server_influx_get` | All | Get InfluxDB configuration (JSON) |

### Monitor endpoints (no platform guard — all platforms)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 40 | `mc` | `server_monitor_config` | All | Configure a monitor (add/edit/delete) |
| 41 | `ml` | `server_monitor_list` | All | List configured monitors (JSON) |
| 42 | `mt` | `server_monitor_types` | All | List supported monitor types (JSON) |

### IEEE 802.15.4 endpoints (ESP32-C5 only)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 43 | `ir` | `server_ieee802154_get` | `ESP32C5` | Get IEEE 802.15.4 radio configuration & active mode |
| 44 | `iw` | `server_ieee802154_set` | `ESP32C5` | Set IEEE 802.15.4 radio mode (triggers reboot) |

### ZigBee endpoints (ESP32-C5 + ZigBee enabled)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 45 | `zj` | `server_zigbee_join_network` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | ZigBee Client: join / search for a network |
| 46 | `zs` | `server_zigbee_status` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | Get ZigBee connection status |
| 47 | `zg` | `server_zigbee_gw_manage` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | ZigBee Gateway: manage devices (list/permit/remove) |
| 48 | `zd` | `server_zigbee_discovered_devices` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | Get list of discovered ZigBee devices |
| 49 | `zo` | `server_zigbee_open_network` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | Open ZigBee network for joining |
| 50 | `zc` | `server_zigbee_clear_flags` | `ESP32C5` + `OS_ENABLE_ZIGBEE` | Clear "new device" flags |

### BLE endpoints (ESP32 family + BLE enabled)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 51 | `bd` | `server_ble_discovered_devices` | `ESP32` + `OS_ENABLE_BLE` | Get list of discovered BLE devices |
| 52 | `bs` | `server_ble_start_scan` | `ESP32` + `OS_ENABLE_BLE` | Start a BLE scan |
| 53 | `bc` | `server_ble_clear_flags` | `ESP32` + `OS_ENABLE_BLE` | Clear "new device" flags for BLE |

### Matter endpoint (ESP32 family + Matter enabled)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 54 | `jm` | `server_json_matter` | `ESP32` + `ENABLE_MATTER` | Get Matter pairing information (QR code, manual code, commissioning status) |

### FYTA plant-sensor endpoints (ESP8266 / ESP32 / OSPi — not bare-metal AVR)

| # | URL key | Handler function | Platform guard | Description |
|---|---------|-----------------|----------------|-------------|
| 55 | `fy` | `server_fyta_query_plants` | `ESP8266 \|\| ESP32 \|\| OSPI` | Query FYTA plant sensor data |
| 56 | `fc` | `server_fyta_get_credentials` | `ESP8266 \|\| ESP32 \|\| OSPI` | Get / set FYTA API credentials |

---

## Non-URL-key routes (registered separately, not in `_url_keys[]`)

These are registered directly in `start_server_client()` (ESP8266 / ESP32 only):

| Route | Handler | Platform guard | Description |
|-------|---------|----------------|-------------|
| `/` | `server_home` | `ESP8266 \|\| ESP32` | Serve home page / index |
| `/index.html` | `server_home` | `ESP8266 \|\| ESP32` | Alias for home page |
| `/update` (GET) | `on_firmware_update` | `ESP8266 \|\| ESP32` | Serve firmware-update HTML page |
| `/update` (POST) | `on_firmware_upload_fin` + `on_firmware_upload` | `ESP8266 \|\| ESP32` | Handle firmware upload & flash |

AP-mode routes (`start_server_ap()`):

| Route | Handler | Platform guard | Description |
|-------|---------|----------------|-------------|
| `/` | `on_ap_home` | `ESP8266 \|\| ESP32` | AP setup home |
| `/jsap` | `on_ap_scan` | `ESP8266 \|\| ESP32` | Scan for WiFi networks |
| `/ccap` | `on_ap_change_config` | `ESP8266 \|\| ESP32` | Apply WiFi configuration |

---

## Endpoints asked about but NOT found in the codebase

| Requested key | Status |
|---------------|--------|
| `we` | **Not found** — no weather endpoint with this key |
| `wc` | **Not found** |
| `ws` | **Not found** |
| `mr` | **Not found** — no Modbus endpoint registered |

> Weather data is fetched *outbound* by the firmware (see `GetWeather.cpp`), not exposed as an inbound API endpoint.

---

## Summary: platform → available endpoint count

| Platform / build | Core (22) | Sensor (14) | Debug (1) | InfluxDB (2) | Monitor (3) | IEEE (2) | Zigbee (6) | BLE (3) | Matter (1) | FYTA (2) | **Total** |
|------------------|-----------|-------------|-----------|-------------|-------------|----------|-----------|---------|-----------|----------|-----------|
| **ESP8266** | ✅ 22 | ✅ 14 | ✅ 1 | ✅ 2 | ✅ 3 | ❌ | ❌ | ❌ | ❌ | ✅ 2 | **44** |
| **ESP32 (non-C5)** | ✅ 22 | ✅ 14 | ✅ 1 | ✅ 2 | ✅ 3 | ❌ | ❌ | ⚙️ 3* | ⚙️ 1* | ✅ 2 | **44–48** |
| **ESP32-C5** | ✅ 22 | ✅ 14 | ✅ 1 | ✅ 2 | ✅ 3 | ✅ 2 | ⚙️ 6* | ⚙️ 3* | ⚙️ 1* | ✅ 2 | **44–56** |
| **OSPi / Linux** | ✅ 22 | ✅ 14 | ✅ 1 | ✅ 2 | ✅ 3 | ❌ | ❌ | ❌ | ❌ | ✅ 2 | **44** |

⚙️ = available only when the corresponding build flag is enabled (`OS_ENABLE_BLE`, `ENABLE_MATTER`, `OS_ENABLE_ZIGBEE`).

---

## Source references

- URL keys array: [`opensprinkler_server.cpp` L4732–L4806](../opensprinkler_server.cpp#L4732-L4806)
- Handler pointer table: [`opensprinkler_server.cpp` L4810–L4872](../opensprinkler_server.cpp#L4810-L4872)
- IEEE 802.15.4 handlers: [`opensprinkler_server.cpp` L4233–L4333](../opensprinkler_server.cpp#L4233-L4333) — guard: `#if defined(ESP32C5)`
- ZigBee handlers: [`opensprinkler_server.cpp` L4336–L4619](../opensprinkler_server.cpp#L4336-L4619) — guard: `#if defined(ESP32C5) && defined(OS_ENABLE_ZIGBEE)`
- BLE handlers: [`opensprinkler_server.cpp` L4622–L4722](../opensprinkler_server.cpp#L4622-L4722) — guard: `#if defined(ESP32) && defined(OS_ENABLE_BLE)`
- Matter handler: [`opensprinkler_server.cpp` L4452–L4494](../opensprinkler_server.cpp#L4452-L4494) — guard: `#if defined(ESP32) && defined(ENABLE_MATTER)`
- FYTA handlers: [`opensprinkler_server.cpp` L3054–L3145](../opensprinkler_server.cpp#L3054-L3145) — guard: `#if defined(ESP8266) || defined(ESP32) || defined(OSPI)`
