#if defined(ESP32)

#include "online_update.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
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
extern OSEthernet eth;  // Ethernet interface object (main.cpp)
extern void reboot_in(uint32_t ms, uint8_t cause); // main.cpp - Ticker-based deferred reboot

// Thread-safe state
static SemaphoreHandle_t s_ota_mutex = NULL;
static OnlineUpdateState s_state = { OTA_STATUS_IDLE, 0, "" };
static OnlineUpdateManifest* s_manifest_ptr = NULL; // heap-allocated only during OTA
static TaskHandle_t s_ota_task = NULL;
// Optional variant override ("zigbee" or "matter"); empty = keep current
static char s_ota_requested_variant[8] = {0};
// Set true during boot when OTA files are detected.  Stays true until the
// update finishes and the device reboots to normal mode.  Heavy services
// (Matter, RainMaker, BLE, Zigbee, sensor radios) must not start.
static bool s_ota_boot = false;

#if defined(ESP32C5)
static IEEE802154BootVariant ota_compiled_variant() {
#if defined(ENABLE_MATTER)
	return IEEE802154BootVariant::MATTER;
#elif defined(OS_ENABLE_ZIGBEE)
	return IEEE802154BootVariant::ZIGBEE;
#else
	// Safe default for unexpected builds without an explicit 802.15.4 role.
	return IEEE802154BootVariant::MATTER;
#endif
}
#endif

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
	if (s_ota_boot) return true;   // booted into OTA-only mode
	if (!s_ota_mutex) return false;
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	bool busy = (s_state.status >= OTA_STATUS_DOWNLOADING_ZIGBEE && s_state.status <= OTA_STATUS_FLASHING_MATTER)
	         || s_state.status == OTA_STATUS_REBOOTING_PHASE2
	         || s_state.status == OTA_STATUS_REBOOTING_OTA;
	xSemaphoreGive(s_ota_mutex);
	return busy;
}

