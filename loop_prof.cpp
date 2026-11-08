/* OpenSprinkler Unified Firmware
 *
 * Main-loop section profiler (ESP32), see loop_prof.h.
 * All state is touched from the Arduino loop task only (the /db handler runs
 * inside otf->loop() in that task), so no locking is needed.
 */
#include "loop_prof.h"

#if defined(LOOP_PROF)

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include <LittleFS.h>
#include <sys/stat.h>

static const char *const s_names[LPS_COUNT] = {
	"outside", "pre", "eth", "flowmode", "current", "net", "http", "updsrv", "ota", "display", "async", "mqtt", "sensapi", "zigbee", "ble",
	"matter", "rainmaker", "ctrl", "ntp", "netchk", "weather", "notif",
	"sens_pre", "sens_logs", "sens_mon", "sens_read", "tail"
};

static_assert(sizeof(s_names) / sizeof(s_names[0]) == LPS_COUNT, "s_names must match LoopProfSection");

struct LpSec {
	uint64_t sum_us;
	uint32_t max_us;
	uint32_t count;
	uint32_t slow;
};

struct LpLoop {            // one slow do_loop()
	uint32_t uptime_ms;
	uint32_t total_ms;
	uint16_t worst_ms;
	uint8_t  worst_sec;
};

struct LpSensor {          // one slow sensor read
	uint32_t uptime_ms;
	uint32_t ms;
	uint16_t nr;
	uint16_t type;
	uint8_t  phase;
};

struct LpHttp {            // one slow HTTP handler
	uint32_t uptime_ms;
	uint32_t ms;
	char     path[16];
	uint8_t  cloud;
};

#define LP_HTTP_RING   12
#define LP_LOOP_RING   16
#define LP_SENSOR_RING 12
#define LP_TASKS_MAX   32
#define LP_HIST_N      8
static const uint16_t s_hist_ms[LP_HIST_N - 1] = { 5, 20, 50, 100, 250, 500, 1000 };

static LpSec    s_sec[LPS_COUNT];
static LpLoop   s_loops[LP_LOOP_RING];
static LpSensor s_sens[LP_SENSOR_RING];
static LpHttp   s_http[LP_HTTP_RING];
static uint8_t  s_http_head = 0;
static uint32_t s_hist[LP_HIST_N];
static uint8_t  s_loop_head = 0, s_sens_head = 0;
static uint32_t s_loops_total = 0;
static uint32_t s_loop_max_us = 0;
static uint64_t s_since_us = 0;      // reset time

static int64_t  s_last_mark = 0;     // time of the previous mark
static int64_t  s_loop_start = 0;
static int64_t  s_loop_end = 0;      // end of the previous do_loop()
static uint32_t s_worst_us = 0;      // worst section of the current loop
static uint8_t  s_worst_sec = 0;

// previous task run-time snapshot for the CPU share since the last query
static struct { TaskHandle_t h; uint32_t rt; } s_prev[LP_TASKS_MAX];
static uint8_t  s_prev_n = 0;
static uint32_t s_prev_total = 0;

static inline void charge(uint8_t sec, uint32_t us) {
	if (sec >= LPS_COUNT) return;
	LpSec &s = s_sec[sec];
	s.sum_us += us;
	s.count++;
	if (us > s.max_us) s.max_us = us;
	if (us >= LOOP_PROF_SLOW_MS * 1000UL) s.slow++;
	if (us > s_worst_us) { s_worst_us = us; s_worst_sec = sec; }
}

void loop_prof_reset() {
	memset(s_sec, 0, sizeof(s_sec));
	memset(s_loops, 0, sizeof(s_loops));
	memset(s_sens, 0, sizeof(s_sens));
	memset(s_http, 0, sizeof(s_http));
	s_http_head = 0;
	memset(s_hist, 0, sizeof(s_hist));
	s_loop_head = s_sens_head = 0;
	s_loops_total = 0;
	s_loop_max_us = 0;
	s_since_us = (uint64_t)esp_timer_get_time();
	s_loop_end = 0;
}

void loop_prof_begin() {
	int64_t now = esp_timer_get_time();
	if (!s_since_us) s_since_us = (uint64_t)now;
	s_worst_us = 0;
	s_worst_sec = 0;
	if (s_loop_end) charge(LPS_OUTSIDE, (uint32_t)(now - s_loop_end));
	s_loop_start = now;
	s_last_mark = now;
}

void loop_prof_mark(uint8_t sec) {
	if (!s_loop_start) return;
	int64_t now = esp_timer_get_time();
	charge(sec, (uint32_t)(now - s_last_mark));
	s_last_mark = now;
}

