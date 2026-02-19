import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { OpenSprinklerClient } from "./client.js";

/**
 * Register all OpenSprinkler tools on the given MCP server instance.
 */
export function registerTools(
  server: McpServer,
  getClient: () => OpenSprinklerClient,
) {
  // ─── Read-only queries ──────────────────────────────────────────────

  server.tool(
    "get_all",
    "Get all OpenSprinkler data in one call (controller variables, options, stations, status, programs). Equivalent to /ja.",
    {},
    async () => {
      const data = await getClient().get("/ja");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_controller_variables",
    "Get controller variables: device time, rain delay, flow count, station states, MQTT/OTC status, reboot info, etc. Equivalent to /jc.",
    {},
    async () => {
      const data = await getClient().get("/jc");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_options",
    "Get controller options: firmware version, timezone, network config, sensor settings, master stations, water level, etc. Equivalent to /jo.",
    {},
    async () => {
      const data = await getClient().get("/jo");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_stations",
    "Get station names, attributes (master ops, ignore rain/sensor flags, disable flags, special station flags, group IDs). Equivalent to /jn.",
    {},
    async () => {
      const data = await getClient().get("/jn");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_station_status",
    "Get current on/off status of all stations. Equivalent to /js.",
    {},
    async () => {
      const data = await getClient().get("/js");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_programs",
    "Get all program data: schedules, durations, flags, names. Equivalent to /jp.",
    {},
    async () => {
      const data = await getClient().get("/jp");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_special_stations",
    "Get special station data (RF, remote, GPIO, HTTP/HTTPS, OTC stations). Equivalent to /je.",
    {},
    async () => {
      const data = await getClient().get("/je");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_log",
    "Get watering log data. Specify a time range (start/end epoch) or history window (days back). Optionally filter by event type. Equivalent to /jl.",
    {
      start: z.number().optional().describe("Start time as Unix epoch seconds (inclusive)"),
      end: z.number().optional().describe("End time as Unix epoch seconds (inclusive)"),
      hist: z.number().optional().describe("History window in days back from today (alternative to start/end)"),
      type: z.enum(["s1", "s2", "rd", "fl", "wl"]).optional().describe("Filter special events: s1=sensor1, s2=sensor2, rd=rain delay, fl=flow, wl=water level"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {};
      if (args.start !== undefined) params.start = args.start;
      if (args.end !== undefined) params.end = args.end;
      if (args.hist !== undefined) params.hist = args.hist;
      if (args.type) params.type = args.type;
      const data = await getClient().get("/jl", params);
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_debug",
    "Get debug/diagnostics info (firmware build, heap, RAM). No authentication required. Equivalent to /db.",
    {},
    async () => {
      const data = await getClient().get("/db");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  // ─── Controller actions ─────────────────────────────────────────────

  server.tool(
    "change_controller_variables",
    "Change controller variables: enable/disable operation, set rain delay, reset stations, reboot. Equivalent to /cv.",
    {
      en: z.number().min(0).max(1).optional().describe("Enable (1) or disable (0) operation"),
      rd: z.number().min(0).max(32767).optional().describe("Rain delay in hours (0 clears)"),
      rsn: z.number().min(0).max(1).optional().describe("Reset all stations (1 to reset)"),
      rrsn: z.number().min(0).max(1).optional().describe("Reset running stations only (1 to reset)"),
      rbt: z.number().min(0).max(1).optional().describe("Reboot controller (1 to reboot)"),
    },
    async (args) => {
      const data = await getClient().command("/cv", args as Record<string, number>);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "change_options",
    "Change controller options (timezone, network, sensors, master stations, water level, logging, etc.). Pass option names and values. Equivalent to /co.",
    {
      options: z.record(z.string(), z.union([z.string(), z.number()])).describe("Key-value pairs of option names and their new values. See API docs for valid option names (e.g. tz, wl, sdt, mas, lg, loc, mqtt, etc.)"),
    },
    async (args) => {
      const params: Record<string, string | number> = {};
      for (const [k, v] of Object.entries(args.options)) {
        params[k] = v;
      }
      const data = await getClient().command("/co", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Station control ────────────────────────────────────────────────

  server.tool(
    "manual_station_run",
    "Manually open or close a single station. Equivalent to /cm.",
    {
      sid: z.number().min(0).describe("Station index (0-based)"),
      en: z.number().min(0).max(1).describe("1 = open station, 0 = close station"),
      t: z.number().min(0).max(64800).optional().describe("Duration in seconds (required when opening, max 64800 = 18h)"),
      qo: z.number().min(0).max(1).optional().describe("Queue option: 0 = append (default), 1 = insert at front"),
      ssta: z.number().min(0).max(1).optional().describe("Shift remaining stations in same group when closing (1 = shift)"),
    },
    async (args) => {
      const data = await getClient().command("/cm", args as Record<string, number>);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "run_once",
    "Run a run-once program with per-station durations. Equivalent to /cr.",
    {
      t: z.string().describe("JSON array of per-station durations in seconds, e.g. '[60,0,120,0,0,0,0,0]'. 0 = skip station."),
      uwt: z.number().min(0).max(1).optional().describe("Use weather adjustment (1 = yes, 0 = no)"),
      qo: z.number().min(0).max(2).optional().describe("Queue option: 0 = append, 1 = insert front, 2 = replace all (default)"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {
        t: args.t,
        uwt: args.uwt,
        qo: args.qo,
      };
      const data = await getClient().command("/cr", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "manual_program_start",
    "Manually start a saved program. Equivalent to /mp.",
    {
      pid: z.number().min(0).describe("Program index (0-based)"),
      uwt: z.number().min(0).max(1).optional().describe("Use weather (1 = apply watering level, 0 = 100%)"),
      qo: z.number().min(0).max(2).optional().describe("Queue option: 0 = append, 1 = insert front, 2 = replace all (default)"),
    },
    async (args) => {
      const data = await getClient().command("/mp", args as Record<string, number>);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "pause_queue",
    "Pause or resume the program queue. Equivalent to /pq.",
    {
      dur: z.number().optional().describe("Toggle-style: if no pause active and dur>0, pause for dur seconds; if pause active, cancel regardless of dur"),
      repl: z.number().optional().describe("Set/replace pause duration in seconds (takes precedence over dur; 0 = cancel)"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {};
      if (args.dur !== undefined) params.dur = args.dur;
      if (args.repl !== undefined) params.repl = args.repl;
      const data = await getClient().command("/pq", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Program management ─────────────────────────────────────────────

  server.tool(
    "change_program",
    "Create or modify a program. Use pid=-1 to create new. Equivalent to /cp.",
    {
      pid: z.number().min(-1).describe("Program index (-1 = create new, 0..N-1 = modify existing)"),
      v: z.string().optional().describe("Program body as JSON array: [flag,days0,days1,[start0,start1,start2,start3],[dur0,dur1,...]]"),
      name: z.string().optional().describe("Program name"),
      en: z.number().min(0).max(1).optional().describe("Enable/disable program (if set, other params ignored)"),
      uwt: z.number().min(0).max(1).optional().describe("Use weather flag (if set, other params ignored)"),
      from: z.number().optional().describe("Date range start: (month<<5)+day"),
      to: z.number().optional().describe("Date range end: (month<<5)+day"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {
        pid: args.pid,
      };
      if (args.v) params.v = args.v;
      if (args.name) params.name = args.name;
      if (args.en !== undefined) params.en = args.en;
      if (args.uwt !== undefined) params.uwt = args.uwt;
      if (args.from !== undefined) params.from = args.from;
      if (args.to !== undefined) params.to = args.to;
      const data = await getClient().command("/cp", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "delete_program",
    "Delete a program or all programs. Equivalent to /dp.",
    {
      pid: z.number().min(-1).describe("Program index to delete (0-based), or -1 to delete ALL programs"),
    },
    async (args) => {
      const data = await getClient().command("/dp", { pid: args.pid });
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "move_program_up",
    "Move a program up in the list (swap with previous). Equivalent to /up.",
    {
      pid: z.number().min(1).describe("Program index to move up (must be >= 1)"),
    },
    async (args) => {
      const data = await getClient().command("/up", { pid: args.pid });
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Station configuration ─────────────────────────────────────────

  server.tool(
    "change_station",
    "Change station names and attributes. Equivalent to /cs.",
    {
      changes: z.record(z.string(), z.union([z.string(), z.number()])).describe("Key-value pairs: s0..sN for names, m0..mN for master1 bits, n0..nN for master2, i0 for ignore_rain, j0 for ignore_sn1, k0 for ignore_sn2, d0 for disable, g0..gN for group IDs"),
      sid: z.number().optional().describe("Special station target index"),
      st: z.number().optional().describe("Special station type (1=RF, 2=Remote, 3=GPIO, 4=HTTP, 5=HTTPS, 6=OTC)"),
      sd: z.string().optional().describe("Special station data payload"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {
        ...args.changes,
      };
      if (args.sid !== undefined) params.sid = args.sid;
      if (args.st !== undefined) params.st = args.st;
      if (args.sd !== undefined) params.sd = args.sd;
      const data = await getClient().command("/cs", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Log management ─────────────────────────────────────────────────

  server.tool(
    "delete_log",
    "Delete log data for a specific day or all logs. Equivalent to /dl.",
    {
      day: z.union([z.number(), z.literal("all")]).describe("Day index (epoch_seconds/86400) or 'all' to delete everything"),
    },
    async (args) => {
      const data = await getClient().command("/dl", { day: args.day });
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Password ───────────────────────────────────────────────────────

  server.tool(
    "set_password",
    "Change the device password. Equivalent to /sp.",
    {
      new_password: z.string().describe("New password (plaintext – will be MD5 hashed)"),
      confirm_password: z.string().describe("Confirm new password (must match)"),
    },
    async (args) => {
      const { md5Hash } = await import("./client.js");
      const npw = md5Hash(args.new_password);
      const cpw = md5Hash(args.confirm_password);
      const data = await getClient().command("/sp", { npw, cpw });
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Script URLs ────────────────────────────────────────────────────

  server.tool(
    "change_script_urls",
    "Change the UI JavaScript path and/or weather script URL. Equivalent to /cu.",
    {
      jsp: z.string().optional().describe("UI/Javascript path URL"),
      wsp: z.string().optional().describe("Weather script URL"),
    },
    async (args) => {
      const params: Record<string, string | undefined> = {};
      if (args.jsp) params.jsp = args.jsp;
      if (args.wsp) params.wsp = args.wsp;
      const data = await getClient().command("/cu", params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── Sensor endpoints ───────────────────────────────────────────────

  server.tool(
    "get_sensors",
    "List all configured sensors with their current values. Equivalent to /sl.",
    {},
    async () => {
      const data = await getClient().get("/sl");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_sensor_values",
    "Get sensor values, optionally filtered by sensor number. Equivalent to /sg.",
    {
      nr: z.number().optional().describe("Sensor number to query (omit for all)"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {};
      if (args.nr !== undefined) params.nr = args.nr;
      const data = await getClient().get("/sg", params);
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_sensor_log",
    "Get sensor log data. Equivalent to /so.",
    {
      nr: z.number().optional().describe("Sensor number"),
      start: z.number().optional().describe("Start time (epoch seconds)"),
      end: z.number().optional().describe("End time (epoch seconds)"),
      hist: z.number().optional().describe("History window in days"),
      format: z.enum(["json", "csv"]).optional().describe("Output format"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {};
      if (args.nr !== undefined) params.nr = args.nr;
      if (args.start !== undefined) params.start = args.start;
      if (args.end !== undefined) params.end = args.end;
      if (args.hist !== undefined) params.hist = args.hist;
      if (args.format) params.format = args.format;
      const data = await getClient().get("/so", params);
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "configure_sensor",
    "Configure a sensor (add/modify/delete). Equivalent to /sc.",
    {
      params: z.record(z.string(), z.union([z.string(), z.number()])).describe("Sensor configuration parameters: nr (sensor number), type, name, enable, ip, port, id, ri (read interval), etc. Use nr with delete=1 to remove."),
    },
    async (args) => {
      const data = await getClient().command("/sc", args.params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "read_sensor_now",
    "Trigger an immediate read of a specific sensor. Equivalent to /sr.",
    {
      nr: z.number().describe("Sensor number to read"),
    },
    async (args) => {
      const data = await getClient().command("/sr", { nr: args.nr });
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_sensor_types",
    "Get list of available sensor types. Equivalent to /sf.",
    {},
    async () => {
      const data = await getClient().get("/sf");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  // ─── Sensor adjustments & monitors ──────────────────────────────────

  server.tool(
    "list_adjustments",
    "List sensor-based program adjustments. Equivalent to /se.",
    {},
    async () => {
      const data = await getClient().get("/se");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "configure_adjustment",
    "Configure a sensor-based program adjustment. Equivalent to /sb.",
    {
      params: z.record(z.string(), z.union([z.string(), z.number()])).describe("Adjustment parameters: nr, sensor, prog, type, factor1, factor2, min, max, etc."),
    },
    async (args) => {
      const data = await getClient().command("/sb", args.params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "list_monitors",
    "List sensor monitors (threshold-based program triggers). Equivalent to /ml.",
    {},
    async () => {
      const data = await getClient().get("/ml");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "configure_monitor",
    "Configure a sensor monitor (threshold trigger). Equivalent to /mc.",
    {
      params: z.record(z.string(), z.union([z.string(), z.number()])).describe("Monitor parameters: nr, sensor, type, prog, min, max, maxrun, cooldown, etc."),
    },
    async (args) => {
      const data = await getClient().command("/mc", args.params);
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  // ─── ZigBee / IEEE 802.15.4 (ESP32-C5 only) ────────────────────────

  server.tool(
    "get_ieee802154_config",
    "Read IEEE 802.15.4 radio configuration (ZigBee/Matter mode). ESP32-C5 only. Equivalent to /ir.",
    {},
    async () => {
      const data = await getClient().get("/ir");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "set_ieee802154_config",
    "Set IEEE 802.15.4 radio mode (WiFi-only, Matter, ZigBee Gateway, ZigBee Client). ESP32-C5 only. Equivalent to /iw.",
    {
      activeMode: z.number().min(0).max(3).describe("Radio mode: 0=WiFi only, 1=Matter, 2=ZigBee Gateway, 3=ZigBee Client"),
    },
    async (args) => {
      const data = await getClient().command("/iw", { activeMode: args.activeMode });
      return { content: [{ type: "text", text: JSON.stringify(data) }] };
    },
  );

  server.tool(
    "get_zigbee_devices",
    "Get ZigBee device list (gateway mode) or discovered devices. ESP32-C5 only. Equivalent to /zg.",
    {},
    async () => {
      const data = await getClient().get("/zg");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "zigbee_join_network",
    "Join a ZigBee network (client mode) or open network for joining (gateway mode). ESP32-C5 only. Equivalent to /zj.",
    {
      action: z.string().optional().describe("Join action (e.g. 'scan', 'join', 'open')"),
    },
    async (args) => {
      const params: Record<string, string | number | undefined> = {};
      if (args.action) params.action = args.action;
      const data = await getClient().command("/zj", params);
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_zigbee_status",
    "Get ZigBee radio status. ESP32-C5 only. Equivalent to /zs.",
    {},
    async () => {
      const data = await getClient().get("/zs");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  // ─── BLE (ESP32 only) ───────────────────────────────────────────────

  server.tool(
    "get_ble_devices",
    "Scan/list BLE devices. ESP32 only. Equivalent to /bd.",
    {},
    async () => {
      const data = await getClient().get("/bd");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  // ─── Backup & diagnostics ──────────────────────────────────────────

  server.tool(
    "backup_sensor_config",
    "Export full sensor/adjustment/monitor configuration backup. Equivalent to /sx.",
    {},
    async () => {
      const data = await getClient().get("/sx");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );

  server.tool(
    "get_system_resources",
    "Get system resource usage (memory, storage). Equivalent to /du.",
    {},
    async () => {
      const data = await getClient().get("/du");
      return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
    },
  );
}
