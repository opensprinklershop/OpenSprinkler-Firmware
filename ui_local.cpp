/* OpenSprinkler Unified Firmware
 *
 * On-device web UI: storage, HTTP delivery, signed package installation.
 * See ui_local.h for the layout and the rules. ESP32 (LittleFS, normally the
 * external 16 MB flash of the OS4 Pro) and OSPi (<data_dir>/ui). Not built
 * for the ESP8266.
 */
#include "ui_local.h"

#if defined(UI_LOCAL_SUPPORTED)

#include "OpenSprinkler.h"
#include "utils.h"
#include "online_update.h"
#include "ui_local_pubkey.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <new>

#if defined(ESP32)
	#include <FS.h>
	#include <LittleFS.h>
	#include <HTTPClient.h>
	#include <WiFiClient.h>
	#include <WiFiClientSecure.h>
	#include <esp_heap_caps.h>
	#include <mbedtls/sha256.h>
	#include <mbedtls/ecdsa.h>
	#include <mbedtls/ecp.h>
	#include <mbedtls/bignum.h>
	#include "psram_utils.h"
	#define UI_BASE_URL OTA_UPDATE_BASE_URL
#else // OSPI
	// OpenSSL 3 marks the SHA256_*/EC_KEY/ECDSA_do_* API deprecated; it is still
	// supported and far smaller to use here than the EVP equivalents.
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	#include <sys/stat.h>
	#include <sys/statvfs.h>
	#include <dirent.h>
	#include <unistd.h>
	#include <pthread.h>
	#include <time.h>
	#include <curl/curl.h>
	#include <openssl/sha.h>
	#include <openssl/ec.h>
	#include <openssl/ecdsa.h>
	#include <openssl/bn.h>
	#include <openssl/obj_mac.h>
	#define UI_BASE_URL "https://opensprinklershop.de/upgrade"
#endif

extern OpenSprinkler os;

/* ------------------------------------------------------------------------- */
/* State: the only static data of this module (rule 5 of the plan).          */
/* ------------------------------------------------------------------------- */
static char s_active[UI_LOCAL_VERSION_LEN] = "";   // version directory being served
static bool s_active_exact = false;                 // s_active == running firmware
static UiInstallState s_state = { UI_INST_IDLE, 0, 0, 0, "" };
static volatile bool s_busy = false;
static unsigned long s_boot_tz = 0;
static unsigned long s_next_auto = 0;
static uint8_t  s_auto_fail = 0;
static bool     s_gc_done = false;

#define UI_IO_BUF_SIZE 4096
#define UI_PATH_MAX    200
#define UI_BOOT_DELAY_S 120
#define UI_GC_DELAY_S   600

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */
static void ui_set_state(uint8_t st, uint8_t progress, const char *msg) {
	s_state.status = st;
	s_state.progress = progress;
	if (msg) {
		strncpy(s_state.message, msg, sizeof(s_state.message) - 1);
		s_state.message[sizeof(s_state.message) - 1] = 0;
	}
}

static void *ui_alloc(size_t n) {
#if defined(ESP32)
	void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!p) p = malloc(n);
	return p;
#else
	return malloc(n);
#endif
}

/** Absolute filesystem path of <rel> inside the UI store. rel may be "". */
static void ui_path(char *out, size_t n, const char *rel) {
#if defined(ESP32)
	if (rel && rel[0]) snprintf(out, n, "/ui/%s", rel);
	else snprintf(out, n, "/ui");
#else
	const char *dd = get_data_dir();
	size_t dl = strlen(dd);
	bool slash = dl && dd[dl - 1] == '/';
	if (rel && rel[0]) snprintf(out, n, "%s%sui/%s", dd, slash ? "" : "/", rel);
	else snprintf(out, n, "%s%sui", dd, slash ? "" : "/");
#endif
}

void ui_local_firmware_version(char *buf, size_t len) {
	snprintf(buf, len, "%d.%d.%d.%d", OS_FW_VERSION / 100, (OS_FW_VERSION / 10) % 10, OS_FW_VERSION % 10, OS_FW_MINOR);
}

void ui_local_default_url(char *buf, size_t len) {
	snprintf(buf, len, "%s/archive/v%u_%u/ui.tar", UI_BASE_URL, (unsigned)OS_FW_VERSION, (unsigned)OS_FW_MINOR);
}

/** "2.4.0.231" -> true. Digits and dots only, 2..3 dots, <= 15 chars. */
static bool ui_valid_version(const char *v) {
	size_t n = v ? strlen(v) : 0;
	if (n < 5 || n >= UI_LOCAL_VERSION_LEN) return false;
	int dots = 0;
	for (size_t i = 0; i < n; i++) {
		if (v[i] == '.') { dots++; if (i == 0 || i == n - 1 || v[i - 1] == '.') return false; }
		else if (v[i] < '0' || v[i] > '9') return false;
	}
	return dots == 2 || dots == 3;
}