void loop_prof_end() {
	if (!s_loop_start) return;
	loop_prof_mark(LPS_TAIL);
	int64_t now = s_last_mark;
	uint32_t total = (uint32_t)(now - s_loop_start);
	s_loops_total++;
	if (total > s_loop_max_us) s_loop_max_us = total;
	uint8_t b = 0;
	while (b < LP_HIST_N - 1 && total >= (uint32_t)s_hist_ms[b] * 1000UL) b++;
	s_hist[b]++;
	if (total >= LOOP_PROF_SLOW_LOOP_MS * 1000UL) {
		LpLoop &l = s_loops[s_loop_head];
		l.uptime_ms = (uint32_t)(now / 1000);
		l.total_ms = total / 1000;
		l.worst_ms = (uint16_t)((s_worst_us / 1000) > 65535 ? 65535 : s_worst_us / 1000);
		l.worst_sec = s_worst_sec;
		s_loop_head = (s_loop_head + 1) % LP_LOOP_RING;
	}
	s_loop_end = now;
}

void loop_prof_sensor(uint16_t nr, uint16_t type, uint8_t phase, uint32_t ms) {
	if (ms < 30) return;          // lower than LOOP_PROF_SLOW_MS: many medium steps add up
	LpSensor &s = s_sens[s_sens_head];
	s.uptime_ms = millis();
	s.ms = ms;
	s.nr = nr;
	s.type = type;
	s.phase = phase;
	s_sens_head = (s_sens_head + 1) % LP_SENSOR_RING;
}

void loop_prof_http(const char *path, uint32_t ms, bool cloud) {
	if (ms < LOOP_PROF_SLOW_MS) return;
	LpHttp &h = s_http[s_http_head];
	h.uptime_ms = millis();
	h.ms = ms;
	h.cloud = cloud ? 1 : 0;
	size_t i = 0;
	for (; path && path[i] && i < sizeof(h.path) - 1; i++) {
		char c = path[i];
		h.path[i] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
	}
	h.path[i] = 0;
	s_http_head = (s_http_head + 1) % LP_HTTP_RING;
}

/* OTF profiling hook (OpenThingsFramework.cpp, weak there): phases of the local
 * server and the websocket poll. Slow ones go into the HTTP ring; the handler
 * phase carries the request path, the others a "#phase" marker. */
extern "C" void otf_prof_hook(uint8_t phase, uint32_t ms, const char *path) {
	if (ms < LOOP_PROF_SLOW_MS) return;
	static const char *const names[] = { "#accept", "#header", "#body", "#handler", "#close", "#ws", "#idle" };
	char p[16];
	if (phase == 3 && path && path[0]) {
		p[0] = '>';  // handler time incl. res.end(), as seen by OTF
		strncpy(p + 1, path, sizeof(p) - 2);
		p[sizeof(p) - 1] = 0;
	} else if (phase == 1 && path && path[0]) {
		p[0] = '@';  // header phase: client address
		strncpy(p + 1, path, sizeof(p) - 2);
		p[sizeof(p) - 1] = 0;
	} else {
		strncpy(p, phase < 7 ? names[phase] : "#?", sizeof(p) - 1);
		p[sizeof(p) - 1] = 0;
	}
	loop_prof_http(p, ms, phase == 5);
}

#define LP_APPEND(...) do { \
	if (n < len) { int _w = snprintf(buf + n, len - n, __VA_ARGS__); if (_w > 0) n += (size_t)_w; } \
} while (0)

