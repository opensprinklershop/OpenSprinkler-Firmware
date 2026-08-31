/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Notifier data structures and functions
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler library
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "notifier.h"
#include "program.h"
#include "ArduinoJson.hpp"
#include "opensprinkler_server.h"
#include "osinfluxdb.h"
#include "sensors.h"
#include "espconnect.h"

#if defined(ESP8266)
#include <user_interface.h>
#endif

NotifNodeStruct* NotifQueue::head = NULL;
NotifNodeStruct* NotifQueue::tail = NULL;
unsigned char NotifQueue::nqueue = 0;

extern OpenSprinkler os;
extern ProgramData pd;
// ether_buffer and tmp_buffer declared in sensors.h
extern float flow_last_gpm;

extern const char *user_agent_string;

// ---------------------------------------------------------------------------
// Notification event log
//
// A small circular buffer that keeps the most recent notification events so the
// mobile app can poll them (GET /nl) and raise push/local notifications for the
// same events that are otherwise only sent to IFTTT / MQTT / Email. Records are
// stored raw (type + values) and rendered to text on demand at poll time to keep
// the memory footprint tiny (important on ESP8266).
// ---------------------------------------------------------------------------
static NotifLogRecord notif_log[NOTIF_LOG_MAXSIZE];
static uint8_t notif_log_head = 0;   // index of the oldest stored record
static uint8_t notif_log_cnt = 0;    // number of stored records
static uint32_t notif_log_nextid = 1;

uint8_t notif_log_count() { return notif_log_cnt; }

uint32_t notif_log_lastid() { return notif_log_nextid - 1; }

const NotifLogRecord* notif_log_at(uint8_t idx) {
	if (idx >= notif_log_cnt) return NULL;
	uint8_t pos = (uint8_t)((notif_log_head + idx) % NOTIF_LOG_MAXSIZE);
	return &notif_log[pos];
}

void notif_log_add(uint32_t type, uint32_t lval, float fval, uint8_t bval) {
	uint8_t pos;
	if (notif_log_cnt < NOTIF_LOG_MAXSIZE) {
		pos = (uint8_t)((notif_log_head + notif_log_cnt) % NOTIF_LOG_MAXSIZE);
		notif_log_cnt++;
	} else {
		// buffer full: overwrite the oldest record
		pos = notif_log_head;
		notif_log_head = (uint8_t)((notif_log_head + 1) % NOTIF_LOG_MAXSIZE);
	}
	notif_log[pos].id = notif_log_nextid++;
	notif_log[pos].time = os.now_tz();
	notif_log[pos].type = type;
	notif_log[pos].lval = lval;
	notif_log[pos].fval = fval;
	notif_log[pos].bval = bval;
	DEBUG_PRINTF("notif_log_add id=%lu type=%lu\n", (unsigned long)notif_log[pos].id, (unsigned long)type);
}

uint8_t notif_priority(uint32_t type) {
	switch (type) {
		// Warnings & alarms -> heads-up + sound (os_high channel)
		case NOTIFY_FLOW_ALERT:
		case NOTIFY_CURR_ALERT:
		case NOTIFY_NOFLOW:
		case NOTIFY_PIPE_BURST:
		case NOTIFY_MONITOR_MID:
		case NOTIFY_MONITOR_HIGH:
			return 2; // high
		// Start/stop & moderate events -> audible (os_med channel)
		case NOTIFY_PROGRAM_SCHED:
		case NOTIFY_PROGRAM_END:
		case NOTIFY_STATION_ON:
		case NOTIFY_STATION_OFF:
		case NOTIFY_REBOOT:
		case NOTIFY_RAINDELAY:
		case NOTIFY_MONITOR_LOW:
			return 1; // medium
		default:
			return 0; // low: weather, sensors, reports -> silent
	}
}

// Render a compact english summary. Floats are formatted without %f so it also
// works on AVR (classic OpenSprinkler) toolchains.
void notif_render_text(uint32_t type, uint32_t lval, float fval, uint8_t bval, char* out, size_t outlen) {
	if (!out || outlen == 0) return;
	out[0] = 0;
	char nbuf[STATION_NAME_SIZE];
	switch (type) {
		case NOTIFY_STATION_ON:
			os.get_station_name(lval, nbuf);
			if ((int)fval > 0)
				snprintf_P(out, outlen, PSTR("Station %s turned on (scheduled %dm %ds)"), nbuf, (int)fval/60, (int)fval%60);
			else
				snprintf_P(out, outlen, PSTR("Station %s turned on"), nbuf);
			break;
		case NOTIFY_STATION_OFF:
			os.get_station_name(lval, nbuf);
			if ((int)fval > 0)
				snprintf_P(out, outlen, PSTR("Station %s closed (ran %dm %ds)"), nbuf, (int)fval/60, (int)fval%60);
			else
				snprintf_P(out, outlen, PSTR("Station %s closed"), nbuf);
			break;
		case NOTIFY_PROGRAM_SCHED: {
			ProgramStruct prog;
			const char* pname = "";
			if (lval < pd.nprograms) { pd.read(lval, &prog); pname = prog.name; }
			if (fval < 0)
				snprintf_P(out, outlen, PSTR("Program %s skipped%s"), pname, (bval > 0) ? " (weather)" : "");
			else
				snprintf_P(out, outlen, PSTR("Program %s scheduled (%d%% water)"), pname, (int)fval);
			break;
		}
		case NOTIFY_PROGRAM_END: {
			ProgramStruct prog;
			const char* pname = "";
			if (lval < pd.nprograms) { pd.read(lval, &prog); pname = prog.name; }
			snprintf_P(out, outlen, PSTR("Program %s finished"), pname);
			break;
		}
		case NOTIFY_SENSOR1:
			snprintf_P(out, outlen, PSTR("Sensor 1 %s"), ((int)fval) ? "activated" : "de-activated");
			break;
		case NOTIFY_SENSOR2:
			snprintf_P(out, outlen, PSTR("Sensor 2 %s"), ((int)fval) ? "activated" : "de-activated");
			break;
		case NOTIFY_RAINDELAY:
			snprintf_P(out, outlen, PSTR("Rain delay %s"), ((int)fval) ? "activated" : "de-activated");
			break;
		case NOTIFY_FLOWSENSOR: {
			float vol = lval * os.get_flow_volume_per_pulse();
			snprintf_P(out, outlen, PSTR("Flow: count %d, volume %d.%02d"), (int)lval, (int)vol, ((int)(vol*100))%100);
			break;
		}
		case NOTIFY_WEATHER_UPDATE:
			snprintf_P(out, outlen, PSTR("Weather update: water level %d%%"), (int)fval);
			break;
		case NOTIFY_REBOOT:
			snprintf_P(out, outlen, PSTR("Controller rebooted (cause %d)"), (int)os.last_reboot_cause);
			break;
		case NOTIFY_FLOW_ALERT:
			os.get_station_name(lval, nbuf);
			snprintf_P(out, outlen, PSTR("FLOW ALERT on station %s"), nbuf);
			break;
		case NOTIFY_CURR_ALERT:
			if (bval == CURR_ALERT_TYPE_UNDER) {
				os.get_station_name(lval, nbuf);
				snprintf_P(out, outlen, PSTR("Undercurrent on station %s (%dmA)"), nbuf, (int)fval);
			} else if (bval == CURR_ALERT_TYPE_OVER_STATION) {
				os.get_station_name(lval, nbuf);
				snprintf_P(out, outlen, PSTR("Overcurrent on station %s (%dmA)"), nbuf, (int)fval);
			} else {
				snprintf_P(out, outlen, PSTR("System overcurrent (%dmA)"), (int)fval);
			}
			break;
		case NOTIFY_NOFLOW:
			os.get_station_name(lval, nbuf);
			snprintf_P(out, outlen, PSTR("No flow detected on station %s"), nbuf);
			break;
		case NOTIFY_PIPE_BURST:
			snprintf_P(out, outlen, PSTR("Pipe burst warning (%lu pulses)"), (unsigned long)lval);
			break;
		case NOTIFY_MONTHLY_REPORT: {
			int v = (int)fval;
			snprintf_P(out, outlen, PSTR("Monthly water report: %d.%02d L"), v, ((int)(fval*100))%100);
			break;
		}
		case NOTIFY_MONITOR_LOW:
		case NOTIFY_MONITOR_MID:
		case NOTIFY_MONITOR_HIGH: {
			Monitor_t* mon = monitor_by_idx(bval);
			const char* mname = mon ? mon->getName() : "";
			int v = (int)fval;
			int frac = (int)(fval*100)%100; if (frac < 0) frac = -frac;
			snprintf_P(out, outlen, PSTR("Monitor %s: %d.%02d"), mname, v, frac);
			break;
		}
		default:
			snprintf_P(out, outlen, PSTR("Event %lu"), (unsigned long)type);
			break;
	}
	out[outlen-1] = 0;
}

