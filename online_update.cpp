#if defined(ESP32)

#include "online_update.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include "ArduinoJson.hpp"
#include <LittleFS.h>
#include <ETH.h>

#if defined(ESP32C5)
#include "ieee802154_config.h"
#include "psram_utils.h"
#endif

extern OpenSprinkler os;
extern bool useEth;   // true when the device is connected via Ethernet (main.cpp)
extern ETHClass eth;  // Ethernet interface object (main.cpp)

// Thread-safe state
static SemaphoreHandle_t s_ota_mutex = NULL;
static OnlineUpdateState s_state = { OTA_STATUS_IDLE, 0, "" };
static OnlineUpdateManifest* s_manifest_ptr = NULL; // heap-allocated only during OTA
static TaskHandle_t s_ota_task = NULL;
// Optional variant override ("zigbee" or "matter"); empty = keep current
static char s_ota_requested_variant[8] = {0};

void online_update_set_variant(const char* variant) {
	if (variant && (strcmp(variant, "zigbee") == 0 || strcmp(variant, "matter") == 0)) {
		strncpy(s_ota_requested_variant, variant, sizeof(s_ota_requested_variant) - 1);
	} else {
		s_ota_requested_variant[0] = 0;
	}
}

// ─── Check helper task ────────────────────────────────────────────────────────
// online_update_check() opens a TCP connection which can use several KB of stack.
// Running it from loopTask (default 8 KB) causes a stack-protection fault.
// This wrapper task gives the check its own 12 KB stack; the caller blocks on a
// notification until the check finishes or times out (20 s).
static void ota_set_state(OnlineUpdateStatus status, uint8_t progress, const char* msg); // fwd decl

struct OTACheckTaskParam {
	OnlineUpdateManifest* manifest;   // caller-allocated, filled by task
	bool*                 newer_out;  // written by task
	TaskHandle_t          caller;     // notified when done
};

static void ota_check_task(void* pvParam) {
	OTACheckTaskParam* p = static_cast<OTACheckTaskParam*>(pvParam);
	*p->newer_out = online_update_check(*p->manifest);
	xTaskNotifyGive(p->caller);
	PSRAM_TASK_SELF_DELETE();
}

/**
 * Run online_update_check() on a dedicated 12 KB task to avoid overflowing the
 * caller's stack.  Blocks until the check completes or the 20 s timeout expires.
 * Returns true when a newer version is available (same semantics as
 * online_update_check).
 */
bool online_update_check_safe(OnlineUpdateManifest &manifest) {
	bool newer = false;
	OTACheckTaskParam param = { &manifest, &newer, xTaskGetCurrentTaskHandle() };
	BaseType_t ok = PSRAM_TASK_CREATE(ota_check_task, "ota_check", 12288, &param, 1, NULL);
	if (ok != pdPASS) {
		DEBUG_PRINTLN(F("[OTA] ERROR: failed to create check task"));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Check task alloc failed");
		return false;
	}
	// Wait up to 20 s for the task to finish
	if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000))) {
		DEBUG_PRINTLN(F("[OTA] check task timed out"));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Check timed out");
		return false;
	}
	return newer;
}

static void ota_set_state(OnlineUpdateStatus status, uint8_t progress, const char* msg) {
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	s_state.status = status;
	s_state.progress = progress;
	strncpy(s_state.message, msg, sizeof(s_state.message) - 1);
	s_state.message[sizeof(s_state.message) - 1] = '\0';
	xSemaphoreGive(s_ota_mutex);
	DEBUG_PRINT(F("[OTA] "));
	DEBUG_PRINTLN(msg);
}

OnlineUpdateState online_update_get_state() {
	OnlineUpdateState copy;
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	copy = s_state;
	xSemaphoreGive(s_ota_mutex);
	return copy;
}

bool online_update_in_progress() {
	if (!s_ota_mutex) return false;
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	bool busy = (s_state.status >= OTA_STATUS_DOWNLOADING_ZIGBEE && s_state.status <= OTA_STATUS_FLASHING_MATTER);
	xSemaphoreGive(s_ota_mutex);
	return busy;
}

bool online_update_check(OnlineUpdateManifest &manifest) {
	memset(&manifest, 0, sizeof(manifest));
	manifest.valid = false;

	DEBUG_PRINTF("[OTA] WiFi status: %d, IP: %s\n",
		(int)WiFi.status(), WiFi.localIP().toString().c_str());
	DEBUG_PRINTF("[OTA] Manifest URL: %s\n", OTA_MANIFEST_URL);

	ota_set_state(OTA_STATUS_CHECKING, 0, "Checking for updates...");

	WiFiClient *client = new WiFiClient();
	if (!client) {
		DEBUG_PRINTLN(F("[OTA] ERROR: WiFiClient allocation failed"));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Failed to create HTTP client");
		return false;
	}

	// Scope block: HTTPClient must be destroyed BEFORE delete client,
	// because its destructor may access the WiFiClient through _client.
	String payload;
	bool httpOk = false;
	{
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(15000);

		DEBUG_PRINTLN(F("[OTA] Connecting to update server..."));
		if (http.begin(*client, OTA_MANIFEST_URL)) {
			int httpCode = http.GET();
			DEBUG_PRINTF("[OTA] HTTP response code: %d\n", httpCode);
			if (httpCode == HTTP_CODE_OK) {
				payload = http.getString();
				DEBUG_PRINTF("[OTA] Manifest received (%u bytes)\n", (unsigned)payload.length());
				httpOk = true;
			} else {
				char buf[64];
				snprintf(buf, sizeof(buf), "HTTP error: %d", httpCode);
				DEBUG_PRINTF("[OTA] ERROR: %s\n", buf);
				ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, buf);
			}
			http.end();
		} else {
			DEBUG_PRINTLN(F("[OTA] ERROR: http.begin() failed — cannot connect to update server"));
			ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "HTTP begin failed");
		}
	} // HTTPClient destroyed here while client is still valid
	delete client;

	if (!httpOk) return false;

	// Parse JSON manifest
	ArduinoJson::JsonDocument doc;
	ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, payload);
	if (err) {
		ota_set_state(OTA_STATUS_ERROR_PARSE, 0, "JSON parse error");
		return false;
	}

	manifest.fw_version = doc["fw_version"] | 0;
	manifest.fw_minor = doc["fw_minor"] | 0;
	strncpy(manifest.zigbee_url, doc["zigbee_url"] | "", sizeof(manifest.zigbee_url) - 1);
	strncpy(manifest.matter_url, doc["matter_url"] | "", sizeof(manifest.matter_url) - 1);
	strncpy(manifest.zigbee_sha256, doc["zigbee_sha256"] | "", sizeof(manifest.zigbee_sha256) - 1);
	strncpy(manifest.matter_sha256, doc["matter_sha256"] | "", sizeof(manifest.matter_sha256) - 1);
	strncpy(manifest.esp8266_sha256, doc["esp8266_sha256"] | "", sizeof(manifest.esp8266_sha256) - 1);
	strncpy(manifest.changelog, doc["changelog"] | "", sizeof(manifest.changelog) - 1);
	manifest.valid = (manifest.fw_version > 0 && manifest.zigbee_url[0] && manifest.matter_url[0]);

	DEBUG_PRINTF("[OTA] Manifest: fw=%u.%u valid=%d\n",
		(unsigned)manifest.fw_version, (unsigned)manifest.fw_minor, (int)manifest.valid);

	if (!manifest.valid) {
		DEBUG_PRINTLN(F("[OTA] ERROR: Manifest missing fw_version, zigbee_url or matter_url"));
		ota_set_state(OTA_STATUS_ERROR_PARSE, 0, "Invalid manifest data");
		return false;
	}

	// Compare versions — newer if major > current OR (same major AND minor > current)
	bool newer = (manifest.fw_version > OS_FW_VERSION) ||
	             (manifest.fw_version == OS_FW_VERSION && manifest.fw_minor > OS_FW_MINOR);

	DEBUG_PRINTF("[OTA] Current: %u.%u  Server: %u.%u  Newer: %d\n",
		(unsigned)OS_FW_VERSION, (unsigned)OS_FW_MINOR,
		(unsigned)manifest.fw_version, (unsigned)manifest.fw_minor, (int)newer);

	if (newer) {
		ota_set_state(OTA_STATUS_AVAILABLE, 0, "Update available");
	} else {
		ota_set_state(OTA_STATUS_UP_TO_DATE, 0, "Firmware is up to date");
	}
	return newer;
}