size_t loop_prof_json(char *buf, size_t len) {
	size_t n = 0;
	if (!buf || !len) return 0;
	buf[0] = 0;
	uint64_t now = (uint64_t)esp_timer_get_time();
	LP_APPEND("{\"uptime_ms\":%lu,\"window_ms\":%lu,\"loops\":%lu,\"loop_max_ms\":%lu,\"hist_ms\":[",
		(unsigned long)(now / 1000), (unsigned long)((now - s_since_us) / 1000),
		(unsigned long)s_loops_total, (unsigned long)(s_loop_max_us / 1000));
	for (uint8_t i = 0; i < LP_HIST_N; i++) {
		if (i < LP_HIST_N - 1) LP_APPEND("%s{\"lt\":%u,\"n\":%lu}", i ? "," : "", s_hist_ms[i], (unsigned long)s_hist[i]);
		else LP_APPEND(",{\"lt\":0,\"n\":%lu}]", (unsigned long)s_hist[i]);
	}

	LP_APPEND(",\"sections\":{");
	bool first = true;
	for (uint8_t i = 0; i < LPS_COUNT; i++) {
		const LpSec &s = s_sec[i];
		if (!s.count) continue;
		LP_APPEND("%s\"%s\":{\"n\":%lu,\"sum_ms\":%lu,\"avg_us\":%lu,\"max_ms\":%lu,\"slow\":%lu}",
			first ? "" : ",", s_names[i], (unsigned long)s.count, (unsigned long)(s.sum_us / 1000),
			(unsigned long)(s.sum_us / s.count), (unsigned long)(s.max_us / 1000), (unsigned long)s.slow);
		first = false;
	}
	LP_APPEND("}");

	LP_APPEND(",\"slow_loops\":[");   // newest first
	first = true;
	for (uint8_t k = 0; k < LP_LOOP_RING; k++) {
		const LpLoop &l = s_loops[(s_loop_head + LP_LOOP_RING - 1 - k) % LP_LOOP_RING];
		if (!l.total_ms) continue;
		LP_APPEND("%s{\"t\":%lu,\"ms\":%lu,\"worst\":\"%s\",\"worst_ms\":%u}", first ? "" : ",",
			(unsigned long)l.uptime_ms, (unsigned long)l.total_ms,
			l.worst_sec < LPS_COUNT ? s_names[l.worst_sec] : "?", (unsigned)l.worst_ms);
		first = false;
	}
	LP_APPEND("],\"slow_sensors\":[");
	first = true;
	for (uint8_t k = 0; k < LP_SENSOR_RING; k++) {
		const LpSensor &s = s_sens[(s_sens_head + LP_SENSOR_RING - 1 - k) % LP_SENSOR_RING];
		if (!s.ms) continue;
		static const char *const ph[] = { "read", "log", "push" };
		LP_APPEND("%s{\"t\":%lu,\"nr\":%u,\"type\":%u,\"phase\":\"%s\",\"ms\":%lu}", first ? "" : ",",
			(unsigned long)s.uptime_ms, (unsigned)s.nr, (unsigned)s.type, s.phase < 3 ? ph[s.phase] : "?",
			(unsigned long)s.ms);
		first = false;
	}
	LP_APPEND("],\"slow_http\":[");
	first = true;
	for (uint8_t k = 0; k < LP_HTTP_RING; k++) {
		const LpHttp &h = s_http[(s_http_head + LP_HTTP_RING - 1 - k) % LP_HTTP_RING];
		if (!h.ms) continue;
		LP_APPEND("%s{\"t\":%lu,\"path\":\"%s\",\"cloud\":%u,\"ms\":%lu}", first ? "" : ",",
			(unsigned long)h.uptime_ms, h.path, (unsigned)h.cloud, (unsigned long)h.ms);
		first = false;
	}
	LP_APPEND("]");

#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
	// CPU share per task since the previous query (run-time counter is a u32 in µs,
	// so the delta is valid for queries less than ~71 min apart).
	UBaseType_t cnt = uxTaskGetNumberOfTasks() + 4;
	TaskStatus_t *ts = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * cnt);
	if (ts) {
		uint32_t total = 0;
		cnt = uxTaskGetSystemState(ts, cnt, &total);
		uint32_t dtotal = total - s_prev_total;
		LP_APPEND(",\"cpu_window_ms\":%lu,\"tasks\":[", (unsigned long)(dtotal / 1000));
		first = true;
		for (UBaseType_t i = 0; i < cnt; i++) {
			uint32_t rt = (uint32_t)ts[i].ulRunTimeCounter;
			uint32_t prev = 0;
			for (uint8_t j = 0; j < s_prev_n; j++) if (s_prev[j].h == ts[i].xHandle) { prev = s_prev[j].rt; break; }
			uint32_t d = rt - prev;
			// per mille of the window; tasks below 0.1 % are left out
			uint32_t pm = dtotal ? (uint32_t)(((uint64_t)d * 1000ULL) / dtotal) : 0;
			if (!pm) continue;
			char name[configMAX_TASK_NAME_LEN + 1];
			strncpy(name, ts[i].pcTaskName ? ts[i].pcTaskName : "?", sizeof(name) - 1);
			name[sizeof(name) - 1] = 0;
			for (char *p = name; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = '_';
			LP_APPEND("%s{\"name\":\"%s\",\"prio\":%u,\"pm\":%lu,\"stack_free\":%lu}", first ? "" : ",",
				name, (unsigned)ts[i].uxCurrentPriority, (unsigned long)pm,
				(unsigned long)ts[i].usStackHighWaterMark);
			first = false;
		}
		LP_APPEND("]");
		s_prev_n = 0;
		for (UBaseType_t i = 0; i < cnt && s_prev_n < LP_TASKS_MAX; i++) {
			s_prev[s_prev_n].h = ts[i].xHandle;
			s_prev[s_prev_n].rt = (uint32_t)ts[i].ulRunTimeCounter;
			s_prev_n++;
		}
		s_prev_total = total;
		free(ts);
	}
#endif
	LP_APPEND("}");
	if (n >= len) { n = len - 1; buf[n] = 0; }  // truncated: caller sees invalid JSON
	return n;
}

