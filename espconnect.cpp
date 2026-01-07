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

String scan_network() {
	DEBUG_PRINTLN("Scanning for networks...");
	#if defined(ESP8266)
	WiFi.setOutputPower(20.5);
	wifi_set_sleep_type(NONE_SLEEP_T);
	WiFi.mode(WIFI_STA);
	#else
	wifi_mode_t prev_mode = WiFi.getMode();
	// If we're in captive portal mode, keep AP running during scan.
	if (prev_mode == WIFI_MODE_AP || prev_mode == WIFI_MODE_APSTA) {
		WiFi.mode(WIFI_MODE_APSTA);
	} else {
		WiFi.mode(WIFI_MODE_STA);
	}
	#endif
	WiFi.disconnect();
	unsigned char n = WiFi.scanNetworks();
	String json;
	if (n>40) n = 40; // limit to 40 ssids max
	// maintain old format of wireless network JSON for mobile app compa
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
	#if defined(ESP32)
	// Restore original mode (best-effort)
	if (WiFi.getMode() != prev_mode) {
		WiFi.mode(prev_mode);
	}
	#endif
	return json;
}

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

	WiFi.softAP(ssid, pass);
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
}
#endif