// Download firmware from `url` into a PSRAM buffer, optionally verify SHA-256,
// then flash the named partition using a small internal-RAM write buffer.
//
// Design:
//   1. HTTP GET → stream full image into a PSRAM buffer (receive via 4 KB internal chunk).
//   2. Verify SHA-256 against `sha256_hex` if provided (mbedtls, no cache impact).
//   3. Flash: copy 4 KB chunks PSRAM → internal-RAM → esp_ota_write().
//      esp_ota_write() freezes the CPU cache; source MUST be internal RAM.
//
// Progress: download pMin→pMin+60%, verify +60-65%, flash +65-100%.
// sha256_hex: 64-char lowercase hex, or NULL/"" to skip verification.
// Returns true on success; error OTA state is set on failure.
static bool ota_download_verify_flash(
		const char* url, const char* sha256_hex, const char* partLabel,
		OnlineUpdateStatus dl_status, OnlineUpdateStatus err_status,
		uint8_t pMin = 0, uint8_t pMax = 100)
{
	constexpr size_t IO_BUF_SIZE = 4096;
	const uint8_t    range       = pMax - pMin;

	char     msg[96];
	bool     success     = false;
	uint8_t *img_buf     = nullptr;  // full firmware image buffered in PSRAM
	uint8_t *io_buf      = nullptr;  // 4 KB internal-RAM rx / write buffer
	int      imgSize     = 0;        // content-length / total bytes
	bool     download_ok = false;

	// ── 0. Normalise https → http ────────────────────────────────────────
	char url_buf[256];
	if (strncmp(url, "https://", 8) == 0) {
		snprintf(url_buf, sizeof(url_buf), "http://%s", url + 8);
		DEBUG_PRINTF("[OTA] Normalised URL: %s\n", url_buf);
		url = url_buf;
	}

	snprintf(msg, sizeof(msg), "Downloading %s firmware...", partLabel);
	ota_set_state(dl_status, pMin, msg);

	// ── 1. HTTP GET + stream into PSRAM buffer ──────────────────────────
	WiFiClient *client = new WiFiClient();
	if (!client) {
		ota_set_state(err_status, 0, "WiFiClient alloc failed");
		return false;
	}

	{   // scope keeps HTTPClient alive until http.end(); destroyed before delete client
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(30000);

		if (!http.begin(*client, url)) {
			DEBUG_PRINTF("[OTA] http.begin() failed for %s\n", partLabel);
			ota_set_state(err_status, 0, "HTTP begin failed");
			goto dl_done;
		}

		{
			int code = http.GET();
			DEBUG_PRINTF("[OTA] HTTP %d for %s\n", code, partLabel);
			if (code != HTTP_CODE_OK) {
				snprintf(msg, sizeof(msg), "%s HTTP error %d", partLabel, code);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto dl_done;
			}

			imgSize = http.getSize();
			DEBUG_PRINTF("[OTA] Content-Length for %s: %d bytes\n", partLabel, imgSize);
			if (imgSize <= 0) {
				snprintf(msg, sizeof(msg), "No Content-Length for %s", partLabel);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto dl_done;
			}

			// Allocate full image buffer in PSRAM
			img_buf = (uint8_t*)heap_caps_malloc(imgSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (!img_buf) {
				DEBUG_PRINTF("[OTA] PSRAM alloc %d bytes failed (PSRAM free=%u)\n",
					imgSize, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
				snprintf(msg, sizeof(msg), "PSRAM alloc failed for %s", partLabel);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto dl_done;
			}
			DEBUG_PRINTF("[OTA] PSRAM image buffer: %d bytes @ %p\n", imgSize, (void*)img_buf);

			// Allocate small internal-RAM I/O buffer for network receive and flash writes
			io_buf = (uint8_t*)heap_caps_malloc(IO_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
			if (!io_buf) {
				DEBUG_PRINTF("[OTA] Internal I/O buf alloc failed (internal free=%u)\n",
					(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
				ota_set_state(err_status, 0, "I/O buf alloc failed");
				http.end();
				goto dl_done;
			}

			WiFiClient *stream = http.getStreamPtr();
			int      totalRead  = 0;
			uint8_t  lastPct    = 255;
			uint32_t t0         = millis();
			uint32_t lastDataMs = millis();

			while ((http.connected() || stream->available()) && totalRead < imgSize) {
				size_t avail = stream->available();
				if (avail > 0) {
					lastDataMs = millis();
					int n = stream->readBytes(io_buf, (int)min(avail, IO_BUF_SIZE));
					if (n > 0) {
						memcpy(img_buf + totalRead, io_buf, n);
						totalRead += n;
						uint8_t raw = (uint8_t)((totalRead * 100LL) / imgSize);
						if (raw / 5 != lastPct / 5) {
							// Download fills pMin → pMin + 60% of range
							uint8_t p = pMin + (uint8_t)((long)raw * range * 60 / 100 / 100);
							snprintf(msg, sizeof(msg), "Downloading %s: %d%%", partLabel, raw);
							ota_set_state(dl_status, p, msg);
							lastPct = raw;
						}
					}
				} else {
					if (millis() - lastDataMs > 10000) {
						DEBUG_PRINTF("[OTA] Download stall for %s after %d/%d bytes\n",
							partLabel, totalRead, imgSize);
						break;
					}
					delay(1);
				}
			}

			http.end();

			uint32_t elapsed = millis() - t0;
			DEBUG_PRINTF("[OTA] Download done: %d/%d bytes in %ums (~%u kB/s)\n",
				totalRead, imgSize, (unsigned)elapsed,
				elapsed ? (unsigned)((uint64_t)totalRead * 1000 / elapsed / 1024) : 0u);

			if (totalRead < imgSize) {
				snprintf(msg, sizeof(msg), "Incomplete: %d/%d bytes", totalRead, imgSize);
				ota_set_state(err_status, 0, msg);
				goto dl_done;
			}

			download_ok = true;
		}

	dl_done:
		;	// HTTPClient destroyed here, before delete client
	}
	delete client;

	if (!download_ok) goto final_cleanup;

	// ── 2. SHA-256 verification ────────────────────────────────────────────
	if (sha256_hex && strlen(sha256_hex) == 64) {
		ota_set_state(dl_status, pMin + (uint8_t)(range * 60 / 100), "Verifying checksum...");

		uint8_t hash[32];
		mbedtls_sha256(img_buf, (size_t)imgSize, hash, 0);  // 0 = SHA-256

		char computed[65];
		for (int i = 0; i < 32; i++) snprintf(computed + i * 2, 3, "%02x", hash[i]);
		computed[64] = '\0';

		DEBUG_PRINTF("[OTA] SHA-256 expected : %s\n", sha256_hex);
		DEBUG_PRINTF("[OTA] SHA-256 computed : %s\n", computed);

		if (strncasecmp(computed, sha256_hex, 64) != 0) {
			snprintf(msg, sizeof(msg), "SHA-256 mismatch for %s", partLabel);
			DEBUG_PRINTF("[OTA] ERROR: %s\n", msg);
			ota_set_state(err_status, 0, msg);
			goto final_cleanup;
		}
		DEBUG_PRINTF("[OTA] SHA-256 OK for %s\n", partLabel);
	} else {
		DEBUG_PRINTF("[OTA] No digest in manifest for %s — skipping verify\n", partLabel);
	}

	// ── 3. Find target partition ───────────────────────────────────────────
	{
		const esp_partition_t *target = esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partLabel);
		if (!target) {
			snprintf(msg, sizeof(msg), "Partition '%s' not found", partLabel);
			ota_set_state(err_status, 0, msg);
			goto final_cleanup;
		}
		if ((size_t)imgSize > target->size) {
			snprintf(msg, sizeof(msg), "Image too large for %s (%d > %u)",
				partLabel, imgSize, (unsigned)target->size);
			ota_set_state(err_status, 0, msg);
			goto final_cleanup;
		}
		DEBUG_PRINTF("[OTA] Flashing %s at 0x%06x (%d bytes)\n",
			target->label, (unsigned)target->address, imgSize);

		// ── 4. Flash PSRAM buffer → flash via internal-RAM io_buf ─────────
		// esp_ota_write() freezes the CPU cache. The source buffer MUST be
		// internal RAM. We copy 4 KB chunks from PSRAM → io_buf (internal RAM),
		// then call esp_ota_write(). Safely decoupled from the network stack.
		snprintf(msg, sizeof(msg), "Flashing %s...", partLabel);
		ota_set_state(dl_status, pMin + (uint8_t)(range * 65 / 100), msg);

		esp_ota_handle_t ota_handle = 0;
		esp_err_t err = esp_ota_begin(target, (size_t)imgSize, &ota_handle);
		if (err != ESP_OK) {
			snprintf(msg, sizeof(msg), "esp_ota_begin failed: 0x%x", err);
			ota_set_state(err_status, 0, msg);
			goto final_cleanup;
		}

		int     offset       = 0;
		uint8_t flashLastPct = 255;
		bool    flash_err    = false;

		while (offset < imgSize) {
			size_t chunk = (size_t)min((int)IO_BUF_SIZE, imgSize - offset);
			memcpy(io_buf, img_buf + offset, chunk);        // PSRAM → internal RAM
			err = esp_ota_write(ota_handle, io_buf, chunk); // internal RAM → flash
			if (err != ESP_OK) {
				snprintf(msg, sizeof(msg), "esp_ota_write 0x%x at +%d", err, offset);
				ota_set_state(err_status, 0, msg);
				esp_ota_abort(ota_handle);
				flash_err = true;
				break;
			}
			offset += (int)chunk;
			uint8_t raw = (uint8_t)((offset * 100LL) / imgSize);
			if (raw / 10 != flashLastPct / 10) {
				// Flash fills pMin+65% → pMax
				uint8_t p = pMin + (uint8_t)(range * 65 / 100)
				           + (uint8_t)((long)raw * range * 35 / 100 / 100);
				snprintf(msg, sizeof(msg), "Flashing %s: %d%%", partLabel, raw);
				ota_set_state(dl_status, p, msg);
				flashLastPct = raw;
			}
		}

		if (!flash_err) {
			err = esp_ota_end(ota_handle);
			if (err != ESP_OK) {
				snprintf(msg, sizeof(msg), "esp_ota_end failed: 0x%x", err);
				ota_set_state(err_status, 0, msg);
				flash_err = true;
			}
		}

		if (!flash_err) {
			DEBUG_PRINTF("[OTA] %s flashed and verified OK\n", partLabel);
			snprintf(msg, sizeof(msg), "%s ready", partLabel);
			ota_set_state(dl_status, pMax, msg);
			success = true;
		}
	}

final_cleanup:
	if (io_buf)  { heap_caps_free(io_buf);  io_buf  = nullptr; }
	if (img_buf) { heap_caps_free(img_buf); img_buf = nullptr; }
	return success;
}

// Backup settings files to /backup/ directory on LittleFS
static bool ota_backup_settings() {
	DEBUG_PRINTLN(F("[OTA] Backing up settings..."));

	const char* files[] = {
		IOPTS_FILENAME, SOPTS_FILENAME, STATIONS_FILENAME,
		STATIONS2_FILENAME, STATIONS3_FILENAME,
		NVCON_FILENAME, PROG_FILENAME,
		"/sensors.json", "/progsensor.json", "/monitors.json"
	};

	LittleFS.mkdir("/backup");

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		const char* src = files[i];
		// Build backup path: /backup/filename.dat
		const char* fname = src;
		if (fname[0] == '/') fname++;
		char dst[48];
		snprintf(dst, sizeof(dst), "/backup/%s", fname);

		File srcFile = LittleFS.open(src, "r");
		if (!srcFile) continue;

		File dstFile = LittleFS.open(dst, "w");
		if (!dstFile) {
			srcFile.close();
			continue;
		}

		uint8_t buf[256];
		while (srcFile.available()) {
			int n = srcFile.read(buf, sizeof(buf));
			if (n > 0) dstFile.write(buf, n);
		}
		dstFile.close();
		srcFile.close();
	}

	DEBUG_PRINTLN(F("[OTA] Settings backup complete"));
	return true;
}

// Restore sensor config files from /backup/ if originals are missing after OTA
static void ota_restore_sensor_files() {
	const char* sensor_files[] = {
		"/sensors.json", "/progsensor.json", "/monitors.json"
	};

	for (size_t i = 0; i < sizeof(sensor_files) / sizeof(sensor_files[0]); i++) {
		const char* path = sensor_files[i];
		const char* fname = path + 1; // skip leading '/'
		char backup_path[48];
		snprintf(backup_path, sizeof(backup_path), "/backup/%s", fname);

		if (!LittleFS.exists(path) && LittleFS.exists(backup_path)) {
			DEBUG_PRINTF("[OTA] Restoring %s from backup\n", path);
			File srcFile = LittleFS.open(backup_path, "r");
			if (!srcFile) continue;
			File dstFile = LittleFS.open(path, "w");
			if (!dstFile) { srcFile.close(); continue; }

			uint8_t buf[256];
			while (srcFile.available()) {
				int n = srcFile.read(buf, sizeof(buf));
				if (n > 0) dstFile.write(buf, n);
			}
			dstFile.close();
			srcFile.close();
			DEBUG_PRINTF("[OTA] Restored %s\n", path);
		}
	}
}

// Save OTA continuation state to LittleFS for phase 2 after reboot.
// url: firmware URL for the remaining partition, label: partition label,
// variant: boot variant name to restore after both partitions are flashed.
// Also saves the current password hash so it can be restored after factory_reset.
static bool ota_save_continuation(const char* url, const char* sha256_hex, const char* label, const char* variant) {
	File f = LittleFS.open(OTA_CONTINUE_FILE, "w");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA] Failed to write continuation file"));
		return false;
	}
	// Save password hash: factory_reset() on phase 2 boot will reset it to default
	char pwBuf[MAX_SOPTS_SIZE + 1];
	os.sopt_load(SOPT_PASSWORD, pwBuf);
	f.printf("{\"url\":\"%s\",\"sha256\":\"%s\",\"label\":\"%s\",\"variant\":\"%s\",\"pw\":\"%s\"}",
	         url, sha256_hex ? sha256_hex : "", label, variant, pwBuf);
	f.close();
	DEBUG_PRINTLN(F("[OTA] Continuation state saved (incl. password + sha256)"));
	return true;
}

// FreeRTOS task that performs the full dual-OTA update
static void ota_update_task(void* param) {
	// Take ownership of heap-allocated manifest from online_update_start()
	OnlineUpdateManifest* cached = (OnlineUpdateManifest*)param;

	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "Starting update...");

	// Step 1: Backup settings (0-4% overall)
	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 2, "Backing up settings...");
	ota_backup_settings();
	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 4, "Settings backed up");

	// Step 2: Use cached manifest or re-fetch
	OnlineUpdateManifest manifest;
	if (cached && cached->valid) {
		manifest = *cached;
	}
	delete cached;
	if (!manifest.valid) {
		if (!online_update_check(manifest)) {
			ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "No update available or check failed");
			s_ota_task = NULL;
			vTaskDelete(NULL);
			return;
		}
	}

