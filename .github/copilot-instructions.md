# Copilot instructions — OpenSprinkler Firmware (ESP32 / native)

This file helps AI coding agents get productive quickly in this repository. It focuses on the *actual* structure, developer workflows, frequently used patterns, and concrete examples you can change safely.

## Quick orientation ✅
- This is a single repo that produces two major families of binaries:
  - Arduino/ESP firmwares (PlatformIO environments in `platformio.ini`, e.g. `espc5-12`, `d1_mini`).
  - Native/Linux OpenSprinkler binary (`OSPI`) built with `build.sh` / `build2.sh` (`-DOSPI`).
- Key entry points: `main.cpp`, `OpenSprinkler.cpp`, `opensprinkler_server.cpp` (HTTP), `mqtt.cpp` (MQTT).

## Big-picture architecture 🔧
- Main runtime: `main.cpp` instantiates a global `OpenSprinkler os` object used across modules.
- Persistent config: integer/string options and station files live as binary files (`iopts.dat`, `sopts.dat`, `stns.dat`) defined in `defines.h`.
- Networking & UI: `opensprinkler_server.cpp` (web UI endpoints), `WebSocket`/`OTF` integrations are optional via build flags.
- Sensors & IO: sensor implementations grouped as `sensor_*.cpp` (e.g. `sensor_rs485_i2c.cpp`, `sensor_*`), GPIO in `gpio.cpp`.

## Build & test workflows 🛠️
- Primary development flow: PlatformIO in VS Code (recommended in `README.txt`). Use "PlatformIO: Build" or `pio run -e <env>`.
- PlatformIO configuration: `platformio.ini` contains envs, board types, build flags and `lib_deps`. Inspect the `espc5-12` env for ESP-C5 specifics.
- Native build (Linux): `./build.sh` (installs deps) which calls `build2.sh` to compile `OpenSprinkler` binary. Run on a Linux device for OSPI targets.
- Docker: `Dockerfile` and `README_Docker.md` provide reproducible build environments.
- Debug: enable `-DENABLE_DEBUG` or `-DSERIAL_DEBUG` in `platformio.ini` or build scripts to get verbose debug prints. Use PlatformIO monitor or `serial` monitor.

## Project-specific conventions & important patterns ⚠️
- Conditional compilation drives platform behavior: many files use `#if defined(ESP32)`, `ESP8266`, or `OSPI` to separate Arduino vs native code paths — don't change behavior without checking both paths.
- Option indices and enums: options are numeric indices in `defines.h` (e.g. `IOPT_*`). When adding an option, append a new enum value **and** update any serialization or default logic in `OpenSprinkler.cpp`.
- Firmware version bump: `OS_FW_VERSION` and `OS_FW_MINOR` in `defines.h` are authoritative. Changing on-disk formats or NV structures should bump these values and handle migrations carefully (backups are warned in `build.sh`).
- I/O and sensor return conventions: sensor/read functions typically return `HTTP_RQT_*` codes (e.g. `HTTP_RQT_SUCCESS`, `HTTP_RQT_NOT_RECEIVED`) and use `sensor->flags.data_ok` + `sensor->repeat_read` to manage asynchronous reads. See `sensor_rs485_i2c.cpp` for an example.
- Global `os` usage: the code frequently uses global `os` and `pd` objects; prefer adding methods on those objects versus scattering global state.

## Integration & external dependencies 🔗
- Libraries pulled via PlatformIO (`lib_deps` in `platformio.ini`), notable ones: Ethernet, PubSubClient (MQTT), esp32_https_server, ArduinoWebSockets, OpenThings Framework.
- Cloud/OTF: OpenThings cloud integration toggled by `USE_OTF` and configured via `DEFAULT_OTC_SERVER_*` in `defines.h`.
- Native OS dependencies (for demo or OSPI): mosquitto, libssl, libgpiod, i2c libs — `build.sh` documents apt packages installed.

## Concrete examples (do this same way) 💡
- Add a new sensor type: follow existing `SensorBase` patterns in `sensors.h`, implement `read()` in `sensor_<type>.cpp`, return `HTTP_RQT_*` codes, set `sensor->flags.data_ok` and `sensor->last_data`.
- Add a new I/O option: add `IOPT_*` enum entry in `defines.h`, add default handling in `OpenSprinkler::load_defaults()` or equivalent, and include any JSON/HTTP key handling in `opensprinkler_server.cpp`.
- Add debug logs: use `DEBUG_PRINT` / `DEBUG_PRINTF` macros; these are enabled by `-DENABLE_DEBUG`.

## Safety notes & tests ✅
- Avoid changing binary on-disk structures without bumping `OS_FW_VERSION` and documenting migration steps; `build.sh` warns about `sopts.dat` size changes.
- Hardware testing: many changes must be tested on real hardware (ESP and OSPI). Where possible, add a native-mode unit test or `DEMO` compile as described in `build.sh`.

## Commit messages ✅
- Always write AI-generated commit messages in English.

## documentation & comments 
-  place for documentation is docs/as_api_docs and for reference docs/docs
📝
## Useful files to inspect 📂
- `main.cpp` (entry / loops)  
- `OpenSprinkler.cpp`, `OpenSprinkler.h` (core state & nv data)  
- `defines.h` (macros, I/O option indices, file names)  
- `opensprinkler_server.cpp` (HTTP API and web UI hooks)  
- `platformio.ini` (build targets and flags)  
- `build.sh` / `build2.sh` (native build and runtime steps)  
- `sensor_*.cpp` (sensor implementations) — e.g. `sensor_rs485_i2c.cpp` as a non-trivial example.

