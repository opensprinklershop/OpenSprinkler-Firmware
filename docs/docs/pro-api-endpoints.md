# OpenSprinklerPro API and platform addendum

This addendum documents firmware endpoints and platform details that were added after the original 2.2.1 manual pages.

The OpenSprinklerShop firmware, including OpenSprinklerPro, uses its own firmware version numbering independent of upstream OpenSprinkler releases. Current OpenSprinklerShop firmware version: **2.4.0(230)**.

## Platform corrections

| Endpoint | Supported platforms | Notes |
|---|---|---|
| `/uc`, `/uu`, `/us`, `/ub` | ESP32, ESP8266 | Firmware online update and full backup endpoints. Not available on OSPi/Linux builds. |
| `/sx` | ESP32, ESP8266, OSPi | Sensor-only configuration backup/import. Distinct from `/ub`. |
| `/ul`, `/uf`, `/ui/*` | ESP32, OSPi | On-device web UI: status/installation, package upload (ESP32 only, port 8080), static UI files. Not on the ESP8266 (no room). |
| `/jw` | ESP32, ESP8266, OSPi | Monthly water-usage data. |
| `/db?lp=1\|2\|3` | ESP32 | Diagnostics: main-loop profiler (`lp=1` read, `lp=2` read and reset, `lp=3` filesystem benchmark, blocks 1-3 s). Without `lp` the usual `/db` diagnostic object. |
| `/tg`, `/tl`, `/ta`, `/tc`, `/tx` | ESP32 | HTTPS certificate and ACME/Let's Encrypt management. |
| `/rk`, `/rp`, `/ru` | ESP32 with `ENABLE_RAINMAKER` | ESP RainMaker status, provisioning and unlink/reset flows. |
| `/jm`, `/mm` | ESP32 with `ENABLE_MATTER` | Matter pairing information and commissioning window. |
| `/bd`, `/bs`, `/bc` | ESP32 with `OS_ENABLE_BLE` | BLE scan and discovered-device management. |
| `/ir`, `/iw`, `/zj`, `/zs`, `/zg`, `/zd`, `/zo`, `/zc` | ESP32-C5 with Zigbee/IEEE 802.15.4 support | Radio mode and Zigbee gateway/client endpoints. |

## EN 18031-1 compliance (build option `EN18031_COMPLIANCE`, default on for ESP32)

Measures required by RED 2014/53/EU Art. 3.3 d (EN 18031-1). Disable with `-D EN18031_COMPLIANCE=0`.

| Behaviour | Platforms | Notes |
|---|---|---|
| "Ignore password" option forced off | ESP32 | `ipas` in `/jo` is always 0 and `/co?ipas=1` has no effect. Every API call needs `pw`. |
| Generated device password | ESP32 | Starting the WiFi setup AP generates a random 12-character password. It is shown on the display (`PW:` line) and on the setup page; `/jsap` returns it as `"gpw"`. It stays valid until changed with `/sp`; a factory reset issues a new one on the next AP start. |
| `/jo` field `pwdef` | ESP32 | `1` while the factory default password (`opendoor`) is still active (devices upgraded from older firmware). The UI should ask the user to set a new password. |
| Login rate limit | ESP32 | 5 failed password checks within 5 minutes lock the API and `/update` for 30 seconds (result 2, unauthorized). |
| Setup AP band | ESP32-C5 | The setup AP is always 2.4 GHz (channel 1); station mode may use 2.4 GHz and 5 GHz. WiFi country code `DE`. |

## Online update and backup

| Endpoint | Purpose |
|---|---|
| `GET /uc?pw=...` | Check update manifest and report whether a newer firmware is available. |
| `GET /uu?pw=...` | Start firmware update. Advanced parameters can override URLs and hashes: `zu`, `mu`, `fu`, `zs`, `ms`; `vt` selects `zigbee` or `matter`. |
| `GET /us?pw=...` | Read update status and progress. |
| `GET /ub?pw=...` | Export full controller configuration for app-side backup before update. |

## On-device web UI (firmware 2.4.0(231) and later, ESP32 and OSPi)

The web interface can be stored on the controller and served by the controller
itself (ESP32: external LittleFS, OSPi: `<data dir>/ui/`). A UI package
(`ui.tar`, built by `tools/mkuipack.py`, ECDSA P-256 signed) is installed per
firmware version under `/ui/<version>/`; the local copy is only used when its
version equals the running firmware. It is the fallback for offline / island
operation: without Internet access the device still serves its own UI.

