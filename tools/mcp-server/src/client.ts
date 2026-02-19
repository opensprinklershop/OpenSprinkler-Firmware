import { createHash } from "crypto";

/** Hashes a plaintext password to MD5 hex (OpenSprinkler convention). */
export function md5Hash(plain: string): string {
  return createHash("md5").update(plain).digest("hex");
}

export interface OpenSprinklerClientConfig {
  /** Base URL of the controller, e.g. "http://192.168.1.100" */
  baseUrl: string;
  /** Password – either already an MD5 hash or plaintext (hashed automatically). */
  password: string;
  /** If true, `password` is already an MD5 hash. Default: false (plaintext). */
  passwordIsHash?: boolean;
}

/**
 * Lightweight HTTP client for the OpenSprinkler REST API.
 *
 * Every request automatically appends the `pw` parameter.
 */
export class OpenSprinklerClient {
  private baseUrl: string;
  private pwHash: string;

  constructor(config: OpenSprinklerClientConfig) {
    this.baseUrl = config.baseUrl.replace(/\/+$/, "");
    this.pwHash = config.passwordIsHash
      ? config.password
      : md5Hash(config.password);
  }

  /** Generic GET request. Returns parsed JSON. */
  async get<T = Record<string, unknown>>(
    path: string,
    params: Record<string, string | number | undefined> = {},
  ): Promise<T> {
    const url = new URL(path, this.baseUrl);
    url.searchParams.set("pw", this.pwHash);
    for (const [k, v] of Object.entries(params)) {
      if (v !== undefined) url.searchParams.set(k, String(v));
    }

    const res = await fetch(url.toString(), {
      signal: AbortSignal.timeout(15_000),
    });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status} ${res.statusText} – ${url.pathname}`);
    }
    const text = await res.text();
    try {
      return JSON.parse(text) as T;
    } catch {
      throw new Error(`Non-JSON response from ${url.pathname}: ${text.slice(0, 200)}`);
    }
  }

  /** Convenience: call GET and check for `{"result":1}` success. */
  async command(
    path: string,
    params: Record<string, string | number | undefined> = {},
  ): Promise<{ result: number; [key: string]: unknown }> {
    return this.get(path, params);
  }
}
