# OpenSprinkler MCP Server — Entwicklerhandbuch

Dieses Handbuch hilft bei der Erweiterung und Wartung des MCP Servers.

## Architektur

```
tools/mcp-server/
├── src/
│   ├── index.ts        # Einstiegspunkt, MCP-Server + Transport
│   ├── client.ts       # HTTP-Client für OpenSprinkler REST-API
│   ├── tools.ts        # MCP-Tool-Registrierung (45+ Tools)
│   └── resources.ts    # MCP-Ressourcen (API-Docs, Status)
├── dist/               # Kompilierter JavaScript (wird generiert)
├── package.json        # Dependencies
├── tsconfig.json       # TypeScript-Konfiguration
├── README.md           # Überblick
├── USER_GUIDE.md       # Benutzerhandbuch
└── DEVELOPER_GUIDE.md  # Dieses Dokument
```

## Development Setup

### Schritt 1: Kompilierung

```bash
cd tools/mcp-server
npm install
npm run build

# Überprüfe, dass dist/index.js erstellt wurde
ls -la dist/
```

### Entwicklung

```bash
# TypeScript im Watch-Mode (kompiliert bei Änderungen)
npm run dev

# ODER manuell kompilieren
npm run build

# Testen mit lokalem Server
# Stelle zuerst sicher, dass die Konfiguration gesetzt ist:
OS_BASE_URL=http://192.168.0.86 OS_PASSWORD_HASH=<YOUR_ADMIN_PASSWORD_HASH> node dist/index.js
```

## Code-Struktur

### 1. index.ts — Einstiegspunkt

```typescript
async function main() {
  const client = createClient();      // HTTP-Client konfiguieren
  const server = new McpServer(...);  // MCP-Server erstellen
  registerTools(server, ...);         // Tools registrieren
  registerResources(server, ...);     // Ressourcen registrieren
  const transport = new StdioServerTransport();
  await server.connect(transport);    // Verbindung öffnen
}
```

**Aufgaben:**
- OpenSprinkler HTTP-Client erstellen
- MCP-Server initialisieren
- Tools und Ressourcen registrieren
- stdio-Transport starten

### 2. client.ts — HTTP-Wrapper

```typescript
class OpenSprinklerClient {
  constructor(config: OpenSprinklerClientConfig);
  
  async get<T>(path: string, params?: Record<string, unknown>): Promise<T>;
  async command(path: string, params?: Record<string, unknown>): Promise<...>;
}
```

**Besonderheiten:**
- Konvertiert Passwort (Klartext oder MD5-Hash)
- Hängt `?pw=<hash>` an alle Anfragen an
- JSON-Parsing mit Error-Handling
- 15-Sekunden Timeout pro Request

**Beispiel:**
```typescript
const data = await client.get('/ja');  // Alle Daten
const result = await client.command('/cv', { sd: 0 });  // Stationen stoppen
```

### 3. tools.ts — Tool-Registrierung

Jedes Tool wird mit `server.tool()` registriert:

```typescript
server.tool({
  name: "get_stations",
  description: "Retrieves station list",
  inputSchema: {
    type: "object",
    properties: { /* ... */ },
    required: [/* ... */]
  }
}, async (input) => {
  const data = await client.get('/jn');
  return { content: [{ type: "text", text: JSON.stringify(data) }] };
});
```

**Struktur einer Tool-Definition:**
- `name` — eindeutiger Identifier
- `description` — was das Tool macht
- `inputSchema` — JSON-Schema der Input-Parameter
- Callback-Funktion — die eigentliche Logik

**Fehlerbehandlung:**

```typescript
try {
  const data = await client.get('/ja');
  if (data.result !== 1) {
    return { content: [{ type: "text", text: `Error: ${data.result}` }] };
  }
  return { content: [{ type: "text", text: JSON.stringify(data) }] };
} catch (error) {
  return { content: [{ type: "text", text: `HTTP Error: ${error.message}` }] };
}
```

### 4. resources.ts — Ressourcen

Ressourcen sind Read-only Informationen für den KI-Assistant:

```typescript
server.resource({
  uri: "opensprinkler://api-overview",
  name: "API Overview",
  description: "Available endpoints and error codes",
  mimeType: "text/markdown"
}, async () => {
  return {
    contents: [{ uri: "opensprinkler://api-overview", mimeType: "text/markdown", text: "..." }]
  };
});
```

**Beispiele:**
- API-Dokumentation
- Fehlercodes-Tabelle
- Sensortyp-Liste
- Live-Controller-Status

## Ein neues Tool hinzufügen

### Schritt 1: Tool-Definition in tools.ts

```typescript
server.tool({
  name: "my_new_tool",
  description: "Beschreibung auf Englisch",
  inputSchema: {
    type: "object",
    properties: {
      param1: { type: "string", description: "First parameter" },
      param2: { type: "number", description: "Second parameter" }
    },
    required: ["param1"]
  }
}, async (input) => {
  // 1. Input validieren
  if (!input.param1) {
    return { content: [{ type: "text", text: "Error: param1 required" }] };
  }

  // 2. OpenSprinkler API aufrufen
  try {
    const result = await client.get('/api-endpoint', {
      key: input.param1,
      val: input.param2
    });

    // 3. Ergebnis überprüfen
    if (result.result !== 1) {
      return { content: [{ type: "text", text: `API returned: ${result.result}` }] };
    }

    // 4. Erfolgreich? Daten zurückgeben
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  } catch (error) {
    return { content: [{ type: "text", text: `Error: ${error.message}` }] };
  }
});
```

### Schritt 2: Build & Test

```bash
npm run build
OS_BASE_URL=http://192.168.0.86 OS_PASSWORD_HASH=... node dist/index.js
```

