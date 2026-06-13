#ifndef _SENSOR_GARDENA_H
#define _SENSOR_GARDENA_H

#if defined(ESP32) || defined(OSPI)

#if defined(ESP32)
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#elif defined(OSPI)
#include "naett.h"
#endif

#include "ArduinoJson.hpp"
#include "OpenSprinkler.h"

using namespace ArduinoJson;

#define GARDENA_URL_AUTH "https://api.authentication.husqvarnagroup.dev/v1/oauth2/token"
#define GARDENA_URL_LOCATIONS "https://api.smart.gardena.dev/v2/locations"
#define GARDENA_URL_LOCATIONF "https://api.smart.gardena.dev/v2/locations/%s"
#define GARDENA_URL_COMMANDF "https://api.smart.gardena.dev/v2/command/%s"

void gardena_check_opts();

class GardenaApi {
public:
	GardenaApi(const String &auth) {
		init();
		authenticate(auth);
	}

	~GardenaApi() {
#if defined(ESP8266) || defined(ESP32)
		http.end();
#endif
	}

	bool authenticate(const String &auth);
	bool getLocationList(JsonDocument &doc);
	bool getLocationData(const String &locationId, JsonDocument &doc);
	bool sendValveCommand(const String &serviceId, bool open, uint16_t seconds);

#if defined(ESP8266) || defined(ESP32)
	String authToken;
	String apiKey;
#else
	std::string authToken;
	std::string apiKey;
#endif

private:
	void init();
#if defined(ESP8266) || defined(ESP32)
	WiFiClientSecure client;
	HTTPClient http;
#endif
};

#include "SensorBase.hpp"

class GardenaSensor : public SensorBase {
public:
	explicit GardenaSensor(uint type) : SensorBase(type) {}
	virtual ~GardenaSensor() {}
	virtual int read(unsigned long time) override;
	virtual unsigned char getUnitId() const override;
};

#endif

#endif // _SENSOR_GARDENA_H