## Debug / Test Environment 🔧

### Device Connection
- **Device IP**: `192.168.0.86`
- **Admin Password**: Required for API access (MD5 hash needed)

### Computing the Admin Password Hash

The REST API requires the MD5 hash of the admin password. Calculate it locally:

```bash
# Linux/Mac
echo -n "your_admin_password" | md5sum | awk '{print $1}'

# Windows PowerShell
$Text = "your_admin_password"
$Bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
$Hash = [System.Security.Cryptography.MD5]::Create().ComputeHash($Bytes)
([System.BitConverter]::ToString($Hash) -replace "-","").ToLower()
```

⚠️ **Security Note**: This hash is only computed and stored locally. Do NOT share it or commit it to public repositories.

### Testing via MCP Server (recommended)
The project includes an MCP server at `tools/mcp-server/` that exposes the OpenSprinkler REST API as MCP tools for AI assistants.

```bash
# Build the MCP server
cd tools/mcp-server && npm install && npm run build

# Run with your settings (replace HASH with your computed MD5)
OS_BASE_URL=http://192.168.0.86 OS_PASSWORD_HASH=<YOUR_ADMIN_PASSWORD_HASH> npm start
```

VS Code MCP configuration is in `.vscode/mcp.json` — the server is auto-started by Copilot/Claude.

Available MCP tools: `get_all`, `get_debug`, `get_sensors`, `get_zigbee_devices`, `get_ble_devices`, `get_system_resources`, `get_station_status`, `get_options`, `get_controller_variables`, etc. See `tools/mcp-server/README.md` for the full list.

### Testing via direct REST API
All OpenSprinkler endpoints accept `?pw=<md5hash>` (use the admin password hash computed above):
```bash
# Example (replace HASH with your computed MD5):
ADMIN_HASH="<YOUR_ADMIN_PASSWORD_HASH>"

# Get all data
curl "http://192.168.0.86/ja?pw=${ADMIN_HASH}"

# Get debug info
curl "http://192.168.0.86/db?pw=${ADMIN_HASH}"

# Get sensors
curl "http://192.168.0.86/sl?pw=${ADMIN_HASH}"

# Get Zigbee devices
curl "http://192.168.0.86/zg?pw=${ADMIN_HASH}"

# Get system resources (RAM, flash)
curl "http://192.168.0.86/du?pw=${ADMIN_HASH}"
```

### Radio Coexistence (ESP32-C5)
The ESP32-C5 shares one 2.4 GHz radio for WiFi, BLE, and IEEE 802.15.4 (Zigbee/Matter). The coex manager (`radio_coex.h`/`radio_coex.cpp`) uses a mutex-based design:
- **WiFi has priority by default** — Zigbee/BLE PTI set to LOW
- **Zigbee/BLE can acquire radio lock** via `coex_request_lock()` — max 10s for sensor reads, max 30s for join/pair
- **Ethernet mode**: WiFi stays off, Zigbee/BLE get permanent priority (no lock needed)
- Integration points: `sensors.cpp`, `sensor_zigbee_gw.cpp`, `sensor_zigbee.cpp`, `sensor_ble.cpp`, `opensprinkler_server.cpp`

### BLE Background Scan (Critical WiFi Stability)
**BACKGROUND SCANNING IS DISABLED** in `sensor_ble.cpp` (BG_SCAN_DURATION_NORMAL = 0).

**Reason**: BLE periodic background scans were causing WiFi TCP/IP crashes at the LWIP level:
- DNS failures (`error -54: connection refused`)
- TCP connection timeouts (`errno: 118, "Host is unreachable"`)
- UDP crashes in lwIP: `assert failed: udp_new_ip_type` with "Required to lock TCPIP core functionality!"

**The Problem**: Even with aggressive reductions (3s scans with 20s pauses), the background scan thread was still releasing the coex mutex during active WiFi operations. This interrupted WiFi's TCP/IP stack mid-operation, causing LWIP's internal state machine to fail.

**The Solution**: 
- ✅ **Disable background scans entirely** — WiFi stability is prioritized
- ✅ **On-demand discovery only** — `sensor_ble_start_discovery_scan()` for explicit discovery
- ✅ **Passive reception still works** — BLE devices still broadcast, controller still receives them passively
- ✅ **Zigbee/RS485 polling unaffected** — Only background BLE scanning disabled

**Implementation**:
- Set `BG_SCAN_DURATION_NORMAL = 0` (no background scanning)
- Set `BG_SCAN_RESTART_MS = UINT32_MAX` (never auto-restart)
- `sensor_ble_loop()` no longer attempts background scan restarts
- User-initiated `sensor_ble_start_discovery_scan(10, false)` still works for UI discovery

**When to Reconsider**: Only if a future hardware version separates the 2.4 GHz radio (e.g., dual-radio or dedicated BLE antenna) or if WiFi can hold the TCPIP mutex for the entire scan period.

---

If any of these sections need more detail or an example PR (e.g. guidelines for adding an IOPT + web UI exposure), let me know and I will expand the file with step-by-step examples.