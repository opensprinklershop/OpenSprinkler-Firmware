#ifndef ONLINE_UPDATE_H
#define ONLINE_UPDATE_H

#if defined(ESP32) || defined(ESP8266)

#include <Arduino.h>

// Update server base URL (no trailing slash)
#define OTA_UPDATE_HOST "opensprinklershop.de"
#define OTA_UPDATE_BASE_URL "https://opensprinklershop.de/upgrade"
#define OTA_MANIFEST_URL OTA_UPDATE_BASE_URL "/manifest.json"
#if defined(ESP8266)
#define OTA_ESP8266_FW_URL "http://opensprinklershop.de/upgrade/firmware_esp8266.bin"
#else
#define OTA_ESP8266_FW_URL OTA_UPDATE_BASE_URL "/firmware_esp8266.bin"
#endif

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
};

// Manifest data parsed from the remote JSON
struct OnlineUpdateManifest {
	uint16_t fw_version;       // e.g. 233
	uint16_t fw_minor;         // e.g. 186
	char zigbee_url[200];
	char matter_url[200];
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

// Start the OTA update process (downloads & flashes both slots).
// This is a blocking call — runs on a FreeRTOS task internally.
void online_update_start();

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

// Continuation file path on LittleFS
#define OTA_CONTINUE_FILE "/ota_continue.json"

#endif // ESP32 || ESP8266
#endif // ONLINE_UPDATE_H
