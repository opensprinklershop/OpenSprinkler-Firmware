/* OpenSprinkler Unified Firmware
 * MCP (Model Context Protocol) Server — built-in HTTP endpoint /mcp
 * ESP32 only. Requires USE_OTF.
 *
 * See mcp_server.h for full documentation.
 */

// defines.h must be included BEFORE the USE_OTF guard so that the macro
// is defined by the time the preprocessor evaluates the #if below.
#include "defines.h"

#if defined(ESP32) && defined(USE_OTF)

#include "mcp_server.h"
#include "OpenSprinkler.h"
#include "program.h"
#include "opensprinkler_server.h"
#include "ArduinoJson.hpp"
#include "psram_utils.h"
#include <LittleFS.h>
#include <WiFi.h>

// ─── External state ─────────────────────────────────────────────────────────

extern OpenSprinkler os;
extern ProgramData pd;
extern BufferFiller bfill;
extern bool useEth;
extern OTF::OpenThingsFramework *otf;
extern ulong flow_count;

// Capture-mode globals (defined in opensprinkler_server.cpp)
extern bool   g_mcp_capture_active;
extern String g_mcp_capture_buf;

// Helpers from opensprinkler_server.cpp
void rewind_ether_buffer();

// Data-building helpers (defined in opensprinkler_server.cpp)
void server_json_controller_main(const OTF::Request& req, OTF::Response& res);
void server_json_options_main();
void server_json_status_main();
void server_json_stations_main(const OTF::Request& req, OTF::Response& res);
void server_json_programs_main(const OTF::Request& req, OTF::Response& res);

// Action helpers from opensprinkler_server.cpp / main.cpp
extern void schedule_all_stations(unsigned long curr_time, unsigned char req_option);
extern void turn_off_station(unsigned char sid, unsigned long curr_time, unsigned char shift);
extern void reset_all_stations(bool running_only);
extern uint32_t reboot_timer;

// ─── Constants ───────────────────────────────────────────────────────────────

#define MCP_PROTOCOL_VERSION  "2024-11-05"
#define MCP_SERVER_NAME       "opensprinkler"
#define MCP_SERVER_VERSION    "1.0.0"

// JSON-RPC 2.0 error codes
#define RPC_ERR_PARSE           -32700
#define RPC_ERR_INVALID_REQ     -32600
#define RPC_ERR_METHOD_NOT_FOUND -32601
#define RPC_ERR_INVALID_PARAMS  -32602
#define RPC_ERR_INTERNAL        -32603

// ─── Authentication ──────────────────────────────────────────────────────────

static bool mcp_check_auth(const OTF::Request& req) {
  if (os.iopts[IOPT_IGNORE_PASSWORD]) return true;

  // 1. Legacy query param  ?pw=<md5>
  const char* pw = req.getQueryParameter("pw");
  if (pw && os.password_verify(pw)) return true;

  // 2. Custom header  X-OS-Password: <md5>
  const char* h_pw = req.getHeader("X-OS-Password");
  if (h_pw && os.password_verify(h_pw)) return true;

  // 3. Bearer token  Authorization: Bearer <md5>
  const char* auth = req.getHeader("Authorization");
  if (auth && strncmp(auth, "Bearer ", 7) == 0 && os.password_verify(auth + 7)) return true;

  return false;
}

// ─── Capture helpers ─────────────────────────────────────────────────────────

// Begin capturing ether_buffer writes into g_mcp_capture_buf.
// Writes `open_brace` literal to ether_buffer before calling the supplied
// function so the caller does not need to manage it.
static void mcp_begin_capture() {
  g_mcp_capture_active = true;
  g_mcp_capture_buf.clear();
  rewind_ether_buffer();
}

// Flush remaining ether_buffer content into g_mcp_capture_buf and stop capture.
static String mcp_end_capture() {
  // Append whatever is still in ether_buffer
  unsigned int remaining = (unsigned int)bfill.position();
  if (remaining > 0) {
    g_mcp_capture_buf.concat(ether_buffer, remaining);
  }
  g_mcp_capture_active = false;
  String result = std::move(g_mcp_capture_buf);
  g_mcp_capture_buf.clear();
  return result;
}

