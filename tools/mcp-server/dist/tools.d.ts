import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { OpenSprinklerClient } from "./client.js";
/**
 * Register all OpenSprinkler tools on the given MCP server instance.
 */
export declare function registerTools(server: McpServer, getClient: () => OpenSprinklerClient): void;
