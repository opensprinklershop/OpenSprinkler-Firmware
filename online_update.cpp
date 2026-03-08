#if defined(ESP32)

#include "online_update.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "ArduinoJson.hpp"
#include <LittleFS.h>

#if defined(ESP32C5)
#include "ieee802154_config.h"
#endif

extern OpenSprinkler os;

// Thread-safe state
static SemaphoreHandle_t s_ota_mutex = NULL;
static OnlineUpdateState s_state = { OTA_STATUS_IDLE, 0, "" };
static OnlineUpdateManifest s_manifest = {};
static TaskHandle_t s_ota_task = NULL;

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

	ota_set_state(OTA_STATUS_CHECKING, 0, "Checking for updates...");

	WiFiClientSecure *client = new WiFiClientSecure();
	if (!client) {
		ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, "Failed to create SSL client");
		return false;
	}
	client->setInsecure(); // opensprinklershop.de — trusted server, skip cert verification for embedded device

	// Scope block: HTTPClient must be destroyed BEFORE delete client,
	// because its destructor may access the WiFiClientSecure through _client.
	String payload;
	bool httpOk = false;
	{
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(15000);

		if (http.begin(*client, OTA_MANIFEST_URL)) {
			int httpCode = http.GET();
			if (httpCode == HTTP_CODE_OK) {
				payload = http.getString();
				httpOk = true;
			} else {
				char buf[64];
				snprintf(buf, sizeof(buf), "HTTP error: %d", httpCode);
				ota_set_state(OTA_STATUS_ERROR_NETWORK, 0, buf);
			}
			http.end();
		} else {
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
	strncpy(manifest.changelog, doc["changelog"] | "", sizeof(manifest.changelog) - 1);
	manifest.valid = (manifest.fw_version > 0 && manifest.zigbee_url[0] && manifest.matter_url[0]);

	if (!manifest.valid) {
		ota_set_state(OTA_STATUS_ERROR_PARSE, 0, "Invalid manifest data");
		return false;
	}

	// Compare versions — newer if major > current OR (same major AND minor > current)
	bool newer = (manifest.fw_version > OS_FW_VERSION) ||
	             (manifest.fw_version == OS_FW_VERSION && manifest.fw_minor > OS_FW_MINOR);

	if (newer) {
		ota_set_state(OTA_STATUS_AVAILABLE, 0, "Update available");
	} else {
		ota_set_state(OTA_STATUS_UP_TO_DATE, 0, "Firmware is up to date");
	}
	return newer;
}

// Flash a single OTA partition from a URL.
// partLabel: "zigbee" or "matter" (matches partition table labels).
// pMin/pMax: overall progress range (0-100) this flash maps to.
// Returns true on success.
static bool ota_flash_partition(const char* url, const char* partLabel,
	OnlineUpdateStatus dl_status, OnlineUpdateStatus err_status,
	uint8_t pMin = 0, uint8_t pMax = 100) {
	char msg[80];
	snprintf(msg, sizeof(msg), "Downloading %s firmware...", partLabel);
	ota_set_state(dl_status, 0, msg);

	// Find target partition by label so we flash the correct slot
	const esp_partition_t *target = esp_partition_find_first(
		ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partLabel);
	if (!target) {
		snprintf(msg, sizeof(msg), "Partition '%s' not found", partLabel);
		ota_set_state(err_status, 0, msg);
		return false;
	}
	DEBUG_PRINTF("[OTA] Target partition: %s at 0x%06x (%u bytes)\n",
		target->label, (unsigned)target->address, (unsigned)target->size);

	WiFiClientSecure *client = new WiFiClientSecure();
	if (!client) {
		ota_set_state(err_status, 0, "Failed to create SSL client");
		return false;
	}
	client->setInsecure();

	// Scope block: HTTPClient must be destroyed BEFORE delete client,
	// because its destructor may access the WiFiClientSecure through _client.
	bool success = false;
	{
		HTTPClient http;
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(30000);

		if (!http.begin(*client, url)) {
			ota_set_state(err_status, 0, "HTTP begin failed");
			goto cleanup;
		}

		{
			int httpCode = http.GET();
			if (httpCode != HTTP_CODE_OK) {
				snprintf(msg, sizeof(msg), "Download %s failed: HTTP %d", partLabel, httpCode);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto cleanup;
			}

			int contentLength = http.getSize();
			if (contentLength <= 0) {
				snprintf(msg, sizeof(msg), "Invalid content length for %s", partLabel);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto cleanup;
			}

			if ((size_t)contentLength > target->size) {
				snprintf(msg, sizeof(msg), "Image too large for %s (%d > %u)", partLabel, contentLength, (unsigned)target->size);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto cleanup;
			}

			snprintf(msg, sizeof(msg), "Flashing %s (%d bytes)...", partLabel, contentLength);
			ota_set_state(dl_status, pMin + 1, msg);

			// Use ESP-IDF OTA API directly — targets the specific partition by label
			esp_ota_handle_t ota_handle = 0;
			esp_err_t err = esp_ota_begin(target, contentLength, &ota_handle);
			if (err != ESP_OK) {
				snprintf(msg, sizeof(msg), "esp_ota_begin failed: 0x%x", err);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto cleanup;
			}

			WiFiClient *stream = http.getStreamPtr();
			uint8_t buf[1024];
			int totalWritten = 0;
			bool write_error = false;

			while (http.connected() && totalWritten < contentLength) {
				size_t avail = stream->available();
				if (avail) {
					int bytesRead = stream->readBytes(buf, min(avail, sizeof(buf)));
					if (bytesRead > 0) {
						err = esp_ota_write(ota_handle, buf, bytesRead);
						if (err != ESP_OK) {
							snprintf(msg, sizeof(msg), "Write error: 0x%x at %d bytes", err, totalWritten);
							ota_set_state(err_status, 0, msg);
							esp_ota_abort(ota_handle);
							http.end();
							write_error = true;
							goto cleanup;
						}
						totalWritten += bytesRead;
						uint8_t raw_pct = (uint8_t)((totalWritten * 100L) / contentLength);
						uint8_t pct = pMin + (uint8_t)((long)raw_pct * (pMax - pMin) / 100);
						snprintf(msg, sizeof(msg), "Flashing %s: %d%%", partLabel, raw_pct);
						ota_set_state(dl_status, pct, msg);
					}
				}
				delay(1);
			}

			if (totalWritten < contentLength) {
				snprintf(msg, sizeof(msg), "Download incomplete: %d/%d bytes", totalWritten, contentLength);
				ota_set_state(err_status, 0, msg);
				esp_ota_abort(ota_handle);
				http.end();
				goto cleanup;
			}

			err = esp_ota_end(ota_handle);
			if (err != ESP_OK) {
				snprintf(msg, sizeof(msg), "esp_ota_end failed: 0x%x", err);
				ota_set_state(err_status, 0, msg);
				http.end();
				goto cleanup;
			}

			http.end();
			success = true;
		}
cleanup:
		; // HTTPClient destroyed here while client is still valid
	}
	delete client;

	if (success) {
		snprintf(msg, sizeof(msg), "%s firmware flashed OK", partLabel);
		ota_set_state(dl_status, pMax, msg);
	}
	return success;
}

// Backup settings files to /backup/ directory on LittleFS
static bool ota_backup_settings() {
	DEBUG_PRINTLN(F("[OTA] Backing up settings..."));

	const char* files[] = {
		IOPTS_FILENAME, SOPTS_FILENAME, STATIONS_FILENAME,
		STATIONS2_FILENAME, STATIONS3_FILENAME,
		NVCON_FILENAME, PROG_FILENAME,
		"/sensors.json", "/progsensor.json"
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

// Save OTA continuation state to LittleFS for phase 2 after reboot.
// url: firmware URL for the remaining partition, label: partition label,
// variant: boot variant name to restore after both partitions are flashed.
// Also saves the current password hash so it can be restored after factory_reset.
static bool ota_save_continuation(const char* url, const char* label, const char* variant) {
	File f = LittleFS.open(OTA_CONTINUE_FILE, "w");
	if (!f) {
		DEBUG_PRINTLN(F("[OTA] Failed to write continuation file"));
		return false;
	}
	// Save password hash: factory_reset() on phase 2 boot will reset it to default
	char pwBuf[MAX_SOPTS_SIZE + 1];
	os.sopt_load(SOPT_PASSWORD, pwBuf);
	f.printf("{\"url\":\"%s\",\"label\":\"%s\",\"variant\":\"%s\",\"pw\":\"%s\"}", url, label, variant, pwBuf);
	f.close();
	DEBUG_PRINTLN(F("[OTA] Continuation state saved (incl. password)"));
	return true;
}

// FreeRTOS task that performs the full dual-OTA update
static void ota_update_task(void* param) {
	(void)param;

	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 1, "Starting update...");

	// Step 1: Backup settings (0-4% overall)
	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 2, "Backing up settings...");
	ota_backup_settings();
	ota_set_state(OTA_STATUS_DOWNLOADING_ZIGBEE, 4, "Settings backed up");

	// Step 2: Check manifest (use cached if valid, otherwise re-fetch)
	OnlineUpdateManifest manifest;
	if (s_manifest.valid) {
		manifest = s_manifest;
	} else {
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
	const char* target_variant_name = (target_variant == IEEE802154BootVariant::MATTER) ? "matter" : "zigbee";

	if (running && strcmp(running->label, "matter") == 0) {
		// Running from matter (ota_1) → flash zigbee (ota_0) first
		if (!ota_flash_partition(manifest.zigbee_url, "zigbee",
		                         OTA_STATUS_DOWNLOADING_ZIGBEE, OTA_STATUS_ERROR_FLASH_ZIGBEE, 5, 48)) {
			s_ota_task = NULL;
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash matter after reboot
		ota_save_continuation(manifest.matter_url, "matter", target_variant_name);
		// Set boot to zigbee (the partition we just flashed) so phase 2 can write matter
		esp_ota_set_boot_partition(esp_partition_find_first(
			ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "zigbee"));
	} else {
		// Running from zigbee (ota_0) → flash matter (ota_1) first
		if (!ota_flash_partition(manifest.matter_url, "matter",
		                         OTA_STATUS_DOWNLOADING_MATTER, OTA_STATUS_ERROR_FLASH_MATTER, 5, 48)) {
			s_ota_task = NULL;
			vTaskDelete(NULL);
			return;
		}
		// Save continuation: need to flash zigbee after reboot
		ota_save_continuation(manifest.zigbee_url, "zigbee", target_variant_name);
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
	if (!ota_flash_partition(manifest.zigbee_url, "app",
	                         OTA_STATUS_DOWNLOADING_ZIGBEE, OTA_STATUS_ERROR_FLASH_ZIGBEE, 5, 98)) {
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	// Done — reboot
	ota_set_state(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
	delay(2000);
	os.reboot_dev(REBOOT_CAUSE_FWUPDATE);
#endif

	s_ota_task = NULL;
	vTaskDelete(NULL);
}

void online_update_start() {
	if (online_update_in_progress()) return;

	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();

	// Cache the manifest so the task doesn't need to re-fetch
	// (it was already checked via /uc before the user clicked update)
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	// s_manifest should already be populated by online_update_check()
	xSemaphoreGive(s_ota_mutex);

	xTaskCreate(ota_update_task, "ota_update", 16384, NULL, 1, &s_ota_task);
}

// FreeRTOS task for phase 2 of OTA update (after reboot)
static void ota_phase2_task(void* param) {
	(void)param;

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

	// Determine status codes based on which partition we're flashing
	OnlineUpdateStatus dl_status = (strcmp(label, "matter") == 0)
		? OTA_STATUS_DOWNLOADING_MATTER : OTA_STATUS_DOWNLOADING_ZIGBEE;
	OnlineUpdateStatus err_status = (strcmp(label, "matter") == 0)
		? OTA_STATUS_ERROR_FLASH_MATTER : OTA_STATUS_ERROR_FLASH_ZIGBEE;

	// Phase 2 flash maps to 52-98% overall progress
	if (!ota_flash_partition(url, label, dl_status, err_status, 52, 98)) {
		LittleFS.remove(OTA_CONTINUE_FILE);
		s_ota_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	// Remove continuation file before setting boot and rebooting
	LittleFS.remove(OTA_CONTINUE_FILE);

#if defined(ESP32C5)
	// Set boot partition to the user's original variant
	IEEE802154BootVariant boot_var = (strcmp(variant, "matter") == 0)
		? IEEE802154BootVariant::MATTER : IEEE802154BootVariant::ZIGBEE;
	ieee802154_select_otf_boot_variant(boot_var);
#endif

	ota_set_state(OTA_STATUS_DONE, 100, "Update complete. Rebooting...");
	delay(2000);
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
	xTaskCreate(ota_phase2_task, "ota_phase2", 16384, NULL, 1, &s_ota_task);
}

// Helper: cache manifest after a check so the update task can use it
void online_update_cache_manifest(const OnlineUpdateManifest &manifest) {
	if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
	xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
	s_manifest = manifest;
	xSemaphoreGive(s_ota_mutex);
}

#endif // ESP32