bool is_notif_enabled(uint32_t type) {
	uint32_t notif = (uint32_t)os.iopts[IOPT_NOTIF_ENABLE] |
		((uint32_t)os.iopts[IOPT_NOTIF2_ENABLE] << 8) |
		((uint32_t)os.iopts[IOPT_NOTIF3_ENABLE] << 16) |
		((uint32_t)os.iopts[IOPT_NOTIF4_ENABLE] << 24);
	return  (notif&type) != 0;
}

uint32_t get_notif_enabled() {
	return (uint32_t)os.iopts[IOPT_NOTIF_ENABLE] |
		((uint32_t)os.iopts[IOPT_NOTIF2_ENABLE] << 8) |
		((uint32_t)os.iopts[IOPT_NOTIF3_ENABLE] << 16) |
		((uint32_t)os.iopts[IOPT_NOTIF4_ENABLE] << 24);
}

void set_notif_enabled(uint32_t notif) {
	os.iopts[IOPT_NOTIF_ENABLE] = notif&0xFF;
	os.iopts[IOPT_NOTIF2_ENABLE] = (notif >> 8)&0xFF;
	os.iopts[IOPT_NOTIF3_ENABLE] = (notif >> 16)&0xFF;
	os.iopts[IOPT_NOTIF4_ENABLE] = (notif >> 24)&0xFF;
}

void ip2string(char* str, size_t str_len, unsigned char ip[4]) {
	snprintf_P(str+strlen(str), str_len, PSTR("%d.%d.%d.%d"), ip[0], ip[1], ip[2], ip[3]);
}

#if defined(ESP8266)
static void append_esp8266_reboot_diag(char* str, size_t str_len) {
	if (!str || str_len == 0) return;

	size_t used = strlen(str);
	if (used >= str_len - 1) return;

	const rst_info* reset_info_ptr = ESP.getResetInfoPtr();
	if (reset_info_ptr) {
		snprintf_P(str + used, str_len - used,
			PSTR(" CrashID: r=%d e=%d epc1=%08x epc2=%08x epc3=%08x excvaddr=%08x depc=%08x."),
			(int)reset_info_ptr->reason,
			(int)reset_info_ptr->exccause,
			(unsigned int)reset_info_ptr->epc1,
			(unsigned int)reset_info_ptr->epc2,
			(unsigned int)reset_info_ptr->epc3,
			(unsigned int)reset_info_ptr->excvaddr,
			(unsigned int)reset_info_ptr->depc);
		used = strlen(str);
		if (used >= str_len - 1) return;
	}

	String reset_reason = ESP.getResetReason();
	String reset_info = ESP.getResetInfo();

	if (reset_info.length() > 0) {
		reset_info.replace("\r", " ");
		reset_info.replace("\n", " | ");
		const size_t max_reset_info_len = 420;
		if (reset_info.length() > max_reset_info_len) {
			reset_info = reset_info.substring(0, max_reset_info_len);
			reset_info += "...";
		}
	}

	if (reset_reason.length() > 0) {
		snprintf_P(str + used, str_len - used, PSTR(" ResetReason: %s."), reset_reason.c_str());
		used = strlen(str);
		if (used >= str_len - 1) return;
	}

	if (reset_info.length() > 0) {
		snprintf_P(str + used, str_len - used, PSTR(" SDK: %s"), reset_info.c_str());
	}
}
#endif

bool NotifQueue::add(uint32_t t, uint32_t l, float f, uint8_t b) {
		if (!is_notif_enabled(t)) { // if not subscribed to this type, return
		return false;
	}
	if(nqueue<NOTIF_QUEUE_MAXSIZE) {
		NotifNodeStruct* node = new NotifNodeStruct(t, l, f, b);
		if(tail==NULL) {
			head = node;
		} else {
			tail->next = node;
		}
		tail = node;
		nqueue++;
		DEBUG_PRINTF("NotifQueue::add (type %d) [%d]\n", t, nqueue);
		return true;
	}
	DEBUG_PRINTLN(F("NotifQueue::add queue is full!"));
	return false;
}

void NotifQueue::clear() {
	while(nqueue!=0) {
		NotifNodeStruct* node = head;
		head = head->next;
		if(head==NULL) {
			tail = NULL;
		}
		delete node;
		nqueue--;
	}
}

void push_message(uint32_t type, uint32_t lval, float fval, uint8_t bval);

bool NotifQueue::run(int n) {
	if(nqueue == 0) return false; // queue is empty
	if(n<=0 || n>nqueue) n=nqueue;
	while(nqueue!=0 && n!=0) {
		NotifNodeStruct* node = head;
		head = head->next;
		if(head==NULL) {
			tail = NULL;
		}
		push_message(node->type, node->lval, node->fval, node->bval);
		DEBUG_PRINTF("NotifQueue::run (type %d) [%d]\n", node->type, nqueue);
		delete node;
		nqueue--;
		n--;
	}
	return true;
}

// --- Outbound reachability backoff ---------------------------------------
// push_message() runs in the main loop and performs SYNCHRONOUS, blocking sends
// (IFTTT ~12s, SMTP TLS connect, InfluxDB, push ~5s). network_connected() only
// proves the LAN is up, not that the internet is reachable. On a LAN-only/offline
// site (e.g. only an NTP server) every send fails on a connect timeout, and with
// scheduled programs constantly refilling the queue the main loop stays blocked
// in dead network calls -> web server unreachable, "only a reset helps".
// After a few consecutive send failures we treat the internet as unreachable and
// skip the blocking channels for a cooldown, while STILL recording the event in
// /nl so the mobile app loses nothing. Any successful send clears the backoff.
static uint8_t  s_outbound_fail_streak = 0;
static uint32_t s_outbound_backoff_until = 0; // millis() deadline; 0 = inactive
#define OUTBOUND_FAIL_STREAK_MAX 3
#define OUTBOUND_BACKOFF_MS      (5UL * 60UL * 1000UL) // 5 min

static bool outbound_backoff_active() {
	return s_outbound_backoff_until != 0 &&
		(int32_t)(millis() - s_outbound_backoff_until) < 0;
}

static void outbound_note_result(bool ok) {
	if (ok) {
		s_outbound_fail_streak = 0;
		s_outbound_backoff_until = 0;
	} else if (s_outbound_fail_streak < 255) {
		if (++s_outbound_fail_streak >= OUTBOUND_FAIL_STREAK_MAX) {
			s_outbound_backoff_until = millis() + OUTBOUND_BACKOFF_MS;
			if (s_outbound_backoff_until == 0) s_outbound_backoff_until = 1; // avoid the 0 sentinel
		}
	}
}

