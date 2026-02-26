# Built-in MCP Server — Firmware HTTP Endpoint `/mcp`

The OpenSprinkler ESP32 firmware includes a built-in [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) server. AI assistants (Claude, GitHub Copilot, etc.) can use it to query and control the controller directly via HTTP — no external Node.js process required.

## Requirements

- **ESP32 firmware** (ESP32-C5 or compatible)
- Firmware compiled with `USE_OTF` (default for all non-AVR, non-DEMO builds)
- Controller accessible on the network

## Endpoint

```
POST http://<controller-ip>/mcp
Content-Type: application/json
```

The endpoint implements the **MCP Streamable-HTTP transport** with **JSON-RPC 2.0** framing.

## Authentication

The password is supplied as the **MD5 hash** of the admin password (same as all other REST API endpoints). Three methods are accepted:

| Method | Example |
|--------|---------|
| Query parameter | `POST /mcp?pw=<md5hash>` |
| Custom header | `X-OS-Password: <md5hash>` |
| Bearer token | `Authorization: Bearer <md5hash>` |

Compute the MD5 hash:
```bash
echo -n "your_password" | md5sum | awk '{print $1}'
```

If the `Ignore Password` option is enabled on the controller, authentication is skipped.

## JSON-RPC Methods

### `initialize`

MCP handshake — must be called first by conforming clients.

**Request:**
```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"my-client","version":"1.0"}}}
```

**Response:**
```json
{
  "jsonrpc":"2.0","id":1,
  "result":{
    "protocolVersion":"2024-11-05",
    "serverInfo":{"name":"opensprinkler","version":"1.0.0"},
    "capabilities":{"tools":{}}
  }
}
```

### `ping`

Liveness check. Returns an empty result object.

```json
{"jsonrpc":"2.0","id":2,"method":"ping"}
```

### `tools/list`

Returns the list of available tools with their input schemas.

```json
{"jsonrpc":"2.0","id":3,"method":"tools/list"}
```

### `tools/call`

Invokes a tool by name.

```json
{
  "jsonrpc":"2.0","id":4,
  "method":"tools/call",
  "params":{
    "name":"get_station_status",
    "arguments":{}
  }
}
```

## Available Tools

### Read-only tools

| Tool | Equivalent REST | Description |
|------|----------------|-------------|
| `get_all` | `/ja` | All controller data: settings, programs, options, status, stations |
| `get_controller_variables` | `/jc` | Time, rain delay, queue status, flow, MQTT/OTC state |
| `get_options` | `/jo` | Firmware version, timezone, sensor config, master stations, water level |
| `get_stations` | `/jn` | Station names and attributes (master op, ignore rain, disable, group) |
| `get_station_status` | `/js` | Current on/off state of all stations |
| `get_programs` | `/jp` | All irrigation programs (schedules, durations, names) |
| `get_debug` | `/db` | Build date/time, free heap, flash usage, WiFi RSSI, PSRAM |

All read-only tools return a JSON text response wrapped in MCP `content[0].text`.

### Control tools

#### `manual_station_run`

Open or close a single station manually (equivalent to `/cm`).

| Argument | Type | Required | Description |
|----------|------|----------|-------------|
| `sid` | integer | Yes | Station index (0-based) |
| `en` | integer | Yes | `1` = open, `0` = close |
| `t` | integer | When `en=1` | Duration in seconds (1–64800) |
| `qo` | integer | No | Queue option: `0` = append (default), `1` = insert front |
| `ssta` | integer | No | Shift remaining stations when closing (`1` = shift) |

**Example — open station 2 for 5 minutes:**
```json
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"manual_station_run","arguments":{"sid":2,"en":1,"t":300}}}
```

Returns `{"result":1}` on success (result code `1` = `HTML_SUCCESS`).

#### `change_controller_variables`

Change controller state (equivalent to `/cv`).

| Argument | Type | Description |
|----------|------|-------------|
| `en` | integer | Enable (`1`) or disable (`0`) irrigation |
| `rd` | integer | Rain delay in hours (`0` = cancel) |
| `rsn` | integer | Reset all stations (`1`) |
| `rrsn` | integer | Reset running stations only (`1`) |
| `rbt` | integer | Reboot the controller (`1`) |

**Example — set rain delay for 24 hours:**
```json
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"change_controller_variables","arguments":{"rd":24}}}
```

#### `pause_queue`

Pause or resume the irrigation queue (equivalent to `/pq`).

| Argument | Type | Description |
|----------|------|-------------|
| `dur` | integer | Toggle-style pause for N seconds (`0` = cancel/resume) |
| `repl` | integer | Set/replace pause duration in seconds (`0` = cancel/resume) |

## curl Examples

```bash
# Compute MD5 (replace with your password):
PW=$(echo -n "opendoor" | md5sum | awk '{print $1}')
BASE="http://192.168.0.86"

# Initialize
curl -s -X POST "$BASE/mcp?pw=$PW" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{}}}'

# List tools
curl -s -X POST "$BASE/mcp?pw=$PW" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

# Get all station status
curl -s -X POST "$BASE/mcp?pw=$PW" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_station_status","arguments":{}}}'

# Open station 0 for 2 minutes (using Authorization header)
curl -s -X POST "$BASE/mcp" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $PW" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"manual_station_run","arguments":{"sid":0,"en":1,"t":120}}}'
```

## Error Codes

JSON-RPC standard error codes are used:

| Code | Meaning |
|------|---------|
| `-32700` | Parse error (invalid JSON) |
| `-32600` | Invalid request (bad JSON-RPC framing, or auth failure with HTTP 401) |
| `-32601` | Method not found |
| `-32602` | Invalid parameters |
| `-32603` | Internal error |

Tool result codes (in `content[0].text`) follow the OpenSprinkler REST API convention (`"result":1` = success, other values indicate specific errors).

## CORS

The endpoint sends permissive CORS headers (`Access-Control-Allow-Origin: *`), allowing browser-based clients to call it directly.

## Comparison: Built-in vs. External MCP Server

| Feature | Built-in (`/mcp`) | External (Node.js stdio) |
|---------|-------------------|--------------------------|
| Transport | HTTP (Streamable-HTTP) | stdio |
| Tools available | 10 core tools | 40+ tools (full REST API) |
| Requires Node.js | No | Yes |
| Separate process | No | Yes |
| Best for | Direct HTTP MCP clients, embedded use | VS Code Copilot, Claude Desktop |

For full controller control (all REST endpoints), use the external Node.js MCP server in `tools/mcp-server/`.

## Implementation Notes

- Guard: `#if defined(ESP32) && defined(USE_OTF)` — available on all ESP32 builds with OTF enabled (default)
- Source: `mcp_server.h` / `mcp_server.cpp`
- Route registration: `opensprinkler_server.cpp` → `start_server_client()`
- The implementation reuses the existing `server_json_*_main()` helper functions via a capture-mode mechanism (`g_mcp_capture_active` / `g_mcp_capture_buf` in `opensprinkler_server.cpp`)