// Flush current ether_buffer segment (used between sections of get_all).
static void mcp_flush_segment() {
  unsigned int len = (unsigned int)bfill.position();
  if (len > 0) {
    g_mcp_capture_buf.concat(ether_buffer, len);
  }
  rewind_ether_buffer();
}

// ─── JSON-RPC response helpers ────────────────────────────────────────────────

// Write a complete JSON-RPC response (result or error) to res.
static void mcp_send_response(OTF::Response& res,
                              const ArduinoJson::JsonDocument& doc) {
  String body;
  ArduinoJson::serializeJson(doc, body);

  res.writeStatus(200);
  res.writeHeader(F("Content-Type"), F("application/json"));
  res.writeHeader(F("Access-Control-Allow-Origin"), F("*"));
  res.writeHeader(F("Cache-Control"), F("no-store"));
  res.writeHeader(F("Content-Length"), (int)body.length());
  res.writeBodyData(body.c_str(), body.length());
}

// Build the JSON-RPC error response document.
static void mcp_build_error(ArduinoJson::JsonDocument& doc,
                            ArduinoJson::JsonVariantConst id,
                            int code, const char* message) {
  doc["jsonrpc"] = "2.0";
  if (!id.isNull()) doc["id"] = id;
  else              doc["id"] = (const char*)nullptr;
  auto err = doc["error"].to<ArduinoJson::JsonObject>();
  err["code"]    = code;
  err["message"] = message;
}

// Build the JSON-RPC success response with a single text content item.
static void mcp_build_text_result(ArduinoJson::JsonDocument& doc,
                                  ArduinoJson::JsonVariantConst id,
                                  const String& text) {
  doc["jsonrpc"] = "2.0";
  if (!id.isNull()) doc["id"] = id;
  else              doc["id"] = (const char*)nullptr;
  auto result  = doc["result"].to<ArduinoJson::JsonObject>();
  auto content = result["content"].to<ArduinoJson::JsonArray>();
  auto item    = content.add<ArduinoJson::JsonObject>();
  item["type"] = "text";
  item["text"] = text;
}

// Build the JSON-RPC success response with a numeric result code (for actions).
static void mcp_build_code_result(ArduinoJson::JsonDocument& doc,
                                  ArduinoJson::JsonVariantConst id,
                                  int result_code) {
  char buf[32];
  snprintf(buf, sizeof(buf), "{\"result\":%d}", result_code);
  mcp_build_text_result(doc, id, String(buf));
}

// ─── Tool: get_all ────────────────────────────────────────────────────────────

static String tool_get_all(const OTF::Request& req, OTF::Response& res) {
  mcp_begin_capture();

  // settings section (server_json_controller_main closes its own "{")
  bfill.emit_p(PSTR("{\"settings\":{"));
  server_json_controller_main(req, res);
  mcp_flush_segment();

  // programs section
  bfill.emit_p(PSTR(",\"programs\":{"));
  server_json_programs_main(req, res);
  mcp_flush_segment();

  // options section (no OTF params needed)
  bfill.emit_p(PSTR(",\"options\":{"));
  server_json_options_main();
  mcp_flush_segment();

  // status section
  bfill.emit_p(PSTR(",\"status\":{"));
  server_json_status_main();
  mcp_flush_segment();

  // stations section + close outer {}
  bfill.emit_p(PSTR(",\"stations\":{"));
  server_json_stations_main(req, res);
  bfill.emit_p(PSTR("}"));  // close outer object

  return mcp_end_capture();
}

// ─── Tool: get_controller_variables ──────────────────────────────────────────

static String tool_get_controller_variables(const OTF::Request& req, OTF::Response& res) {
  mcp_begin_capture();
  bfill.emit_p(PSTR("{"));
  server_json_controller_main(req, res);
  return mcp_end_capture();
}

