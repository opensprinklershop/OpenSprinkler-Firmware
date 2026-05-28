# Changelog

All notable changes to the OpenSprinkler Firmware are documented in this file.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).  
Versions: `<FW_VERSION>.<FW_MINOR>` — e.g. `2.4.0 (187)` means `OS_FW_VERSION=240`, `OS_FW_MINOR=187`.

---

## [2.4.0(203)] — 2026-05-26

### Fixed
- **Zigbee Gateway: Newly-joined devices stayed labelled as "Unknown"** because the Basic Cluster (manufacturer 0x0004 / model 0x0005) read responses were dispatched to a handler that only cleared the pending flag but never stored the strings. `zbAttributeRead` now routes Basic Cluster string attributes through `gw_handleBasicClusterResponse`, so devices such as the GIEX GX02 single-zone valve are correctly identified after joining and surface their real manufacturer/model in `/zd` and the sensor / station editors.
- **GX02 valve switching in zone control**: added runtime fallback in Zigbee zone switching so `_TZE200_sh1btabb|TS0601` (and already-detected Tuya devices) are controlled via Tuya DP writes even if the stored zone mode is still Standard ZCL. This fixes cases where pairing/identification worked but turning the zone on/off did not actuate the valve.

---

## [2.4.0(202)] — 2026-05-25

### Added
- **Gardena API Integration (ESP32 only)**: added support for Gardena Smart Gateway cloud connectivity on ESP32, allowing remote valve coordination via Gardena's Cloud API. Gardena soil moisture and temperature sensors are now fully accessible in the UI.
- **Native Zigbee & Tuya Zone Control**: implemented direct and native control of Zigbee and Tuya zones, allowing the firmware to directly manage wireless valves, relays, smart plugs, and switches over the Zigbee/Matter networks.
- **RS485, Zigbee & Tuya Water Meters**: added support for RS485 (Modbus), Zigbee, and Tuya-based water meters, enabling precise water consumption logging and flow sensors integration.
- **Special Station Zone Icons**: introduced dedicated dashboard vector-style badges for specialized station zones (RS485 / Modbus, Zigbee, Gardena). These icons remain permanently visible in both active and idle states to guarantee persistent diagnostics.
- **Gardena Credentials Assistant**: added an comprehensive, localized "Setup Gardena Credentials" help wizard in the Analog Sensor options pane, complete with interactive guides and an automated OAuth 2.0 login hook.

### Changed
- **Gated Special Zone Selection**: station editor choices for Gardena and Zigbee zones are now filtered dynamically, hiding options when the target controller lacks the required hardware capabilities or runs older firmware.
- **Improved Matter & Zigbee Detection**: optimized version and option-based runtime checks to reliably detect when Matter or Zigbee features are active on the board.

### Fixed
- **Zone Style Desynchronization**: fixed a bug where changing a zone's station type would not update the dashboard icon immediately or would revert after a device reboot. The UI now bypasses stale settings cache and calls `/je` directly.
- **RS485 Hex Payload Crash Preventer**: added defensive padding to string boundaries during RS485 Modbus configuration parsing, resolving a runtime crash when parsing short hex buffers.
- **Platform-specific Overheads**: optimized Gardena overheads; ESP8266 and OSPi builds contain zero Gardena libraries or memory footprints.

## [2.4.0(201)] — 2026-05-20

### Fixed
- **Zigbee client battery contamination**: in `zigbee_attribute_callback`, battery reports (`POWER_CONFIG/0x0021`) in client mode no longer overwrite the sensor's `last_data`. An early-exit guard now updates only `last_battery` and `last_lqi` and skips the rest of the update path, so the battery percentage can never be logged or displayed as the soil-moisture / temperature / humidity value.
- **Tuya DP14 soil state priority**: added `dp_soil_exact_received` flag so that DP3 (exact soil moisture %) always takes priority over DP14 (categorical dry/moist/wet state) within the same Tuya report frame.
- **Zigbee Soil battery contamination (gateway)**: in `gw_updateSensorFromReport`, battery reports (`POWER_CONFIG/0x0021` or Tuya DP 15) no longer overwrite the sensor's `last_data` / `last_native_data` / `flags.data_ok` / `last_read`. A short-circuit at the top of the function now updates only `last_battery` and `last_lqi` and returns early, so the battery percentage (50/76/100/…) can never be logged or displayed as the soil-moisture / temperature / humidity value.

## [2.4.0(200)] — 2026-05-19

### Added
- **Stale sensor handling**: added timeout and fallback policies so stale sensor readings can be handled predictably instead of silently influencing adjustments indefinitely
- **Flow pulse sensor support**: added pulse-based flow sensing and water-consumption logging, including unit handling and Zigbee gateway integration hooks
- **Matter maintenance controls**: added firmware-side support to remove Matter commissioning data/fabrics and to write Matter KVS partition data for provisioning workflows
- **Monitor logs via MCP**: added `get_monitor_log` support so live serial monitor logs can be retrieved through the MCP tooling after deploy/monitor runs

### Changed
- **ESP32-C5 Matter support**: improved Matter integration for Ethernet and sensor capabilities, including linker/build compatibility updates for the C5 release environments
- **Firmware release workflow**: expanded `fw.sh` release/full-flash handling, Matter KVS sync, online deploy behavior, and GitHub release automation
- **Async HTTP handling**: improved asynchronous request handling and related firmware-management paths to reduce blocking behavior during network operations
- **ESP32-C5 boot menu**: refactored boot-menu option setup and improved legacy reset-path handling