#if defined(ESP32C5)
	// Two-phase OTA: we cannot esp_ota_begin on the running partition (ESP_ERR_OTA_PARTITION_CONFLICT).
	// Phase 1 (5-48%): flash the NON-running partition, save state, reboot into it.
	// Phase 2 (52-98%, after reboot): flash the remaining partition, set boot to target, reboot.
	const esp_partition_t *running = esp_ota_get_running_partition();
	DEBUG_PRINTF("[OTA] Running partition: %s\n", running ? running->label : "unknown");

	IEEE802154BootVariant target_variant = ieee802154_get_boot_variant();
	// Consume requested variant override (set by server_update_upgrade via online_update_set_variant)
	char requested_variant_buf[8] = {0};
	strncpy(requested_variant_buf, s_ota_requested_variant, sizeof(requested_variant_buf) - 1);
	s_ota_requested_variant[0] = 0; // consume
	if (requested_variant_buf[0]) {
		target_variant = (strcmp(requested_variant_buf, "matter") == 0)
			? IEEE802154BootVariant::MATTER : IEEE802154BootVariant::ZIGBEE;
	}
	const char* target_variant_name = (target_variant == IEEE802154BootVariant::MATTER) ? "matter" : "zigbee";

	if (running && strcmp(running->label, "matter") == 0) {
		// Running from matter (ota_1) → flash zigbee (ota_0) first
		if (!ota_download_verify_flash(manifest.zigbee_url, manifest.zigbee_sha256, "zigbee",
		                               OTA_STATUS_DOWNLOADING_ZIGBEE, OTA_STATUS_ERROR_FLASH_ZIGBEE, 5, 48)) {
			s_ota_task = NULL;
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash matter after reboot
		ota_save_continuation(manifest.matter_url, manifest.matter_sha256, "matter", target_variant_name);
		// Set boot to zigbee (the partition we just flashed) so phase 2 can write matter
		esp_ota_set_boot_partition(esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "zigbee"));
	} else {
		// Running from zigbee (ota_0) → flash matter (ota_1) first
		if (!ota_download_verify_flash(manifest.matter_url, manifest.matter_sha256, "matter",
		                               OTA_STATUS_DOWNLOADING_MATTER, OTA_STATUS_ERROR_FLASH_MATTER, 5, 48)) {
			s_ota_task = NULL;
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash zigbee after reboot
		ota_save_continuation(manifest.zigbee_url, manifest.zigbee_sha256, "zigbee", target_variant_name);
		// Set boot to matter (the partition we just flashed) so phase 2 can write zigbee
		esp_ota_set_boot_partition(esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "matter"));
	}

	// Reboot into the newly flashed partition for phase 2
	ota_set_state(OTA_STATUS_REBOOTING_PHASE2, 50, "Phase 1 done. Rebooting for phase 2...");
	delay(2000);
	os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