/** Compare dotted versions numerically: <0, 0, >0. */
static int ui_version_cmp(const char *a, const char *b) {
	int pa[4] = {0, 0, 0, 0}, pb[4] = {0, 0, 0, 0};
	sscanf(a, "%d.%d.%d.%d", &pa[0], &pa[1], &pa[2], &pa[3]);
	sscanf(b, "%d.%d.%d.%d", &pb[0], &pb[1], &pb[2], &pb[3]);
	for (int i = 0; i < 4; i++) if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Filesystem abstraction (ESP32 LittleFS / OSPi POSIX)                      */
/* ------------------------------------------------------------------------- */
static bool fs_exists(const char *p) {
#if defined(ESP32)
	return LittleFS.exists(p);
#else
	struct stat st; return stat(p, &st) == 0;
#endif
}

static bool fs_is_dir(const char *p) {
#if defined(ESP32)
	File f = LittleFS.open(p);
	bool d = f && f.isDirectory();
	if (f) f.close();
	return d;
#else
	struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static long fs_file_size(const char *p) {
#if defined(ESP32)
	File f = LittleFS.open(p, "r");
	if (!f || f.isDirectory()) { if (f) f.close(); return -1; }
	long n = (long)f.size();
	f.close();
	return n;
#else
	struct stat st; if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) return -1; return (long)st.st_size;
#endif
}

static bool fs_mkdir(const char *p) {
#if defined(ESP32)
	return LittleFS.mkdir(p);
#else
	return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

/** Create all missing directories of the (absolute) file path, not the file. */
static bool fs_mkdir_parents(const char *file_path) {
	char tmp[UI_PATH_MAX];
	strncpy(tmp, file_path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	char *base = tmp;
	// never touch anything above the UI store
	char store[UI_PATH_MAX];
	ui_path(store, sizeof(store), "");
	size_t sl = strlen(store);
	if (strncmp(tmp, store, sl) != 0) return false;
	base = tmp + sl;
	for (char *p = base + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (!fs_is_dir(tmp) && !fs_mkdir(tmp)) return false;
			*p = '/';
		}
	}
	return true;
}

static bool fs_rename(const char *from, const char *to) {
#if defined(ESP32)
	return LittleFS.rename(from, to);
#else
	return rename(from, to) == 0;
#endif
}

/** Recursively delete a directory (or a file). Depth is bounded (UI tree <= 4). */
static bool fs_remove_tree(const char *p, int depth = 0) {
	if (depth > 6) return false;
	if (!fs_is_dir(p)) {
#if defined(ESP32)
		return LittleFS.remove(p);
#else
		return unlink(p) == 0;
#endif
	}
	bool ok = true;
#if defined(ESP32)
	// Collect entry names first: deleting while iterating is unreliable.
	for (;;) {
		File d = LittleFS.open(p);
		if (!d) return false;
		File e = d.openNextFile();
		if (!e) { d.close(); break; }
		char child[UI_PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", p, e.name());
		bool is_dir = e.isDirectory();
		e.close(); d.close();
		if (is_dir) ok = fs_remove_tree(child, depth + 1) && ok;
		else ok = LittleFS.remove(child) && ok;
		if (!ok) return false;
	}
	return LittleFS.rmdir(p) && ok;
#else
	DIR *d = opendir(p);
	if (!d) return false;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char child[UI_PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", p, e->d_name);
		ok = fs_remove_tree(child, depth + 1) && ok;
	}
	closedir(d);
	return rmdir(p) == 0 && ok;
#endif
}

/** Iterate the names of the entries of the UI store; cb returns false to stop. */
template <typename F>
static void fs_each_store_entry(F cb) {
	char store[UI_PATH_MAX];
	ui_path(store, sizeof(store), "");
#if defined(ESP32)
	File d = LittleFS.open(store);
	if (!d || !d.isDirectory()) { if (d) d.close(); return; }
	for (File e = d.openNextFile(); e; e = d.openNextFile()) {
		char name[UI_LOCAL_VERSION_LEN + 16];
		strncpy(name, e.name(), sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		bool is_dir = e.isDirectory();
		e.close();
		if (!cb(name, is_dir)) break;
	}
	d.close();
#else
	DIR *d = opendir(store);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char child[UI_PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", store, e->d_name);
		if (!cb(e->d_name, fs_is_dir(child))) break;
	}
	closedir(d);
#endif
}

size_t ui_local_free_bytes(size_t exact_below) {
#if defined(ESP32)
	return fs_free_bytes(exact_below);  // cached: usedBytes() walks the whole FS
#else
	(void)exact_below;
	struct statvfs st;
	if (statvfs(get_data_dir(), &st) == 0) return (size_t)st.f_bavail * st.f_bsize;
	return 0;
#endif
}

size_t ui_local_total_bytes() {
#if defined(ESP32)
	return fs_total_bytes();
#else
	struct statvfs st;
	if (statvfs(get_data_dir(), &st) == 0) return (size_t)st.f_blocks * st.f_frsize;
	return 0;
#endif
}

/** A version directory counts as installed when its marker and the two files
 *  the loader needs first are present. */
static bool ui_version_complete(const char *ver) {
	char p[UI_PATH_MAX], rel[48];
	snprintf(rel, sizeof(rel), "%s/ui.json", ver);
	ui_path(p, sizeof(p), rel);
	if (fs_file_size(p) <= 0) return false;
	snprintf(rel, sizeof(rel), "%s/js/home.js.gz", ver);
	ui_path(p, sizeof(p), rel);
	if (fs_file_size(p) <= 0) return false;
	snprintf(rel, sizeof(rel), "%s/index.html.gz", ver);
	ui_path(p, sizeof(p), rel);
	return fs_file_size(p) > 0;
}

size_t ui_local_list_versions(char *buf, size_t len) {
	size_t n = 0;
	if (len) buf[0] = 0;
	fs_each_store_entry([&](const char *name, bool is_dir) -> bool {
		if (!is_dir || !ui_valid_version(name)) return true;
		size_t l = strlen(name);
		if (n + l + 2 >= len) return false;
		if (n) buf[n++] = ',';
		memcpy(buf + n, name, l + 1);
		n += l;
		return true;
	});
	return n;
}

/** Decide which version directory is served (rules 1 and 8 of section 6.2). */
static void ui_select_active() {
	char fw[UI_LOCAL_VERSION_LEN];
	ui_local_firmware_version(fw, sizeof(fw));
	s_active[0] = 0;
	s_active_exact = false;
	if (ui_version_complete(fw)) {
		strncpy(s_active, fw, sizeof(s_active) - 1);
		s_active_exact = true;
		return;
	}
	if (os.iopts[IOPT_UI_SOURCE] != UI_SOURCE_LOCAL_ONLY) return;
	// local-only: newest installed version that is older than the firmware
	char best[UI_LOCAL_VERSION_LEN] = "";
	fs_each_store_entry([&](const char *name, bool is_dir) -> bool {
		if (!is_dir || !ui_valid_version(name)) return true;
		if (ui_version_cmp(name, fw) >= 0) return true;
		if (!best[0] || ui_version_cmp(name, best) > 0) {
			if (ui_version_complete(name)) { strncpy(best, name, sizeof(best) - 1); best[sizeof(best) - 1] = 0; }
		}
		return true;
	});
	if (best[0]) strncpy(s_active, best, sizeof(s_active) - 1);
}

/* ------------------------------------------------------------------------- */
/* Public state                                                              */
/* ------------------------------------------------------------------------- */
const char* ui_local_version() { return s_active; }
bool ui_local_available() { return s_active_exact; }
bool ui_local_stale() { return s_active[0] && !s_active_exact; }

bool ui_local_use() {
	return s_active[0] && os.iopts[IOPT_UI_SOURCE] == UI_SOURCE_LOCAL_ONLY;
}

bool ui_local_fallback() {
	return s_active[0] && os.iopts[IOPT_UI_SOURCE] == UI_SOURCE_AUTO;
}

bool ui_local_install_busy() { return s_busy; }
UiInstallState ui_local_install_state() { return s_state; }
void ui_local_request_check() { s_next_auto = 0; s_auto_fail = 0; }
void ui_local_refresh() { ui_select_active(); s_next_auto = 0; }

void ui_local_init() {
	char p[UI_PATH_MAX];
	ui_path(p, sizeof(p), "");
	if (!fs_is_dir(p)) fs_mkdir(p);
	// leftovers of an interrupted installation
	ui_path(p, sizeof(p), ".new");
	if (fs_exists(p)) fs_remove_tree(p);

	// First boot with this option: a deliberately changed UI URL is a decision
	// (self-hosted UI), keep it; the default URL means "nothing chosen" -> device.
	if (os.iopts[IOPT_UI_SOURCE] >= UI_SOURCE_UNSET) {
		char *url = (char*)ui_alloc(MAX_SOPTS_SIZE + 1);
		uint8_t src = UI_SOURCE_AUTO;
		if (url) {
			os.sopt_load(SOPT_JAVASCRIPTURL, url, MAX_SOPTS_SIZE);
			url[MAX_SOPTS_SIZE] = 0;
			if (strcmp(url, DEFAULT_JAVASCRIPT_URL) != 0) src = UI_SOURCE_EXTERNAL;
			free(url);
		}
		os.iopts[IOPT_UI_SOURCE] = src;
		os.iopts_save();
		DEBUG_PRINTF("[UI] source migrated to %u\n", (unsigned)src);
	}
	ui_select_active();
	DEBUG_PRINTF("[UI] local UI: %s%s free=%lu kB\n", s_active[0] ? s_active : "none",
		s_active_exact ? "" : (s_active[0] ? " (older)" : ""), (unsigned long)(ui_local_free_bytes() / 1024));
}

/* ------------------------------------------------------------------------- */
/* HTTP delivery: GET /ui/...                                                */
/* ------------------------------------------------------------------------- */
static const char* ui_mime(const char *name, bool *text) {
	const char *dot = strrchr(name, '.');
	*text = false;
	if (!dot) return "application/octet-stream";
	dot++;
	if (!strcmp(dot, "js"))    { *text = true; return "application/javascript"; }
	if (!strcmp(dot, "css"))   { *text = true; return "text/css"; }
	if (!strcmp(dot, "html"))  { *text = true; return "text/html"; }
	if (!strcmp(dot, "json") || !strcmp(dot, "map")) { *text = true; return "application/json"; }
	if (!strcmp(dot, "txt"))   { *text = true; return "text/plain"; }
	if (!strcmp(dot, "svg"))   { *text = true; return "image/svg+xml"; }
	if (!strcmp(dot, "png"))   return "image/png";
	if (!strcmp(dot, "ico"))   return "image/x-icon";
	if (!strcmp(dot, "gif"))   return "image/gif";
	if (!strcmp(dot, "jpg") || !strcmp(dot, "jpeg")) return "image/jpeg";
	if (!strcmp(dot, "webp"))  return "image/webp";
	if (!strcmp(dot, "woff"))  return "font/woff";
	if (!strcmp(dot, "woff2")) return "font/woff2";
	if (!strcmp(dot, "ttf"))   return "font/ttf";
	if (!strcmp(dot, "wasm"))  return "application/wasm";
	return "application/octet-stream";
}

/** Only [A-Za-z0-9._/-], no "..", no leading '/', no "//", bounded length. */
static bool ui_safe_rel(const char *rel) {
	size_t n = strlen(rel);
	if (n == 0 || n > 120 || rel[0] == '/' || rel[n - 1] == '/') return false;
	for (size_t i = 0; i < n; i++) {
		char c = rel[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		          c == '.' || c == '_' || c == '-' || c == '/';
		if (!ok) return false;
		if (c == '.' && i + 1 < n && rel[i + 1] == '.') return false;
		if (c == '/' && i + 1 < n && rel[i + 1] == '/') return false;
	}
	return true;
}

static void ui_404(OTF::Response &res) {
	res.writeStatus(404, F("Not found"));
	res.writeHeader(F("Content-Type"), F("text/plain"));
	res.writeHeader(F("Cache-Control"), F("max-age=0, no-cache, no-store, must-revalidate"));
	res.writeHeader(F("Connection"), F("close"));
	const char *msg = "The requested page does not exist";
	res.writeBodyData(msg, strlen(msg));
}

static void ui_send_small(OTF::Response &res, const char *ctype, const char *body) {
	res.writeStatus(200, F("OK"));
	res.writeHeader(F("Content-Type"), ctype);
	res.writeHeader(F("Content-Length"), (int)strlen(body));
	res.writeHeader(F("Cache-Control"), F("max-age=0, no-cache, no-store, must-revalidate"));
	res.writeHeader(F("Connection"), F("close"));
	res.writeBodyData(body, strlen(body));
}

void ui_local_serve(const OTF::Request &req, OTF::Response &res) {
	ui_local_serve_path(req.getPath(), res);
}

void ui_local_serve_path(const char *path, OTF::Response &res) {
	if (!path || strncmp(path, "/ui/", 4) != 0 || !s_active[0]) { ui_404(res); return; }
	const char *rel = path + 4;

	if (!strcmp(rel, "versions.json")) {
		char body[96];
		snprintf(body, sizeof(body), "{\"versions\":[\"%s\"],\"default\":\"%s\"}", s_active, s_active);
		ui_send_small(res, "application/json", body);
		return;
	}
	bool versioned = false;
	bool no_cache = false;
	if (!strcmp(rel, "js/home.js")) { no_cache = true; }   // the loader: always revalidated
	else {
		size_t al = strlen(s_active);
		if (!strncmp(rel, s_active, al) && rel[al] == '/') { rel += al + 1; versioned = true; }
		else if (rel[0] >= '0' && rel[0] <= '9') { ui_404(res); return; } // another (cached) version
	}
	if (!ui_safe_rel(rel)) { ui_404(res); return; }

	char fpath[UI_PATH_MAX];
	char vrel[UI_PATH_MAX];
	snprintf(vrel, sizeof(vrel), "%s/%s.gz", s_active, rel);
	ui_path(fpath, sizeof(fpath), vrel);
	bool gz = true;
	long size = fs_file_size(fpath);
	if (size < 0) {
		gz = false;
		snprintf(vrel, sizeof(vrel), "%s/%s", s_active, rel);
		ui_path(fpath, sizeof(fpath), vrel);
		size = fs_file_size(fpath);
	}
	if (size < 0) { ui_404(res); return; }

	bool text;
	const char *mime = ui_mime(rel, &text);
	res.writeStatus(200, F("OK"));
	if (text) {
		char ct[64];
		snprintf(ct, sizeof(ct), "%s; charset=utf-8", mime);
		res.writeHeader(F("Content-Type"), ct);
	} else {
		res.writeHeader(F("Content-Type"), mime);
	}
	if (gz) {
		res.writeHeader(F("Content-Encoding"), F("gzip"));
		res.writeHeader(F("Vary"), F("Accept-Encoding"));
	}
	res.writeHeader(F("Content-Length"), (int)size);
	if (no_cache) res.writeHeader(F("Cache-Control"), F("max-age=0, no-cache, no-store, must-revalidate"));
	else if (versioned) res.writeHeader(F("Cache-Control"), F("public, max-age=31536000, immutable"));
	else res.writeHeader(F("Cache-Control"), F("public, max-age=3600"));
	res.writeHeader(F("Connection"), F("close"));

	size_t bufsz = UI_IO_BUF_SIZE;
	uint8_t *buf = (uint8_t*)ui_alloc(bufsz);
	if (!buf) { bufsz = 512; buf = (uint8_t*)malloc(bufsz); }
	if (!buf) return;
#if defined(ESP32)
	File f = LittleFS.open(fpath, "r");
	if (f) {
		while (f.available()) {
			size_t n = f.read(buf, bufsz);
			if (!n) break;
			res.writeBodyData((const char*)buf, n);
		}
		f.close();
	}
#else
	FILE *f = fopen(fpath, "rb");
	if (f) {
		size_t n;
		while ((n = fread(buf, 1, bufsz, f)) > 0) res.writeBodyData((const char*)buf, n);
		fclose(f);
	}
#endif
	free(buf);
}

/* ------------------------------------------------------------------------- */
/* Package installation: streaming ustar parser + SHA-256 + ECDSA            */
/* ------------------------------------------------------------------------- */
#if defined(ESP32)
typedef File ui_out_t;
#else
typedef FILE* ui_out_t;
#endif

struct UiInstallCtx {
#if defined(ESP32)
	mbedtls_sha256_context sha;
#else
	SHA256_CTX sha;
#endif
	char     version[UI_LOCAL_VERSION_LEN];
	char     source[24];
	uint8_t  hdr[512];
	uint16_t hdr_fill;
	uint32_t remaining;       // data bytes left in the current entry
	uint32_t pad;             // padding bytes to skip after the entry
	bool     in_entry;
	bool     to_file;         // entry data goes to out
	bool     to_meta;         // entry data is ui.json
	bool     ended;           // end-of-archive marker seen
	ui_out_t out;
	bool     out_open;
	char     meta[256];
	uint16_t meta_len;
	uint32_t files;
	uint32_t bytes;
	uint32_t total_hint;
	bool     error;
	char     err[48];
};
static UiInstallCtx *s_ctx = NULL;

static void ctx_fail(const char *why) {
	if (!s_ctx) return;
	if (!s_ctx->error) {
		s_ctx->error = true;
		strncpy(s_ctx->err, why, sizeof(s_ctx->err) - 1);
		s_ctx->err[sizeof(s_ctx->err) - 1] = 0;
		DEBUG_PRINTF("[UI] install error: %s\n", why);
	}
}

static void ctx_close_out() {
	if (!s_ctx || !s_ctx->out_open) return;
#if defined(ESP32)
	s_ctx->out.close();
#else
	fclose(s_ctx->out);
	s_ctx->out = NULL;
#endif
	s_ctx->out_open = false;
}

static void ctx_destroy() {
	if (!s_ctx) return;
	ctx_close_out();
#if defined(ESP32)
	fs_used_invalidate();  // an install writes/removes megabytes past the file helpers
#endif
#if defined(ESP32)
	mbedtls_sha256_free(&s_ctx->sha);
#endif
	s_ctx->~UiInstallCtx();
	free(s_ctx);
	s_ctx = NULL;
}

static uint32_t tar_octal(const uint8_t *p, size_t n) {
	uint32_t v = 0;
	for (size_t i = 0; i < n && p[i]; i++) {
		if (p[i] == ' ') continue;
		if (p[i] < '0' || p[i] > '7') break;
		v = (v << 3) | (uint32_t)(p[i] - '0');
	}
	return v;
}

/** Header complete: open the target file / directory, or decide to skip. */
static void tar_begin_entry() {
	UiInstallCtx *c = s_ctx;
	const uint8_t *h = c->hdr;
	bool all_zero = true;
	for (int i = 0; i < 512 && all_zero; i++) if (h[i]) all_zero = false;
	if (all_zero) { c->ended = true; return; }

	// checksum (unsigned sum with the checksum field treated as spaces)
	uint32_t sum = 0;
	for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
	if (sum != tar_octal(h + 148, 8)) { ctx_fail("tar checksum"); return; }

	char name[160];
	size_t nl = 0;
	if (!memcmp(h + 257, "ustar", 5) && h[345]) {       // prefix field
		for (size_t i = 0; i < 155 && h[345 + i] && nl < sizeof(name) - 2; i++) name[nl++] = (char)h[345 + i];
		name[nl++] = '/';
	}
	for (size_t i = 0; i < 100 && h[i] && nl < sizeof(name) - 1; i++) name[nl++] = (char)h[i];
	name[nl] = 0;
	while (nl && name[nl - 1] == '/') name[--nl] = 0;   // directory entries end with '/'
	if (!strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name + 2) + 1);

	uint32_t size = tar_octal(h + 124, 12);
	char type = (char)h[156];
	c->remaining = size;
	c->pad = (512 - (size % 512)) % 512;
	c->in_entry = true;
	c->to_file = false;
	c->to_meta = false;

	if (type == '5') return;                              // directories are created on demand
	if (type != '0' && type != 0 && type != '7') return;  // links, pax headers etc.: skip data
	if (!ui_safe_rel(name)) { ctx_fail("tar path"); return; }

	if (!strcmp(name, "ui.json")) {
		if (size >= sizeof(c->meta)) { ctx_fail("ui.json size"); return; }
		c->to_meta = true;
		c->meta_len = 0;
		return;
	}
	char rel[UI_PATH_MAX];
	snprintf(rel, sizeof(rel), ".new/%s", name);
	char full[UI_PATH_MAX];
	ui_path(full, sizeof(full), rel);
	if (!fs_mkdir_parents(full)) { ctx_fail("mkdir"); return; }
#if defined(ESP32)
	c->out = LittleFS.open(full, "w");
	c->out_open = (bool)c->out;
#else
	c->out = fopen(full, "wb");
	c->out_open = c->out != NULL;
#endif
	if (!c->out_open) { ctx_fail("open"); return; }
	c->to_file = true;
	c->files++;
}

static bool tar_write_data(const uint8_t *d, size_t n) {
	UiInstallCtx *c = s_ctx;
	if (c->to_meta) {
		memcpy(c->meta + c->meta_len, d, n);
		c->meta_len += n;
		c->meta[c->meta_len] = 0;
		return true;
	}
	if (!c->to_file) return true;
#if defined(ESP32)
	size_t w = c->out.write(d, n);
#else
	size_t w = fwrite(d, 1, n, c->out);
#endif
	if (w != n) { ctx_fail("write (flash full?)"); return false; }
	c->bytes += n;
	return true;
}

static void tar_end_entry() {
	UiInstallCtx *c = s_ctx;
	if (c->to_file) ctx_close_out();
	if (c->to_meta) {
		// {"version":"2.4.0.231", ...}
		const char *k = strstr(c->meta, "\"version\"");
		const char *q = k ? strchr(k + 9, '"') : NULL;
		if (q) {
			q++;
			const char *e = strchr(q, '"');
			size_t l = e ? (size_t)(e - q) : 0;
			if (l && l < sizeof(c->version)) { memcpy(c->version, q, l); c->version[l] = 0; }
		}
		if (!ui_valid_version(c->version)) ctx_fail("ui.json version");
	}
	c->in_entry = false;
	c->to_file = false;
	c->to_meta = false;
	c->hdr_fill = 0;
}

bool ui_local_install_begin(uint32_t total_hint, const char *source) {
	if (s_ctx) return false;
	size_t need = total_hint ? (size_t)total_hint + UI_LOCAL_MIN_FREE : (size_t)(3UL * 1024UL * 1024UL);
	size_t free_b = ui_local_free_bytes(need);
	if (free_b < need) {
		char m[64];
		snprintf(m, sizeof(m), "Not enough flash: %lu kB free, %lu kB needed",
			(unsigned long)(free_b / 1024), (unsigned long)(need / 1024));
		ui_set_state(UI_INST_ERROR, 0, m);
		return false;
	}
	void *mem = ui_alloc(sizeof(UiInstallCtx));
	if (!mem) { ui_set_state(UI_INST_ERROR, 0, "Out of memory"); return false; }
	s_ctx = new (mem) UiInstallCtx();   // value-initialized: PODs zero, File empty
#if defined(ESP32)
	mbedtls_sha256_init(&s_ctx->sha);
	mbedtls_sha256_starts(&s_ctx->sha, 0);
#else
	SHA256_Init(&s_ctx->sha);
#endif
	s_ctx->total_hint = total_hint;
	strncpy(s_ctx->source, source ? source : "", sizeof(s_ctx->source) - 1);
	char p[UI_PATH_MAX];
	ui_path(p, sizeof(p), ".new");
	if (fs_exists(p)) fs_remove_tree(p);
	if (!fs_mkdir(p)) { ctx_destroy(); ui_set_state(UI_INST_ERROR, 0, "Cannot create staging dir"); return false; }
	s_busy = true;
	s_state.received = 0;
	s_state.total = total_hint;
	ui_set_state(UI_INST_DOWNLOADING, 1, "Receiving UI package...");
	return true;
}

bool ui_local_install_write(const uint8_t *data, size_t len) {
	UiInstallCtx *c = s_ctx;
	if (!c || c->error) return false;
#if defined(ESP32)
	mbedtls_sha256_update(&c->sha, data, len);
#else
	SHA256_Update(&c->sha, data, len);
#endif
	s_state.received += len;
	if (s_state.total) {
		uint32_t pct = (uint32_t)((uint64_t)s_state.received * 90 / s_state.total);
		if (pct > 90) pct = 90;
		s_state.progress = (uint8_t)(pct ? pct : 1);
	}
	while (len && !c->error) {
		if (c->ended) { len = 0; break; }               // trailing zero blocks
		if (!c->in_entry) {
			size_t take = 512 - c->hdr_fill;
			if (take > len) take = len;
			memcpy(c->hdr + c->hdr_fill, data, take);
			c->hdr_fill += take; data += take; len -= take;
			if (c->hdr_fill == 512) {
				tar_begin_entry();
				if (c->error) break;
				if (c->ended) break;
				if (c->remaining == 0 && c->pad == 0) tar_end_entry();
			}
			continue;
		}
		if (c->remaining) {
			size_t take = c->remaining < len ? c->remaining : len;
			if (!tar_write_data(data, take)) break;
			c->remaining -= take; data += take; len -= take;
		}
		if (!c->remaining && c->pad) {
			size_t take = c->pad < len ? c->pad : len;
			c->pad -= take; data += take; len -= take;
		}
		if (!c->remaining && !c->pad) tar_end_entry();
	}
	return !c->error;
}

void ui_local_install_abort(const char *why) {
	if (s_ctx) {
		ctx_destroy();
		char p[UI_PATH_MAX];
		ui_path(p, sizeof(p), ".new");
		if (fs_exists(p)) fs_remove_tree(p);
	}
	ui_set_state(UI_INST_ERROR, 0, why ? why : "aborted");
	s_busy = false;
}

/* --- base64 ----------------------------------------------------------------- */
static int b64val(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}
static size_t b64decode(const char *in, uint8_t *out, size_t outlen) {
	size_t n = 0; int bits = 0; uint32_t acc = 0;
	for (; *in; in++) {
		if (*in == '=' || *in == '\r' || *in == '\n' || *in == ' ') continue;
		int v = b64val(*in);
		if (v < 0) return 0;
		acc = (acc << 6) | (uint32_t)v; bits += 6;
		if (bits >= 8) { bits -= 8; if (n < outlen) out[n++] = (uint8_t)((acc >> bits) & 0xFF); else return 0; }
	}
	return n;
}

/* --- ECDSA P-256 verification --------------------------------------------- */
static bool ui_verify_sig(const uint8_t hash[32], const uint8_t sig[64]) {
#if defined(ESP32)
	mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi r, s;
	mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
	bool ok = false;
	if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
	    mbedtls_ecp_point_read_binary(&grp, &Q, UI_LOCAL_PUBKEY, sizeof(UI_LOCAL_PUBKEY)) == 0 &&
	    mbedtls_mpi_read_binary(&r, sig, 32) == 0 && mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0) {
		ok = mbedtls_ecdsa_verify(&grp, hash, 32, &Q, &r, &s) == 0;
	}
	mbedtls_mpi_free(&s); mbedtls_mpi_free(&r); mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
	return ok;
#else
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	bool ok = false;
	EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!key) return false;
	const EC_GROUP *grp = EC_KEY_get0_group(key);
	EC_POINT *pt = EC_POINT_new(grp);
	ECDSA_SIG *es = ECDSA_SIG_new();
	BIGNUM *r = BN_bin2bn(sig, 32, NULL), *s = BN_bin2bn(sig + 32, 32, NULL);
	if (pt && es && r && s &&
	    EC_POINT_oct2point(grp, pt, UI_LOCAL_PUBKEY, sizeof(UI_LOCAL_PUBKEY), NULL) == 1 &&
	    EC_KEY_set_public_key(key, pt) == 1 && ECDSA_SIG_set0(es, r, s) == 1) {
		r = s = NULL; // owned by es now
		ok = ECDSA_do_verify(hash, 32, es, key) == 1;
	}
	if (r) BN_free(r);
	if (s) BN_free(s);
	if (es) ECDSA_SIG_free(es);
	if (pt) EC_POINT_free(pt);
	EC_KEY_free(key);
	return ok;
	#pragma GCC diagnostic pop
#endif
}

bool ui_local_install_finish(const char *sig_b64) {
	UiInstallCtx *c = s_ctx;
	if (!c) return false;
	ctx_close_out();
	if (c->error) { char why[64]; snprintf(why, sizeof(why), "Package rejected: %s", c->err); ui_local_install_abort(why); return false; }
	if (!c->version[0] || !c->files) { ui_local_install_abort("Package rejected: no ui.json"); return false; }

	ui_set_state(UI_INST_VERIFYING, 92, "Verifying signature...");
	uint8_t hash[32];
#if defined(ESP32)
	mbedtls_sha256_finish(&c->sha, hash);
#else
	SHA256_Final(hash, &c->sha);
#endif
	uint8_t sig[64];
	if (!sig_b64 || b64decode(sig_b64, sig, sizeof(sig)) != 64) { ui_local_install_abort("Package rejected: missing signature"); return false; }
	if (!ui_verify_sig(hash, sig)) { ui_local_install_abort("Package rejected: bad signature"); return false; }

	// the version directory must be complete before it is activated
	char staged[UI_PATH_MAX], rel[64];
	snprintf(rel, sizeof(rel), ".new/js/home.js.gz"); ui_path(staged, sizeof(staged), rel);
	long a = fs_file_size(staged);
	snprintf(rel, sizeof(rel), ".new/index.html.gz"); ui_path(staged, sizeof(staged), rel);
	long b = fs_file_size(staged);
	if (a <= 0 || b <= 0) { ui_local_install_abort("Package rejected: incomplete"); return false; }

	// write the marker last
	snprintf(rel, sizeof(rel), ".new/ui.json"); ui_path(staged, sizeof(staged), rel);
	{
#if defined(ESP32)
		File f = LittleFS.open(staged, "w");
		bool ok = f && f.write((const uint8_t*)c->meta, c->meta_len) == c->meta_len;
		if (f) f.close();
#else
		FILE *f = fopen(staged, "wb");
		bool ok = f && fwrite(c->meta, 1, c->meta_len, f) == c->meta_len;
		if (f) fclose(f);
#endif
		if (!ok) { ui_local_install_abort("Cannot write ui.json"); return false; }
	}

	ui_set_state(UI_INST_ACTIVATING, 96, "Activating...");
	char version[UI_LOCAL_VERSION_LEN];
	strncpy(version, c->version, sizeof(version));
	char from[UI_PATH_MAX], to[UI_PATH_MAX];
	ui_path(from, sizeof(from), ".new");
	ui_path(to, sizeof(to), version);
	if (fs_exists(to)) {
		// replacing the served version: take it offline first
		if (!strcmp(s_active, version)) { s_active[0] = 0; s_active_exact = false; }
		fs_remove_tree(to);
	}
	bool ok = fs_rename(from, to);
	ctx_destroy();
	if (!ok) { ui_local_install_abort("Activation failed (rename)"); return false; }
	ui_select_active();
	char m[64];
	snprintf(m, sizeof(m), "UI %s installed%s", version, s_active_exact && !strcmp(s_active, version) ? "" : " (not for this firmware)");
	ui_set_state(UI_INST_DONE, 100, m);
	s_auto_fail = 0;
	s_busy = false;
	DEBUG_PRINTF("[UI] %s, free=%lu kB\n", m, (unsigned long)(ui_local_free_bytes() / 1024));
	return true;
}

bool ui_local_remove(const char *version) {
	if (s_busy) return false;
	bool ok = true;
	char p[UI_PATH_MAX];
	if (version && version[0]) {
		if (!ui_valid_version(version)) return false;
		ui_path(p, sizeof(p), version);
		ok = fs_remove_tree(p);
	} else {
		char list[160];
		ui_local_list_versions(list, sizeof(list));
		char *save = NULL;
		for (char *v = strtok_r(list, ",", &save); v; v = strtok_r(NULL, ",", &save)) {
			ui_path(p, sizeof(p), v);
			ok = fs_remove_tree(p) && ok;
		}
	}
	ui_select_active();
	ui_set_state(UI_INST_IDLE, 0, "");
	return ok;
}

/* ------------------------------------------------------------------------- */
/* Download                                                                  */
/* ------------------------------------------------------------------------- */
#if defined(ESP32)
static WiFiClient* ui_make_client(const char *url) {
	if (!strncasecmp(url, "https://", 8)) {
		WiFiClientSecure *c = new (std::nothrow) WiFiClientSecure();
		if (c) c->setInsecure();   // origin is guaranteed by the package signature
		return c;
	}
	return new (std::nothrow) WiFiClient();
}

/** GET url. If `small` is given the body (<= small_len-1) is stored there,
 *  otherwise it is streamed into the installer. Returns HTTP code or -1. */
static int ui_http_get(const char *url, char *small, size_t small_len) {
	WiFiClient *client = ui_make_client(url);
	if (!client) return -1;
	int code = -1;
	{
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(20000);
		http.setReuse(false);
		if (http.begin(*client, url)) {
			code = http.GET();
			if (code == HTTP_CODE_OK) {
				int size = http.getSize();
				WiFiClient *stream = http.getStreamPtr();
				if (small) {
					size_t got = 0;
					uint32_t idle = millis();
					while (http.connected() && got < small_len - 1 && (size < 0 || (int)got < size)) {
						int avail = stream->available();
						if (avail > 0) {
							size_t take = (size_t)avail;
							if (take > small_len - 1 - got) take = small_len - 1 - got;
							got += stream->readBytes(small + got, take);
							idle = millis();
						} else {
							if (millis() - idle > 10000) break;
							delay(1);
						}
					}
					small[got] = 0;
				} else {
					if (!ui_local_install_begin(size > 0 ? (uint32_t)size : 0, "download")) { code = -2; }
					else {
						uint8_t *buf = (uint8_t*)ui_alloc(UI_IO_BUF_SIZE);
						if (!buf) { ui_local_install_abort("Out of memory"); code = -3; }
						else {
							int got = 0;
							uint32_t idle = millis();
							while (http.connected() && (size < 0 || got < size)) {
								int avail = stream->available();
								if (avail > 0) {
									size_t take = (size_t)avail > UI_IO_BUF_SIZE ? UI_IO_BUF_SIZE : (size_t)avail;
									size_t n = stream->readBytes(buf, take);
									if (n && !ui_local_install_write(buf, n)) { code = -4; break; }
									got += (int)n;
									idle = millis();
								} else {
									if (millis() - idle > 20000) { code = -5; break; }
									delay(1);
								}
							}
							if (code == HTTP_CODE_OK && size > 0 && got < size) code = -6;
							free(buf);
						}
					}
				}
			}
			http.end();
		}
	}
	delete client;
	return code;
}
#else
struct CurlSmall { char *buf; size_t len; size_t got; };
static size_t ui_curl_small(void *p, size_t sz, size_t nm, void *ud) {
	CurlSmall *s = (CurlSmall*)ud;
	size_t n = sz * nm;
	size_t room = s->len - 1 - s->got;
	if (n > room) n = room;
	memcpy(s->buf + s->got, p, n);
	s->got += n;
	s->buf[s->got] = 0;
	return sz * nm;
}
static size_t ui_curl_stream(void *p, size_t sz, size_t nm, void *ud) {
	(void)ud;
	size_t n = sz * nm;
	if (!s_ctx) {
		// first body bytes: start the installer with the announced length
		CURL *h = (CURL*)ud;
		curl_off_t cl = -1;
		curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
		if (!ui_local_install_begin(cl > 0 ? (uint32_t)cl : 0, "download")) return 0;
	}
	return ui_local_install_write((const uint8_t*)p, n) ? n : 0;
}
static int ui_http_get(const char *url, char *small, size_t small_len) {
	CURL *h = curl_easy_init();
	if (!h) return -1;
	CurlSmall cs = { small, small_len, 0 };
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 100L);
	curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
	if (small) {
		curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, ui_curl_small);
		curl_easy_setopt(h, CURLOPT_WRITEDATA, &cs);
	} else {
		curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, ui_curl_stream);
		curl_easy_setopt(h, CURLOPT_WRITEDATA, h);
	}
	CURLcode rc = curl_easy_perform(h);
	long code = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(h);
	if (rc != CURLE_OK) return code >= 400 ? (int)code : -1;
	if (!small && !s_ctx) return -2;
	return (int)code;
}
#endif

