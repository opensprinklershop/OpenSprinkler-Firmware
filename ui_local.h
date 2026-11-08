/* OpenSprinkler Unified Firmware
 *
 * On-device web UI (ESP32 with external LittleFS, OSPi).
 *
 * The OpenSprinkler app (ui.opensprinklershop.de) is stored on the controller
 * and served by the controller itself instead of being loaded from the cloud:
 *
 *   /ui/js/home.js, /ui/...      root assets of the active version (home.js cuts
 *                                "js/home.js" off its URL -> UI root = /ui/)
 *   /ui/<version>/...            one directory per UI version, files stored
 *                                pre-gzipped (name.gz) as they come out of the
 *                                signed package (tools/mkuipack.py, ustar)
 *   /ui/<version>/ui.json        {"version":"2.4.0.231",...}, written last;
 *                                marks a complete, verified installation
 *   /ui/.new                     staging directory during an installation
 *
 * A local UI is only served when its version equals the running firmware
 * (OS_FW_VERSION.OS_FW_MINOR); anything else falls back to the external URL
 * (SOPT_JAVASCRIPTURL). Packages are ECDSA P-256 signed (ui_local_pubkey.h);
 * nothing is activated before hash and signature verified, and the version
 * directory is replaced atomically (rename), never updated in place.
 *
 * Memory rules (see docs/as_api_docs/plan_ui_on_device.md section 7): no
 * static buffers besides the 16-byte active version string and the small POD
 * state; stream buffers are allocated per request / per installation and
 * released again; the download task is created on demand and deletes itself.
 *
 * Not compiled on the ESP8266.
 */
#ifndef UI_LOCAL_H
#define UI_LOCAL_H

#include "defines.h"

#if defined(ESP32) || defined(OSPI)
#define UI_LOCAL_SUPPORTED 1

#include <stdint.h>
#include <stddef.h>
#include <OpenThingsFramework.h>

#define UI_LOCAL_VERSION_LEN   16          // "2.4.0.231" + spare
#define UI_LOCAL_URL_LEN       200
#define UI_LOCAL_MIN_FREE      (512UL * 1024UL)   // headroom kept free after an install
#define UI_LOCAL_KEEP_VERSIONS 2           // active + previous (rollback / second OTA slot)

/** IOPT_UI_SOURCE values. The external UI is faster than the copy on the
 *  controller, so the device copy is the fallback for offline / island
 *  operation, not the default. The browser decides: server_home() emits the
 *  external loader plus a watchdog; when the external script fails or does not
 *  run within IOPT_UI_FALLBACK seconds the page reloads as /?ui=local. */
enum UiSource {
	UI_SOURCE_AUTO = 0,        // external URL first, device copy when the external loader fails (offline)
	UI_SOURCE_LOCAL_ONLY = 1,  // always the device copy; on version mismatch the newest older installed UI
	UI_SOURCE_EXTERNAL = 2,    // external URL only, never the device copy (package not installed either)
	UI_SOURCE_UNSET = 3        // first boot after the upgrade: migrated in ui_local_init()
};
#define UI_FALLBACK_TIMEOUT_DEFAULT 6   // s; IOPT_UI_FALLBACK, 0 = only on load error

enum UiInstallStatus {
	UI_INST_IDLE = 0,
	UI_INST_DOWNLOADING,
	UI_INST_VERIFYING,
	UI_INST_ACTIVATING,
	UI_INST_DONE,
	UI_INST_ERROR
};

struct UiInstallState {
	uint8_t  status;      // UiInstallStatus
	uint8_t  progress;    // 0..100
	uint32_t received;    // bytes consumed so far
	uint32_t total;       // expected bytes (0 = unknown)
	char     message[64];
};

/* ---- lifecycle / state --------------------------------------------------- */
void ui_local_init();                                   // after the filesystem is mounted
void ui_local_loop(unsigned long now_tz);               // once per minute from do_loop()
void ui_local_firmware_version(char *buf, size_t len);  // "2.4.0.231"
const char* ui_local_version();                         // version served locally ("" = none)
bool ui_local_available();                              // exact match for the running firmware installed
bool ui_local_use();                                    // server_home: emit only the local loader (device-only mode)
bool ui_local_fallback();                               // server_home: external loader + watchdog falling back to local
bool ui_local_stale();                                  // local-only mode serving an older version
void ui_local_default_url(char *buf, size_t len);       // archive URL of the package for this firmware
size_t ui_local_free_bytes(size_t exact_below = 0);  // ESP32: cached, exact near exact_below
size_t ui_local_total_bytes();
size_t ui_local_list_versions(char *buf, size_t len);   // comma separated installed versions

/* ---- HTTP: static files below /ui/ (registered as OTF missing-page handler) */
void ui_local_serve(const OTF::Request &req, OTF::Response &res);
void ui_local_serve_path(const char *path, OTF::Response &res);   // same, explicit "/ui/..." path

/* ---- installation ---------------------------------------------------------- */
bool ui_local_install_busy();
UiInstallState ui_local_install_state();
/** Push interface used by the downloader, the /uf upload and the OTA task. */
bool ui_local_install_begin(uint32_t total_hint, const char *source);
bool ui_local_install_write(const uint8_t *data, size_t len);
bool ui_local_install_finish(const char *sig_b64);     // verify + activate; false = rejected
void ui_local_install_abort(const char *why);
/** Download <url> and <url>.sig and install. async: own task/thread (returns at once). */
bool ui_local_install_from_url(const char *url, bool async);
bool ui_local_remove(const char *version);             // NULL = all versions
void ui_local_request_check();                         // retry the automatic install soon
void ui_local_refresh();                               // re-evaluate after IOPT_UI_SOURCE changed

#else  // ESP8266 and others: no local UI
inline void ui_local_init() {}
inline void ui_local_loop(unsigned long) {}
inline const char* ui_local_version() { return ""; }
inline bool ui_local_available() { return false; }
inline bool ui_local_use() { return false; }
inline bool ui_local_fallback() { return false; }
inline bool ui_local_stale() { return false; }
inline bool ui_local_install_busy() { return false; }
inline void ui_local_refresh() {}
#endif // ESP32 || OSPI

#endif // UI_LOCAL_H