### Fixed
- **PCF8591 fresh-data reads (OSPi)**: adjusted the PCF8591 read pipeline so fresh conversion data is handled correctly with a smaller buffer path
- **Truebner RS485 timeout recovery**: removed an unnecessary `data_ok` reset on transient timeouts so the last valid sensor state is preserved correctly
- **Zigbee Tuya data handling**: cached Tuya DP reports in the gateway path to improve handling of manufacturer-specific reports
- **ESP32-C5 OTA/update flow**: improved OTA update handling and firmware upload behavior for C5 builds

## [2.4.0(199)] — 2026-05-09

### Fixed
- **Group sensors restored**: fixed regression where `SENSOR_GROUP_MIN/MAX/AVG/SUM` stopped producing values after `sensor_update_groups()` had been turned into a no-op; aggregation + `data_ok` + sensor logging are active again
- **ESP32-C5 startup boot menu**: added firmware-type selection as first boot option (ZigBee/Matter), including OTF boot-slot selection, persisted IEEE 802.15.4 mode, and reboot into selected variant
- **Matter sensor update spam before commissioning**: sensor attribute updates are now gated by commissioning state to avoid repeated failed updates while device is still in pairing mode
- **W5500 MAC address display**: the MAC shown on the OLED display (button B2) and reported via API now uses `esp_read_mac(ESP_MAC_ETH)` — identical to the MAC assigned by the W5500 Ethernet driver — instead of the previously wrong eFuse base MAC with an inverted last byte (ESP32)
- **NTP sync no longer freezes the display**: removed the blocking `delay(1000) × 3` polling loop in `getNtpTime()` (ESP32); ESP32 SNTP is asynchronous and syncs in the background — the function now checks `time()` once and returns 0 if not yet synced, letting the scheduler retry without stalling the main loop
- **UI: Irrigation Database 404**: the in-app irrigation database lookup was calling `/irrigationdb/api.php` as a device-relative URL (not implemented in firmware); corrected to `https://opensprinklershop.de/irrigationdb/api.php`

---

## [2.4.0(198)] — 2026-05-03

### Added
- **UI: Sensor name heading in Analog Sensor Chart**: when opening the chart from a specific sensor (e.g. via a dashboard click), the sensor's name is now shown as a heading above the charts

### Fixed
- **PCF8591 A/D conversion (OSPi)**: the read function now issues a correct conversion-trigger write before reading, fixing stale/incorrect ADC values on the PCF8591 chip
- **ESP8266 online update HTTP fallback**: the firmware now falls back from HTTPS to plain HTTP when the TLS handshake fails during OTA download, improving update reliability on ESP8266 devices with limited TLS support
- **Firmware version sync on boot**: `OS_FW_VERSION` and `OS_FW_MINOR` constants are now written to NV options on every startup, preventing stale version numbers being reported after OTA updates
- **MQTT buffer handling**: improved buffer size management in the MQTT client reduces the risk of truncated messages on constrained devices
- **`fw.sh deploy all` OTA boot order**: after flashing all variants, `otadata` is now erased so the bootloader always selects OTA0 (zigbee) — previously the device could boot into the Matter firmware after a full deploy

---

## [2.4.0(197)] — 2026-04-24

### Fixed
- **Memory leak in e-mail sender**: fixed heap corruption caused by an allocator mismatch (`strdup`/`delete[]`) in `EMailSender::setSMTPServer()` and a missing `delete[]` for temporary recipient arrays in the `send()` overloads — improves long-term stability on ESP8266

---

## [2.4.0(196)] — 2026-04-17

### Added
- **Valve current measurement (ESP32-C5)**: new dynamic baseline tracking and valve-only current calculation — the firmware continuously measures idle system current and subtracts it from the total when a valve is running, displaying only the solenoid current draw
- **API fields `vcurr` and `blcurr`**: the `/jc` endpoint now returns `vcurr` (valve-only current in mA) and `blcurr` (baseline idle current in mA) alongside the existing `curr` (total current)
- **UI: valve current display**: the status bar shows "Valve Current: X mA (Total: Y mA)" when the firmware supports it; falls back to the classic "Current: X mA" for older firmware versions
- **German locale**: added translations for "Valve Current" → "Ventilstrom" and "Total" → "Gesamt"

### Changed
- **ESP32-C5 current sense scale factor**: calibrated to 15.0 (AC) / 21.2 (DC) for the ESP32-C5 board's current sense circuit (was using the ESP32 OS 3.0 scale of 2.85)
- **Multi-sample ADC averaging (ESP32-C5)**: `read_current()` now averages 20 ADC samples over one full 50 Hz AC cycle (20 ms) for phase-independent readings — eliminates the large noise swings from single random-phase samples
- **Two-stage EMA smoothing (ESP32-C5)**: first-stage EMA in `read_current()` uses α=0.05 (was α=0.2); a second-stage display EMA (α=0.2) in `get_valve_current()` provides stable UI output with ±7% variance (was ±22%)

### Fixed
- **Baseline current no longer hardcoded**: replaced the fixed `baseline_current = 80` with a dynamic EMA-based measurement that adapts to the actual idle current of each individual board

## [2.4.0(194)] — 2026-04-06

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

## [2.4.0(193)] — 2026-04-03

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

## [2.4.0(187)] — 2026-03-20

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

## [2.4.0(186)] — 2026-03-15

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