Teste mit Claude/Copilot:
> "Use the my_new_tool to..."

## OpenSprinkler API-Endpunkte

Wichtigste Endpunkte (alle brauchen `?pw=<hash>`):

| Endpunkt | Methode | Beschreibung | Beispiel |
|----------|---------|-------------|---------|
| `/ja` | GET | Alle Daten | `client.get('/ja')` |
| `/jo` | GET | Optionen | `client.get('/jo')` |
| `/jn` | GET | Stationsnamen | `client.get('/jn')` |
| `/js` | GET | Station-Status | `client.get('/js')` |
| `/jp` | GET | Programme | `client.get('/jp')` |
| `/db` | GET | Debug-Info | `client.get('/db')` |
| `/sl` | GET | Sensoren | `client.get('/sl')` |
| `/zg` | GET | Zigbee-Geräte | `client.get('/zg')` |
| `/cv` | GET | Controller-Var ändern | `client.get('/cv', {sd: 0})` |
| `/cm` | GET | Station manuell | `client.get('/cm', {sid: 0, en: 1, t: 300})` |
| `/cp` | GET | Programm ändern | `client.get('/cp', {...})` |

## Datentypen

### Controller-Variablen (`/jc`)
```typescript
{
  "devt": 1771683933,      // Device time (Unix)
  "nbrd": 1,               // Number of boards
  "en": 1,                 // Enabled
  "rd": 0,                 // Rain delay (hours)
  "rssi": -69,             // WiFi signal strength
  "otcs": 3                // OTF connection status
}
```

### Optionen (`/jo`)
```typescript
{
  "fwv": 233,              // Firmware version
  "model": 184,            // Hardware model
  "hwv": 2,                // Hardware version
  "name": "OpenSprinkler",
  "loc": "49.2,8.5",       // Location (lat,lon)
  "wto": { ... }           // Weather settings
}
```

### Station Status (`/js`)
```typescript
{
  "sn": [0, 0, 0, 0, ...], // Array: 0=off, 1=on für jede Station
  "nstations": 8           // Anzahl Stationen
}
```

### Sensoren (`/sl`)
```typescript
{
  "sensors": [
    {
      "nr": 10,
      "type": 1,           // Sensor-Typ
      "name": "SMT100 Feucht",
      "data": 17.27,       // Aktueller Wert
      "data_ok": 1,        // 1=gültig, 0=veraltet
      "unit": "%"
    },
    // ...
  ]
}
```

## TypeScript-Tipps

### Strict Mode

```json
{
  "compilerOptions": {
    "strict": true,
    "noImplicitAny": true,
    "strictNullChecks": true
  }
}
```

### Async/Await

```typescript
// ✅ Gut
async function doSomething() {
  try {
    const data = await client.get('/ja');
    return data;
  } catch (error) {
    console.error('Failed:', error);
  }
}

// ❌ Schlecht
function doSomething() {
  return client.get('/ja').then(data => data);
}
```

### Error-Handling

```typescript
if (data?.result !== 1) {
  return { content: [{ type: "text", text: `Error code: ${data?.result}` }] };
}
```

## Testing & Debugging

### Logs aktivieren

```bash
DEBUG=* npm start
```

### Mit curl testen

```bash
# Ersetze mit deinen Werten:
DEVICE_IP="192.168.0.86"
ADMIN_HASH="your_md5_hash_here"

# Get stations
curl "http://${DEVICE_IP}/jn?pw=${ADMIN_HASH}"

# Manual station
curl "http://${DEVICE_IP}/cm?pw=${ADMIN_HASH}&sid=0&en=1&t=300"
```

### VS Code Debugging

Starte mit Inspector:
```bash
node --inspect dist/index.js
```

Dann in Chrome: `chrome://inspect`

## Performance

### Caching

Der Client cachert nichts — jede Anfrage geht zum Controller.
Falls nötig: LocalStorage in client.ts nutzen.

```typescript
private cache = new Map<string, { data: unknown; time: number }>();

async getCached(key: string, maxAge: number) {
  const cached = this.cache.get(key);
  if (cached && Date.now() - cached.time < maxAge) {
    return cached.data;
  }
  // Frische Daten holen...
}
```

### Timeout

Standard ist 15 Sekunden — ändern in client.ts:
```typescript
signal: AbortSignal.timeout(30_000)  // 30 Sekunden
```

## Dependencies

Im `package.json`:
- `@modelcontextprotocol/sdk` — MCP Framework
- `typescript` — TypeScript-Compiler
- `node` — Runtime

Keine anderen Dependencies! Client ist von Grund auf mit `fetch` geschrieben.

## OpenSprinkler MCP Server — Sicherheit

### Passwort-Hash

- Der `OS_PASSWORD_HASH` ist das MD5-Hash des **Admin-Passworts** (nicht User-Passwort)
- Er wird als Umgebungsvariable gespeichert und nur lokal verarbeitet
- Niemals in öffentlichen Repositories committen
- Bei lokaler Verwendung: `echo -n "passwort" | md5sum` im Terminal berechnen

### Lokale vs. Remote

- **Lokal (empfohlen)**: MCP Server läuft auf deinem PC, verbindet sich übers Heimnetzwerk mit ESP32
- **Remote (nicht empfohlen)**: Den ESP32 nicht direkt ins Internet freigeben → VPN verwenden

## Lizenz

GPL-3.0 — wie das OpenSprinkler Firmware Projekt.

## Kontakt & Support

- Issues: https://github.com/OpenSprinkler/OpenSprinkler-Firmware/issues
- OpenSprinkler Forum: https://opensprinkler.com/forums/
- GitHub Discussions

