/* OpenSprinkler Unified Firmware
 * MCP (Model Context Protocol) Server — built-in HTTP endpoint /mcp
 *
 * ESP32 only. Requires USE_OTF.
 *
 * Implements the MCP Streamable-HTTP transport (JSON-RPC 2.0).
 *
 * Authentication (same MD5 password hash as the regular REST API):
 *   - ?pw=<md5>              – query parameter  (same as /ja, /jc, …)
 *   - X-OS-Password: <md5>   – HTTP request header
 *   - Authorization: Bearer <md5> – HTTP Bearer token
 *
 * Endpoint:
 *   POST /mcp   Content-Type: application/json
 *
 * Supported JSON-RPC methods:
 *   initialize          – MCP handshake
 *   ping                – liveness check
 *   tools/list          – enumerate available tools
 *   tools/call          – invoke a tool by name
 *
 * Available tools (mirror the external Node.js MCP server):
 *   get_all                    – /ja
 *   get_controller_variables   – /jc
 *   get_options                – /jo
 *   get_stations               – /jn
 *   get_station_status         – /js
 *   get_programs               – /jp
 *   get_debug                  – /db
 *   manual_station_run         – /cm  (sid, en, t, qo)
 *   change_controller_variables– /cv  (en, rd, rsn, rrsn, rbt)
 *   pause_queue                – /pq  (dur, repl)
 */

#pragma once

#if defined(ESP32) && defined(USE_OTF)

namespace OTF {
  class Request;
  class Response;
}

/**
 * Handle an HTTP POST /mcp request.
 * Registered in start_server_client() via:
 *   otf->on("/mcp", server_mcp_handler, OTF::OTF_HTTP_POST);
 */
void server_mcp_handler(const OTF::Request& req, OTF::Response& res);

#endif // ESP32 && USE_OTF
