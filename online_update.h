#ifndef ONLINE_UPDATE_H
#define ONLINE_UPDATE_H

#if defined(ESP32) || defined(ESP8266)

#include <Arduino.h>

// Update server base URL (no trailing slash)
#define OTA_UPDATE_HOST "www.opensprinklershop.de"
#define OTA_UPDATE_BASE_URL "https://www.opensprinklershop.de/upgrade"
#define OTA_MANIFEST_URL OTA_UPDATE_BASE_URL "/manifest.json"
#define OTA_ESP8266_FW_URL OTA_UPDATE_BASE_URL "/firmware_esp8266.bin"

// Status codes for the online update process
enum OnlineUpdateStatus {
	OTA_STATUS_IDLE = 0,
	OTA_STATUS_CHECKING,
	OTA_STATUS_AVAILABLE,
	OTA_STATUS_UP_TO_DATE,
	OTA_STATUS_DOWNLOADING_ZIGBEE,
	OTA_STATUS_DOWNLOADING_MATTER,
	OTA_STATUS_FLASHING_ZIGBEE,
	OTA_STATUS_FLASHING_MATTER,
	OTA_STATUS_DONE,
	OTA_STATUS_ERROR_NETWORK,
	OTA_STATUS_ERROR_PARSE,
	OTA_STATUS_ERROR_FLASH_ZIGBEE,
	OTA_STATUS_ERROR_FLASH_MATTER,
	OTA_STATUS_REBOOTING_PHASE2,     // Rebooting to flash remaining partition
	OTA_STATUS_REBOOTING_OTA,        // Rebooting to free RAM before starting OTA (phase 0)
	OTA_STATUS_ERROR_LOW_MEMORY,     // Not enough internal RAM — disable IEEE802.15.4 and reboot
};

// Manifest data parsed from the remote JSON
struct OnlineUpdateManifest {
	uint16_t fw_version;       // e.g. 233
	uint16_t fw_minor;         // e.g. 186
	char zigbee_url[200];
	char matter_url[200];
	char zigbee_sha256[65];    // lowercase hex SHA-256, or empty if not provided
	char matter_sha256[65];
	char esp8266_sha256[65];
	char changelog[512];
	bool valid;
};

// Current update state (read by the REST API)
struct OnlineUpdateState {
	OnlineUpdateStatus status;
	uint8_t progress;          // 0-100
	char message[128];
};

// Check the remote manifest and populate the manifest struct.
// Returns true if a newer version is available.
bool online_update_check(OnlineUpdateManifest &manifest);

// Safe variant: runs online_update_check() on a dedicated 12 KB FreeRTOS task to
// prevent overflowing the caller's stack (loopTask default = 8 KB).  Blocks up to
// 20 s.  Use this from HTTP request handlers instead of online_update_check().
bool online_update_check_safe(OnlineUpdateManifest &manifest);

// Start the OTA update process (downloads & flashes both slots).
// This is a blocking call — runs on a FreeRTOS task internally.
void online_update_start();

// Override which variant (\"zigbee\" or \"matter\") is set as the boot target after OTA.
// Must be called before online_update_start(). Empty/invalid string clears override.
void online_update_set_variant(const char* variant);

// Get current update state (thread-safe read).
OnlineUpdateState online_update_get_state();

// Returns true if an update task is currently running.
bool online_update_in_progress();

// Cache the manifest after a check so the update task can use it.
void online_update_cache_manifest(const OnlineUpdateManifest &manifest);

// Check for and resume a two-phase OTA update after reboot.
// Call during boot after network is established.
void online_update_resume();

// Service deferred online update work from the main loop.
void online_update_loop();

// Continuation file path on LittleFS (phase 2: second partition after reboot)
#define OTA_CONTINUE_FILE "/ota_continue.json"
// Start manifest file (phase 0: full manifest saved before reboot-to-OTA)
#define OTA_START_FILE    "/ota_start.json"

#else // not ESP32 || ESP8266
// Linux/OSPi stubs: online update not supported on this platform
#include <string>
struct OnlineUpdateManifest {};
enum OnlineUpdateState { OTA_STATE_IDLE = 0 };
inline bool online_update_in_progress() { return false; }
inline void online_update_resume() {}
inline void online_update_loop() {}
inline OnlineUpdateState online_update_get_state() { return OTA_STATE_IDLE; }
inline void online_update_cache_manifest(const OnlineUpdateManifest &) {}
#endif // ESP32 || ESP8266
#endif // ONLINE_UPDATE_H
