/** Hashes a plaintext password to MD5 hex (OpenSprinkler convention). */
export declare function md5Hash(plain: string): string;
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
export declare class OpenSprinklerClient {
    private baseUrl;
    private pwHash;
    constructor(config: OpenSprinklerClientConfig);
    /** Generic GET request. Returns parsed JSON. */
    get<T = Record<string, unknown>>(path: string, params?: Record<string, string | number | undefined>): Promise<T>;
    /** Convenience: call GET and check for `{"result":1}` success. */
    command(path: string, params?: Record<string, string | number | undefined>): Promise<{
        result: number;
        [key: string]: unknown;
    }>;
}
