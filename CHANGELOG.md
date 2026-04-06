# Changelog

All notable changes to the OpenSprinkler Firmware are documented in this file.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).  
Versions: `<FW_VERSION>.<FW_MINOR>` — e.g. `2.4.0 (187)` means `OS_FW_VERSION=240`, `OS_FW_MINOR=187`.

---

## [2.4.0 (194)] — 2026-04-06

### Added
- **Inverse group scheduling**: new inverted logic mode for station groups. When enabled, stations in the same group can run in parallel, while different groups run one after another; group `P` always remains sequential
- **MCP control by name**: the MCP server can now start saved programs and watering zones directly by name, making voice assistants and AI workflows easier to use

### Changed
- **MCP documentation expanded**: the built-in MCP endpoint and the external MCP server now describe program encoding, sensor configuration, program adjustments, monitor behavior, and inverse logic more clearly
- **Automatic certificate handling (ESP32)**: self-generated HTTPS certificates are now checked after time sync and regenerated automatically when expired

### Fixed
- **More stable MCP requests on ESP8266**: improved memory handling reduces fragmentation and avoids failed MCP calls on low-memory devices
- **Network reliability**: ESP32 and OSPi now use a unified ping implementation, improving network checks and reducing platform-specific issues
- **Weather updates after transient network errors**: weather requests are no longer blocked by stale generic network-failure counters
- **HTTPS fallback on low memory**: if there is not enough memory for SSL, requests now fall back to HTTP instead of failing outright
- **OSPi build compatibility**: Linux build scripts now detect Debian-like and non-Debian systems more robustly

## [2.4.0 (193)] — 2026-04-03

### Added
- **Sensor interface warnings (`/sl` API)**: new `emit_sensor_warnings()` emits a `"warnings"` array in the sensor list response; warns when configured sensor interfaces are unavailable (I2C board missing, RS485 adapter missing, MQTT disabled/disconnected, Zigbee wrong mode, BLE not available)
- **`sensor_request_save()` API**: public function to schedule a deferred sensor config save (~5 s) instead of saving immediately — avoids heap pressure from rapid-fire `/sc` requests

### Changed
- **MQTT lazy allocation**: MQTT client and WiFiClient are no longer allocated in `_init()` but lazily in `_connect()` on first use; `suspend()` on ESP8266 now frees both objects (~7 KB), `resume()` recreates them on next connect
- **MQTT buffer size (ESP32)**: increased from 4 KB to 8 KB for larger payloads
- **Network check improvements**: ping now uses single-packet (`Ping(…, 1)`); pinger is recreated after failure to clear stuck internal state; added debug logging for gateway/WiFi checks
- **ESP8266 HTTP timeout**: `client->setTimeout(2000)` now also applied on ESP8266 (was ESP32-only)
- **Web client servicing**: additional `otf->loop()` calls inserted between blocking operations in the main loop (after sensor reads, after sensor/radio maintenance) to improve HTTP responsiveness
- **Reboot notification timing**: deferred until network is connected and boot elapsed ≥ 10 s (was immediate)

### Fixed
- **ESP8266 stack smashing on `/sc`**: replaced 320-byte stack-local `decoded_value[TMP_BUFFER_SIZE]` with global `tmp_buffer` in `server_sensor_config()` — the previous allocation plus `JsonDocument` on the stack exceeded ESP8266's 4 KB continuation stack
- **ESP8266 OOM crash on sensor save**: `sensor_save()` refactored to stream-serialize one sensor at a time instead of building a full in-memory `JsonDocument` of all sensors — peak heap drops from O(all sensors) to O(one sensor)
- **ESP8266 `/sc` deferred save**: `server_sensor_config()` now calls `sensor_define(config, false)` + `sensor_request_save()` instead of `sensor_define(config, true)` — prevents rapid-fire requests from each triggering a full file write that OOMs
- **MQTT suspend null-check**: `suspend()` now checks `mqtt_client` before calling `_connected()` to avoid null pointer dereference

---

## [2.4.0 (187)] — 2026-03-20

