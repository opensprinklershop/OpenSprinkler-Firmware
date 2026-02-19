import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { OpenSprinklerClient } from "./client.js";
/**
 * Register MCP resources that provide reference documentation to the LLM.
 */
export declare function registerResources(server: McpServer, getClient: () => OpenSprinklerClient): void;