/** Fetch signature + package and install. Runs in the caller's context. */
static bool ui_install_run(const char *url) {
	char sig_url[UI_LOCAL_URL_LEN + 8];
	snprintf(sig_url, sizeof(sig_url), "%s.sig", url);
	char sig[128];
	s_busy = true;
	s_state.received = 0; s_state.total = 0;
	ui_set_state(UI_INST_DOWNLOADING, 0, "Fetching signature...");
	DEBUG_PRINTF("[UI] install from %s\n", url);
	int code = ui_http_get(sig_url, sig, sizeof(sig));
	if (code != 200 || strlen(sig) < 80) {
		char m[64]; snprintf(m, sizeof(m), "Signature download failed (%d)", code);
		ui_local_install_abort(m);
		s_auto_fail++;
		return false;
	}
	ui_set_state(UI_INST_DOWNLOADING, 1, "Downloading UI package...");
	code = ui_http_get(url, NULL, 0);
	if (code != 200 || !s_ctx) {
		if (s_ctx || s_state.status != UI_INST_ERROR) {
			char m[64]; snprintf(m, sizeof(m), "Package download failed (%d)", code);
			ui_local_install_abort(m);
		} else {
			s_busy = false;
		}
		s_auto_fail++;
		return false;
	}
	bool ok = ui_local_install_finish(sig);
	if (!ok) s_auto_fail++;
	return ok;
}