// ─── Tool: get_options ───────────────────────────────────────────────────────

static String tool_get_options(const OTF::Request& /*req*/, OTF::Response& /*res*/) {
  mcp_begin_capture();
  bfill.emit_p(PSTR("{"));
  server_json_options_main();
  return mcp_end_capture();
}

// ─── Tool: get_stations ──────────────────────────────────────────────────────

static String tool_get_stations(const OTF::Request& req, OTF::Response& res) {
  mcp_begin_capture();
  bfill.emit_p(PSTR("{"));
  server_json_stations_main(req, res);
  return mcp_end_capture();
}

// ─── Tool: get_station_status ─────────────────────────────────────────────────

static String tool_get_station_status(const OTF::Request& /*req*/, OTF::Response& /*res*/) {
  mcp_begin_capture();
  bfill.emit_p(PSTR("{"));
  server_json_status_main();
  return mcp_end_capture();
}

// ─── Tool: get_programs ───────────────────────────────────────────────────────

static String tool_get_programs(const OTF::Request& req, OTF::Response& res) {
  mcp_begin_capture();
  bfill.emit_p(PSTR("{"));
  server_json_programs_main(req, res);
  return mcp_end_capture();
}

// ─── Tool: get_debug ─────────────────────────────────────────────────────────

static String tool_get_debug(const OTF::Request& /*req*/, OTF::Response& /*res*/) {
  ArduinoJson::JsonDocument doc;
  auto obj = doc.to<ArduinoJson::JsonObject>();
  obj["date"]  = __DATE__;
  obj["time"]  = __TIME__;
  obj["heap"]  = (unsigned long)ESP.getFreeHeap();
  obj["flash"] = (unsigned long)LittleFS.totalBytes();
  obj["used"]  = (unsigned long)LittleFS.usedBytes();
  if (useEth) {
    obj["ETH"] = 1;
  } else {
    obj["rssi"] = (int)WiFi.RSSI();
    obj["bssid"] = WiFi.BSSIDstr().c_str();
  }
#if defined(BOARD_HAS_PSRAM)
  obj["psram_free"] = (unsigned long)ESP.getFreePsram();
  obj["psram_total"] = (unsigned long)ESP.getPsramSize();
#endif
  String out;
  ArduinoJson::serializeJson(doc, out);
  return out;
}

// ─── Tool: manual_station_run ────────────────────────────────────────────────

static int tool_manual_station_run(const ArduinoJson::JsonObjectConst& args) {
  if (!args.containsKey("sid") || !args.containsKey("en")) return 16; // HTML_DATA_MISSING

  int sid = args["sid"].as<int>();
  int en  = args["en"].as<int>();

  if (sid < 0 || sid >= os.nstations) return 17; // HTML_DATA_OUTOFBOUND

  unsigned long curr_time = os.now_tz();

  if (en) {
    if (!args.containsKey("t")) return 16; // timer required
    int timer = args["t"].as<int>();
    if (timer <= 0 || timer > 64800) return 17;

    // Refuse to schedule master stations
    if ((os.status.mas == (unsigned char)(sid + 1)) ||
        (os.status.mas2 == (unsigned char)(sid + 1))) return 48; // HTML_NOT_PERMITTED

    unsigned char qo = args["qo"] | (unsigned char)0;

    RuntimeQueueStruct *q = nullptr;
    unsigned char sqi = pd.station_qid[sid];
    if (sqi == 0xFF) {
      q = pd.enqueue();
    }
    if (!q) return 48;

    q->st  = 0;
    q->dur = (uint16_t)timer;
    q->sid = (unsigned char)sid;
    q->pid = 99; // manual = pid 99
    schedule_all_stations(curr_time, qo);

  } else {
    // Turn off station
    if (pd.station_qid[sid] == 255) return 17; // not running
    RuntimeQueueStruct *q = pd.queue + pd.station_qid[sid];
    q->deque_time = curr_time;
    unsigned char ssta = args["ssta"] | (unsigned char)0;
    turn_off_station((unsigned char)sid, curr_time, ssta);
  }
  return 1; // HTML_SUCCESS
}