#else
	// Single-partition boards: flash only the zigbee URL as main firmware
	if (!ota_download_verify_flash(manifest.zigbee_url, manifest.zigbee_sha256, "app",
	                               OTA_STATUS_DOWNLOADING_ZIGBEE, OTA_STATUS_ERROR_FLASH_ZIGBEE, 5, 98)) {
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	// Done — restore sensor files from backup if originals were lost, then reboot
	ota_restore_sensor_files();
	ota_set_state(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
	delay(5000);
	os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
#endif

	s_ota_task = NULL;
	vTaskDelete(NULL);
}

void online_update_start() {
	if (online_update_in_progress()) return;

	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();

	// Transfer cached manifest to heap param for the task; clear cache
	OnlineUpdateManifest* param = NULL;
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	param = s_manifest_ptr;  // transfer ownership
	s_manifest_ptr = NULL;
	xSemaphoreGive(s_ota_mutex);

	// ota_update_task calls esp_ota_write() which freezes the CPU cache to write flash.
	// With cache frozen PSRAM is inaccessible — any stack access would crash the CPU.
	// This task MUST have its stack in internal RAM; use xTaskCreate, not PSRAM_TASK_CREATE.
	BaseType_t task_ok = xTaskCreate(ota_update_task, "ota_update", 8192, param, 1, &s_ota_task);
	if (task_ok != pdPASS) {
		DEBUG_PRINTF("[OTA] ERROR: ota_update_task create failed (err=%d, int_free=%u)\n",
		             (int)task_ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "OTA task creation failed (low memory)");
		delete param;
	}
}

// FreeRTOS task for phase 2 of OTA update (after reboot)
static void ota_phase2_task(void* param) {
	(void)param;

	// Wait for network connectivity before attempting the download.
	// This task is started early during boot (from online_update_resume()), before
	// WiFi/Ethernet has had time to connect.  Without this wait the HTTP request
	// fails, the continuation file gets deleted, and phase 2 never completes.
	// Supports both WiFi (WL_CONNECTED) and Ethernet (useEth + valid IP).
	{
		auto network_ready = []() -> bool {
			if (useEth) return (bool)eth.localIP();
			return WiFi.status() == WL_CONNECTED;
		};
		const uint32_t NETWORK_WAIT_MS = 60000; // 60 s max
		uint32_t t0 = millis();
		while (!network_ready()) {
			if (millis() - t0 > NETWORK_WAIT_MS) {
				DEBUG_PRINTLN(F("[OTA] Phase 2: network not ready after 60 s — retrying on next boot"));
				// Do NOT remove the continuation file so the next boot can retry.
				s_ota_task = NULL;
				vTaskDelete(NULL);
				return;
			}
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		if (useEth) {
			DEBUG_PRINTF("[OTA] Phase 2: Ethernet ready (%s) — starting download\n",
			             eth.localIP().toString().c_str());
		} else {
			DEBUG_PRINTF("[OTA] Phase 2: WiFi ready (%s) — starting download\n",
			             WiFi.localIP().toString().c_str());
		}
	}

	// Read continuation file
	File f = LittleFS.open(OTA_CONTINUE_FILE, "r");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA] Phase 2: continuation file not readable"));
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}
	String json = f.readString();
	f.close();

	ArduinoJson::JsonDocument doc;
	ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, json);
	if (err) {
		DEBUG_PRINTLN(F("[OTA] Phase 2: JSON parse error in continuation file"));
		LittleFS.remove(OTA_CONTINUE_FILE);
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	const char* url = doc["url"] | "";
	const char* sha256_hex = doc["sha256"] | "";
	const char* label = doc["label"] | "";
	const char* variant = doc["variant"] | "";
	if (!url[0] || !label[0]) {
		DEBUG_PRINTLN(F("[OTA] Phase 2: invalid continuation data"));
		LittleFS.remove(OTA_CONTINUE_FILE);
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	DEBUG_PRINTF("[OTA] Phase 2: flashing %s from %s\n", label, url);
	if (sha256_hex[0]) DEBUG_PRINTF("[OTA] Phase 2 SHA-256: %s\n", sha256_hex);

	// Determine status codes based on which partition we're flashing
	OnlineUpdateStatus dl_status = (strcmp(label, "matter") == 0)
		? OTA_STATUS_DOWNLOADING_MATTER : OTA_STATUS_DOWNLOADING_ZIGBEE;
	OnlineUpdateStatus err_status = (strcmp(label, "matter") == 0)
		? OTA_STATUS_ERROR_FLASH_MATTER : OTA_STATUS_ERROR_FLASH_ZIGBEE;

	// Phase 2: download → verify → flash (52-98% overall progress)
	if (!ota_download_verify_flash(url, sha256_hex[0] ? sha256_hex : nullptr, label,
	                               dl_status, err_status, 52, 98)) {
		LittleFS.remove(OTA_CONTINUE_FILE);
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	// Remove continuation file before setting boot and rebooting
	LittleFS.remove(OTA_CONTINUE_FILE);

	// Restore sensor files from backup if originals were lost
	ota_restore_sensor_files();

#if defined(ESP32C5)
	// Set boot partition to the user's original variant
	IEEE802154BootVariant boot_var = (strcmp(variant, "matter") == 0)
		? IEEE802154BootVariant::MATTER : IEEE802154BootVariant::ZIGBEE;
	ieee802154_select_otf_boot_variant(boot_var);
#endif

	ota_set_state(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
	delay(5000);
	os.reboot_dev(REBOOT_CAUSE_FWUPDATE);

	s_ota_task = NULL;
	vTaskDelete(NULL);
}

// Called during boot to check for and resume a two-phase OTA update.
// Restores password immediately (factory_reset in options_setup may have wiped it).
void online_update_resume() {
	if (!LittleFS.exists(OTA_CONTINUE_FILE)) return;

	DEBUG_PRINTLN(F("[OTA] Continuation file found — resuming phase 2"));
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();

	// Restore password BEFORE starting the phase 2 task.
	// factory_reset() in options_setup() may have wiped it to the default.
	// We read it now so the HTTP server can authenticate UI requests.
	{
		File f = LittleFS.open(OTA_CONTINUE_FILE, "r");
		if (f) {
			String json = f.readString();
			f.close();
			ArduinoJson::JsonDocument doc;
			if (!ArduinoJson::deserializeJson(doc, json)) {
				const char* savedPw = doc["pw"] | "";
				if (savedPw[0]) {
					os.sopt_save(SOPT_PASSWORD, savedPw);
					DEBUG_PRINTLN(F("[OTA] Password restored from continuation file"));
				}
			}
		}
	}

	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 50, "Resuming update phase 2...");
	// Same constraint as ota_update_task: esp_ota_write requires an internal-RAM stack.
	BaseType_t task_ok = xTaskCreate(ota_phase2_task, "ota_phase2", 8192, NULL, 1, &s_ota_task);
	if (task_ok != pdPASS) {
		DEBUG_PRINTF("[OTA] ERROR: ota_phase2_task create failed (err=%d, int_free=%u)\n",
		             (int)task_ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Phase 2 task creation failed (low memory)");
	}
}

// Helper: cache manifest after a check so the update task can use it
void online_update_cache_manifest(const OnlineUpdateManifest &manifest) {
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	if (!s_manifest_ptr) s_manifest_ptr = new OnlineUpdateManifest();
	*s_manifest_ptr = manifest;
	xSemaphoreGive(s_ota_mutex);
}

// Service deferred online update work from the main loop (ESP32: no-op, task-based).
void online_update_loop() {
	// ESP32 OTA work is task-based and does not require loop servicing.
}

#elif defined(ESP8266)

#include "online_update.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include "opensprinkler_server.h"
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Updater.h>
#include <LittleFS.h>
#include "ArduinoJson.hpp"

extern OpenSprinkler os;

static OnlineUpdateState s_state = { OTA_STATUS_IDLE, 0, "" };
static OnlineUpdateManifest* s_manifest_ptr = NULL; // heap-allocated only during OTA
static bool s_update_in_progress = false;
static bool s_update_pending = false;

#define OTA_ESP8266_RESTORE_FILE "/ota_esp8266_restore.json"

static void ota_set_state(OnlineUpdateStatus status, uint8_t progress, const char* msg);

static void ota_save_esp8266_restore_state() {
	yield();
	File f = LittleFS.open(OTA_ESP8266_RESTORE_FILE, "w");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Failed to open restore file"));
		return;
	}

	// Use String-returning sopt_load to avoid 1300+ bytes on stack
	ArduinoJson::JsonDocument doc;
	doc["pw"] = os.sopt_load(SOPT_PASSWORD);
	doc["ssid"] = os.sopt_load(SOPT_STA_SSID);
	doc["pass"] = os.sopt_load(SOPT_STA_PASS);
	doc["bssid_chl"] = os.sopt_load(SOPT_STA_BSSID_CHL);
	doc["wifi_mode"] = os.iopts[IOPT_WIFI_MODE];

	if (ArduinoJson::serializeJson(doc, f) == 0) {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Failed to write restore state"));
	} else {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Saved restore state (pw/wifi)"));
	}
	f.close();
	yield();

	// Backup sensor config files to /backup/ directory
	const char* sensor_files[] = {
		"/sensors.json", "/progsensor.json", "/monitors.json"
	};
	LittleFS.mkdir("/backup");
	for (size_t i = 0; i < sizeof(sensor_files) / sizeof(sensor_files[0]); i++) {
		const char* src = sensor_files[i];
		if (!LittleFS.exists(src)) continue;
		char dst[48];
		snprintf(dst, sizeof(dst), "/backup%s", src); // e.g. /backup/sensors.json
		File srcFile = LittleFS.open(src, "r");
		if (!srcFile) continue;
		File dstFile = LittleFS.open(dst, "w");
		if (!dstFile) { srcFile.close(); continue; }
		uint8_t buf[128];
		while (srcFile.available()) {
			int n = srcFile.read(buf, sizeof(buf));
			if (n > 0) dstFile.write(buf, n);
		}
		dstFile.close();
		srcFile.close();
		yield();
	}
	DEBUG_PRINTLN(F("[OTA-ESP8266] Sensor files backed up"));
}

static bool ota_parse_http_url(const char* url, String &host, uint16_t &port, String &uri) {
	if (!url || strncmp(url, "http://", 7) != 0) {
		return false;
	}
	const char* p = url + 7;
	const char* slash = strchr(p, '/');
	String host_port = slash ? String(p).substring(0, (int)(slash - p)) : String(p);
	uri = slash ? String(slash) : String("/");
	int colon = host_port.indexOf(':');
	if (colon >= 0) {
		host = host_port.substring(0, colon);
		port = (uint16_t)host_port.substring(colon + 1).toInt();
		if (port == 0) port = 80;
	} else {
		host = host_port;
		port = 80;
	}
	return host.length() > 0;
}

static bool ota_flash_via_http_client(HTTPClient &http, size_t max_sketch_space, int *last_http_code = NULL, const String &host_header = String()) {
	const char* headerkeys[] = { "Content-Length", "Content-Type", "Location", "x-MD5" };
	http.collectHeaders(headerkeys, sizeof(headerkeys) / sizeof(headerkeys[0]));
	http.setTimeout(30000);
	// Some proxies/vhosts reject the previous HTTP/1.0 style with 400.
	// Keep HTTP/1.1 and force connection close to avoid chunked/keep-alive issues.
	http.useHTTP10(false);
	http.setUserAgent(F("ESP8266-http-Update"));
	http.addHeader(F("Connection"), F("close"), false, true);
	http.addHeader(F("x-ESP8266-mode"), F("sketch"), false, true);
	if (host_header.length()) {
		http.addHeader(F("Host"), host_header, false, true);
		DEBUG_PRINTF("[OTA-ESP8266] Host header: %s\n", host_header.c_str());
	}

	int http_code = http.GET();
	if (last_http_code) {
		*last_http_code = http_code;
	}
	DEBUG_PRINTF("[OTA-ESP8266] HTTP GET code: %d\n", http_code);
	if (http.hasHeader("Content-Type")) {
		DEBUG_PRINTF("[OTA-ESP8266] Content-Type: %s\n", http.header("Content-Type").c_str());
	}
	if (http.hasHeader("Location")) {
		DEBUG_PRINTF("[OTA-ESP8266] Location: %s\n", http.header("Location").c_str());
	}
	if (http.hasHeader("x-MD5")) {
		DEBUG_PRINTF("[OTA-ESP8266] x-MD5: %s\n", http.header("x-MD5").c_str());
	}

	if (http_code != HTTP_CODE_OK) {
		String body = http.getString();
		if (body.length()) {
			DEBUG_PRINTF("[OTA-ESP8266] HTTP error body: %s\n", body.substring(0, 180).c_str());
		}
		char msg[96];
		snprintf(msg, sizeof(msg), "HTTP GET returned %d", http_code);
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, msg);
		http.end();
		return false;
	}

	int content_length = http.getSize();
	DEBUG_PRINTF("[OTA-ESP8266] Content-Length: %d\n", content_length);
	DEBUG_PRINTF("[OTA-ESP8266] Max sketch space: %u\n", (unsigned)max_sketch_space);
	if (content_length <= 0) {
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Missing or invalid Content-Length");
		http.end();
		return false;
	}
	if ((uint32_t)content_length > max_sketch_space) {
		char msg[96];
		snprintf(msg, sizeof(msg), "Binary too large (%d > %u)", content_length, (unsigned)max_sketch_space);
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, msg);
		http.end();
		return false;
	}

	WiFiClient *stream = http.getStreamPtr();
	if (!stream) {
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "HTTP stream unavailable");
		http.end();
		return false;
	}

	Update.onStart([]() {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Update.begin started"));
	});
	Update.onEnd([]() {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Update.end finished"));
	});
	Update.onError([](uint8_t error) {
		DEBUG_PRINTF("[OTA-ESP8266] Update error %u: %s\n", error, Update.getErrorString().c_str());
	});
	Update.onProgress([](size_t cur, size_t total) {
		if (total == 0) return;
		uint8_t progress = 10 + (uint8_t)((cur * 90ULL) / total);
		if (progress > 99) progress = 99;
		s_state.progress = progress;
	});

	if (!Update.begin((size_t)content_length)) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.begin failed: %s\n", Update.getErrorString().c_str());
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}
	if (http.hasHeader("x-MD5")) {
		String md5 = http.header("x-MD5");
		if (md5.length() && !Update.setMD5(md5.c_str())) {
			DEBUG_PRINTF("[OTA-ESP8266] Update.setMD5 failed for %s\n", md5.c_str());
			ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Update.setMD5 failed");
			Update.end(false);
			http.end();
			return false;
		}
	}

	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 10, "Downloading firmware...");
	Update.runAsync(false);

	uint8_t buf[1024];
	size_t written = 0;
	uint32_t last_data_ms = millis();
	while (written < (size_t)content_length) {
		size_t avail = stream->available();
		if (avail > 0) {
			size_t to_read = avail < sizeof(buf) ? avail : sizeof(buf);
			size_t n = stream->readBytes(buf, to_read);
			if (n > 0) {
				size_t w = Update.write(buf, n);
				if (w != n) {
					String err = Update.getErrorString();
					if (!err.length()) err = F("Flash write failed");
					DEBUG_PRINTF("[OTA-ESP8266] Update.write failed: %s\n", err.c_str());
					ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, err.c_str());
					Update.end(false);
					http.end();
					return false;
				}
				written += n;
				last_data_ms = millis();
				uint8_t progress = 10 + (uint8_t)((written * 90ULL) / (size_t)content_length);
				if (progress > 99) progress = 99;
				s_state.progress = progress;
			}
		}

		if ((millis() - last_data_ms) > 15000) {
			DEBUG_PRINTLN(F("[OTA-ESP8266] Stream timeout while reading firmware"));
			ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Stream Read Timeout");
			Update.end(false);
			http.end();
			return false;
		}

		yield();
	}

	DEBUG_PRINTF("[OTA-ESP8266] Written bytes: %u of %d\n", (unsigned)written, content_length);

	if (!Update.end()) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.end failed: %s\n", Update.getErrorString().c_str());
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}

	if (Update.hasError()) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.hasError: %s\n", Update.getErrorString().c_str());
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}

	http.end();
	return true;
}

