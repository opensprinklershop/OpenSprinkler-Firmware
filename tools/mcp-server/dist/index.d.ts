#!/usr/bin/env node
/**
 * OpenSprinkler MCP Server
 *
 * Exposes the OpenSprinkler HTTP API as MCP (Model Context Protocol) tools
 * so that AI assistants can query and control irrigation controllers.
 *
 * Configuration via environment variables:
 *   OS_BASE_URL   – Controller URL  (default: http://localhost:8080)
 *   OS_PASSWORD    – Device password in plaintext
 *   OS_PASSWORD_HASH – Device password already MD5-hashed (alternative to OS_PASSWORD)
 */
export {};