| Endpoint | Purpose |
|---|---|
| `GET /ul?pw=...` | Status: `fw`, `installed` (version served locally), `exact`, `use`, `stale`, `src` (0 device/auto, 1 device only, 2 external), `fb` (fallback), `versions`, `state` (0 idle, 1 downloading, 2 verifying, 3 activating, 4 done, 5 error), `progress`, `received`, `total`, `free`, `fs`, `busy`, `url` (archive package URL for this firmware), `msg`. |
| `GET /ul?pw=...&install=1[&url=...]` | Download `<url>` and `<url>.sig` (default: `upgrade/archive/v<fw>_<minor>/ui.tar`) in a background task, verify the signature and activate. `action` in the reply is 1 when the task was started. |
| `GET /ul?pw=...&remove=1[&ver=x.y.z.n]` | Remove one or all installed UI versions; the home page falls back to the external URL. |
| `GET /ul?pw=...&check=1` | Trigger the automatic installation check now (otherwise 2 min after boot, then with 1 h / 6 h / 24 h backoff). |
| `POST :8080/uf` (multipart `file`, fields `pw`, `sig`) | Upload a package for devices without Internet access. `sig` is the base64 signature (`ui.tar.sig`). Unsigned or foreign packages are rejected. ESP32 only. |
| `GET /ui/...` | Static files of the active UI version (`/ui/js/home.js`, `/ui/versions.json`, `/ui/<version>/js/...`), served gzip-encoded with immutable caching for versioned paths. |
| `GET /su`, `GET /cu?pw=...&uis=0|2&uifb=0|1&jsp=...` | Firmware-own settings page and its form target: UI source, fallback, external URL, weather URL. |

The external UI is faster, so it stays the default: in mode 0 the home page
loads the external loader with an error handler and a watchdog and reloads
itself as `/?ui=local` (device copy) when the loader fails or has not run after
`uifb` seconds; `/?ui=ext` forces the external loader. Related options (`/jo`,
`/co`): `uisr` (0 external first / device copy when offline, 1 always the device
copy, 2 external only, no package installed), `uifb` (seconds before the
browser switches to the device copy, default 6, 0 = only on load error). `/jo` also reports `uiv` (installed version
served locally), `uia` (1 = home page uses the local copy) and `uist` (1 = an
older local version is served in "device only" mode). On the first boot after
the upgrade a custom `jsp` URL keeps the external source (`uisr=2`), the default
URL switches to the device copy (`uisr=0`). The OTA update installs the matching
package at the end of phase 2; `manifest.json` carries `ui_url`, `ui_version`,
`ui_size`, `ui_sha256`, `ui_sig`.

## HTTPS certificates and ACME

| Endpoint | Purpose |
|---|---|
| `GET /tg?pw=...` | Read active certificate type, subject, issuer and validity. |
| `POST /tl?pw=...` | Upload custom PEM certificate and key. Reboot afterwards. |
| `GET /ta?pw=...` | Read ACME configuration and status. |
| `POST /tc?pw=...` | Save ACME settings and optionally request a certificate. |
| `GET /tx?pw=...` | Delete ACME data and return to the internal certificate. |

## RainMaker and Matter

| Endpoint | Purpose |
|---|---|
| `GET /rk?pw=...` | Read RainMaker status. Optional `reset_mapping=1` or `factory_reset=1` perform maintenance actions. |
| `GET /rp?pw=...&uid=...&key=...` | Start RainMaker provisioning. |
| `GET /ru?pw=...` | Unlink RainMaker account and trigger reset/reboot behavior. |
| `GET /jm?pw=...` | Read Matter commissioning state, QR-code URL and manual pairing code. |
| `GET /mm?pw=...&t=300` | Open Matter commissioning window; `t` is optional timeout in seconds, max `900`. |

## Water usage and sensors

| Endpoint | Purpose |
|---|---|
| `GET /jw?pw=...` | Read monthly water usage: pulse rate, current month and stored records. |
| `GET /sf?pw=...` | List supported sensor types for the current build. |
| `GET /sx?pw=...` | Export/import sensor configuration only. |
| `GET /mc`, `/ml`, `/mt` | Configure, list and discover monitor types. |
| `GET /mc?type=5&monitor=N&master=1\|2` | Monitor type 5 *SET MASTER* (water source switch, 2.4.0(231)): while monitor `N` is active, stations bound to `master` run with the other master. Reported by `/ml` as `monitor`/`master`; `master` outside 1..2 is rejected (result 18). The active redirect is in `/jc` as `masrd` (0/1/2). All platforms. |

## Expanded Sensor API (compatible with official firmware 2.2.1(5))

The official OpenSprinkler firmware 2.2.1(5) introduced an "Expanded Sensor" API. OpenSprinklerShop firmware 2.4.0(230) and later implements the same endpoints as a facade over its own sensor system: there is one sensor store, the upstream `uuid` is the OpenSprinklerShop sensor `nr`, and the official OpenSprinkler App as well as third-party clients written against the official API work unchanged. The OpenSprinklerShop endpoints (`/sl`, `/sc`, `/so`, `/se`, ...) continue to work on the same data.

