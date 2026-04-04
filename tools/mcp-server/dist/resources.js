/**
 * Register MCP resources that provide reference documentation to the LLM.
 */
export function registerResources(server, getClient) {
    // Static API reference resource
    server.resource("api-overview", "opensprinkler://api-overview", {
        description: "OpenSprinkler API reference – program encoding, sensor configuration, program adjustments, monitors, error codes",
        mimeType: "text/markdown",
    }, async () => ({
        contents: [
            {
                uri: "opensprinkler://api-overview",
                mimeType: "text/markdown",
                text: API_OVERVIEW,
            },
        ],
    }));
    // Dynamic resource: live controller summary
    server.resource("controller-summary", "opensprinkler://controller-summary", {
        description: "Live summary of the connected OpenSprinkler controller",
        mimeType: "text/markdown",
    }, async () => {
        try {
            const data = await getClient().get("/ja");
            const opts = (data.options ?? {});
            const settings = (data.settings ?? {});
            const status = (data.status ?? {});
            const stations = (data.stations ?? {});
            const lines = [
                "# OpenSprinkler Controller Summary",
                "",
                `- **Firmware**: ${opts.fwv ?? "?"}.${opts.fwm ?? "?"}`,
                `- **Hardware**: v${opts.hwv ?? "?"}  type=0x${(opts.hwt ?? 0).toString(16).toUpperCase()}`,
                `- **Stations**: ${status.nstations ?? "?"}`,
                `- **Programs**: ${(data.programs?.nprogs) ?? "?"}`,
                `- **Operation**: ${settings.en === 1 ? "Enabled" : "Disabled"}`,
                `- **Rain Delay**: ${settings.rd === 1 ? `Active (until ${settings.rdst})` : "Inactive"}`,
                `- **Water Level**: ${opts.wl ?? "?"}%`,
                `- **WiFi RSSI**: ${settings.RSSI ?? "N/A"} dBm`,
                `- **Device Name**: ${settings.dname ?? "(not set)"}`,
                `- **Station Names**: ${JSON.stringify(stations.snames ?? [])}`,
                "",
                `_Fetched at ${new Date().toISOString()}_`,
            ];
            return {
                contents: [
                    {
                        uri: "opensprinkler://controller-summary",
                        mimeType: "text/markdown",
                        text: lines.join("\n"),
                    },
                ],
            };
        }
        catch (err) {
            return {
                contents: [
                    {
                        uri: "opensprinkler://controller-summary",
                        mimeType: "text/plain",
                        text: `Error fetching controller summary: ${err}`,
                    },
                ],
            };
        }
    });
}
const API_OVERVIEW = `# OpenSprinkler API Quick Reference

## Authentication
All endpoints require \`pw\` parameter with the MD5 hash of the device password.

## Error Codes
| Code | Meaning |
|------|---------|
| 1 | Success |
| 2 | Unauthorized |
| 3 | Mismatch (password) |
| 16 | Data Missing |
| 17 | Out of Range |
| 18 | Data Format Error |
| 32 | Page Not Found |
| 48 | Not Permitted |
| 64 | Upload Failed |

---

## Program Encoding (change_program \`v\` parameter)

Format: \`[flag, days0, days1, [start0,start1,start2,start3], [dur0,dur1,...durN]]\`

### Flag Bitfield (byte 0)
| Bit | Meaning |
|-----|---------|
| 0 | Enable (1=enabled) |
| 1 | Use weather adjustment (1=yes) |
| 2-3 | Restriction: 0=none, 1=odd days only, 2=even days only |
| 4-5 | Schedule type: 0=weekly, 1=single-run, 2=monthly, 3=interval |
| 6 | Start-time type: 0=repeating, 1=fixed times |
| 7 | Date range enable (1=use from/to) |

### Days Encoding (depends on schedule type)
- **Weekly** (type=0): days0 = weekday bitmask (bit0=Mon, bit1=Tue, … bit6=Sun). Example: 31=Mon–Fri, 127=every day
- **Single-run** (type=1): days0 + days1 encode epoch day count
- **Monthly** (type=2): days0 = day of month (1–31, 32=last day of month)
- **Interval** (type=3): days0 = remainder (starting day offset), days1 = interval in days

### Start-time Encoding (4 slots)
- **Fixed** (flag bit6=1): up to 4 independent start times in minutes from midnight (0–1439). Use -1 for unused slots
- **Repeating** (flag bit6=0): start0=start time, start1=end time, start2=repeat interval (minutes)
- **Sunrise offset**: set bit14. Bit12=sign (0=after, 1=before). Bits 0–11=offset minutes
- **Sunset offset**: set bit13. Bit12=sign (0=after, 1=before). Bits 0–11=offset minutes
- **Disabled slot**: set bit15 (value ≥ 32768)

### Duration Array
One entry per station, in seconds. Length must match the number of stations on the controller. Use 0 to skip a station.

### Date Range Encoding (from/to parameters)
Formula: \`(month << 5) + day\`. Examples: Jan 1 = 33, Jun 15 = 207, Dec 31 = 415.

### Program Examples

**Daily morning (6:00 AM, all days, stations 0+1 for 15/20 min, weather-adjusted):**
\`v=[3,127,0,[360,-1,-1,-1],[900,1200,0,0,0,0,0,0]]\`
- flag=3 (enabled + weather), days0=127 (Mon–Sun), start0=360 (6:00)

**Every 3 days, repeating every 4h from 6:00–18:00:**
\`v=[1,0,3,[360,1080,240,-1],[600,0,0,0,0,0,0,0]]\`
- flag=1 (enabled, no weather, interval type=3→bits4-5=11=0x30… actually: type=3 means bits4-5=11, so flag = 0b00110001 = 49)
- Correction: flag for interval+repeating+enabled = (3<<4)|1 = 49

**Sunrise + 30 min, Mon/Wed/Fri, even days only:**
\`v=[13,21,0,[16414,-1,-1,-1],[300,300,0,0,0,0,0,0]]\`
- flag=13 (enabled + even + fixed), days0=21 (Mon+Wed+Fri), start0=16414 (bit14 + 30 min offset)

---

## Sensor Configuration (/sc)

### Parameters
| Param | Type | Description |
|-------|------|-------------|
| \`nr\` | uint | Sensor number (1–65535). Required |
| \`type\` | uint | Sensor type (0=delete). Required |
| \`name\` | string | Display name (max 30 chars) |
| \`group\` | uint | Group assignment (sensor nr of group sensor) |
| \`ip\` | uint32 | IP address for network sensors |
| \`port\` | uint | TCP port |
| \`id\` | uint | Device/Modbus ID or ADC channel |
| \`ri\` | uint | Read interval in seconds |
| \`enable\` | 0/1 | Enable sensor |
| \`log\` | 0/1 | Enable logging |
| \`show\` | 0/1 | Show on main screen |
| \`fac\` | int16 | Factor for value conversion |
| \`div\` | int16 | Divider for value conversion |
| \`offset\` | int16 | Offset in millivolts |
| \`unit\` | string | Custom unit string (max 8 chars) |
| \`unitid\` | uint | Unit ID (99=custom) |
| \`topic\` | string | MQTT topic (type=90) |
| \`filter\` | string | JSON filter path (type=90) |
| \`device_ieee\` | string | ZigBee IEEE address (type=95) |
| \`endpoint\` | uint | ZigBee endpoint (type=95) |
| \`cluster_id\` | uint | ZigBee cluster ID (type=95) |
| \`attribute_id\` | uint | ZigBee attribute ID (type=95) |
| \`device_mac\` | string | BLE MAC address (type=96) |
| \`char_uuid\` | string | BLE characteristic UUID (type=96) |

### Sensor Types
| ID | Name | Description |
|----|------|-------------|
| 0 | NONE | Delete sensor |
| 1 | SMT100_MOIS | Truebner SMT100 moisture (RS485) |
| 2 | SMT100_TEMP | Truebner SMT100 temperature (RS485) |
| 3 | SMT100_PMTY | Truebner SMT100 permittivity (RS485) |
| 4 | TH100_MOIS | Truebner TH100 humidity (RS485) |
| 5 | TH100_TEMP | Truebner TH100 temperature (RS485) |
| 9 | MODBUS_RTU | Generic Modbus RTU |
| 10 | ANALOG_EXT | Analog extension board (0–4V) |
| 11 | ANALOG_EXT_P | Analog extension board (0–100%) |
| 15 | SMT50_MOIS | SMT50 moisture (VWC%) |
| 16 | SMT50_TEMP | SMT50 temperature |
| 17 | SMT100_A_MOIS | SMT100 analog moisture |
| 18 | SMT100_A_TEMP | SMT100 analog temperature |
| 30 | VH400 | Vegetronix VH400 |
| 31 | THERM200 | Vegetronix THERM200 |
| 32 | AQUAPLUMB | Vegetronix Aquaplumb |
| 49 | USERDEF | User-defined (custom fac/div/offset) |
| 50-53 | OSPI_ANALOG | OSPi analog variants |
| 54 | INTERNAL_TEMP | Internal chip temperature |
| 60 | FYTA_MOIS | FYTA cloud moisture |
| 61 | FYTA_TEMP | FYTA cloud temperature |
| 90 | MQTT | MQTT subscriber |
| 95 | ZIGBEE | ZigBee sensor |
| 96 | BLE | Bluetooth Low Energy |
| 100 | REMOTE | Remote OpenSprinkler sensor |
| 101 | WEATHER_TEMP_F | Weather temperature °F |
| 102 | WEATHER_TEMP_C | Weather temperature °C |
| 103 | WEATHER_HUM | Weather humidity % |
| 105 | WEATHER_PRECIP_IN | Weather precipitation (inches) |
| 106 | WEATHER_PRECIP_MM | Weather precipitation (mm) |
| 107 | WEATHER_WIND_MPH | Weather wind (mph) |
| 108 | WEATHER_WIND_KMH | Weather wind (km/h) |
| 109 | WEATHER_ETO | Weather evapotranspiration |
| 110 | WEATHER_RADIATION | Weather solar radiation |
| 1000 | GROUP_MIN | Group: minimum value |
| 1001 | GROUP_MAX | Group: maximum value |
| 1002 | GROUP_AVG | Group: average value |
| 1003 | GROUP_SUM | Group: sum of values |
| 10000 | FREE_MEMORY | System free memory |
| 10001 | FREE_STORE | System free storage |

### Unit IDs
| ID | Unit | Description |
|----|------|-------------|
| 0 | % | Percent |
| 1 | V | Volt |
| 2 | mV | Millivolt |
| 3 | °C | Celsius |
| 4 | °F | Fahrenheit |
| 5 | kPa | Kilopascal |
| 6 | cbar | Centibar |
| 7 | mm | Millimeter |
| 8 | in | Inch |
| 9 | km/h | Kilometer/hour |
| 10 | Level | Water level |
| 11 | DK | Permittivity |
| 12 | lm | Lumen |
| 13 | lx | Lux |
| 99 | (custom) | Custom unit from \`unit\` parameter |

---

## Program Adjustments (/sb — configure_adjustment)

Sensor-based program adjustments automatically scale watering duration based on live sensor readings.

### Parameters
| Param | Type | Description |
|-------|------|-------------|
| \`nr\` | uint | Adjustment number (1–65535). Required |
| \`type\` | uint | Adjustment type (0=delete). Required |
| \`sensor\` | uint | Sensor number to read |
| \`prog\` | uint | Program number to adjust (0-based) |
| \`factor1\` | double | First factor / boundary value |
| \`factor2\` | double | Second factor / boundary value |
| \`min\` | double | Sensor range minimum |
| \`max\` | double | Sensor range maximum |
| \`name\` | string | Adjustment name (max 30 chars) |

### Adjustment Types
| Type | Name | Formula |
|------|------|---------|
| 0 | DELETE | Delete this adjustment |
| 1 | LINEAR | Linear interpolation between min→max mapped to factor1→factor2 |
| 2 | DIGITAL_MIN | If sensor ≤ min: apply factor1, else: apply factor2 |
| 3 | DIGITAL_MAX | If sensor ≥ max: apply factor2, else: apply factor1 |
| 4 | DIGITAL_MINMAX | If sensor ≤ min OR sensor ≥ max: apply factor1, else: apply factor2 |

### Linear Interpolation (type=1)
The factor values represent watering multipliers (1.0 = 100%, 0.5 = 50%, 2.0 = 200%).

If factor1 > factor2 (decreasing): \`result = (max - sensor) / (max - min) × (factor1 - factor2) + factor2\`
If factor1 < factor2 (increasing): \`result = (sensor - min) / (max - min) × (factor2 - factor1) + factor1\`

**Example — reduce watering as soil moisture increases:**
- sensor=SMT100 moisture, min=10, max=90, factor1=2.0, factor2=0.0
- At 10% moisture → 200% watering, at 50% → 100%, at 90% → 0% (skip)

**Example — increase watering as temperature rises:**
- sensor=temperature, min=15, max=40, factor1=0.5, factor2=1.5
- At 15°C → 50% watering, at 27.5°C → 100%, at 40°C → 150%

### Digital Examples
**DIGITAL_MIN (type=2) — rain cutoff:**
- sensor=rain sensor, min=5 (mm), factor1=0 (stop), factor2=1 (normal)
- Rain ≤ 5mm → stop watering (factor 0), Rain > 5mm → normal watering

**DIGITAL_MINMAX (type=4) — frost/heat protection:**
- sensor=temperature, min=3, max=35, factor1=0 (stop), factor2=1 (normal)
- Below 3°C or above 35°C → stop watering, otherwise → normal

---

## Monitors (/mc — configure_monitor)

Monitors watch sensor values or conditions and trigger/block programs based on thresholds and logic.

### Core Parameters
| Param | Type | Description |
|-------|------|-------------|
| \`nr\` | uint | Monitor number (1–65535). Required |
| \`type\` | uint | Monitor type (0=delete). Required |
| \`sensor\` | uint | Sensor number (for MIN/MAX types) |
| \`prog\` | uint | Program number to control |
| \`zone\` | uint | Zone number |
| \`name\` | string | Monitor name (max 30 chars) |
| \`maxrun\` | ulong | Maximum runtime in seconds |
| \`prio\` | uint8 | Priority: 0=low, 1=medium, 2=high |
| \`rs\` | ulong | Reset/cooldown time in seconds |

### Monitor Types
| Type | Name | Description |
|------|------|-------------|
| 0 | DELETE | Delete this monitor |
| 1 | MIN | Trigger if sensor value ≤ threshold |
| 2 | MAX | Trigger if sensor value ≥ threshold |
| 3 | SENSOR12 | Read digital OpenSprinkler rain/soil sensor |
| 4 | SET_SENSOR12 | Write to digital OS sensor |
| 10 | AND | Logical AND of up to 4 monitors |
| 11 | OR | Logical OR of up to 4 monitors |
| 12 | XOR | Logical XOR of up to 4 monitors |
| 13 | NOT | Logical NOT of one monitor |
| 14 | TIME | Time window trigger (weekday + time range) |
| 100 | REMOTE | Remote monitor on another OpenSprinkler |

### MIN/MAX Parameters (types 1, 2)
| Param | Type | Description |
|-------|------|-------------|
| \`value1\` | double | Threshold value |
| \`value2\` | double | Secondary value (hysteresis) |

### SENSOR12 Parameters (type 3)
| Param | Type | Description |
|-------|------|-------------|
| \`sensor12\` | uint16 | Sensor 12 number |
| \`invers\` | 0/1 | 0=normal, 1=inverted logic |

### Logic Gate Parameters (types 10, 11, 12)
| Param | Type | Description |
|-------|------|-------------|
| \`monitor1\` | uint16 | First monitor ID |
| \`monitor2\` | uint16 | Second monitor ID |
| \`monitor3\` | uint16 | Third monitor ID (optional) |
| \`monitor4\` | uint16 | Fourth monitor ID (optional) |
| \`invers1\`–\`invers4\` | 0/1 | Invert logic per input |

### NOT Parameters (type 13)
| Param | Type | Description |
|-------|------|-------------|
| \`monitor\` | uint16 | Monitor ID to invert |

### TIME Parameters (type 14)
| Param | Type | Description |
|-------|------|-------------|
| \`from\` | uint16 | Start time as HHMM (e.g. 0630 = 6:30 AM) |
| \`to\` | uint16 | End time as HHMM (e.g. 1800 = 6:00 PM) |
| \`wdays\` | uint8 | Weekday bitmask (bit0=Mon … bit6=Sun) |

### REMOTE Parameters (type 100)
| Param | Type | Description |
|-------|------|-------------|
| \`rmonitor\` | uint16 | Remote monitor ID |
| \`ip\` | uint32 | Remote device IP |
| \`port\` | uint16 | Remote device port |

### Monitor Examples

**Stop watering if soil moisture > 60%:**
- type=2 (MAX), sensor=soil moisture sensor nr, value1=60, prog=0, maxrun=0

**Water only between 6:00–10:00 on weekdays:**
- type=14 (TIME), from=0600, to=1000, wdays=31 (Mon–Fri)

**Combined: water if dry AND during allowed time:**
- Monitor A: type=1 (MIN), sensor=moisture, value1=30 (trigger if <30%)
- Monitor B: type=14 (TIME), from=0600, to=1000, wdays=127
- Monitor C: type=10 (AND), monitor1=A.nr, monitor2=B.nr → triggers prog only when both conditions met

---

## Special Log Events (pid=0)
| sid | Meaning |
|-----|---------|
| s1 | Sensor 1 |
| s2 | Sensor 2 |
| rd | Rain delay |
| fl | Flow reading |
| wl | Watering level |

## Reboot Causes (lrbtc)
0=None, 1=Factory reset, 2=Button, 3=AP mode, 4=API/timer, 5=API reboot,
6=AP→client, 7=FW update, 8=Weather fail>24h, 9=Network fail, 10=NTP sync, 99=Power-on
`;
//# sourceMappingURL=resources.js.map