### Added
- **ESP RainMaker integration**: new `opensprinkler_rainmaker.cpp/h` exposes irrigation zones as RainMaker Switch devices and sensor data (rain, flow, temperature, soil moisture) as RainMaker devices for Alexa & Google Home (ESP32 only, `ENABLE_RAINMAKER`) (e8a5397)
- **MCP Server — session management**: MCP Streamable HTTP transport now generates a persistent `Mcp-Session-Id` header (chip-ID + uptime) returned on every MCP response (e8a5397)
- **MCP Server — RainMaker tool**: added `get_rainmaker_status` tool exposing ESP RainMaker node/cloud state via MCP (e8a5397)
- **InfluxDB suspend/resume**: added `suspend()` / `resume()` methods to InfluxDB class for controlled pausing during resource-intensive operations (36d9504)
- **lgpio support (OSPi)**: added support for `lgpio` library on Debian 13+ (Trixie) with automatic fallback from `libgpiod` → `lgpio` → `sysfs` (52cd0a6)
- **Zigbee notifications**: trigger Zigbee notification after sensor updates (36d9504)
- **fw.sh dist copy**: all builds now automatically copy firmware binaries to `.pio/dist/` for easy access
- **fw.sh all-platform builds**: `build`, `deploy`, and `upload` without variant parameter now include ESP8266 alongside Matter and ZigBee

### Changed
- **MCP Server — auth headers**: fixed header name lookup to use lowercase (OTF lowercases header names during parsing); affects `x-os-password` and `authorization` headers (e8a5397)
- **BLE sensor debug counters**: refactored to use cleaner structure (36d9504)
- **Modbus RTU**: streamlined IP handling for Modbus RTU sensors (36d9504)
- **Remote sensor**: improved IP extraction logic (36d9504)
- **Weather requests**: SSL handling now adapts based on available memory (36d9504)
- **Sensor JSON output**: removed duplicate `factor`/`divider` fields from JSON serialization; only `fac`/`div` are emitted (backward-compatible on read)

### Fixed
- **ASB sensor scheduling**: fixed critical bug where SENSOR_USERDEF (type 49) sensors stopped being read after the first averaging period — scheduler's `last_read` overwrite conflicted with the averaging period check; introduced independent `period_start` field (e8a5397)
- **ASB sensor first read**: fixed regression where sensors produced no data until the first full `read_interval` elapsed after boot; now returns first sample immediately when no prior data exists (e8a5397)
- **ASB sensor error handling**: improved error handling in ASB sensor read path (36d9504)
- **InfluxDB data handling**: optimized InfluxDB data submission flow (36d9504)
- OSPi: fixed PCF8591 sensor compile error (3cfa17a, 2026-03-19)
- OSPi: fixed startup failure introduced in prior refactoring (b53ae07, 2026-03-16)
- OSPi: fixed compile error after sensor refactoring (e3a98cf, 2026-03-16)

---

## [2.4.0 (186)] — 2026-03-15

### Added
- **OTA Online Update**: implemented online firmware update functionality — pull firmware directly from the upgrade server without USB (1470e9e)
- **OTA Logging**: enhanced OTA update process with detailed progress logging; progress jumps to 100 % only after confirmed device completion (5a7f9e2)
- **MCP Server** (`tools/mcp-server`): new Node.js MCP server that exposes the full OpenSprinkler REST API as MCP tools for AI assistants (4013b9c)
- **Matter (CHIP) protocol**: added initial Matter/Thread protocol support for ESP32-C5; `esp32-c5-matter` PlatformIO environment (d2f11de)
- **IEEE 802.15.4 / Zigbee**: full ZigBee support for ESP32-C5 — join network, device discovery, cluster database, vendor API lookups; `esp32-c5-zigbee` environment (04721d0, 26bab15)
- **Zigbee client mode**: local data exposure via HTTP endpoint; enriched device info structure (5822c4b, fac89e6)
- **BLE coexistence manager**: radio coexistence layer (WiFi / BLE / Zigbee) with PTI yield during BLE scans; predictive boost for MQTT (74d1791, 14c6fb8)
- **BLE Basic Cluster support**: extended BLE and Zigbee sensor capabilities with Basic Cluster attribute reads (e4ecb06)
- **BLE discovery diagnostics**: added volatile debug counters (`ble_dbg_disc_*`) and `ble_dbg_get_json()` API for lock/miss/add/update statistics (5a7f9e2)
- **Unified Pinger**: cross-platform ICMP ping implementation for ESP32 and Linux/OSPi (2268540)
- **PSRAM utilities**: added PSRAM memory analysis helpers and `analyze_psram.py` tooling (49c0a67, 5b577c3)
- **TLS 1.3 optimizations** for ESP32-C5 to reduce heap usage (5133fad)
- **Modbus RTU** sensor type and support for new RS485 boards (cbd3f9a)
- **API Documentation**: REST API docs added under `docs/as_api_docs/` (de2fee1, 009359a)
- **ESP32-C5 build environment**: new `espc5-12` PlatformIO target with Matter and Zigbee variants