// ─── Tool: change_controller_variables ───────────────────────────────────────

static int tool_change_controller_variables(const ArduinoJson::JsonObjectConst& args) {
  if (args.containsKey("rsn") && args["rsn"].as<int>() > 0) {
    reset_all_stations(false);
  }
  if (args.containsKey("rrsn") && args["rrsn"].as<int>() > 0) {
    reset_all_stations(true);
  }
  if (args.containsKey("en")) {
    os.status.enabled = args["en"].as<int>() ? 1 : 0;
    os.iopts[IOPT_DEVICE_ENABLE] = os.status.enabled;
    os.iopts_save();
  }
  if (args.containsKey("rd")) {
    int rd = args["rd"].as<int>();
    if (rd > 0) {
      os.nvdata.rd_stop_time = os.now_tz() + (unsigned long)rd * 3600UL;
      os.raindelay_start();
    } else if (rd == 0) {
      os.raindelay_stop();
    }
  }
  if (args.containsKey("rbt") && args["rbt"].as<int>() > 0) {
    reboot_timer = 1000; // reboot in 1 second
  }
  return 1; // HTML_SUCCESS
}

// ─── Tool: pause_queue ────────────────────────────────────────────────────────

static int tool_pause_queue(const ArduinoJson::JsonObjectConst& args) {
  ulong duration = 0;
  if (args.containsKey("repl")) {
    duration = (ulong)args["repl"].as<long>();
    pd.toggle_pause(duration);
    return 1;
  }
  if (args.containsKey("dur")) {
    duration = (ulong)args["dur"].as<long>();
  }
  pd.toggle_pause(duration);
  return 1; // HTML_SUCCESS
}

// ─── tools/list ─────────────────────────────────────────────────────────────

// Build the "inputSchema" for a tool with no parameters.
static void add_empty_schema(ArduinoJson::JsonObject& tool) {
  auto schema = tool["inputSchema"].to<ArduinoJson::JsonObject>();
  schema["type"] = "object";
  schema["properties"].to<ArduinoJson::JsonObject>();
}

