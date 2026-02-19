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

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { OpenSprinklerClient } from "./client.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";

function createClient(): OpenSprinklerClient {
  const baseUrl = process.env.OS_BASE_URL ?? "http://localhost:8080";
  const passwordHash = process.env.OS_PASSWORD_HASH;
  const password = process.env.OS_PASSWORD;

  if (!passwordHash && !password) {
    throw new Error(
      "Set OS_PASSWORD (plaintext) or OS_PASSWORD_HASH (MD5) environment variable.",
    );
  }

  return new OpenSprinklerClient({
    baseUrl,
    password: (passwordHash ?? password)!,
    passwordIsHash: !!passwordHash,
  });
}

async function main() {
  const client = createClient();

  const server = new McpServer({
    name: "opensprinkler",
    version: "1.0.0",
  });

  registerTools(server, () => client);
  registerResources(server, () => client);

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("Fatal:", err);
  process.exit(1);
});