static void ota_set_state(OnlineUpdateStatus status, uint8_t progress, const char* msg) {
	s_state.status = status;
	s_state.progress = progress;
	strncpy(s_state.message, msg ? msg : "", sizeof(s_state.message) - 1);
	s_state.message[sizeof(s_state.message) - 1] = '\0';
	DEBUG_PRINT(F("[OTA-ESP8266] "));
	DEBUG_PRINTLN(s_state.message);
}

OnlineUpdateState online_update_get_state() {
	return s_state;
}

bool online_update_in_progress() {
	return s_update_in_progress || s_update_pending;
}

bool online_update_check(OnlineUpdateManifest &manifest) {
	memset(&manifest, 0, sizeof(manifest));
	strncpy(manifest.zigbee_url, OTA_ESP8266_FW_URL, sizeof(manifest.zigbee_url) - 1);
	manifest.fw_version = OS_FW_VERSION;
	manifest.fw_minor = OS_FW_MINOR;
	strncpy(manifest.changelog, "ESP8266 direct online update", sizeof(manifest.changelog) - 1);
	manifest.valid = true;
	ota_set_state(OTA_STATUS_AVAILABLE, 0, "Ready to download firmware");
	return true;
}

void online_update_cache_manifest(const OnlineUpdateManifest &manifest) {
	if (!s_manifest_ptr) s_manifest_ptr = new OnlineUpdateManifest();
	*s_manifest_ptr = manifest;
}

