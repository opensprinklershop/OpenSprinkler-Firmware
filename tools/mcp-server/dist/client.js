import { createHash } from "crypto";
/** Hashes a plaintext password to MD5 hex (OpenSprinkler convention). */
export function md5Hash(plain) {
    return createHash("md5").update(plain).digest("hex");
}
/**
 * Endpoints that only exist on specific firmware variants. A 404 on one of them
 * means the controller lacks the feature (e.g. ESP8266 / OS 3.x has no radio
 * stack and no on-device MCP), not that the request was malformed.
 */
const PLATFORM_ENDPOINTS = {
    "/ir": "IEEE 802.15.4 radio mode (ESP32-C5 only)",
    "/iw": "IEEE 802.15.4 radio mode (ESP32-C5 only)",
    "/zg": "Zigbee gateway (ESP32-C5 Zigbee variant only)",
    "/zd": "Zigbee discovery (ESP32-C5 Zigbee variant only)",
    "/zj": "Zigbee pairing (ESP32-C5 Zigbee variant only)",
    "/zs": "Zigbee status (ESP32-C5 Zigbee variant only)",
    "/zo": "Zigbee on/off (ESP32-C5 Zigbee variant only)",
    "/zc": "Zigbee coordinator (ESP32-C5 Zigbee variant only)",
    "/zl": "Zigbee logical devices (ESP32-C5 Zigbee variant only)",
    "/bd": "BLE devices (ESP32 only)",
    "/rk": "RainMaker (ESP32 only)",
    "/rp": "RainMaker (ESP32 only)",
    "/mcp": "on-device MCP endpoint (ESP32 only; use this external server on ESP8266/OSPi)",
};
function httpErrorMessage(status, statusText, path) {
    const base = `HTTP ${status} ${statusText} – ${path}`;
    if (status === 404) {
        const feature = PLATFORM_ENDPOINTS[path];
        if (feature) {
            return `${base}: ${feature}. This controller (ESP8266/OS 3.x, OSPi or another firmware variant) does not provide this endpoint; check get_options (fwv/fwm, feature) or get_debug for the platform.`;
        }
        return `${base}: endpoint not available on this controller's firmware version or platform.`;
    }
    return base;
}
/**
 * Lightweight HTTP client for the OpenSprinkler REST API.
 *
 * Every request automatically appends the `pw` parameter.
 */
export class OpenSprinklerClient {
    baseUrl;
    pwHash;
    constructor(config) {
        this.baseUrl = config.baseUrl.replace(/\/+$/, "");
        this.pwHash = config.passwordIsHash
            ? config.password
            : md5Hash(config.password);
    }
    /** Generic GET request. Returns parsed JSON. */
    async get(path, params = {}) {
        const url = new URL(path, this.baseUrl);
        url.searchParams.set("pw", this.pwHash);
        for (const [k, v] of Object.entries(params)) {
            if (v !== undefined)
                url.searchParams.set(k, String(v));
        }
        const res = await fetch(url.toString(), {
            signal: AbortSignal.timeout(15_000),
        });
        if (!res.ok) {
            throw new Error(httpErrorMessage(res.status, res.statusText, url.pathname));
        }
        const text = await res.text();
        try {
            return JSON.parse(text);
        }
        catch {
            throw new Error(`Non-JSON response from ${url.pathname}: ${text.slice(0, 200)}`);
        }
    }
    /** GET request that returns raw text (useful for CSV, plain text, etc.). */
    async getRaw(path, params = {}) {
        const url = new URL(path, this.baseUrl);
        url.searchParams.set("pw", this.pwHash);
        for (const [k, v] of Object.entries(params)) {
            if (v !== undefined)
                url.searchParams.set(k, String(v));
        }
        const res = await fetch(url.toString(), {
            signal: AbortSignal.timeout(15_000),
        });
        if (!res.ok) {
            throw new Error(httpErrorMessage(res.status, res.statusText, url.pathname));
        }
        return res.text();
    }
    /** Convenience: call GET and check for `{"result":1}` success. */
    async command(path, params = {}) {
        return this.get(path, params);
    }
}
//# sourceMappingURL=client.js.map