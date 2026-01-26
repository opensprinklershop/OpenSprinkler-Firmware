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

#include <ArduinoOTA.h>

#if defined(ESP32)
	#include <esp_wifi.h>
#endif

String scan_network() {
	// Note: This function is called by the AP captive portal endpoint (/jsap).
	// It must never block for long, otherwise the UI gets stuck at "(Scanning...)".
	#if defined(ESP8266)
	DEBUG_PRINTLN("Scanning for networks...");
	WiFi.setOutputPower(20.5);
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.mode(WIFI_STA);
	#else
	wifi_mode_t prev_mode = WiFi.getMode();
	bool scan_needs_apsta = false;
	// If we're in captive portal mode, keep AP running during scan.
	if (prev_mode == WIFI_MODE_AP || prev_mode == WIFI_MODE_APSTA) {
		scan_needs_apsta = true;
		WiFi.mode(WIFI_MODE_APSTA);
	} else {
		WiFi.mode(WIFI_MODE_STA);
	}
	#endif
	WiFi.disconnect();
	unsigned char n = WiFi.scanNetworks();
	String json;
	if (n > 40) n = 40;
	json = "{\"ssids\":[";
	for(int i=0;i<n;i++) {
		json += "\"";
		json += WiFi.SSID(i);
		json += "\"";
		if(i<n-1) json += ",";
	}
	json += "],";
	json += "\"scanned\":[";
	for(int i=0;i<n;i++) {
		json += "[\"" + WiFi.SSID(i) + "\",";
		json += "\"" + WiFi.BSSIDstr(i) + "\",";
		json += String(WiFi.RSSI(i))+",",
		json += String(WiFi.channel(i))+"]";
		if(i<n-1) json += ",";
	}
	json += "]}";
	#if defined(ESP32)
	// Restore original mode (best-effort) after scan.
	if (WiFi.getMode() != prev_mode) {
		WiFi.mode(prev_mode);
	}
	#endif
	return json;
}

#if defined(ARDUINOOTA)
static bool arduino_ota_started = false;

static String default_ota_hostname() {
	// Keep hostname short and deterministic.
	// Must be called after basic WiFi init to avoid early all-zero MAC reads.
	String hn;
#if defined(ESP32)
	uint64_t efuse = ESP.getEfuseMac();
	uint32_t suffix = (uint32_t)(efuse & 0xFFFFFFULL);
	hn = String(F("os-")) + String(suffix, HEX);
#else
	hn = String(F("os-")) + String(ESP.getChipId(), HEX);
#endif
	hn.toLowerCase();
	return hn;
}

void start_arduino_ota(const char *hostname) {
	if (arduino_ota_started) return;

	String hn = (hostname && hostname[0]) ? String(hostname) : default_ota_hostname();
	ArduinoOTA.setHostname(hn.c_str());

	ArduinoOTA.onStart([]() {
		DEBUG_PRINTLN(F("ArduinoOTA start"));
	});
	ArduinoOTA.onEnd([]() {
		DEBUG_PRINTLN(F("ArduinoOTA end"));
	});
	ArduinoOTA.onError([](ota_error_t error) {
		DEBUG_PRINTF(F("ArduinoOTA error: %u\n"), (unsigned)error);
	});

	ArduinoOTA.begin();
	arduino_ota_started = true;
	DEBUG_PRINTF(F("ArduinoOTA ready (hostname=%s)\n"), hn.c_str());
}

void handle_arduino_ota() {
	if (!arduino_ota_started) return;
	ArduinoOTA.handle();
}
#endif

void start_network_ap(const char *ssid, const char *pass) {
	DEBUG_PRINTLN("Starting AP mode");
	if(!ssid || !ssid[0]) return;
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	WiFi.mode(WIFI_AP_STA);
	#endif
	#if defined(ESP32)
	WiFi.mode(WIFI_MODE_AP);
	#endif
	// Ensure the AP interface has the expected default IP.
	// Some code paths display localIP() which can be 0.0.0.0 in AP-only mode;
	// having the AP configured explicitly keeps behavior consistent.
	IPAddress ap_ip(192, 168, 4, 1);
	IPAddress ap_gw(192, 168, 4, 1);
	IPAddress ap_sn(255, 255, 255, 0);
	WiFi.softAPConfig(ap_ip, ap_gw, ap_sn);

	WiFi.softAP(ssid, pass);
	#if defined(ARDUINOOTA)
	start_arduino_ota(NULL);
	#endif
	DEBUG_PRINTLN("Starting AP mode done");
}

void start_network_sta_with_ap(const char *ssid, const char *pass, int32_t channel, const unsigned char *bssid) {
	DEBUG_PRINTLN("Starting STA with AP mode");
	if(!ssid || !pass) return;
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	#endif

	#if defined(ESP8266)
	WiFi.mode(WIFI_AP_STA);
	#else
	WiFi.mode(WIFI_MODE_APSTA);
	#endif
	WiFi.begin(ssid, pass, channel, bssid);
	#if defined(ARDUINOOTA)
	start_arduino_ota(NULL);
	#endif
}

void start_network_sta(const char *ssid, const char *pass, int32_t channel, const unsigned char *bssid) {
	DEBUG_PRINTLN("Starting STA mode");
	if(!ssid || !pass) return;
	#if defined(ESP8266)
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.setOutputPower(20.5);
	#endif

	#if defined(ESP8266)
	WiFi.mode(WIFI_STA);
	#else
	WiFi.mode(WIFI_MODE_STA);
	#endif
	WiFi.begin(ssid, pass, channel, bssid);
	#if defined(ARDUINOOTA)
	start_arduino_ota(NULL);
	#endif
}
#endif