// Variant selection is ESP32C5-only; no-op stub for ESP8266
void online_update_set_variant(const char* variant) {
	(void)variant;
}

// Safe-check wrapper is ESP32-only; on ESP8266 call the regular check directly
bool online_update_check_safe(OnlineUpdateManifest &manifest) {
	return online_update_check(manifest);
}

void online_update_start() {
	if (s_update_in_progress || s_update_pending) return;

	if (!s_manifest_ptr || !s_manifest_ptr->valid) {
		if (!s_manifest_ptr) s_manifest_ptr = new OnlineUpdateManifest();
		if (!online_update_check(*s_manifest_ptr)) {
			delete s_manifest_ptr;
			s_manifest_ptr = NULL;
			return;
		}
	}

	s_update_pending = true;
	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "ESP8266 update scheduled...");
}

void online_update_loop() {
	if (!s_update_pending || s_update_in_progress) {
		return;
	}

	s_update_pending = false;
	s_update_in_progress = true;
	os.status.req_mqtt_restart = false;
	yield();
	OnlineUpdateManifest manifest;
	if (s_manifest_ptr) {
		manifest = *s_manifest_ptr;
		delete s_manifest_ptr;
		s_manifest_ptr = NULL;
	}
	uint32_t max_sketch_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
	DEBUG_PRINTF("[OTA-ESP8266] URL: %s\n", manifest.zigbee_url);
	DEBUG_PRINTF("[OTA-ESP8266] ESP.getFreeSketchSpace(): %u\n", (unsigned)ESP.getFreeSketchSpace());
	DEBUG_PRINTF("[OTA-ESP8266] Computed max sketch space: %u\n", (unsigned)max_sketch_space);

	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 5, "Starting ESP8266 update...");
	bool ok = false;

	const char* ota_urls[2] = {
		manifest.zigbee_url,
		"http://www.opensprinklershop.de/upgrade/firmware_esp8266.bin"
	};
	int last_http_code = 0;
	for (uint8_t i = 0; i < 2 && !ok; i++) {
		if (i > 0) {
			DEBUG_PRINTF("[OTA-ESP8266] Retrying with fallback URL: %s\n", ota_urls[i]);
		}
		WiFiClient plainClient;
		HTTPClient http;
		String host;
		String uri;
		uint16_t port = 80;
		String host_header;
		bool began = false;
		if (ota_parse_http_url(ota_urls[i], host, port, uri)) {
			IPAddress resolved_ip;
			int resolve_ok = WiFi.hostByName(host.c_str(), resolved_ip, 2000);
			if (resolve_ok == 1 && (bool)resolved_ip) {
				String ip_host = resolved_ip.toString();
				DEBUG_PRINTF("[OTA-ESP8266] Resolved %s -> %s\n", host.c_str(), ip_host.c_str());
				host_header = host;
				began = http.begin(plainClient, ip_host, port, uri);
			} else {
				DEBUG_PRINTF("[OTA-ESP8266] DNS resolve failed for %s, falling back to URL begin\n", host.c_str());
			}
		}
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		if (!began) {
			began = http.begin(plainClient, ota_urls[i]);
		}
		if (!began) {
			ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "HTTP begin failed");
			continue;
		}
		last_http_code = 0;
		ok = ota_flash_via_http_client(http, max_sketch_space, &last_http_code, host_header);
		if (!ok && last_http_code != HTTP_CODE_NOT_FOUND) {
			break;
		}
	}

	if (ok) {
		ota_save_esp8266_restore_state();
		ota_set_state(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
		delay(500);
		s_update_in_progress = false;
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
		return;
	}

	s_update_in_progress = false;
}

