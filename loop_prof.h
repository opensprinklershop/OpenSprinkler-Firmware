/* OpenSprinkler Unified Firmware
 *
 * Main-loop section profiler (ESP32).
 *
 * do_loop() is split into sections by LOOP_PROF_MARK(sec): each mark charges the
 * time since the previous mark to <sec>. Per section the count, total and maximum
 * time and the number of calls above LOOP_PROF_SLOW_MS are kept; loops slower than
 * LOOP_PROF_SLOW_LOOP_MS are recorded in a small ring together with their worst
 * section, slow sensor reads in a second ring. FreeRTOS run-time counters give the
 * CPU share per task since the last query (the C5 has one core: a busy radio task
 * shows up here, not in a do_loop section).
 *
 * Read with /db?lp=1, reset with /db?lp=2. Costs ~1 KB of static RAM, no heap
 * outside the request.
 */
#ifndef _LOOP_PROF_H
#define _LOOP_PROF_H

#include <stdint.h>
#include <stddef.h>

#if defined(ESP32)
#define LOOP_PROF 1
#endif

enum LoopProfSection : uint8_t {
	LPS_OUTSIDE = 0, // between two do_loop() calls (Arduino loop task, preemption)
	LPS_PRE,         // Arduino OTA, sensor_api_connect, flow poll (remainder)
	LPS_ETH,         // eth_w5500_loop
	LPS_FLOWMODE,    // flow_update_input_mode
	LPS_CURRENT,     // current polling / overcurrent check
	LPS_NET,         // network state machine (without the HTTP server)
	LPS_HTTP,        // otf->loop(): local HTTP server + OTC websocket (all call sites)
	LPS_UPDSRV,      // update_server->handleClient()
	LPS_OTA,         // online_update_loop
	LPS_DISPLAY,     // ui_state_machine
	LPS_ASYNC,       // os.process_async_http_requests
	LPS_MQTT,        // mqtt restart + mqtt.loop
	LPS_SENSAPI,     // sensor_api_loop remainder
	LPS_ZIGBEE,      // sensor_zigbee_loop
	LPS_BLE,         // sensor_ble_loop
	LPS_MATTER,
	LPS_RAINMAKER,
	LPS_CTRL,        // 1 s block: rain delay, binary sensors, scheduling, run-time keeping
	LPS_NTP,
	LPS_NETCHK,      // check_network
	LPS_WEATHER,     // check_weather
	LPS_NOTIF,       // notif.run
	LPS_SENS_PRE,    // read_all_sensors: pending flush, deferred MQTT, groups
	LPS_SENS_LOGS,   // calc_sensorlogs
	LPS_SENS_MON,    // check_monitors + hourly sensor_save
	LPS_SENS_READ,   // sensor read sweep
	LPS_TAIL,        // remainder of do_loop
	LPS_COUNT
};

#define LOOP_PROF_SLOW_MS      100   // a section call above this counts as "slow"
#define LOOP_PROF_SLOW_LOOP_MS 200   // a do_loop() above this goes into the ring

#if defined(LOOP_PROF)
void loop_prof_begin();
void loop_prof_mark(uint8_t sec);
void loop_prof_end();
/** phase: 0 = read_sensor, 1 = sensor logs, 2 = push (MQTT/InfluxDB/...) */
void loop_prof_sensor(uint16_t nr, uint16_t type, uint8_t phase, uint32_t ms);
/** A slow HTTP handler (two-letter API dispatcher), path truncated to 15 chars. */
void loop_prof_http(const char *path, uint32_t ms, bool cloud);
void loop_prof_reset();
/** Writes the profile as a JSON object into buf. Returns the length. */
size_t loop_prof_json(char *buf, size_t len);
/** Filesystem micro-benchmark (blocks the loop ~1-3 s): raw flash read vs LittleFS ops. */
size_t loop_prof_fsbench(char *buf, size_t len);
#define LOOP_PROF_BEGIN()     loop_prof_begin()
#define LOOP_PROF_MARK(s)     loop_prof_mark(s)
#define LOOP_PROF_END()       loop_prof_end()
#define LOOP_PROF_SENSOR(n, t, ph, ms) loop_prof_sensor((n), (t), (ph), (ms))
#define LOOP_PROF_HTTP(p, ms, c) loop_prof_http((p), (ms), (c))
#else
#define LOOP_PROF_BEGIN()
#define LOOP_PROF_MARK(s)
#define LOOP_PROF_END()
#define LOOP_PROF_SENSOR(n, t, ph, ms)
#define LOOP_PROF_HTTP(p, ms, c)
#endif

#endif // _LOOP_PROF_H