// Build the tools list result.
static void build_tools_list(ArduinoJson::JsonObject& result) {
  auto tools = result["tools"].to<ArduinoJson::JsonArray>();

  // Helper lambda to add a read-only tool with no parameters.
  auto add_ro = [&](const char* name, const char* desc) {
    auto t = tools.add<ArduinoJson::JsonObject>();
    t["name"]        = name;
    t["description"] = desc;
    add_empty_schema(t);
  };

  add_ro("get_all",
    "Get all OpenSprinkler data: settings, programs, options, status, stations. Equivalent to /ja.");
  add_ro("get_controller_variables",
    "Get controller variables: time, rain delay, station states, queue, MQTT/OTC status. Equivalent to /jc.");
  add_ro("get_options",
    "Get all controller options: firmware version, timezone, sensor config, master stations, water level. Equivalent to /jo.");
  add_ro("get_stations",
    "Get station names and attributes (master ops, ignore-rain, disable, group IDs). Equivalent to /jn.");
  add_ro("get_station_status",
    "Get current on/off state of all stations. Equivalent to /js.");
  add_ro("get_programs",
    "Get all irrigation programs (schedules, durations, names). Equivalent to /jp.");
  add_ro("get_debug",
    "Get firmware build date/time, free heap, flash usage, WiFi RSSI. Equivalent to /db.");

  // manual_station_run
  {
    auto t = tools.add<ArduinoJson::JsonObject>();
    t["name"]        = "manual_station_run";
    t["description"] = "Manually open or close a single station. Equivalent to /cm.";
    auto schema = t["inputSchema"].to<ArduinoJson::JsonObject>();
    schema["type"] = "object";
    auto props = schema["properties"].to<ArduinoJson::JsonObject>();
    auto sid = props["sid"].to<ArduinoJson::JsonObject>();
    sid["type"] = "integer"; sid["description"] = "Station index (0-based)";
    auto en = props["en"].to<ArduinoJson::JsonObject>();
    en["type"] = "integer"; en["description"] = "1 = open station, 0 = close station";
    auto ti = props["t"].to<ArduinoJson::JsonObject>();
    ti["type"] = "integer"; ti["description"] = "Duration in seconds when opening (1–64800)";
    auto qo = props["qo"].to<ArduinoJson::JsonObject>();
    qo["type"] = "integer"; qo["description"] = "Queue option: 0 = append (default), 1 = insert front";
    auto ssta = props["ssta"].to<ArduinoJson::JsonObject>();
    ssta["type"] = "integer"; ssta["description"] = "Shift remaining stations when closing (1 = shift)";
    auto req_arr = schema["required"].to<ArduinoJson::JsonArray>();
    req_arr.add("sid"); req_arr.add("en");
  }

  // change_controller_variables
  {
    auto t = tools.add<ArduinoJson::JsonObject>();
    t["name"]        = "change_controller_variables";
    t["description"] = "Change controller state: enable/disable, rain delay, reset stations, reboot. Equivalent to /cv.";
    auto schema = t["inputSchema"].to<ArduinoJson::JsonObject>();
    schema["type"] = "object";
    auto props = schema["properties"].to<ArduinoJson::JsonObject>();
    auto en = props["en"].to<ArduinoJson::JsonObject>();
    en["type"] = "integer"; en["description"] = "Enable (1) or disable (0) irrigation";
    auto rd = props["rd"].to<ArduinoJson::JsonObject>();
    rd["type"] = "integer"; rd["description"] = "Rain delay in hours (0 = cancel)";
    auto rsn = props["rsn"].to<ArduinoJson::JsonObject>();
    rsn["type"] = "integer"; rsn["description"] = "Reset all stations (1)";
    auto rrsn = props["rrsn"].to<ArduinoJson::JsonObject>();
    rrsn["type"] = "integer"; rrsn["description"] = "Reset running stations only (1)";
    auto rbt = props["rbt"].to<ArduinoJson::JsonObject>();
    rbt["type"] = "integer"; rbt["description"] = "Reboot controller (1)";
    schema["properties"] = props;
  }

  // pause_queue
  {
    auto t = tools.add<ArduinoJson::JsonObject>();
    t["name"]        = "pause_queue";
    t["description"] = "Pause or resume the program queue. Equivalent to /pq.";
    auto schema = t["inputSchema"].to<ArduinoJson::JsonObject>();
    schema["type"] = "object";
    auto props = schema["properties"].to<ArduinoJson::JsonObject>();
    auto dur = props["dur"].to<ArduinoJson::JsonObject>();
    dur["type"] = "integer"; dur["description"] = "Toggle-style: pause for this many seconds (0 = cancel)";
    auto repl = props["repl"].to<ArduinoJson::JsonObject>();
    repl["type"] = "integer"; repl["description"] = "Set/replace pause duration in seconds (0 = cancel)";
  }
}

// ─── Main handler ─────────────────────────────────────────────────────────────