### Changed
- **Sensor storage format**: refactored storage to JSON-based format for sensor and program adjustment data (295c4c6)
- **Sensor unit ID mapping**: updated sensor unit IDs; boot variant configuration improved (3b3e44f)
- **Notification handling**: refactored notification dispatch and sensor event handling (1a7399c)
- **Sensor detection & JSON handling**: enhanced type detection and JSON payload parsing (67e479f)
- **Zigbee + Matter firmware handling**: updated firmware variant selection logic; added ESP8266 pin definitions (7ce4c29)
- **MQTT**: removed non-persistent MQTT fields; fixed MQTT option handling; deferred messaging with predictive boost (87e4dcf, cbd3f9a, 0a2dfd7)
- **Weather options**: improved persistence of weather configuration; fixed config saving (2d443a1, 70553ae)
- **Memory management**: refactored heap/PSRAM allocation for ESP32-C5; lazy-loading sensor interface (148f018, 6a051f1)
- **OTF library**: fixed long-message CRLF insertion bug (b4c16e5)
- **BLE device info**: enriched with UUID and human-readable name fields (87e4dcf)
- **WiFi state machine**: fixed logic error in `OS_STATE_CONNECTED` transition (0083ef8)
- **Code structure**: multiple readability and maintainability refactoring passes (e67642f, ee4cef9, 540d786, cf60284)

### Fixed
- Pinger: fixed `IPAddress` private-member access incompatibility with Arduino native class (538f778, 9bcb985)
- InfluxDB: fixed compile error (e1bddbc)
- `tmp_buffer` declaration conflict fixed (8541d2e)
- RS485 I2C address fix (7ccda85)
- Debug output verbosity fixed (fc78f5b)
- Memory reduction for ESP32-C5 constrained flash (efdb03)
- ESP8266 compile errors fixed (3641fe7)
- BLE hard-timeout added to prevent scanner from stalling indefinitely (7c90b1e)

### Removed
- SSL optimization flags removed — caused connectivity issues (331f16b)

---

## [2.3.3 (175)] — 2025-04-06 `tag: v233_175`

### Added
- New monitor type: **Time**
- InfluxDB always-active option

### Fixed
- OSPi: fixed compile error
- ADS1115: fixed "too many open files" leak
- MQTT subscribe fix
- Docker: added `libgpiod2`; fixed linking error
- AVR: fixed large-packet handling in option writes
- FlowPulseRateFactor calculation (credit: Ray)
- Floating-point `printf` for AVR (`snprintf_P` does not support `%f`)
- Flow alert message formatting

---

## [2.3.3 (168)] — 2025-02-18 `tag: v233_168`  

### Added
- Master-off notification events
- `lcd_dimming` support for OSPi
- `/boot/firmware/config.txt` handling

### Fixed
- Firmware update rejection when "ignore password" is set
- Flow pulse rate calculation (use `strtod` instead of `sscanf` on AVR)
- Notification message spacing
- Linux compilation error

---

## [2.3.2 (167)] — `tag: v232_167`

See git log `v231_166..v232_167` for details.

---

## Older releases

For releases prior to v2.3.2, refer to the upstream repository history at  
https://github.com/OpenSprinkler/OpenSprinkler-Firmware