bool online_update_check(OnlineUpdateManifest &manifest) {
	memset(&manifest, 0, sizeof(manifest));
	manifest.valid = false;

	if (useEth) {
		DEBUG_PRINTF("[OTA] Ethernet IP: %s\n", eth.localIP().toString().c_str());
	} else {
		DEBUG_PRINTF("[OTA] WiFi status: %d, IP: %s, RSSI: %d dBm\n",
			(int)WiFi.status(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
	}
	DEBUG_PRINTF("[OTA] Manifest URL: %s\n", OTA_MANIFEST_URL);
	DEBUG_PRINTF("[OTA] heap_free=%u  internal_free=%u  psram_free=%u\n",
		(unsigned)ESP.getFreeHeap(),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

	ota_set_state(OTA_STATUS_CHECKING, 0, "Checking for updates...");

	WiFiClientSecure *client = new WiFiClientSecure();
	if (!client) {
		DEBUG_PRINTLN(F("[OTA] ERROR: WiFiClientSecure allocation failed"));
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Failed to create HTTPS client");
		return false;
	}
	client->setInsecure();

	// Scope block: HTTPClient must be destroyed BEFORE delete client,
	// because its destructor may access the WiFiClient through _client.
	String payload;
	bool httpOk = false;
	{
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(15000);

		DEBUG_PRINTLN(F("[OTA] Connecting to update server (HTTPS)..."));
		uint32_t t_conn = millis();
		if (http.begin(*client, OTA_MANIFEST_URL)) {
			int httpCode = http.GET();
			DEBUG_PRINTF("[OTA] HTTP response code: %d  (after %u ms)\n",
				httpCode, (unsigned)(millis() - t_conn));
			if (httpCode == HTTP_CODE_OK) {
				payload = http.getString();
				DEBUG_PRINTF("[OTA] Manifest received (%u bytes)\n", (unsigned)payload.length());
				httpOk = true;
			} else {
				DEBUG_PRINTF("[OTA] ERROR: HTTP %d (%s) | heap=%u rssi=%d\n",
					httpCode, HTTPClient::errorToString(httpCode).c_str(),
					(unsigned)ESP.getFreeHeap(), useEth ? 0 : (int)WiFi.RSSI());
				char buf[80];
				snprintf(buf, sizeof(buf), "HTTP error %d", httpCode);
				ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, buf);
			}
			http.end();
		} else {
			DEBUG_PRINTF("[OTA] ERROR: http.begin() failed — connect failed | heap=%u rssi=%d\n",
				(unsigned)ESP.getFreeHeap(), useEth ? 0 : (int)WiFi.RSSI());
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
	constexpr int    MAX_DL_RETRIES = 10;
	constexpr int    RETRY_DELAY_MS = 5000;
	const uint8_t    range       = pMax - pMin;

	char     msg[96];
	bool     success     = false;
	uint8_t *img_buf     = nullptr;  // full firmware image buffered in PSRAM
	uint8_t *io_buf      = nullptr;  // 4 KB internal-RAM rx / write buffer
	int      imgSize     = 0;        // content-length / total bytes
	bool     download_ok = false;

	snprintf(msg, sizeof(msg), "Downloading %s firmware...", partLabel);
	ota_set_state(dl_status, pMin, msg);

	// Allocate internal-RAM I/O buffer once — reused across retries and for flash writes
	io_buf = (uint8_t*)heap_caps_malloc(IO_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (!io_buf) {
		DEBUG_PRINTF("[OTA] Internal I/O buf alloc failed (internal free=%u)\n",
			(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
		ota_set_state(err_status, 0, "I/O buf alloc failed");
		goto final_cleanup;
	}

	DEBUG_PRINTF("[OTA] Download URL (%s): %s\n", partLabel, url);

	// ── 1. HTTP download with retry ───────────────────────────────────
	for (int attempt = 1; attempt <= MAX_DL_RETRIES && !download_ok; attempt++) {
		if (attempt > 1) {
			snprintf(msg, sizeof(msg), "Retry %d/%d: downloading %s...", attempt, MAX_DL_RETRIES, partLabel);
			ota_set_state(dl_status, pMin, msg);
			DEBUG_PRINTF("[OTA] Download retry %d/%d for %s\n", attempt, MAX_DL_RETRIES, partLabel);
			vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
		}

		// Free previous image buffer when retrying
		if (img_buf) { heap_caps_free(img_buf); img_buf = nullptr; }
		imgSize = 0;

		WiFiClientSecure *client = new WiFiClientSecure();
		if (!client) {
			DEBUG_PRINTLN(F("[OTA] WiFiClientSecure alloc failed"));
			continue; // retry with fresh allocation
		}
		client->setInsecure();

		{   // scope: HTTPClient must be destroyed BEFORE delete client
			HTTPClient http;
			http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
			http.setTimeout(30000);

			if (!http.begin(*client, url)) {
				DEBUG_PRINTF("[OTA] http.begin() failed for %s (attempt %d) | heap=%u rssi=%d\n",
					partLabel, attempt, (unsigned)ESP.getFreeHeap(),
					useEth ? 0 : (int)WiFi.RSSI());
				delete client;
				continue;
			}

			int code = http.GET();
			DEBUG_PRINTF("[OTA] HTTP %d for %s (attempt %d)\n", code, partLabel, attempt);
			if (code != HTTP_CODE_OK) {
				snprintf(msg, sizeof(msg), "%s HTTP %d", partLabel, code);
				DEBUG_PRINTF("[OTA] %s (%s) heap=%u rssi=%d (attempt %d)\n",
					msg, HTTPClient::errorToString(code).c_str(),
					(unsigned)ESP.getFreeHeap(),
					useEth ? 0 : (int)WiFi.RSSI(), attempt);
				http.end();
				delete client;
				continue;
			}

			imgSize = http.getSize();
			DEBUG_PRINTF("[OTA] Content-Length for %s: %d bytes\n", partLabel, imgSize);
			if (imgSize <= 0) {
				DEBUG_PRINTF("[OTA] No Content-Length for %s (attempt %d)\n", partLabel, attempt);
				http.end();
				delete client;
				continue;
			}

			// Allocate full image buffer in PSRAM
			img_buf = (uint8_t*)heap_caps_malloc(imgSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (!img_buf) {
				DEBUG_PRINTF("[OTA] PSRAM alloc %d bytes failed (PSRAM free=%u)\n",
					imgSize, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
				snprintf(msg, sizeof(msg), "PSRAM alloc failed for %s", partLabel);
				ota_set_state(err_status, 0, msg);
				http.end();
				delete client;
				goto final_cleanup; // PSRAM exhaustion won't be helped by retry
			}
			DEBUG_PRINTF("[OTA] PSRAM image buffer: %d bytes @ %p\n", imgSize, (void*)img_buf);

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
						DEBUG_PRINTF("[OTA] Download stall for %s after %d/%d bytes (attempt %d)\n",
							partLabel, totalRead, imgSize, attempt);
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
				snprintf(msg, sizeof(msg), "Incomplete: %d/%d bytes (attempt %d/%d)",
					totalRead, imgSize, attempt, MAX_DL_RETRIES);
				DEBUG_PRINTF("[OTA] %s heap=%u rssi=%d\n",
					msg, (unsigned)ESP.getFreeHeap(),
					useEth ? 0 : (int)WiFi.RSSI());
				delete client;
				continue; // retry
			}

			download_ok = true;
		} // HTTPClient destroyed here, before delete client
		delete client;
	} // retry loop

	if (!download_ok) {
		snprintf(msg, sizeof(msg), "Download failed after %d attempts for %s", MAX_DL_RETRIES, partLabel);
		ota_set_state(err_status, 0, msg);
		goto final_cleanup;
	}

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

// Save the full manifest to OTA_START_FILE for a deferred phase-0 start after reboot.
// Called by online_update_start() to persist the manifest before triggering a
// reboot-to-lean-mode so the OTA task has enough free internal RAM for its stack.
static bool ota_save_start_manifest(const OnlineUpdateManifest& m) {
	File f = LittleFS.open(OTA_START_FILE, "w");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA] Failed to write start manifest file"));
		return false;
	}
	static PSRAM_BSS_ATTR char pwBuf[MAX_SOPTS_SIZE + 1];
	os.sopt_load(SOPT_PASSWORD, pwBuf);
	f.printf("{\"fw_version\":%u,\"fw_minor\":%u,"
	         "\"zigbee_url\":\"%s\",\"matter_url\":\"%s\","
	         "\"zigbee_sha256\":\"%s\",\"matter_sha256\":\"%s\","
	         "\"variant\":\"%s\",\"pw\":\"%s\"}",
	         m.fw_version, m.fw_minor,
	         m.zigbee_url, m.matter_url,
	         m.zigbee_sha256, m.matter_sha256,
	         s_ota_requested_variant, pwBuf);
	f.close();
	DEBUG_PRINTLN(F("[OTA] Start manifest saved — OTA will begin after reboot"));
	return true;
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
	static PSRAM_BSS_ATTR char pwBuf[MAX_SOPTS_SIZE + 1];
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

	// Wait for network connectivity. This is a no-op when the task is created
	// from a running system, but essential when started via online_update_resume()
	// on a fresh reboot (phase 0: reboot-before-OTA path) before WiFi is up.
	{
		auto network_ready = []() -> bool {
			if (useEth) return (bool)eth.localIP();
			return WiFi.status() == WL_CONNECTED;
		};
		const uint32_t NETWORK_WAIT_MS = 60000;
		uint32_t t0 = millis();
		while (!network_ready()) {
			if (millis() - t0 > NETWORK_WAIT_MS) {
				DEBUG_PRINTLN(F("[OTA] Network not ready after 60 s — aborting"));
				ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Network not ready for OTA");
				delete cached;
				s_ota_task = NULL;
				delay(5000);
				os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
				vTaskDelete(NULL);
				return;
			}
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		DEBUG_PRINTLN(F("[OTA] Network ready — proceeding with update"));
	}

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
			delay(5000);
			os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
			vTaskDelete(NULL);
			return;
		}
	}

#if defined(ESP32C5)
	// Two-phase OTA: we cannot esp_ota_begin on the running partition (ESP_ERR_OTA_PARTITION_CONFLICT).
	// Phase 1 (5-48%): flash the NON-running partition, save state, reboot into it.
	// Phase 2 (52-98%, after reboot): flash the remaining partition, set boot to target, reboot.
	const esp_partition_t *running = esp_ota_get_running_partition();
	DEBUG_PRINTF("[OTA] Running partition: %s (subtype=%d)\n",
	             running ? running->label : "unknown",
	             running ? (int)running->subtype : -1);
	if (!running) {
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Running partition unknown");
		s_ota_task = NULL;
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
		vTaskDelete(NULL);
		return;
	}

	// Default target must follow the currently running firmware image type
	// (compile-time), not only the current OTA slot. This keeps behavior stable
	// even if slots were previously swapped.
	IEEE802154BootVariant target_variant = ota_compiled_variant();
	esp_partition_subtype_t expected_running_subtype =
		(target_variant == IEEE802154BootVariant::MATTER)
			? ESP_PARTITION_SUBTYPE_APP_OTA_1
			: ESP_PARTITION_SUBTYPE_APP_OTA_0;
	if (running->subtype != expected_running_subtype) {
		DEBUG_PRINTF("[OTA] Detected swapped slot layout: compiled=%s expects subtype=%d, running subtype=%d\n",
			(target_variant == IEEE802154BootVariant::MATTER) ? "matter" : "zigbee",
			(int)expected_running_subtype,
			(int)running->subtype);
	}
	// Consume requested variant override (set by server_update_upgrade via online_update_set_variant)
	char requested_variant_buf[8] = {0};
	strncpy(requested_variant_buf, s_ota_requested_variant, sizeof(requested_variant_buf) - 1);
	s_ota_requested_variant[0] = 0; // consume
	if (requested_variant_buf[0]) {
		target_variant = (strcmp(requested_variant_buf, "matter") == 0)
			? IEEE802154BootVariant::MATTER : IEEE802154BootVariant::ZIGBEE;
	}
	const char* target_variant_name = (target_variant == IEEE802154BootVariant::MATTER) ? "matter" : "zigbee";

	if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
		// Running from matter (ota_1) → flash zigbee (ota_0) first
		if (!ota_download_verify_flash(manifest.zigbee_url, manifest.zigbee_sha256, "zigbee",
		                               OTA_STATUS_DOWNLOADING_ZIGBEE, OTA_STATUS_ERROR_FLASH_ZIGBEE, 5, 48)) {
			s_ota_task = NULL;
			delay(5000);
			os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash matter after reboot
		ota_save_continuation(manifest.matter_url, manifest.matter_sha256, "matter", target_variant_name);
		// Set boot to zigbee (the partition we just flashed) so phase 2 can write matter
		esp_ota_set_boot_partition(esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr));
	} else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
		// Running from zigbee (ota_0) → flash matter (ota_1) first
		if (!ota_download_verify_flash(manifest.matter_url, manifest.matter_sha256, "matter",
		                               OTA_STATUS_DOWNLOADING_MATTER, OTA_STATUS_ERROR_FLASH_MATTER, 5, 48)) {
			s_ota_task = NULL;
			delay(5000);
			os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash zigbee after reboot
		ota_save_continuation(manifest.zigbee_url, manifest.zigbee_sha256, "zigbee", target_variant_name);
		// Set boot to matter (the partition we just flashed) so phase 2 can write zigbee
		esp_ota_set_boot_partition(esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr));
	} else {
		ota_set_state(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Unsupported running partition subtype");
		s_ota_task = NULL;
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
		vTaskDelete(NULL);
		return;
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
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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

	if (!param || !param->valid) {
		delete param;
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "No manifest cached — check for updates first");
		return;
	}

	// Always reboot into OTA-only mode: save manifest to LittleFS and reboot.
	// On the next boot online_update_resume() picks up OTA_START_FILE and creates
	// the OTA task before any heavy services (Matter/BLE/Zigbee/RainMaker/sensors)
	// are initialized.  The s_ota_boot flag prevents those services from starting
	// in do_loop(), guaranteeing enough internal RAM for the 8 KB OTA task stack.
	DEBUG_PRINTF("[OTA] Saving manifest and rebooting into OTA-only mode (int_free=%u)\n",
	             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

	if (ota_save_start_manifest(*param)) {
		delete param;
		ota_set_state(OTA_STATUS_REBOOTING_OTA, 0, "Rebooting to free memory for OTA...");
		reboot_in(1500, REBOOT_CAUSE_FWUPDATE);
	} else {
		delete param;
		ota_set_state(OTA_STATUS_ERROR_LOW_MEMORY, 0,
			"Failed to save OTA manifest. Try rebooting and retrying.");
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
				delay(5000);
				os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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
		delay(5000);
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
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
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();

	// ── Detect OTA-only boot ──────────────────────────────────────────────────
	// If either the start manifest or continuation file exists, we booted into
	// OTA-only mode.  Set s_ota_boot so that online_update_in_progress() returns
	// true immediately, preventing do_loop() from starting heavy services
	// (Matter, RainMaker, BLE, Zigbee, sensor radios).
	if (LittleFS.exists(OTA_START_FILE) || LittleFS.exists(OTA_CONTINUE_FILE)) {
		s_ota_boot = true;
		DEBUG_PRINTLN(F("[OTA] OTA-only boot mode — heavy services will be skipped"));
	}

	// ── Phase 0: deferred start ───────────────────────────────────────────────
	// online_update_start() saved the manifest and rebooted into OTA-only mode.
	// Pick it up here — before Zigbee/BLE/Matter are initialized — so the 8 KB
	// internal-RAM task stack can be allocated without contention.
	if (LittleFS.exists(OTA_START_FILE)) {
		DEBUG_PRINTLN(F("[OTA] Phase-0 start manifest found — beginning OTA after reboot"));

		OnlineUpdateManifest* task_param = new OnlineUpdateManifest();
		bool ok = false;

		File f = LittleFS.open(OTA_START_FILE, "r");
		if (f) {
			String json = f.readString();
			f.close();
			LittleFS.remove(OTA_START_FILE); // consume before starting task

			ArduinoJson::JsonDocument doc;
			if (!ArduinoJson::deserializeJson(doc, json)) {
				task_param->fw_version = doc["fw_version"] | (uint16_t)0;
				task_param->fw_minor   = doc["fw_minor"]   | (uint16_t)0;
				strncpy(task_param->zigbee_url,    doc["zigbee_url"]    | "", sizeof(task_param->zigbee_url)    - 1);
				strncpy(task_param->matter_url,    doc["matter_url"]    | "", sizeof(task_param->matter_url)    - 1);
				strncpy(task_param->zigbee_sha256, doc["zigbee_sha256"] | "", sizeof(task_param->zigbee_sha256) - 1);
				strncpy(task_param->matter_sha256, doc["matter_sha256"] | "", sizeof(task_param->matter_sha256) - 1);
				task_param->valid = (task_param->zigbee_url[0] != '\0');

				const char* variant = doc["variant"] | "";
				if (variant[0])
					strncpy(s_ota_requested_variant, variant, sizeof(s_ota_requested_variant) - 1);

				const char* savedPw = doc["pw"] | "";
				if (savedPw[0]) {
					DEBUG_PRINTF("[OTA] restoring password slot from manifest pw='%.*s'\n", 16, savedPw);
					os.sopt_save(SOPT_PASSWORD, savedPw);
					DEBUG_PRINTLN(F("[OTA] Password restored from phase-0 manifest"));
				}
				ok = task_param->valid;
			} else {
				DEBUG_PRINTLN(F("[OTA] Failed to parse phase-0 manifest JSON"));
			}
		} else {
			LittleFS.remove(OTA_START_FILE);
		}

		if (ok) {
			ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "Starting update (post-reboot)...");
			DEBUG_PRINTF("[OTA] Creating OTA task (int_free=%u)\n",
			             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
			BaseType_t task_ok = xTaskCreate(ota_update_task, "ota_update", 8192, task_param, 1, &s_ota_task);
			if (task_ok != pdPASS) {
				DEBUG_PRINTF("[OTA] ERROR: task create failed even after reboot (int_free=%u)\n",
				             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
				ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "OTA task creation failed even after reboot");
				delete task_param;
			}
		} else {
			DEBUG_PRINTLN(F("[OTA] Phase-0 manifest invalid — ignoring"));
			delete task_param;
		}
		return; // do not fall through to phase-2 check
	}

	// ── Phase 2: continuation after phase-1 reboot ───────────────────────────
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
					DEBUG_PRINTF("[OTA] restoring password slot from continuation pw='%.*s'\n", 16, savedPw);
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
#include <WiFiUdp.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include <LittleFS.h>
#include <memory>
#include "ArduinoJson.hpp"

extern OpenSprinkler os;

#define OTA_ESP8266_RESTORE_FILE "/ota_esp8266_restore.json"

// All ESP8266 online-update state and logic is encapsulated here. A single
// static instance (s_updater) holds what used to be a set of file-scope
// statics. The public online_update_* free functions (declared in
// online_update.h, called from the web server and main loop) are thin wrappers
// that delegate to this instance.
class ESP8266OTAUpdater {
public:
	~ESP8266OTAUpdater() { delete _manifest; }

	OnlineUpdateState getState() const { return _state; }
	bool inProgress() const { return _updateInProgress || _updatePending || _checkPending; }
	// True when the instance holds no work and no cached manifest, so it can be
	// freed to return the RAM (incl. the 1 KB stream buffer) to the heap.
	bool isReleasable() const {
		return !_updateInProgress && !_updatePending && !_checkPending && _manifest == NULL;
	}
	bool check(OnlineUpdateManifest &manifest);
	bool checkSafe(OnlineUpdateManifest &manifest) { return check(manifest); }
	void cacheManifest(const OnlineUpdateManifest &manifest);
	void setVariant(const char* variant) { (void)variant; } // ESP32C5-only; no-op here
	void start();
	void loop();
	static void resume(); // reads LittleFS only — no instance state needed

private:
	void setState(OnlineUpdateStatus status, uint8_t progress, const char* msg);
	void saveRestoreState();
	bool flashViaHttpClient(HTTPClient &http, size_t max_sketch_space, int *last_http_code = NULL);
	static bool parseHttpUrl(const char* url, String &host, uint16_t &port, String &uri, bool &is_https);

	OnlineUpdateState _state = { OTA_STATUS_IDLE, 0, "" };
	OnlineUpdateManifest* _manifest = NULL; // heap-allocated only during OTA
	bool _updateInProgress = false;
	bool _updatePending = false;
	// Set by start() when no manifest is cached yet. The (blocking) manifest
	// download then runs in loop() instead of the web request handler, so the
	// /uu response is sent immediately and the hardware watchdog is fed during
	// the fetch (avoids rst cause:4 wdt reset).
	bool _checkPending = false;
	uint8_t _streamBuf[1024];
};

// The updater is created dynamically only while an update is scheduled/running,
// so idle devices don't reserve its RAM (incl. the 1 KB stream buffer).
// s_lastState preserves the final status for the UI (/us polling) after the
// instance has been released.
static ESP8266OTAUpdater* s_updater = NULL;
static OnlineUpdateState s_lastState = { OTA_STATUS_IDLE, 0, "" };

static ESP8266OTAUpdater* ota_ensure_updater() {
	if (!s_updater) s_updater = new ESP8266OTAUpdater();
	return s_updater;
}

void ESP8266OTAUpdater::saveRestoreState() {
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

bool ESP8266OTAUpdater::parseHttpUrl(const char* url, String &host, uint16_t &port, String &uri, bool &is_https) {
	is_https = false;
	const char* p = nullptr;
	if (strncmp(url, "https://", 8) == 0) {
		is_https = true;
		p = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		p = url + 7;
	} else {
		return false;
	}
	const char* slash = strchr(p, '/');
	String host_port = slash ? String(p).substring(0, (int)(slash - p)) : String(p);
	uri = slash ? String(slash) : String("/");
	int colon = host_port.indexOf(':');
	if (colon >= 0) {
		host = host_port.substring(0, colon);
		port = (uint16_t)host_port.substring(colon + 1).toInt();
		if (port == 0) port = (is_https ? 443 : 80);
	} else {
		host = host_port;
		port = (is_https ? 443 : 80);
	}
	return host.length() > 0;
}

bool ESP8266OTAUpdater::flashViaHttpClient(HTTPClient &http, size_t max_sketch_space, int *last_http_code) {
	const char* headerkeys[] = { "Content-Length", "Content-Type", "Location", "x-MD5" };
	http.collectHeaders(headerkeys, sizeof(headerkeys) / sizeof(headerkeys[0]));
	http.setTimeout(30000);
	// Some proxies/vhosts reject the previous HTTP/1.0 style with 400.
	// Keep HTTP/1.1 and force connection close to avoid chunked/keep-alive issues.
	http.useHTTP10(false);
	http.setUserAgent(F("ESP8266-http-Update"));
	http.addHeader(F("Connection"), F("close"), false, true);
	http.addHeader(F("x-ESP8266-mode"), F("sketch"), false, true);

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
		setState(OTA_STATUS_ERROR_NETWORK, 0, msg);
		http.end();
		return false;
	}

	int content_length = http.getSize();
	DEBUG_PRINTF("[OTA-ESP8266] Content-Length: %d\n", content_length);
	DEBUG_PRINTF("[OTA-ESP8266] Max sketch space: %u\n", (unsigned)max_sketch_space);
	if (content_length <= 0) {
		setState(OTA_STATUS_ERROR_NETWORK, 0, "Missing or invalid Content-Length");
		http.end();
		return false;
	}
	if ((uint32_t)content_length > max_sketch_space) {
		char msg[96];
		snprintf(msg, sizeof(msg), "Binary too large (%d > %u)", content_length, (unsigned)max_sketch_space);
		setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, msg);
		http.end();
		return false;
	}

	WiFiClient *stream = http.getStreamPtr();
	if (!stream) {
		setState(OTA_STATUS_ERROR_NETWORK, 0, "HTTP stream unavailable");
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
	Update.onProgress([this](size_t cur, size_t total) {
		if (total == 0) return;
		uint8_t progress = 10 + (uint8_t)((cur * 90ULL) / total);
		if (progress > 99) progress = 99;
		_state.progress = progress;
	});

	if (!Update.begin((size_t)content_length)) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.begin failed: %s\n", Update.getErrorString().c_str());
		setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}
	if (http.hasHeader("x-MD5")) {
		String md5 = http.header("x-MD5");
		if (md5.length() && !Update.setMD5(md5.c_str())) {
			DEBUG_PRINTF("[OTA-ESP8266] Update.setMD5 failed for %s\n", md5.c_str());
			setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Update.setMD5 failed");
			Update.end(false);
			http.end();
			return false;
		}
	}

	setState(OTA_STATUS_DOWNLOADING_ZIGBEE, 10, "Downloading firmware...");
	Update.runAsync(false);
	DEBUG_PRINTF("[OTA-ESP8266] Streaming start: heap=%u connected=%d\n",
		(unsigned)ESP.getFreeHeap(), stream->connected() ? 1 : 0);

	size_t written = 0;
	uint32_t last_data_ms = millis();
	uint32_t loop_start_ms = millis();
	uint32_t last_progress_log_ms = millis();
	size_t last_logged_written = 0;
	uint32_t stall_iters = 0;
	while (written < (size_t)content_length) {
		size_t avail = stream->available();
		if (avail > 0) {
			stall_iters = 0;
			size_t to_read = avail < sizeof(_streamBuf) ? avail : sizeof(_streamBuf);
			size_t n = stream->readBytes(_streamBuf, to_read);
			if (n > 0) {
				size_t w = Update.write(_streamBuf, n);
				if (w != n) {
					String err = Update.getErrorString();
					if (!err.length()) err = F("Flash write failed");
					DEBUG_PRINTF("[OTA-ESP8266] Update.write failed at %u/%d (wrote %u of %u): %s\n",
						(unsigned)written, content_length, (unsigned)w, (unsigned)n, err.c_str());
					setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, err.c_str());
					Update.end(false);
					http.end();
					return false;
				}
				written += n;
				last_data_ms = millis();
				uint8_t progress = 10 + (uint8_t)((written * 90ULL) / (size_t)content_length);
				if (progress > 99) progress = 99;
				_state.progress = progress;
			} else {
				DEBUG_PRINTF("[OTA-ESP8266] readBytes returned 0 (avail=%u) at %u/%d\n",
					(unsigned)avail, (unsigned)written, content_length);
			}
		} else {
			// No data yet: track how often we spin without progress so a stalled
			// stream is visible in the log before the 15s timeout trips.
			stall_iters++;
			if ((stall_iters % 2000) == 0) {
				DEBUG_PRINTF("[OTA-ESP8266] Waiting for data: written=%u/%d idle=%lums heap=%u connected=%d\n",
					(unsigned)written, content_length, (unsigned long)(millis() - last_data_ms),
					(unsigned)ESP.getFreeHeap(), stream->connected() ? 1 : 0);
			}
		}

		// Periodic progress heartbeat (every ~2s) with throughput + heap so a
		// slow-down or WDT reset can be correlated against the last logged point.
		if ((millis() - last_progress_log_ms) >= 2000) {
			uint32_t dt = millis() - last_progress_log_ms;
			size_t delta = written - last_logged_written;
			DEBUG_PRINTF("[OTA-ESP8266] Progress %u/%d (%u%%) %uB/s heap=%u elapsed=%lums\n",
				(unsigned)written, content_length, (unsigned)_state.progress,
				(unsigned)(dt ? (delta * 1000UL / dt) : 0), (unsigned)ESP.getFreeHeap(),
				(unsigned long)(millis() - loop_start_ms));
			last_progress_log_ms = millis();
			last_logged_written = written;
		}

		if ((millis() - last_data_ms) > 15000) {
			DEBUG_PRINTF("[OTA-ESP8266] Stream timeout: written=%u/%d heap=%u connected=%d\n",
				(unsigned)written, content_length, (unsigned)ESP.getFreeHeap(),
				stream->connected() ? 1 : 0);
			setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, "Stream Read Timeout");
			Update.end(false);
			http.end();
			return false;
		}

		yield();
	}

	DEBUG_PRINTF("[OTA-ESP8266] Streaming done: %u of %d bytes in %lums heap=%u\n",
		(unsigned)written, content_length, (unsigned long)(millis() - loop_start_ms),
		(unsigned)ESP.getFreeHeap());

	if (!Update.end()) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.end failed: %s\n", Update.getErrorString().c_str());
		setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}

	if (Update.hasError()) {
		DEBUG_PRINTF("[OTA-ESP8266] Update.hasError: %s\n", Update.getErrorString().c_str());
		setState(OTA_STATUS_ERROR_FLASH_ZIGBEE, 0, Update.getErrorString().c_str());
		http.end();
		return false;
	}

	http.end();
	return true;
}

void ESP8266OTAUpdater::setState(OnlineUpdateStatus status, uint8_t progress, const char* msg) {
	_state.status = status;
	_state.progress = progress;
	strncpy(_state.message, msg ? msg : "", sizeof(_state.message) - 1);
	_state.message[sizeof(_state.message) - 1] = '\0';
	DEBUG_PRINT(F("[OTA-ESP8266] "));
	DEBUG_PRINTLN(_state.message);
}

bool ESP8266OTAUpdater::check(OnlineUpdateManifest &manifest) {
	memset(&manifest, 0, sizeof(manifest));
	manifest.valid = false;
	setState(OTA_STATUS_CHECKING, 0, "Checking for updates...");

	// ESP8266 uses plain HTTP — BearSSL cannot negotiate TLS with Ionos
	// (server ignores max_fragment_length; returned record overflows the 512-byte buffer).
	WiFiClient client;

	HTTPClient http;
	http.setTimeout(15000);
	String payload;
	bool http_ok = false;

	DEBUG_PRINTF("[OTA-ESP8266] Manifest URL: %s\n", OTA_MANIFEST_URL);
	if (http.begin(client, OTA_MANIFEST_URL)) {
		int http_code = http.GET();
		DEBUG_PRINTF("[OTA-ESP8266] HTTP response code: %d\n", http_code);
		if (http_code == HTTP_CODE_OK) {
			payload = http.getString();
			http_ok = true;
		} else {
			char buf[64];
			snprintf(buf, sizeof(buf), "HTTP error: %d", http_code);
			setState(OTA_STATUS_ERROR_NETWORK, 0, buf);
		}
		http.end();
	} else {
		setState(OTA_STATUS_ERROR_NETWORK, 0, "HTTP begin failed");
	}

	if (!http_ok) return false;

	ArduinoJson::JsonDocument doc;
	ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, payload);
	if (err) {
		setState(OTA_STATUS_ERROR_PARSE, 0, "JSON parse error");
		return false;
	}

	manifest.fw_version = doc["fw_version"] | 0;
	manifest.fw_minor = doc["fw_minor"] | 0;
	{
		// Downgrade https:// → http:// for the firmware URL.  The server ignores the
		// TLS max_fragment_length extension, so BearSSL cannot connect.  The SHA-256
		// field still provides integrity verification over the plain-HTTP download.
		const char* fw_url = doc["esp8266_url"] | OTA_ESP8266_FW_URL;
		char fw_url_http[sizeof(manifest.zigbee_url)];
		if (strncmp(fw_url, "https://", 8) == 0) {
			snprintf(fw_url_http, sizeof(fw_url_http), "http://%s", fw_url + 8);
			fw_url = fw_url_http;
		}
		strncpy(manifest.zigbee_url, fw_url, sizeof(manifest.zigbee_url) - 1);
		manifest.zigbee_url[sizeof(manifest.zigbee_url) - 1] = '\0';
	}
	strncpy(manifest.esp8266_sha256, doc["esp8266_sha256"] | "", sizeof(manifest.esp8266_sha256) - 1);
	manifest.esp8266_sha256[sizeof(manifest.esp8266_sha256) - 1] = '\0';
	strncpy(manifest.changelog, doc["changelog"] | "", sizeof(manifest.changelog) - 1);
	manifest.changelog[sizeof(manifest.changelog) - 1] = '\0';
	manifest.valid = (manifest.fw_version > 0 && manifest.zigbee_url[0]);

	if (!manifest.valid) {
		setState(OTA_STATUS_ERROR_PARSE, 0, "Invalid manifest data");
		return false;
	}

	bool newer = (manifest.fw_version > OS_FW_VERSION) ||
	             (manifest.fw_version == OS_FW_VERSION && manifest.fw_minor > OS_FW_MINOR);
	if (newer) {
		setState(OTA_STATUS_AVAILABLE, 0, "Update available");
	} else {
		setState(OTA_STATUS_UP_TO_DATE, 0, "Firmware is up to date");
	}
	return newer;
}

void ESP8266OTAUpdater::cacheManifest(const OnlineUpdateManifest &manifest) {
	if (!_manifest) _manifest = new OnlineUpdateManifest();
	*_manifest = manifest;
}

void ESP8266OTAUpdater::start() {
	DEBUG_PRINTF("[OTA-ESP8266] start(): in_progress=%d pending=%d check_pending=%d manifest=%d\n",
		_updateInProgress ? 1 : 0, _updatePending ? 1 : 0, _checkPending ? 1 : 0,
		(_manifest && _manifest->valid) ? 1 : 0);
	if (_updateInProgress || _updatePending || _checkPending) {
		DEBUG_PRINTLN(F("[OTA-ESP8266] start ignored: already scheduled/running"));
		return;
	}

	// If a valid manifest is already cached (e.g. via /uc or a zu= URL override
	// on /uu), we can flash directly. Otherwise defer the BLOCKING manifest
	// download to loop() — running it here (inside the /uu web request handler)
	// blocks the ESP8266 for up to 15s and trips the hardware watchdog
	// (rst cause:4) before the {"result":1} response is even sent.
	if (_manifest && _manifest->valid) {
		_updatePending = true;
		setState(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "ESP8266 update scheduled...");
	} else {
		_checkPending = true;
		setState(OTA_STATUS_CHECKING, 1, "Checking for updates...");
	}
}

void ESP8266OTAUpdater::loop() {
	// Deferred manifest check (kept out of the web request handler). Runs here
	// so yield()/watchdog feeding happens between HTTP reads and the debug
	// output is visible on the serial monitor.
	if (_checkPending && !_updateInProgress) {
		_checkPending = false;
		DEBUG_PRINTLN(F("[OTA-ESP8266] Deferred manifest check starting..."));
		if (!_manifest) _manifest = new OnlineUpdateManifest();
		if (!check(*_manifest)) {
			DEBUG_PRINTLN(F("[OTA-ESP8266] Deferred manifest check failed / no update"));
			delete _manifest;
			_manifest = NULL;
			return;
		}
		_updatePending = true;
		setState(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "ESP8266 update scheduled...");
	}

	if (!_updatePending || _updateInProgress) {
		return;
	}

	_updatePending = false;
	_updateInProgress = true;
	os.status.req_mqtt_restart = false;
	yield();
	OnlineUpdateManifest* manifest = _manifest;
	_manifest = NULL;
	if (!manifest) {
		_updateInProgress = false;
		setState(OTA_STATUS_ERROR_PARSE, 0, "Manifest cache missing");
		return;
	}
	uint32_t max_sketch_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
	DEBUG_PRINTF("[OTA-ESP8266] URL: %s\n", manifest->zigbee_url);
	DEBUG_PRINTF("[OTA-ESP8266] ESP.getFreeSketchSpace(): %u\n", (unsigned)ESP.getFreeSketchSpace());
	DEBUG_PRINTF("[OTA-ESP8266] Computed max sketch space: %u\n", (unsigned)max_sketch_space);
	DEBUG_PRINTF("[OTA-ESP8266] Update start: heap=%u maxblock=%u wifi=%d online=%d\n",
		(unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
		(int)WiFi.status(), os.network_connected() ? 1 : 0);

	setState(OTA_STATUS_DOWNLOADING_ZIGBEE, 5, "Starting ESP8266 update...");

	// Free RAM before the download. On the memory-tight ESP8266 the heap can be
	// as low as ~5 KB with MQTT + sensors + the OTC cloud websocket running,
	// which makes HTTPClient/Update.begin abort (OOM). Suspend MQTT, close UDP
	// sockets (NTP/mDNS) and drop the cloud websocket (frees its TLS buffers).
	// A successful update reboots; these services recover on reboot.
	if (OSMqtt::enabled()) OSMqtt::suspend();
	WiFiUDP::stopAll();
	if (otf) otf->disconnectCloud();
	yield();
	DEBUG_PRINTF("[OTA-ESP8266] Heap after freeing services: %u (maxblock=%u)\n",
		(unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());

	bool ok = false;

	const char* ota_urls[2] = {
		manifest->zigbee_url,
		OTA_ESP8266_FW_URL   // plain HTTP fallback defined in online_update.h
	};
	int last_http_code = 0;
	for (uint8_t i = 0; i < 2 && !ok; i++) {
		if (i > 0) {
			DEBUG_PRINTF("[OTA-ESP8266] Retrying with fallback URL: %s\n", ota_urls[i]);
		}
		// Only allocate BearSSL (large heap footprint) when the URL is actually
		// https. ESP8266 OTA URLs are downgraded to plain http, so this stays
		// null and avoids an OOM abort at ~5 KB free heap.
		WiFiClient plainClient;
		std::unique_ptr<BearSSL::WiFiClientSecure> secureClient;
		HTTPClient http;
		String host;
		String uri;
		uint16_t port = 80;
		bool is_https = false;
		if (!parseHttpUrl(ota_urls[i], host, port, uri, is_https)) {
			setState(OTA_STATUS_ERROR_NETWORK, 0, "Invalid OTA URL");
			continue;
		}
		if (is_https) {
			secureClient.reset(new (std::nothrow) BearSSL::WiFiClientSecure());
			if (!secureClient) {
				setState(OTA_STATUS_ERROR_NETWORK, 0, "TLS client alloc failed");
				continue;
			}
			secureClient->setInsecure();
			secureClient->setBufferSizes(512, 512);
		}
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		bool began = is_https
			? http.begin(*secureClient, ota_urls[i])
			: http.begin(plainClient, ota_urls[i]);
		if (!began) {
			setState(OTA_STATUS_ERROR_NETWORK, 0, "HTTP begin failed");
			continue;
		}
		last_http_code = 0;
		ok = flashViaHttpClient(http, max_sketch_space, &last_http_code);
		if (!ok && last_http_code != HTTP_CODE_NOT_FOUND) {
			break;
		}
	}
	delete manifest;

	if (ok) {
		saveRestoreState();
		setState(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
		delay(500);
		_updateInProgress = false;
		os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
		return;
	}

	_updateInProgress = false;
}

void ESP8266OTAUpdater::resume() {
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

	if (pw[0]) {
		DEBUG_PRINTF("[OTA-ESP8266] restoring password slot from restore file pw='%.*s'\n", 16, pw);
		os.sopt_save(SOPT_PASSWORD, pw);
	}
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

// --- Public C-style API (declared in online_update.h) — thin wrappers around
// --- the dynamically-created ESP8266OTAUpdater instance. ------------------
OnlineUpdateState online_update_get_state() { return s_updater ? s_updater->getState() : s_lastState; }
bool online_update_in_progress() { return s_updater && s_updater->inProgress(); }
bool online_update_check(OnlineUpdateManifest &manifest) { return ota_ensure_updater()->check(manifest); }
bool online_update_check_safe(OnlineUpdateManifest &manifest) { return ota_ensure_updater()->checkSafe(manifest); }
void online_update_cache_manifest(const OnlineUpdateManifest &manifest) { ota_ensure_updater()->cacheManifest(manifest); }
void online_update_set_variant(const char* variant) { if (s_updater) s_updater->setVariant(variant); }
void online_update_start() { ota_ensure_updater()->start(); }

void online_update_loop() {
	if (!s_updater) return;
	s_updater->loop();
	// Release the instance (and its 1 KB stream buffer) once there is nothing
	// left to do. Preserve the final status so the UI can still poll /us.
	if (s_updater->isReleasable()) {
		s_lastState = s_updater->getState();
		delete s_updater;
		s_updater = NULL;
	}
}

// resume() only reads LittleFS (no persistent instance needed); call statically.
void online_update_resume() { ESP8266OTAUpdater::resume(); }

#endif // ESP32 / ESP8266
