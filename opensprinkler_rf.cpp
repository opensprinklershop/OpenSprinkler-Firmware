#include "opensprinkler_rf.h"

#include <Arduino.h>

#include "defines.h"

#if defined(ESP32)
#include <WiFi.h>
#endif

#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR)
#include "sensor_zigbee.h"
#endif

#include "opensprinkler_matter.h"

#if defined(ENABLE_MATTER) && !defined(ZIGBEE_MODE_ZCZR)
uint8_t current_rf_mode = RF_MODE_MATTER;
#elif defined(ZIGBEE_MODE_ZCZR)
uint8_t current_rf_mode = RF_MODE_ZIGBEE;
#else
uint8_t current_rf_mode = RF_MODE_NONE;
#endif

static bool rf_mode_supported(uint8_t mode) {
	if (mode == RF_MODE_NONE) return true;
	if (mode == RF_MODE_MATTER) {
		#ifdef ENABLE_MATTER
		return true;
		#else
		return false;
		#endif
	}
	if (mode == RF_MODE_ZIGBEE) {
		#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR)
		return true;
		#else
		return false;
		#endif
	}
	return false;
}

bool switch_rf_mode(uint8_t new_mode) {
	if (current_rf_mode == new_mode) return true;
	if (!rf_mode_supported(new_mode)) return false;

	// Stop current mode (best-effort)
	if (current_rf_mode == RF_MODE_MATTER) {
		#ifdef ENABLE_MATTER
		DEBUG_PRINTLN(F("RF: Stopping Matter..."));
		matter_shutdown();
		#endif
	} else if (current_rf_mode == RF_MODE_ZIGBEE) {
		// NOTE: Zigbee stop/deinit is intentionally not implemented in this firmware.
		// Zigbee is started on-demand elsewhere and (once started) may remain active.
		DEBUG_PRINTLN(F("RF: Zigbee stop not supported; leaving Zigbee running"));
	}

	delay(250);

	// Start new mode
	if (new_mode == RF_MODE_MATTER) {
		#ifdef ENABLE_MATTER
		DEBUG_PRINTLN(F("RF: Starting Matter..."));
		matter_init();
		#endif
	} else if (new_mode == RF_MODE_ZIGBEE) {
		#if defined(ESP32C5) && defined(ZIGBEE_MODE_ZCZR)
		#if defined(ESP32)
		if (WiFi.status() == WL_CONNECTED) {
			DEBUG_PRINTLN(F("RF: Starting Zigbee..."));
			sensor_zigbee_start();
		} else {
			DEBUG_PRINTLN(F("RF: WiFi not connected; Zigbee start deferred"));
		}
		#else
		DEBUG_PRINTLN(F("RF: Starting Zigbee..."));
		sensor_zigbee_start();
		#endif
		#endif
	}

	current_rf_mode = new_mode;
	return true;
}