#if defined(ESP32)
static void ui_install_task(void *param) {
	char *url = (char*)param;
	ui_install_run(url);
	free(url);
	PSRAM_TASK_SELF_DELETE();
}
#else
static void* ui_install_thread(void *param) {
	char *url = (char*)param;
	ui_install_run(url);
	free(url);
	return NULL;
}
#endif

bool ui_local_install_from_url(const char *url, bool async) {
	if (s_busy || online_update_in_progress()) return false;
	char *copy = (char*)ui_alloc(UI_LOCAL_URL_LEN);
	if (!copy) return false;
	if (url && url[0]) { strncpy(copy, url, UI_LOCAL_URL_LEN - 1); copy[UI_LOCAL_URL_LEN - 1] = 0; }
	else ui_local_default_url(copy, UI_LOCAL_URL_LEN);
	if (strncasecmp(copy, "http://", 7) && strncasecmp(copy, "https://", 8)) { free(copy); return false; }
	if (!async) {
		bool ok = ui_install_run(copy);
		free(copy);
		return ok;
	}
	s_busy = true;   // reserved until the task takes over
	ui_set_state(UI_INST_DOWNLOADING, 0, "Starting...");
#if defined(ESP32)
	if (PSRAM_TASK_CREATE(ui_install_task, "ui_inst", 12288, copy, 1, NULL) != pdPASS) {
		free(copy);
		ui_set_state(UI_INST_ERROR, 0, "Cannot start install task");
		s_busy = false;
		return false;
	}
#else
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &attr, ui_install_thread, copy) != 0) {
		pthread_attr_destroy(&attr);
		free(copy);
		ui_set_state(UI_INST_ERROR, 0, "Cannot start install thread");
		s_busy = false;
		return false;
	}
	pthread_attr_destroy(&attr);