static uint32_t lp_us_since(int64_t t0) { return (uint32_t)(esp_timer_get_time() - t0); }

size_t loop_prof_fsbench(char *buf, size_t len) {
	size_t n = 0;
	if (!buf || !len) return 0;
	buf[0] = 0;
	LP_APPEND("{");
	// raw read of the external flash, no filesystem: 16 x 4 KB
	const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs_ext");
	uint8_t *blk = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (part && blk) {
		int64_t t0 = esp_timer_get_time();
		for (int i = 0; i < 16; i++) esp_partition_read(part, (size_t)i * 4096, blk, 4096);
		uint32_t us4k = lp_us_since(t0) / 16;
		t0 = esp_timer_get_time();
		for (int i = 0; i < 64; i++) esp_partition_read(part, (size_t)i * 4096 + 256, blk, 128);
		uint32_t us128 = lp_us_since(t0) / 64;
		LP_APPEND("\"raw_4k_us\":%lu,\"raw_128b_us\":%lu,", (unsigned long)us4k, (unsigned long)us128);
	} else {
		LP_APPEND("\"raw\":\"no partition\",");
	}
	if (blk) free(blk);

	struct { const char *name; const char *path; } probes[] = {
		{ "sopts", "/sopts.dat" }, { "stns", "/stns.dat" }, { "ui_js", "/ui/2.4.0.231/js/main.js.gz" },
		{ "missing", "/does_not_exist.dat" }, { "ui_missing", "/ui/2.4.0.231/js/nope.js" },
	};
	LP_APPEND("\"open_us\":{");
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		int64_t t0 = esp_timer_get_time();
		File f = LittleFS.open(probes[i].path, "r");
		uint32_t t_open = lp_us_since(t0);
		uint32_t t_read = 0;
		if (f) {
			uint8_t tmp[64];
			t0 = esp_timer_get_time();
			f.read(tmp, sizeof(tmp));
			t_read = lp_us_since(t0);
			f.close();
		}
		t0 = esp_timer_get_time();
		bool ex = LittleFS.exists(probes[i].path);
		uint32_t t_exists = lp_us_since(t0);
		LP_APPEND("%s\"%s\":{\"open\":%lu,\"read64\":%lu,\"exists\":%lu,\"found\":%d}", i ? "," : "",
			probes[i].name, (unsigned long)t_open, (unsigned long)t_read, (unsigned long)t_exists, ex ? 1 : 0);
	}
	LP_APPEND("}");

	// the same through POSIX directly (mount point /littlefs), no Arduino VFS wrapper
	LP_APPEND(",\"posix_us\":{");
	for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		char full[64];
		snprintf(full, sizeof(full), "/littlefs%s", probes[i].path);
		struct stat st;
		int64_t t0 = esp_timer_get_time();
		int src = stat(full, &st);
		uint32_t t_stat = lp_us_since(t0);
		t0 = esp_timer_get_time();
		FILE *fp = fopen(full, "rb");
		uint32_t t_open = lp_us_since(t0);
		uint32_t t_read = 0, t_close = 0;
		if (fp) {
			uint8_t tmp[64];
			t0 = esp_timer_get_time();
			fread(tmp, 1, sizeof(tmp), fp);
			t_read = lp_us_since(t0);
			t0 = esp_timer_get_time();
			fclose(fp);
			t_close = lp_us_since(t0);
		}
		LP_APPEND("%s\"%s\":{\"stat\":%lu,\"fopen\":%lu,\"read64\":%lu,\"fclose\":%lu,\"found\":%d}", i ? "," : "",
			probes[i].name, (unsigned long)t_stat, (unsigned long)t_open, (unsigned long)t_read,
			(unsigned long)t_close, src == 0 ? 1 : 0);
	}
	LP_APPEND("}");

	// size of the root directory (every path lookup scans it)
	int64_t t0 = esp_timer_get_time();
	uint32_t entries = 0, dirs = 0;
	File root = LittleFS.open("/");
	if (root) {
		File e = root.openNextFile();
		while (e) { entries++; if (e.isDirectory()) dirs++; e.close(); e = root.openNextFile(); }
		root.close();
	}
	LP_APPEND(",\"root_entries\":%lu,\"root_dirs\":%lu,\"root_list_ms\":%lu",
		(unsigned long)entries, (unsigned long)dirs, (unsigned long)(lp_us_since(t0) / 1000));
	t0 = esp_timer_get_time();
	size_t used = LittleFS.usedBytes();
	LP_APPEND(",\"used\":%lu,\"usedbytes_ms\":%lu}", (unsigned long)used, (unsigned long)(lp_us_since(t0) / 1000));
	if (n >= len) { n = len - 1; buf[n] = 0; }
	return n;
}

#endif // LOOP_PROF