void server_mcp_handler(const OTF::Request& req, OTF::Response& res) {
  ArduinoJson::JsonDocument resp_doc;

  // ── CORS pre-flight ──────────────────────────────────────────────────────
  // OTF registers this handler for POST only, but browsers send OPTIONS first.
  // If OTF ever calls us for OPTIONS (via OTF_HTTP_ANY registration), handle it.
  {
    const char* method_hdr = req.getHeader("Access-Control-Request-Method");
    if (method_hdr) {
      res.writeStatus(204);
      res.writeHeader(F("Access-Control-Allow-Origin"),  F("*"));
      res.writeHeader(F("Access-Control-Allow-Methods"), F("POST, OPTIONS"));
      res.writeHeader(F("Access-Control-Allow-Headers"),
                      F("Content-Type, X-OS-Password, Authorization"));
      res.writeHeader(F("Access-Control-Max-Age"), F("86400"));
      res.writeBodyData("", 0);
      return;
    }
  }

  // ── Authentication ───────────────────────────────────────────────────────
  if (!mcp_check_auth(req)) {
    mcp_build_error(resp_doc, ArduinoJson::JsonVariantConst{}, -32600,
                    "Unauthorized – supply ?pw=<md5> or X-OS-Password / Authorization: Bearer header");
    res.writeStatus(401);
    res.writeHeader(F("Content-Type"), F("application/json"));
    res.writeHeader(F("Access-Control-Allow-Origin"), F("*"));
    String body;
    ArduinoJson::serializeJson(resp_doc, body);
    res.writeHeader(F("Content-Length"), (int)body.length());
    res.writeBodyData(body.c_str(), body.length());
    return;
  }

  // ── Parse JSON-RPC body ──────────────────────────────────────────────────
  const char* body_raw   = req.getBody();
  size_t      body_len   = req.getBodyLength();

  if (!body_raw || body_len == 0) {
    mcp_build_error(resp_doc, ArduinoJson::JsonVariantConst{}, RPC_ERR_INVALID_REQ, "Empty request body");
    mcp_send_response(res, resp_doc);
    return;
  }

  ArduinoJson::JsonDocument req_doc;
  ArduinoJson::DeserializationError parse_err =
      ArduinoJson::deserializeJson(req_doc, body_raw, body_len);

  if (parse_err) {
    mcp_build_error(resp_doc, ArduinoJson::JsonVariantConst{}, RPC_ERR_PARSE, "JSON parse error");
    mcp_send_response(res, resp_doc);
    return;
  }

  // Must be a JSON object
  if (!req_doc.is<ArduinoJson::JsonObject>()) {
    mcp_build_error(resp_doc, ArduinoJson::JsonVariantConst{}, RPC_ERR_INVALID_REQ, "Expected JSON object");
    mcp_send_response(res, resp_doc);
    return;
  }
  auto rpc_req = req_doc.as<ArduinoJson::JsonObjectConst>();

  // Extract id (may be null/missing)
  ArduinoJson::JsonVariantConst rpc_id = rpc_req["id"];

  // Validate jsonrpc field
  const char* jsonrpc = rpc_req["jsonrpc"] | "";
  if (strcmp(jsonrpc, "2.0") != 0) {
    mcp_build_error(resp_doc, rpc_id, RPC_ERR_INVALID_REQ, "jsonrpc must be \"2.0\"");
    mcp_send_response(res, resp_doc);
    return;
  }

  const char* method = rpc_req["method"] | "";
  if (!method || method[0] == '\0') {
    mcp_build_error(resp_doc, rpc_id, RPC_ERR_INVALID_REQ, "Missing method");
    mcp_send_response(res, resp_doc);
    return;
  }

  // ── Dispatch ─────────────────────────────────────────────────────────────

  // ── initialize ───────────────────────────────────────────────────────────
  if (strcmp(method, "initialize") == 0) {
    resp_doc["jsonrpc"] = "2.0";
    resp_doc["id"]      = rpc_id;
    auto result = resp_doc["result"].to<ArduinoJson::JsonObject>();
    result["protocolVersion"] = MCP_PROTOCOL_VERSION;
    auto info = result["serverInfo"].to<ArduinoJson::JsonObject>();
    info["name"]    = MCP_SERVER_NAME;
    info["version"] = MCP_SERVER_VERSION;
    auto caps = result["capabilities"].to<ArduinoJson::JsonObject>();
    caps["tools"].to<ArduinoJson::JsonObject>(); // tools capability with empty config
    mcp_send_response(res, resp_doc);
    return;
  }

  // ── ping ─────────────────────────────────────────────────────────────────
  if (strcmp(method, "ping") == 0) {
    resp_doc["jsonrpc"] = "2.0";
    resp_doc["id"]      = rpc_id;
    resp_doc["result"].to<ArduinoJson::JsonObject>(); // empty result
    mcp_send_response(res, resp_doc);
    return;
  }

  // ── tools/list ───────────────────────────────────────────────────────────
  if (strcmp(method, "tools/list") == 0) {
    resp_doc["jsonrpc"] = "2.0";
    resp_doc["id"]      = rpc_id;
    auto result = resp_doc["result"].to<ArduinoJson::JsonObject>();
    build_tools_list(result);
    mcp_send_response(res, resp_doc);
    return;
  }

  // ── tools/call ───────────────────────────────────────────────────────────
  if (strcmp(method, "tools/call") == 0) {
    ArduinoJson::JsonObjectConst params = rpc_req["params"].as<ArduinoJson::JsonObjectConst>();
    if (params.isNull()) {
      mcp_build_error(resp_doc, rpc_id, RPC_ERR_INVALID_PARAMS, "params required");
      mcp_send_response(res, resp_doc);
      return;
    }

    const char* tool_name = params["name"] | "";
    if (!tool_name || tool_name[0] == '\0') {
      mcp_build_error(resp_doc, rpc_id, RPC_ERR_INVALID_PARAMS, "params.name required");
      mcp_send_response(res, resp_doc);
      return;
    }

    ArduinoJson::JsonObjectConst args =
        params["arguments"].as<ArduinoJson::JsonObjectConst>();

    // Read-only tools that capture internal JSON output:
    String content_json;
    bool handled = true;

    if (strcmp(tool_name, "get_all") == 0) {
      content_json = tool_get_all(req, res);

    } else if (strcmp(tool_name, "get_controller_variables") == 0) {
      content_json = tool_get_controller_variables(req, res);

    } else if (strcmp(tool_name, "get_options") == 0) {
      content_json = tool_get_options(req, res);

    } else if (strcmp(tool_name, "get_stations") == 0) {
      content_json = tool_get_stations(req, res);

    } else if (strcmp(tool_name, "get_station_status") == 0) {
      content_json = tool_get_station_status(req, res);

    } else if (strcmp(tool_name, "get_programs") == 0) {
      content_json = tool_get_programs(req, res);

    } else if (strcmp(tool_name, "get_debug") == 0) {
      content_json = tool_get_debug(req, res);

    // Action tools:
    } else if (strcmp(tool_name, "manual_station_run") == 0) {
      if (args.isNull()) {
        mcp_build_error(resp_doc, rpc_id, RPC_ERR_INVALID_PARAMS, "arguments required");
        mcp_send_response(res, resp_doc);
        return;
      }
      int code = tool_manual_station_run(args);
      mcp_build_code_result(resp_doc, rpc_id, code);
      mcp_send_response(res, resp_doc);
      return;

    } else if (strcmp(tool_name, "change_controller_variables") == 0) {
      int code = tool_change_controller_variables(args);
      mcp_build_code_result(resp_doc, rpc_id, code);
      mcp_send_response(res, resp_doc);
      return;

    } else if (strcmp(tool_name, "pause_queue") == 0) {
      ArduinoJson::JsonObjectConst actual_args = args;
      int code = tool_pause_queue(actual_args);
      mcp_build_code_result(resp_doc, rpc_id, code);
      mcp_send_response(res, resp_doc);
      return;

    } else {
      handled = false;
    }

    if (!handled) {
      mcp_build_error(resp_doc, rpc_id, RPC_ERR_METHOD_NOT_FOUND,
                      "Unknown tool name");
      mcp_send_response(res, resp_doc);
      return;
    }

    // Return text result (for read-only tools)
    mcp_build_text_result(resp_doc, rpc_id, content_json);
    mcp_send_response(res, resp_doc);
    return;
  }

  // ── Unknown method ───────────────────────────────────────────────────────
  mcp_build_error(resp_doc, rpc_id, RPC_ERR_METHOD_NOT_FOUND, "Method not found");
  mcp_send_response(res, resp_doc);
}

#endif // ESP32 && USE_OTF