void online_update_resume() {
	if (!LittleFS.exists(OTA_ESP8266_RESTORE_FILE)) {
		return;
	}

	DEBUG_PRINTLN(F("[OTA-ESP8266] Restore state file found"));
	File f = LittleFS.open(OTA_ESP8266_RESTORE_FILE, "r");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Failed to open restore state file"));
		return;
	}

	String json = f.readString();
	f.close();

	ArduinoJson::JsonDocument doc;
	ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, json);
	if (err) {
		DEBUG_PRINTLN(F("[OTA-ESP8266] Restore JSON parse failed"));
		LittleFS.remove(OTA_ESP8266_RESTORE_FILE);
		return;
	}

	const char* pw = doc["pw"] | "";
	const char* ssid = doc["ssid"] | "";
	const char* pass = doc["pass"] | "";
	const char* bssid_chl = doc["bssid_chl"] | "";
	uint8_t wifi_mode = doc["wifi_mode"] | WIFI_MODE_STA;

	DEBUG_PRINTF("[OTA-ESP8266] Restore pw=%s ssid=%s wifi_mode=%d\n", pw, ssid, (int)wifi_mode);

	if (pw[0]) os.sopt_save(SOPT_PASSWORD, pw);
	if (ssid[0]) {
		os.sopt_save(SOPT_STA_SSID, ssid);
		os.wifi_ssid = ssid;
	}
	if (pass[0]) {
		os.sopt_save(SOPT_STA_PASS, pass);
		os.wifi_pass = pass;
	}
	if (bssid_chl[0]) os.sopt_save(SOPT_STA_BSSID_CHL, bssid_chl);

	os.iopts[IOPT_WIFI_MODE] = wifi_mode;
	os.iopts_save();

	LittleFS.remove(OTA_ESP8266_RESTORE_FILE);
	DEBUG_PRINTLN(F("[OTA-ESP8266] Restored password and WiFi settings"));

	// Restore sensor files from backup if originals are missing
	const char* sensor_files[] = {
		"/sensors.json", "/progsensor.json", "/monitors.json"
	};
	for (size_t i = 0; i < sizeof(sensor_files) / sizeof(sensor_files[0]); i++) {
		const char* path = sensor_files[i];
		char backup_path[48];
		snprintf(backup_path, sizeof(backup_path), "/backup%s", path);
		if (!LittleFS.exists(path) && LittleFS.exists(backup_path)) {
			DEBUG_PRINTF("[OTA-ESP8266] Restoring %s from backup\n", path);
			File srcFile = LittleFS.open(backup_path, "r");
			if (!srcFile) continue;
			File dstFile = LittleFS.open(path, "w");
			if (!dstFile) { srcFile.close(); continue; }
			uint8_t buf[128];
			while (srcFile.available()) {
				int n = srcFile.read(buf, sizeof(buf));
				if (n > 0) dstFile.write(buf, n);
			}
			dstFile.close();
			srcFile.close();
		}
		// Clean up backup file
		LittleFS.remove(backup_path);
	}
	LittleFS.rmdir("/backup");
	DEBUG_PRINTLN(F("[OTA-ESP8266] Sensor restore complete"));
}

#endif // ESP32 / ESP8266