Available on ESP32, ESP8266 and OSPi. Endpoint keys have three letters.

| Endpoint | Purpose |
|---|---|
| `GET /jsn?pw=...` | List sensors with the latest reading (`sn[]`, `count`). Keys: `uuid`, `name`, `type`, `unit`, `flag`, `status`, `interval` (minutes), `min`, `max`, `value`, `extra`. |
| `GET /csn?pw=...&uuid=-1&type=...` | Add (`uuid=-1` or `sid=-1`) or modify a sensor. Common parameters: `name`, `min`, `max`, `interval`, `unit`, `flag`. Type parameters: `children`/`action` (Aggregate), `pin`/`subtype`/`scale`/`offset`/`points` (ADS1115), `action` (Weather), `metric` (System Internal), `input` (Onboard Digital), `ntype` (type 5, see below). |
| `GET /dsn?pw=...&uuid=...` | Delete one sensor; `uuid=-1` deletes all sensors. |
| `GET /jsd?pw=...` | Sensor descriptions for schema-driven editors: `sensors[]` (index = `type`), `units[]`, `enums`, `as`, `flags`. |
| `GET /jsl?pw=...` | Sensor log: `[[uuid,ts,value],...]`; `fmt=csv` or `fmt=binary`; filters `uuid`/`sid`, `before`, `after`, `count`, `cursor`; `page=1` enables slot pagination with the `X-OS-*` response headers. |
| `GET /dsl?pw=...&uuid=...` | Delete log records of one sensor (`uuid=-1`: whole log). `page=1` returns the JSON progress object; the delete completes in one request (`done=1`). |
| `GET /jpa?pw=...` | Per-program weather (`wa`), sensor (`sa`) and total (`ta`) adjustment factors plus `maxrt`. |
| `GET /jp` | Each program entry carries the sensor adjustment object `{flag,uuid,splits[]}` as 8th element (`{}` when none). |
| `GET /cp?...&snadj=flag,uuid,x0,y0,...` | Set the program sensor adjustment; `snadj=0,0` removes it. `v` is still required. |
| `GET /mp?...&usa=0|1` | Manual program start with (`1`) or without (`0`) the sensor adjustment. Without `usa` the OpenSprinklerShop behaviour is kept and the adjustment is applied. |

Type mapping (`/jsn.type`, index of `/jsd.sensors`):

| type | Official name | OpenSprinklerShop sensors |
|---|---|---|
| 0 | Aggregate | Sensor groups MIN/MAX/AVG/SUM and the new MEDIAN (1004) / RANGE (1005). `children` are the group members; child `scale`/`offset` must be `1`/`0`. |
| 1 | ADS1115 | Analog Sensor Board types (10, 11, 12 piecewise, 15-18, 30-32, 49) and OSPi ADC types (50-53). `pin` 1-16 = board address 0x48-0x4B x channel 0-3. Subtypes: 0 Linear, 1 Piecewise (new type 12), 10/11 SMT50, 12/13 SMT100 analog, 15 VH400, 16 THERM200, 17 AquaPlumb. `scale`/`offset` are applied after the conversion. |
| 2 | Weather | Weather service sensors (101-110); `action` indexes `enums.WeatherAction`. |
| 3 | System Internal | Free memory (`metric` 0), free storage (1), CPU temperature (3). |
| 4 | Onboard Digital | New type 56: debounced state of SN1 (`input` 0) or SN2 (`input` 1). |
| 5 | OpenSprinklerShop sensor | Every other type (RS485, MQTT, Zigbee, BLE, FYTA, Gardena, remote, flow meter, ...). `extra.ntype` is the OpenSprinklerShop type; type-specific settings are edited with `/sc`. |

Behavioural notes:

- `min`/`max` clamp the sensor output when they are sent with `/csn` (stored as `cmin`/`cmax`/`clamp` in `sensors.json`); `status` reports bit 3/4 when a value was clamped. Sensors created with `/sc` are not clamped and report their natural range.
- `interval` is in minutes; OpenSprinklerShop `ri` is in seconds and is rounded to full minutes in `/jsn`.
- Baked subtypes report their native unit; a different unit of the same group (e.g. Fahrenheit) is rejected because the firmware does not convert units.
- Program adjustments: a `snadj` curve with two points becomes a `PROG_LINEAR` adjustment, a two-point step becomes `PROG_DIGITAL_MIN`, other curves use the new adjustment type `PROG_PIECEWISE` (5). `/jp` reports the first adjustment of a program; additional adjustments defined with `/sb` are still applied by the scheduler.

For the complete legacy API reference, see [Firmware 2.2.1 API Reference](2.2.1/221_4_api.md).
