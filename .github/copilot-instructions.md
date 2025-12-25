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

## Useful files to inspect 📂
- `main.cpp` (entry / loops)  
- `OpenSprinkler.cpp`, `OpenSprinkler.h` (core state & nv data)  
- `defines.h` (macros, I/O option indices, file names)  
- `opensprinkler_server.cpp` (HTTP API and web UI hooks)  
- `platformio.ini` (build targets and flags)  
- `build.sh` / `build2.sh` (native build and runtime steps)  
- `sensor_*.cpp` (sensor implementations) — e.g. `sensor_rs485_i2c.cpp` as a non-trivial example.

---

If any of these sections need more detail or an example PR (e.g. guidelines for adding an IOPT + web UI exposure), let me know and I will expand the file with step-by-step examples.