#endif
	return true;
}

/* ------------------------------------------------------------------------- */
/* Housekeeping: automatic installation, garbage collection                  */
/* ------------------------------------------------------------------------- */
static void ui_gc() {
	// keep the active version and the newest other one (rollback / second slot)
	char list[160];
	ui_local_list_versions(list, sizeof(list));
	char keep[UI_LOCAL_VERSION_LEN] = "";
	int count = 0;
	char *save = NULL;
	char copy[160];
	strncpy(copy, list, sizeof(copy));
	for (char *v = strtok_r(copy, ",", &save); v; v = strtok_r(NULL, ",", &save)) {
		count++;
		if (!strcmp(v, s_active)) continue;
		if (!keep[0] || ui_version_cmp(v, keep) > 0) { strncpy(keep, v, sizeof(keep) - 1); keep[sizeof(keep) - 1] = 0; }
	}
	bool low = ui_local_free_bytes(UI_LOCAL_MIN_FREE * 2) < UI_LOCAL_MIN_FREE * 2;
	if (count <= UI_LOCAL_KEEP_VERSIONS && !low) return;
	save = NULL;
	strncpy(copy, list, sizeof(copy));
	for (char *v = strtok_r(copy, ",", &save); v; v = strtok_r(NULL, ",", &save)) {
		if (!strcmp(v, s_active)) continue;
		if (!strcmp(v, keep) && !low) continue;
		char p[UI_PATH_MAX];
		ui_path(p, sizeof(p), v);
		DEBUG_PRINTF("[UI] removing old UI %s\n", v);
		fs_remove_tree(p);
#if defined(ESP32)
		fs_used_invalidate();
#endif
	}
}

void ui_local_loop(unsigned long now_tz) {
	if (!s_boot_tz) s_boot_tz = now_tz;
	if (now_tz - s_boot_tz < UI_BOOT_DELAY_S) return;
	if (!s_gc_done && now_tz - s_boot_tz >= UI_GC_DELAY_S && !s_busy) { s_gc_done = true; ui_gc(); }

	if (s_active_exact || s_busy) return;
	if (os.iopts[IOPT_UI_SOURCE] == UI_SOURCE_EXTERNAL) return;   // external only: no package needed
	if (online_update_in_progress() || !os.network_connected()) return;
	if (now_tz < s_next_auto) return;
	// backoff: 1 h, 6 h, then daily
	unsigned long wait = s_auto_fail == 0 ? 3600UL : (s_auto_fail == 1 ? 6UL * 3600UL : 24UL * 3600UL);
	s_next_auto = now_tz + wait;
	ui_local_install_from_url(NULL, true);
}

#endif // UI_LOCAL_SUPPORTED