#define PUSH_TOPIC_LEN	120
#define PUSH_PAYLOAD_LEN TMP_BUFFER_SIZE

#if defined(ESP8266) || defined(ESP32) || defined(OSPI) || defined(OSBO)
#if defined(ESP8266) || defined(ESP32)
extern bool useEth;
#endif

// Append a JSON-escaped copy of src into dst (bounded). Control characters are
// dropped; quotes and backslashes are escaped so the body stays valid JSON.
static void push_json_escape(char* dst, size_t dstlen, const char* src, size_t maxsrc) {
	size_t di = 0;
	for (size_t si = 0; src[si] && si < maxsrc && di + 2 < dstlen; si++) {
		char c = src[si];
		if (c == '"' || c == '\\') {
			dst[di++] = '\\';
			dst[di++] = c;
		} else if ((unsigned char)c >= 0x20) {
			dst[di++] = c;
		}
	}
	dst[di] = 0;
}

// Parse an integer value for "key" from the stored config fragment (e.g. en).
static bool push_cfg_int(const char* cfg, const char* key, int* out) {
	char pat[20];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char* p = strstr(cfg, pat);
	if (!p) return false;
	p = strchr(p + strlen(pat), ':');
	if (!p) return false;
	*out = atoi(p + 1);
	return true;
}

// Parse a quoted string value for "key" from the stored config fragment (url).
static bool push_cfg_str(const char* cfg, const char* key, char* out, size_t outlen) {
	char pat[20];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char* p = strstr(cfg, pat);
	if (!p) return false;
	p = strchr(p + strlen(pat), ':');
	if (!p) return false;
	p++;
	while (*p == ' ' || *p == '\t') p++;
	if (*p != '"') return false;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < outlen) {
		if (*p == '\\' && *(p + 1)) p++;
		out[i++] = *p++;
	}
	out[i] = 0;
	return true;
}

