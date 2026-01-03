/* ESPConnect functions
 * December 2016 @ opensprinkler.com
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
#if defined(ESP8266) || defined(ESP32)

#include "espconnect.h"

// Forward declarations for Zigbee/BLE coexistence management
static bool zigbee_was_enabled = false;
static bool ble_was_enabled = false;

#if defined(ESP32) && defined(ZIGBEE_MODE_ZCZR)
// Zigbee/BLE coexistence management for ESP32-C5
// Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html
// WiFi SOFTAP + Zigbee Router = NOT SUPPORTED (X)
// Solution: Disable Zigbee/BLE during WiFi setup (SOFTAP mode)

// Include ESP-IDF headers only when Zigbee is enabled
extern "C" {
	#if __has_include("esp_bt.h")
		#include "esp_bt.h"
	#endif
	#if __has_include("esp_bt_main.h")
		#include "esp_bt_main.h"
	#endif
	#if __has_include("esp_zigbee_core.h")
		#include "esp_zigbee_core.h"
	#endif
}

void disable_zigbee_ble_for_softap() {
	// NOTE: This function is now a placeholder
	// Zigbee/BLE are NOT initialized during SOFTAP mode (see main.cpp)
	// sensor_api_init() is deferred until WiFi switches to STA mode
	DEBUG_PRINTLN(F("[COEX] WiFi SOFTAP mode - Zigbee/BLE init deferred"));
}

void reenable_zigbee_ble_after_softap() {
	// NOTE: This function is now a placeholder
	// Zigbee/BLE are initialized when WiFi connects in STA mode (see main.cpp)
	// sensor_api_init() is called in OS_STATE_CONNECTING -> OS_STATE_CONNECTED
	DEBUG_PRINTLN(F("[COEX] WiFi STA mode - Zigbee/BLE will be initialized"));
}
#else
// Stub functions for non-Zigbee platforms
void disable_zigbee_ble_for_softap() {
	DEBUG_PRINTLN(F("[COEX] Zigbee/BLE management not available on this platform"));
}
void reenable_zigbee_ble_after_softap() {
	DEBUG_PRINTLN(F("[COEX] Zigbee/BLE management not available on this platform"));
}
#endif

String scan_network() {
	#if defined(ESP8266)
	WiFi.setOutputPower(20.5);
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.mode(WIFI_STA);
	#else 
	WiFi.setTxPower(WIFI_POWER_19_5dBm); // set tx power to 19.5dBm
	WiFi.mode(WIFI_MODE_STA); // set mode to STA
	#endif
	WiFi.disconnect();
	unsigned char n = WiFi.scanNetworks();
	String json;
	if (n>40) n = 40; // limit to 40 ssids max
	// maintain old format of wireless network JSON for mobile app compat
	json = "{\"ssids\":[";
	for(int i=0;i<n;i++) {
		json += "\"";
		json += WiFi.SSID(i);
		json += "\"";
		if(i<n-1) json += ",";
	}
	json += "],";
	// scanned contains complete wireless info including bssid and channel
	json += "\"scanned\":[";
	for(int i=0;i<n;i++) {
		json += "[\"" + WiFi.SSID(i) + "\",";
		json += "\"" + WiFi.BSSIDstr(i) + "\",";
		json += String(WiFi.RSSI(i))+",",
		json += String(WiFi.channel(i))+"]";
		if(i<n-1) json += ",";
	}
	json += "]}";
	return json;
}

void start_network_ap(const char *ssid, const char *pass) {
	if(!ssid) return;
	
	// CRITICAL: Disable Zigbee/BLE before entering SOFTAP mode
	// WiFi SOFTAP + Zigbee Router = NOT SUPPORTED on ESP32-C5
	disable_zigbee_ble_for_softap();
	
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	#else
	WiFi.setTxPower(WIFI_POWER_19_5dBm); // set tx power to 19.5dBm
	#endif
	if(pass) WiFi.softAP(ssid, pass);
	else WiFi.softAP(ssid);
	WiFi.mode(WIFI_AP_STA); // start in AP_STA mode
	WiFi.disconnect();	// disconnect from router
}

void start_network_sta_with_ap(const char *ssid, const char *pass, int32_t channel, const unsigned char *bssid) {
	if(!ssid || !pass) return;
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	#else
	WiFi.setTxPower(WIFI_POWER_19_5dBm); // set tx power to 19.5dBm
	#endif

	WiFi.mode(WIFI_AP_STA);
	WiFi.begin(ssid, pass, channel, bssid);
}

void start_network_sta(const char *ssid, const char *pass, int32_t channel, const unsigned char *bssid) {
	if(!ssid || !pass) return;
	
	// Re-enable Zigbee/BLE when switching to STA mode (STABLE on ESP32-C5)
	reenable_zigbee_ble_after_softap();
	
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	#else
	WiFi.setTxPower(WIFI_POWER_19_5dBm); // set tx power to 19.5dBm
	#endif

	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, pass, channel, bssid);
}
#endif
