/**
 * Register MCP resources that provide reference documentation to the LLM.
 */
export function registerResources(server, getClient) {
    // Static API reference resource
    server.resource("api-overview", "opensprinkler://api-overview", {
        description: "OpenSprinkler API quick reference – error codes, program encoding, special event codes",
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

## Program Flag Bitfield
| Bits | Meaning |
|------|---------|
| 0 | Enable (1=enabled) |
| 1 | Use weather (1=yes) |
| 2-3 | Restriction: 0=none, 1=odd, 2=even |
| 4-5 | Day type: 0=weekly, 1=single, 2=monthly, 3=interval |
| 6 | Start type: 0=repeating, 1=fixed |
| 7 | Date range enable |

## Start-time Encoding
- Standard: minute of day (0-1439)
- Sunset-based: bit13=1, bit12=sign, bits0-10=offset minutes
- Sunrise-based: bit14=1, bit12=sign, bits0-10=offset minutes
- Disabled: bit15=1

## Date Range Encoding
\`(month << 5) + day\`. Jan 1 = 33, Dec 31 = 415.

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

## Sensor Types
| ID Range | Type |
|----------|------|
| 1-9 | RS485/Modbus |
| 10-49 | Analog (ASB) |
| 50-54 | OSPI analog |
| 60-61 | FYTA cloud |
| 90 | MQTT |
| 95 | ZigBee |
| 96 | BLE |
| 100 | Remote OS |
| 101-110 | Weather service |
| 1000-1003 | Sensor groups |
| 10000-10001 | Diagnostics |
`;
//# sourceMappingURL=resources.js.map