// Firmware-initiated push. When enabled via SOPT_PUSH_OPTS ({"en":1,"url":...})
// the controller POSTs the just-logged notification event to the external push
// forwarder itself, so real push works without OTC (e.g. on the same LAN or
// right after a reboot). Ownership is proven by the device password hash (the
// same value the app knows as pw); the forwarder stores only its sha256.
static void push_forward_event(uint32_t type, uint32_t lval, float fval, uint8_t bval, uint32_t event_id) {
	DEBUG_PRINTF("push: enter event id=%lu type=%lu\n", (unsigned long)event_id, (unsigned long)type);
	// Reuse tmp_buffer (free at this point in push_message) as scratch instead of
	// permanent static buffers, so RAM is returned to the heap when push is idle.
	os.sopt_load(SOPT_PUSH_OPTS, tmp_buffer, TMP_BUFFER_SIZE - 1);
	if (tmp_buffer[0] == 0) { DEBUG_PRINTLN(F("push: SKIP - SOPT_PUSH_OPTS empty (not configured)")); return; } // not configured -> disabled (privacy: opt-in only)

	// Parse the tiny {en,url} fragment manually to avoid a heap-allocating JSON
	// document on the RAM-tight ESP8266.
	int en = 0;
	if (!push_cfg_int(tmp_buffer, "en", &en) || !en) { DEBUG_PRINTF("push: SKIP - disabled (en=%d) cfg=%s\n", en, tmp_buffer); return; }

	char urlbuf[160];
	if (!push_cfg_str(tmp_buffer, "url", urlbuf, sizeof(urlbuf)) || urlbuf[0] == 0) {
		strncpy(urlbuf, DEFAULT_PUSH_URL, sizeof(urlbuf) - 1);
		urlbuf[sizeof(urlbuf) - 1] = 0;
	}
	const char* url = urlbuf;

	// Parse the URL into scheme/host/port/path.
	bool usessl = false;
	const char* rest = url;
	if (strncmp(rest, "https://", 8) == 0) { usessl = true; rest += 8; }
	else if (strncmp(rest, "http://", 7) == 0) { usessl = false; rest += 7; }
	else { DEBUG_PRINTF("push: SKIP - unsupported scheme url=%s\n", url); return; } // unsupported scheme

	char host[96];
	char path[128];
	const char* slash = strchr(rest, '/');
	size_t hostlen = slash ? (size_t)(slash - rest) : strlen(rest);
	if (hostlen == 0 || hostlen >= sizeof(host)) { DEBUG_PRINTF("push: SKIP - bad host len=%u\n", (unsigned)hostlen); return; }
	memcpy(host, rest, hostlen); host[hostlen] = 0;
	if (slash) { strncpy(path, slash, sizeof(path) - 1); path[sizeof(path) - 1] = 0; }
	else { strcpy(path, "/"); }

	uint16_t port = usessl ? 443 : 80;
	char* colon = strchr(host, ':');
	if (colon) { *colon = 0; int p = atoi(colon + 1); if (p > 0) port = (uint16_t)p; }

	// Never OOM the controller mid-watering: skip the push when heap is critically low.
	// The event is still recorded in /nl for the app to poll.
#if defined(ESP8266)
	DEBUG_PRINTF("push: ESP8266 freeheap=%u maxblk=%u ssl=%d\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(), (int)usessl);
	if (ESP.getFreeHeap() < 3500 || ESP.getMaxFreeBlockSize() < 2000) {
		DEBUG_PRINTLN(F("push: SKIP - low heap"));
		return;
	}
	// TLS (BearSSL) needs ~11KB free plus a large contiguous block. Attempting it
	// on a low/fragmented heap forces the free_tmp_memory()/restore_tmp_memory()
	// dance (suspend+re-init MQTT/sensors/InfluxDB) which itself OOM-crashes here.
	// The forwarder accepts plain-HTTP POST on port 80, so downgrade to HTTP when
	// there isn't ample TLS headroom — this avoids the memory dance entirely.
	if (usessl && (ESP.getFreeHeap() < 16000 || ESP.getMaxFreeBlockSize() < 9000)) {
		DEBUG_PRINTLN(F("push: TLS headroom too low -> HTTP on port 80"));
		usessl = false;
		if (port == 443) port = 80;
	}
#elif defined(ESP32)
	DEBUG_PRINTF("push: ESP32 freeheap=%u internal=%u ssl=%d\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (int)usessl);
	if (ESP.getFreeHeap() < 3500) {
		DEBUG_PRINTLN(F("push: SKIP - low heap"));
		return;
	}
#endif

	DEBUG_PRINTF("push: url=%s -> host=%s port=%u path=%s ssl=%d\n", url, host, port, path, (int)usessl);

	// device_key = the same MAC the controller reports in /jc ("mac").
	unsigned char mac[6] = {0};
	#if defined(ARDUINO)
	os.load_hardware_mac(mac, useEth);
	#else
	os.load_hardware_mac(mac, true); // matches the "mac" reported by /jc on OSPi
	#endif
	char device_key[13];
	snprintf_P(device_key, sizeof(device_key), PSTR("%02X%02X%02X%02X%02X%02X"),
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	// auth = the stored device password hash (md5), identical to the app's pw.
	// Zero-init: on ESP8266 an unset/short SOPT_PASSWORD could otherwise leave the
	// buffer as uninitialized stack (leaking earlier scratch like the push cfg
	// fragment into the JSON body, corrupting it -> forwarder returns 400).
	char auth[40];
	memset(auth, 0, sizeof(auth));
	os.sopt_load(SOPT_PASSWORD, auth, sizeof(auth) - 1);
	if (auth[0] == 0) { DEBUG_PRINTLN(F("push: SKIP - SOPT_PASSWORD empty")); return; }
	// The auth value must be a hex password hash. If it contains JSON/quote chars
	// (e.g. leaked scratch), a raw insert would break the JSON body -> reject.
	for (size_t ai = 0; auth[ai]; ai++) {
		if (auth[ai] == '"' || auth[ai] == '\\' || auth[ai] == '{' || auth[ai] == ',') {
			DEBUG_PRINTF("push: SKIP - SOPT_PASSWORD not a valid hash ('%s')\n", auth);
			return;
		}
	}

	char text[128];
	notif_render_text(type, lval, fval, bval, text, sizeof(text));
	char text_esc[160];
	push_json_escape(text_esc, sizeof(text_esc), text, sizeof(text));

	uint8_t prio = notif_priority(type);

	// Build the JSON body into tmp_buffer, then the HTTP request into ether_buffer.
	snprintf_P(tmp_buffer, TMP_BUFFER_SIZE,
		PSTR("{\"device_key\":\"%s\",\"auth\":\"%s\",\"id\":%lu,\"type\":%lu,\"prio\":%u,\"text\":\"%s\"}"),
		device_key, auth, (unsigned long)event_id, (unsigned long)type, (unsigned)prio, text_esc);

	// NOTE: the BufferFiller capacity must be the ether_buffer size, NOT
	// TMP_BUFFER_SIZE (320) — otherwise longer requests are truncated mid-body,
	// producing invalid JSON that the push forwarder rejects with HTTP 400.
	BufferFiller bf = BufferFiller(ether_buffer, ETHER_BUFFER_SIZE);
	bf.emit_p(PSTR("POST $S HTTP/1.0\r\n"
					"Host: $S\r\n"
					"User-Agent: $S\r\n"
					"Accept: */*\r\n"
					"Content-Length: $D\r\n"
					"Content-Type: application/json\r\n\r\n$S"),
					path, host, user_agent_string, strlen(tmp_buffer), tmp_buffer);

	// Synchronous send: block until the push forwarder has been contacted.
	DEBUG_PRINTF("push: sending %u body bytes to %s:%u ssl=%d\n", (unsigned)strlen(tmp_buffer), host, port, (int)usessl);
	int8_t rc = os.send_http_request(host, port, ether_buffer, NULL, usessl, 5000, true);
	DEBUG_PRINTF("push: send_http_request rc=%d\n", (int)rc);
	// A connect error means the host was unreachable (the slow, main-loop-stalling
	// case). Any other result means we reached the forwarder -> internet is up.
	outbound_note_result(rc != HTTP_RQT_CONNECT_ERR);
}
#endif

void push_message(uint32_t type, uint32_t lval, float fval, uint8_t bval) {
	if (!is_notif_enabled(type)) {
		return;
	}
	// Record this event in the in-memory log so the mobile app can poll it (GET /nl)
	// and show a push/local notification. Reset to false when an event turns out to
	// be a non-event (e.g. a flow reading below the alert setpoint).
	bool log_this = true;
	char topic[PUSH_TOPIC_LEN+1];
	char payload[PUSH_PAYLOAD_LEN+1];
	char* postval = tmp_buffer+1; // +1 so we can fit a opening { before the loaded config

	// check if ifttt key exists and also if the enable bit is set
	os.sopt_load(SOPT_IFTTT_KEY, tmp_buffer);
	bool ifttt_enabled = (strlen(tmp_buffer)!=0);
	float flow_volume_per_pulse = os.get_flow_volume_per_pulse();

#define DEFAULT_EMAIL_PORT	465

	// parse email variables
	#if defined(SUPPORT_EMAIL)
	// define email variables
	String email_host;
	String email_username;
	String email_login;
	String email_password;
	String email_recipient;
	int  email_port = DEFAULT_EMAIL_PORT;
	int  email_en = 0;
#if defined(ESP8266)
	// ESP8266: single heap block instead of ~965 B of permanent DRAM.
	// We free this early after parsing to avoid fragmenting the heap during the TLS memory check.
	char *email_buf = (char*)malloc(2 * (MAX_SOPTS_SIZE + 1) + (MAX_SOPTS_SIZE + 3));
	char *saved_email_config = email_buf;
	char *email_config = email_buf ? email_buf + (MAX_SOPTS_SIZE + 1) : NULL;
	char *email_json   = email_buf ? email_buf + 2 * (MAX_SOPTS_SIZE + 1) : NULL;
	bool email_buf_ok = (email_buf != NULL);
#else
	static PSRAM_BSS_ATTR char saved_email_config[MAX_SOPTS_SIZE + 1];
	static PSRAM_BSS_ATTR char email_config[MAX_SOPTS_SIZE + 1];
	static PSRAM_BSS_ATTR char email_json[MAX_SOPTS_SIZE + 3];
	bool email_buf_ok = true;
#endif

	if (email_buf_ok) {
		os.sopt_load(SOPT_EMAIL_OPTS, saved_email_config);
		strcpy(email_config, saved_email_config);
		if (!normalize_json_object_fragment(email_config, MAX_SOPTS_SIZE + 1)) {
			email_config[0] = 0;
		}
		if (strcmp(saved_email_config, email_config) != 0) {
			os.sopt_save(SOPT_EMAIL_OPTS, email_config);
		}

		if (email_config[0] != 0) {
			size_t len = strlen(email_config);
			memmove(email_json + 1, email_config, len + 1);
			email_json[0] = '{';
			email_json[len + 1] = '}';
			email_json[len + 2] = 0;

			ArduinoJson::JsonDocument doc;
			ArduinoJson::DeserializationError error = ArduinoJson::deserializeJson(doc, email_json);
			// Test the parsing otherwise parse
			if (error) {
				DEBUG_PRINT(F("email: deserializeJson() failed: "));
				DEBUG_PRINTLN(error.c_str());
			} else {
				email_en = doc["en"];
				email_host = doc["host"] | "smtp.gmail.com";
				email_port = doc["port"];
				email_username = doc["user"] | "";
				email_login = doc["login"] | email_username;
				email_password = doc["pass"] | "";
				email_recipient = doc["recipient"] | "";
			}
		}
	}
#if defined(ESP8266)
	free(email_buf); // free early to prevent heap fragmentation
#endif
	#endif

	#if defined(ESP8266) || defined(ESP32)
		EMailSender::EMailMessage email_message;
	#else
		struct {
			String subject;
			String message;
		} email_message;
	#endif

	bool email_enabled = false;
	bool html_email_set = false;
	bool influxdb_enabled = os.influxdb.isEnabled();
	const char *sval = NULL;
#if defined(SUPPORT_EMAIL)
	if(!email_en){  // todo: this should be simplified
		email_enabled = false;
	}else{
		email_enabled = true;
	}
#endif

	// NOTE: Do NOT return here when no IFTTT/Email/MQTT channel is configured.
	// The in-memory notification log (/nl) is the mobile app's own delivery
	// channel (local/push notifications) and must be populated independently of
	// the other channels. All delivery below is individually guarded
	// (ifttt_enabled / email_enabled / os.mqtt.enabled()), so falling through is
	// safe and the event still reaches notif_log_add() at the end.

	if (ifttt_enabled || email_enabled) {
		strcpy_P(postval, PSTR("{\"value1\":\"On site ["));
		os.sopt_load(SOPT_DEVICE_NAME, topic, PUSH_TOPIC_LEN);
		topic[PUSH_TOPIC_LEN]=0;
		strcat(postval+strlen(postval), topic);
		strcat_P(postval, PSTR("], "));
		if(email_enabled) {		
			strcat(topic, " ");
			email_message.subject = topic; // prefix the email subject with device name
		}
	}

	if (os.mqtt.enabled()) {
		topic[0] = 0;
		payload[0] = 0;
	}

	switch(type) {
		case  NOTIFY_STATION_ON:

			if (os.mqtt.enabled()) {
				snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("station/%d"), lval);
				strcat_P(payload, PSTR("{\"state\":1"));
				if((int)fval > 0){
					snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR(",\"duration\":%d"), (int)fval);
				}
				strcat_P(payload, PSTR("}"));
			}
			if (ifttt_enabled || email_enabled) {
				strcat_P(postval, PSTR("Station ["));
				os.get_station_name(lval, postval+strlen(postval));
				strcat_P(postval, PSTR("] just turned on."));
				if((int)fval > 0){
					strcat_P(postval, PSTR(" It's scheduled to run for "));
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" %d minutes %d seconds."), (int)fval/60, (int)fval%60);
				}
				if(email_enabled) { email_message.subject += PSTR("station event"); }
			}			break;

		case NOTIFY_STATION_OFF: {

			if (os.mqtt.enabled()) {
				snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("station/%d"), lval);
				strcat_P(payload, PSTR("{\"state\":0"));
				if((int)fval > 0) {
					snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR(",\"duration\":%d"), (int)fval);
					if (os.iopts[IOPT_SENSOR1_TYPE]==SENSOR_TYPE_FLOW) {
						float gpm = flow_last_gpm * flow_volume_per_pulse;
						#if defined(OS_AVR)
						snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR(",\"flow\":%d.%02d"), (int)gpm, (int)(gpm*100)%100);
						#else
						snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR(",\"flow\":%.2f"), gpm);
						#endif
					}
				}
				strcat_P(payload, PSTR("}"));
			}
			if (ifttt_enabled || email_enabled) {
				strcat_P(postval, PSTR("Station ["));
				os.get_station_name(lval, postval+strlen(postval));
				strcat_P(postval, PSTR("] closed."));
				if((int)fval > 0) {
					strcat_P(postval, PSTR(" It ran for "));
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" %d minutes %d seconds."), (int)fval/60, (int)fval%60);
				}

				if(os.iopts[IOPT_SENSOR1_TYPE]==SENSOR_TYPE_FLOW) {
					float gpm = flow_last_gpm * flow_volume_per_pulse;
					#if defined(OS_AVR)
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" Flow rate: %d.%02d"), (int)gpm, (int)(gpm*100)%100);
					#else
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" Flow rate: %.2f"), gpm);
					#endif
				}
				if(email_enabled) { email_message.subject += PSTR("station event"); }
			}

			break;
		}

		case NOTIFY_FLOW_ALERT:{
			//First determine if a Flow Alert should be sent based on flow amount and setpoint

			//Added variable to track flow alert status
			bool flow_alert_flag = false;

			//Added variable for flow_gpm_alert_setpoint and set default value to max
			float flow_gpm_alert_setpoint = 999.9f;
			
			//Added variable for flow_gpm_alert_setpoint 
			uint16_t fasp = os.get_flow_alert_setpoint(lval);
			if (fasp > 0) {
				flow_gpm_alert_setpoint = (float)(fasp) / 100.0f;
				if ((flow_last_gpm * flow_volume_per_pulse) > flow_gpm_alert_setpoint) {
					flow_alert_flag = true;
				}
			}

			//Added variable for tmp station name
			char tmp_station_name[STATION_NAME_SIZE];

			//Get station name
			os.get_station_name(lval, tmp_station_name);

			// Backward compatibility: if no explicit setpoint is configured, try legacy station-name suffix.
			if (fasp == 0 && flow_last_gpm > 0 && strlen(tmp_station_name) > 5) {
				const char *station_name_last_five_chars = tmp_station_name;
				// extract the last 5 characters
				station_name_last_five_chars = tmp_station_name + strlen(tmp_station_name) - 5;
				// Convert last five characters to number and check if valid
				// Had to switch to use strtod because sscanf in AVR doesn't work with float :(
				char *endptr;
				flow_gpm_alert_setpoint = strtod(station_name_last_five_chars, &endptr);
				if (endptr != station_name_last_five_chars) {
					//station_name_last_five_chars was successfully converted to a number 
					//flow_last_gpm is actually collected and stored as pulses per minute, not gallons per minute
					// Alert Check - Compare flow_gpm_alert_setpoint with flow_last_gpm and enable flow_alert_flag if flow is above setpoint
					if ((flow_last_gpm * flow_volume_per_pulse) > flow_gpm_alert_setpoint) {
					flow_alert_flag = true;
				}
				} else {
					// Could not convert suffix to a valid number: keep flow_alert_flag false.
					flow_alert_flag = false;
				}
			}

			// If flow_alert_flag is true, format the appropriate messages, else don't send alert
			if (flow_alert_flag == true) {

				if (os.mqtt.enabled()) {
					//Format mqtt message
					snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("station/%d/alert/flow"), lval);
					float gpm = flow_last_gpm * flow_volume_per_pulse;
					#if defined(OS_AVR)
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"flow_rate\":%d.%02d,\"duration\":%d,\"alert_setpoint\":%d.%02d}"), (int)gpm, (int)(gpm*100)%100,
					(int)fval, (int)flow_gpm_alert_setpoint, (int)(flow_gpm_alert_setpoint*100)%100);
					#else
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"flow_rate\":%.2f,\"duration\":%d,\"alert_setpoint\":%.4f}"), gpm, (int)fval, flow_gpm_alert_setpoint);
					#endif
				}


				if (ifttt_enabled || email_enabled) {
					//Format ifttt\email message

					// Get and format current local time as "YYYY-MM-DD hh:mm:ss AM/PM"
					strcat_P(postval, PSTR("at "));
					time_os_t curr_time = os.now_tz();
					#if defined(ARDUINO)
					tmElements_t tm;
					breakTime(curr_time, tm);
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
						1970+tm.Year, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
					#else
					struct tm *ti = gmtime(&curr_time);
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
						ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
					#endif

					strcat_P(postval, PSTR(", Station ["));
					//Truncate flow setpoint value off station name to shorten ifttt\email message
					tmp_station_name[(strlen(tmp_station_name) - 5)] = '\0';
					strcat_P(postval, tmp_station_name);
					strcat_P(postval, PSTR("]"));
					if(fval > 0){ // if there is a valid duration
						strcat_P(postval, PSTR(" ran for "));
						snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("%d minutes %d seconds."), (int)fval/60, ((int)fval%60));
					}

					strcat_P(postval, PSTR(" FLOW ALERT!"));
					float gpm = flow_last_gpm * flow_volume_per_pulse;
					#if defined(OS_AVR)
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" | Flow rate: %d.%02d > Flow alert setpoint: %d.%02d"),
						(int)gpm, (int)(gpm*100)%100, (int)flow_gpm_alert_setpoint, (int)(flow_gpm_alert_setpoint*100)%100);
					#else
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" | Flow rate: %.2f > Flow alert setpoint: %.4f"),
						gpm, flow_gpm_alert_setpoint);
					#endif

					if(email_enabled) { email_message.subject += PSTR("- FLOW ALERT"); }

				}
			} else {
				//Do not send an alert.  Flow was not above setpoint or setpoint not valid. 
				//Must force ifftt_enabled and email_enabled to false to prevent sending
				//Can not force os.mqtt.enabled() off, but it will not publish an mqtt message as topic\payload will be empty.
				ifttt_enabled=false;
				email_enabled=false;
				log_this=false; // below setpoint: not a real flow alert, don't log for the app
			}
		break;
		}
 
		case NOTIFY_PROGRAM_SCHED:
			if (os.mqtt.enabled()) {
				snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("program/%d"), lval);
				if(fval<0) {
					strcat_P(payload, PSTR("{\"state\":\"skipped\",\"wtrestr\":"));
					snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR("%d"), (int)bval); // if a program is skipped, also output the wt_restricted variable
				} else {
					strcat_P(payload, PSTR("{\"state\":1,\"wl\":"));
					snprintf_P(payload+strlen(payload), PUSH_PAYLOAD_LEN, PSTR("%d"), (int)fval);
				}
				strcat_P(payload, PSTR("}"));
			}
			if (ifttt_enabled || email_enabled) {
				if(fval<0) {
					strcat_P(postval, PSTR("skipped"));
					if(bval>0) {
						strcat_P(postval, PSTR(" due to weather restriction."));
					}
				} else {
					if (bval) strcat_P(postval, PSTR("manually scheduled "));
					else strcat_P(postval, PSTR("automatically scheduled "));
				}
				{
					ProgramStruct prog;
					pd.read(lval, &prog);
					if(lval<pd.nprograms) strcat(postval, prog.name);
				}
				if(fval>0) {
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" with %d%% water level."), (int)fval);
				}

				if(email_enabled) { email_message.subject += PSTR("program event"); }
			}
			break;

		case NOTIFY_PROGRAM_END:
			if (os.mqtt.enabled()) {
				snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("program/%d"), lval);
				strcat_P(payload, PSTR("{\"state\":\"finished\"}"));
			}
			if (ifttt_enabled || email_enabled) {
				ProgramStruct prog;
				pd.read(lval, &prog);
				if(lval < pd.nprograms) {
					strcat_P(postval, PSTR("program "));
					strcat(postval, prog.name);
					strcat_P(postval, PSTR(" finished."));
				}
				if(email_enabled) { email_message.subject += PSTR("program event"); }
			}
			break;

		case NOTIFY_SENSOR1:

			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("sensor1"));
				snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"state\":%d}"), (int)fval);
			}
			if (ifttt_enabled || email_enabled) {
				strcat_P(postval, PSTR("sensor 1 "));
				strcat_P(postval, ((int)fval)?PSTR("activated."):PSTR("de-activated."));
				if(email_enabled) { email_message.subject += PSTR("sensor 1 event"); }
			}
			break;

		case NOTIFY_SENSOR2:

			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("sensor2"));
				snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"state\":%d}"), (int)fval);
			}
			if (ifttt_enabled || email_enabled) {
				strcat_P(postval, PSTR("sensor 2 "));
				strcat_P(postval, ((int)fval)?PSTR("activated."):PSTR("de-activated."));
				if(email_enabled) { email_message.subject += PSTR("sensor 2 event"); }
			}
			break;

		case NOTIFY_RAINDELAY:

			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("raindelay"));
				snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"state\":%d}"), (int)fval);
			}
			if (ifttt_enabled || email_enabled) {
				strcat_P(postval, PSTR("rain delay "));
				strcat_P(postval, ((int)fval)?PSTR("activated."):PSTR("de-activated."));
				if(email_enabled) { email_message.subject += PSTR("rain delay event"); }
			}
			break;

		case NOTIFY_FLOWSENSOR:
			{
				float vol = lval * flow_volume_per_pulse;
				if (os.mqtt.enabled()) {
					strcpy_P(topic, PSTR("sensor/flow"));
					#if defined(OS_AVR)
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"count\":%d,\"volume\":%d.%02d}"), (int)lval, (int)vol, (int)(vol*100)%100);
					#else
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"count\":%d,\"volume\":%.2f}"), (int)lval, vol);
					#endif
				}
				if (ifttt_enabled || email_enabled) {
					#if defined(OS_AVR)
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("Flow count: %d, volume: %d.%02d"), (int)lval, (int)vol, (int)(vol*100)%100);
					#else
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("Flow count: %d, volume: %.2f"), (int)lval, vol);
					#endif
					if(email_enabled) { email_message.subject += PSTR("flow sensor event"); }
				}
			}
			break;

		case NOTIFY_CURR_ALERT:
			{
				int16_t curr = (int16_t)fval;
				int16_t imin = os.get_imin();
				int16_t imax = os.get_imax();
				if (os.mqtt.enabled()) {
					//Format mqtt message
					switch(bval) {
						case CURR_ALERT_TYPE_UNDER:
						case CURR_ALERT_TYPE_OVER_STATION:
							snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("station/%d/alert/curr"), lval);
							if(bval==CURR_ALERT_TYPE_UNDER)
								snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"curr_value\":%d,\"imin_threshold\":%d}"), curr, imin);
							else
								snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"curr_value\":%d,\"imax_limit\":%d}"), curr, (imax+OVERCURRENT_INRUSH_EXTRA));
							break;
						case CURR_ALERT_TYPE_OVER_SYSTEM:
							strcpy_P(topic, PSTR("overcurrent"));
							snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"curr_value\":%d,\"imax_limit\":%d}"), curr, imax);
							break;
					}
				}
				if (ifttt_enabled || email_enabled) {
					//Format ifttt\email message

					// Get and format current local time as "YYYY-MM-DD hh:mm:ss AM/PM"
					strcat_P(postval, PSTR("at "));
					time_os_t curr_time = os.now_tz();
					#if defined(ARDUINO)
					tmElements_t tm;
					breakTime(curr_time, tm);
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
						1970+tm.Year, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
					#else
					struct tm *ti = gmtime(&curr_time);
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
						ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
					#endif

					if(bval==CURR_ALERT_TYPE_UNDER || bval==CURR_ALERT_TYPE_OVER_STATION) {
						// the current alert is associated with a specific station
						char tmp_station_name[STATION_NAME_SIZE];
						os.get_station_name(lval, tmp_station_name);
						strcat_P(postval, PSTR(", Station ["));
						strcat_P(postval, tmp_station_name);
						strcat_P(postval, PSTR("]"));
					} else {
						strcat_P(postval, PSTR(", System"));
					}

					switch(bval) {
						case CURR_ALERT_TYPE_UNDER:
							strcat_P(postval, PSTR(" UNDERCURRENT detected!"));
							snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" | %dmA < imin threshold: %dmA"),
								curr, imin);
							break;
						case CURR_ALERT_TYPE_OVER_STATION:
						case CURR_ALERT_TYPE_OVER_SYSTEM:
							strcat_P(postval, PSTR(" OVERCURRENT detected!"));
							snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR(" | %dmA > imax limit: %dmA. The affected station(s) have been closed."),
								curr, imax+((bval==CURR_ALERT_TYPE_OVER_STATION)?OVERCURRENT_INRUSH_EXTRA:0));
							break;
					}

					if(email_enabled) { email_message.subject += PSTR("- CURRENT ALERT"); }
				}
			}
			break;

		case NOTIFY_WEATHER_UPDATE:

			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("weather"));
				snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"water level\":%d}"), (int)fval);
			}
			if (ifttt_enabled || email_enabled) {
				if(lval>0) {
					strcat_P(postval, PSTR("external IP updated: "));
					unsigned char ip[4] = {(unsigned char)((lval>>24)&0xFF),
									(unsigned char)((lval>>16)&0xFF),
									(unsigned char)((lval>>8)&0xFF),
									(unsigned char)(lval&0xFF)};
					ip2string(postval, TMP_BUFFER_SIZE, ip);
				}
				if(fval>=0) {
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("water level updated: %d%%."), (int)fval);
				}
				if(email_enabled) { email_message.subject += PSTR("weather update event"); }
			}
			break;

		case NOTIFY_REBOOT:
			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("system"));
				snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"state\":\"started\",\"cause\":%d}"), (int)os.last_reboot_cause);
			}
			if (ifttt_enabled || email_enabled) {
				#if defined(ARDUINO)
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE, PSTR("rebooted. Cause: %d. Device IP: "), os.last_reboot_cause);
					#if defined(ESP8266) || defined(ESP32)
					{
						IPAddress _ip;
						if (useEth) {
							//_ip = Ethernet.localIP();
							_ip = eth.localIP();
						} else {
							_ip = WiFi.localIP();
						}
						unsigned char ip[4] = {_ip[0], _ip[1], _ip[2], _ip[3]};
						ip2string(postval, TMP_BUFFER_SIZE, ip);
					}
					#if defined(ESP8266)
						if (os.last_reboot_cause == REBOOT_CAUSE_POWERON) {
							strncat_P(postval, PSTR(". Unexpected reboot detected."), TMP_BUFFER_SIZE - strlen(postval) - 1);
						}
						append_esp8266_reboot_diag(postval, TMP_BUFFER_SIZE);
					#endif
					#else
						ip2string(postval, TMP_BUFFER_SIZE, &(Ethernet.localIP()[0]));
					#endif
				#else
					strcat_P(postval, PSTR("controller process restarted."));
				#endif
				if(email_enabled) { email_message.subject += PSTR("reboot event"); }
			}
			break;

		case NOTIFY_MONTHLY_REPORT:
			{
				// lval = flow pulse count, fval = volume in liters
				uint16_t ym = 0;
				if(os.mwdata.nrecords > 0) {
					ym = os.mwdata.records[os.mwdata.nrecords - 1].ym;
				}
				uint16_t rpt_year = ym / 12;
				uint8_t rpt_month = (ym % 12) + 1;

				if (os.mqtt.enabled()) {
					strcpy_P(topic, PSTR("monthly_water"));
					#if defined(OS_AVR)
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"year\":%d,\"month\":%d,\"count\":%lu,\"volume\":%d.%02d}"),
						rpt_year, rpt_month, (ulong)lval, (int)fval, (int)(fval*100)%100);
					#else
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"year\":%d,\"month\":%d,\"count\":%lu,\"volume\":%.2f}"),
						rpt_year, rpt_month, (ulong)lval, fval);
					#endif
				}
				if (ifttt_enabled) {
					#if defined(OS_AVR)
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE,
						PSTR("Monthly water report %d/%02d: flow count %lu, volume %d.%02d L"),
						rpt_year, rpt_month, (ulong)lval, (int)fval, (int)(fval*100)%100);
					#else
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE,
						PSTR("Monthly water report %d/%02d: flow count %lu, volume %.2f L"),
						rpt_year, rpt_month, (ulong)lval, fval);
					#endif
				}
				if(email_enabled) {
					email_message.subject += PSTR("monthly water report");
					#if !defined(OS_AVR)
					// Generate HTML email report
					{
						static const char* const mon_names[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
						float volume_per_pulse = os.get_flow_volume_per_pulse();
						char dname[32];
						os.sopt_load(SOPT_DEVICE_NAME, dname, 31);
						dname[31] = 0;
						char buf[320];

						String html;
						html.reserve(1024);
						html += F("<!DOCTYPE html><html><body style=\"font-family:Arial,Helvetica,sans-serif;color:#333;margin:0;padding:20px;\">");
						html += F("<div style=\"max-width:600px;margin:0 auto;\">");
						html += F("<h2 style=\"color:#2c7be5;border-bottom:2px solid #2c7be5;padding-bottom:8px;\">");
						html += F("Monthly Water Report</h2>");
						snprintf(buf, sizeof(buf), "<p><b>Device:</b> %s</p>", dname);
						html += buf;
						if(rpt_month >= 1 && rpt_month <= 12) {
							snprintf(buf, sizeof(buf), "<p><b>Report for:</b> %s %d</p>", mon_names[rpt_month-1], rpt_year);
						} else {
							snprintf(buf, sizeof(buf), "<p><b>Report for:</b> %d/%02d</p>", rpt_year, rpt_month);
						}
						html += buf;

						// Summary of last month
						snprintf(buf, sizeof(buf),
							"<div style=\"background:#e8f4fd;padding:12px;border-radius:6px;margin:12px 0;\">"
							"<b>Last month total:</b> Flow pulses: %lu &mdash; Volume: %.2f L</div>",
							(unsigned long)lval, fval);
						html += buf;

						// Table of all recorded months
						html += F("<table style=\"border-collapse:collapse;width:100%;margin-top:12px;\">");
						html += F("<tr style=\"background:#2c7be5;color:#fff;\">"
							"<th style=\"padding:8px 12px;text-align:left;border:1px solid #ddd;\">Month</th>"
							"<th style=\"padding:8px 12px;text-align:right;border:1px solid #ddd;\">Flow Pulses</th>"
							"<th style=\"padding:8px 12px;text-align:right;border:1px solid #ddd;\">Volume (L)</th></tr>");

						for(uint8_t i = 0; i < os.mwdata.nrecords; i++) {
							uint16_t m_ym = os.mwdata.records[i].ym;
							uint16_t y = m_ym / 12;
							uint8_t m = m_ym % 12; // 0-based month
							float vol = os.mwdata.records[i].flow_count * volume_per_pulse;
							bool is_last = (i == os.mwdata.nrecords - 1);
							const char *bg = is_last ? "#e8f4fd" : (i % 2 ? "#f9f9f9" : "#fff");
							snprintf(buf, sizeof(buf),
								"<tr style=\"background:%s;\">"
								"<td style=\"padding:6px 12px;border:1px solid #ddd;\">%s %d</td>"
								"<td style=\"padding:6px 12px;border:1px solid #ddd;text-align:right;\">%lu</td>"
								"<td style=\"padding:6px 12px;border:1px solid #ddd;text-align:right;\">%.2f</td></tr>",
								bg, (m < 12 ? mon_names[m] : "?"), y,
								(unsigned long)os.mwdata.records[i].flow_count, vol);
							html += buf;
						}
						html += F("</table>");

						// Current month running total
						float curr_vol = os.mwdata.curr_flow * volume_per_pulse;
						uint16_t cy = os.mwdata.curr_ym / 12;
						uint8_t cm = os.mwdata.curr_ym % 12;
						snprintf(buf, sizeof(buf),
							"<p style=\"margin-top:16px;padding:10px;background:#f0f0f0;border-radius:4px;\">"
							"<b>Current month (%s %d):</b> %lu pulses &mdash; %.2f L so far</p>",
							(cm < 12 ? mon_names[cm] : "?"), cy,
							(unsigned long)os.mwdata.curr_flow, curr_vol);
						html += buf;

						html += F("<p style=\"color:#999;font-size:0.85em;margin-top:20px;\">This report is generated automatically by OpenSprinkler on the 1st of each month.</p>");
						html += F("</div></body></html>");
						email_message.message = html;
						html_email_set = true;
					}
					#endif
				}
			}
			break;

		case NOTIFY_NOFLOW:
			{
				char sname[STATION_NAME_SIZE];
				os.get_station_name(lval, sname);
				if (os.mqtt.enabled()) {
					snprintf_P(topic, PUSH_TOPIC_LEN, PSTR("station/%d/alert/noflow"), lval);
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"station\":%d,\"name\":\"%s\"}"), lval, sname);
				}
				if (ifttt_enabled || email_enabled) {
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE,
						PSTR("No flow detected: station [%s] is open but flow sensor reports no water flow"), sname);
					if(email_enabled) { email_message.subject += PSTR("- NO FLOW ALERT"); }
				}
			}
			break;

		case NOTIFY_PIPE_BURST:
			{
				if (os.mqtt.enabled()) {
					strcpy_P(topic, PSTR("alert/pipe_burst"));
					snprintf_P(payload, PUSH_PAYLOAD_LEN, PSTR("{\"flow_pulses\":%lu}"), (ulong)lval);
				}
				if (ifttt_enabled || email_enabled) {
					snprintf_P(postval+strlen(postval), TMP_BUFFER_SIZE,
						PSTR("Pipe burst warning: %lu flow pulses detected while all stations are closed"), (ulong)lval);
					if(email_enabled) { email_message.subject += PSTR("- PIPE BURST ALERT"); }
				}
			}
			break;
			
		case NOTIFY_MONITOR_LOW: 
		case NOTIFY_MONITOR_MID:
		case NOTIFY_MONITOR_HIGH:

			Monitor_t *mon = monitor_by_idx(bval);
			sval = (mon == NULL) ? NULL : monitor_by_idx(bval)->getName();
			if (os.mqtt.enabled()) {
				strcpy_P(topic, PSTR("monitoring"));
				int len = strlen(payload);
				snprintf_P(payload+len, PUSH_PAYLOAD_LEN-len, PSTR("{\"warning\":\"%s\",\"prio\":%u,\"value\":%d.%02d}"), sval, lval, (int)fval, (int)fval*100%100);
			}
			if (ifttt_enabled || email_enabled) {
				int len = strlen(postval);
				snprintf_P(postval+len, TMP_BUFFER_SIZE-len, PSTR("monitoring: Warning %s with priority %u current value %d.%02d"), sval, lval, (int)fval, (int)fval*100%100);
				if(email_enabled) { email_message.subject += PSTR("Warning"); }
			}
			break;

	}

	if (os.mqtt.enabled() && strlen(topic) && strlen(payload))
		os.mqtt.publish(topic, payload);

	// When the internet has been unreachable for several consecutive events, skip
	// the blocking online channels (IFTTT/Email/InfluxDB/push) so notif.run() does
	// not stall the main loop on dead connect timeouts. MQTT (typically a local
	// broker) and the /nl log below are unaffected.
	bool skip_online = outbound_backoff_active();

	if (ifttt_enabled && !skip_online) {
		strcat_P(postval, PSTR("\"}"));

		BufferFiller bf = BufferFiller(ether_buffer, TMP_BUFFER_SIZE);
		bf.emit_p(PSTR("POST /trigger/sprinkler/with/key/$O HTTP/1.0\r\n"
						"Host: $S\r\n"
						"User-Agent: $S\r\n"
						"Accept: */*\r\n"
						"Content-Length: $D\r\n"
						"Content-Type: application/json\r\n\r\n$S"),
						SOPT_IFTTT_KEY, DEFAULT_IFTTT_URL, user_agent_string, strlen(postval), postval);

		int8_t ifttt_rc = os.send_http_request(DEFAULT_IFTTT_URL, 80, ether_buffer, NULL);
		outbound_note_result(ifttt_rc != HTTP_RQT_CONNECT_ERR);
	}

	if(email_enabled && !skip_online){
		if(!html_email_set) {
			email_message.message = strchr(postval, 'O'); // ad-hoc: remove the value1 part from the ifttt message
			#if defined(ESP8266) || defined(ESP32)
				// Plain-text notifications: send as text/plain so line breaks are
				// preserved. The EMailMessage default (text/html) would wrap the
				// text in <html> and collapse newlines to a single line.
				email_message.mime = "text/plain";
			#endif
		}
		#if defined(ARDUINO)
			#if defined(ESP8266) || defined(ESP32)
				if(email_host.length()>0 && email_login.length()>0 && email_password.length()>0 && email_recipient.length()>0) { // make sure all are valid
					// TLS handshake headroom required before opening the SMTP connection.
					// ESP8266 (BearSSL, no PSRAM): needs ~8-10KB internal heap plus
					// fragmentation headroom. Require 16000 so the 75% maxblock check
					#if defined(ESP8266)
						const size_t email_mem_needed = 16000;
					#else
						const size_t email_mem_needed = 10000;
					#endif
					bool mem_ok = free_tmp_memory(email_mem_needed);
					#if defined(ESP32)
						// ESP32 handles TLS memory gracefully (or uses PSRAM). We use free_tmp_memory 
						// to proactively suspend MQTT/Influx if internal heap is tight, but we 
						// don't hard-block the email attempt if the strict contiguous check fails.
						mem_ok = true;
					#endif

					if (!mem_ok) {
						// Not enough contiguous heap to open a TLS connection right now
						// (typical during active watering on RAM-tight boards). Skip only
						// the SMTP send — do NOT return, so this event is still recorded in
						// the in-memory notification log (/nl) and pushed to InfluxDB below.
						// Returning here used to make program-start/station-off events vanish
						// entirely (no email, no app notification) whenever memory was tight.
						DEBUG_PRINTLN(F("Not enough memory to send email (event still logged)"));
						restore_tmp_memory(email_mem_needed);
					} else {
						DEBUG_PRINTLN(F("Sending email..."));
						EMailSender emailSend(email_login.c_str(), email_password.c_str(), email_username.c_str(), "OpenSprinkler");
						emailSend.setSMTPServer(email_host.c_str());
						emailSend.setSMTPPort(email_port);
						// Use EHLO (ESMTP) instead of the library default HELO. AUTH is an
						// ESMTP service extension that servers only advertise/enable after
						// EHLO; some providers (e.g. GMX, Zoho) reject "AUTH LOGIN" issued
						// after a plain HELO. The multi-line EHLO reply is parsed correctly
						// (final line detected via the "250 " vs "250-" indicator).
						emailSend.setEHLOCommand(true);
						EMailSender::Response resp = emailSend.send(email_recipient.c_str(), email_message);
						DEBUG_PRINTLN(F("Sending Status:"));
						DEBUG_PRINTLN(resp.status);
						DEBUG_PRINTLN(resp.code);
						DEBUG_PRINTLN(resp.desc);
						outbound_note_result(resp.status);
						restore_tmp_memory(email_mem_needed);
					}
				}
			#endif
		#else
			struct smtp *smtp = NULL;
			String email_port_str = to_string(email_port);
			smtp_status_code rc;
			if(email_host.length()>0 && email_login.length()>0 && email_password.length()>0 && email_recipient.length()>0) { // make sure all are valid
				rc = smtp_open(email_host.c_str(), email_port_str.c_str(), SMTP_SECURITY_TLS, SMTP_NO_CERT_VERIFY, NULL, &smtp);
				rc = smtp_auth(smtp, SMTP_AUTH_PLAIN, email_login.c_str(), email_password.c_str());
				rc = smtp_address_add(smtp, SMTP_ADDRESS_FROM, email_username.c_str(), "OpenSprinkler");
				rc = smtp_address_add(smtp, SMTP_ADDRESS_TO, email_recipient.c_str(), "User");
				rc = smtp_header_add(smtp, "Subject", email_message.subject.c_str());
				if(html_email_set) {
					rc = smtp_header_add(smtp, "Content-Type", "text/html; charset=UTF-8");
				}
				rc = smtp_mail(smtp, email_message.message.c_str());
				rc = smtp_close(smtp);
				outbound_note_result(rc == SMTP_STATUS_OK);
				if (rc!=SMTP_STATUS_OK) {
					DEBUG_PRINTF("SMTP: Error %s\n", smtp_status_code_errstr(rc));
				}
			}
		#endif
	}
	if (influxdb_enabled && !skip_online)
		os.influxdb.push_message(type, lval, fval, sval);

	if (log_this) {
		notif_log_add(type, lval, fval, bval);
		#if defined(ESP8266) || defined(ESP32) || defined(OSPI) || defined(OSBO)
		// After recording the event, push it out to the forwarder if the user
		// opted in. This delivers real push without OTC (LAN / post-reboot).
		if (!skip_online)
			push_forward_event(type, lval, fval, bval, notif_log_lastid());
		#endif
	}
}

