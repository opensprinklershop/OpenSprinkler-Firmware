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

/*
 * Gardena smart system (Husqvarna) cloud client.
 *
 * Configuration lives in SOPT_GARDENA_OPTS as a compact JSON object (the slot
 * holds MAX_SOPTS_SIZE = 320 bytes, so nothing volatile is stored there):
 *   api_key        Application key from the Husqvarna developer portal
 *                  (doubles as OAuth client_id)
 *   client_id      only stored when it differs from api_key (legacy)
 *   client_secret  Application secret -> OAuth client_credentials grant
 *   refresh_token  optional, from the OAuth login assistant (authorization
 *                  code flow); only used when no client_secret is configured.
 *                  Husqvarna rotates it on every refresh, the new value is
 *                  written back.
 *   location_id    discovered automatically (first location) and persisted
 *   poll           minimum seconds between two location fetches (default
 *                  GARDENA_DEFAULT_POLL_S, min GARDENA_MIN_POLL_S)
 *
 * The access token is kept in RAM only (expires after ~24 h, renewed 5 min
 * before). One location fetch serves every Gardena sensor and every Gardena
 * station: the trimmed service list (sensors + valves) is cached and the API
 * is hit at most once per `poll` seconds, which keeps a setup with several
 * sensors far below the Husqvarna quota (700-1000 requests per week and
 * application; exceeding it blocks the application for 24 h). Failed calls
 * back off exponentially (5 min .. 1 h, 1 h on HTTP 429) while the sensors
 * keep serving the cached values for up to GARDENA_CACHE_MAX_AGE_S.
 */

#define GARDENA_URL_AUTH "https://api.authentication.husqvarnagroup.dev/v1/oauth2/token"
#define GARDENA_URL_LOCATIONS "https://api.smart.gardena.dev/v2/locations"
#define GARDENA_URL_LOCATIONF "https://api.smart.gardena.dev/v2/locations/%s"
#define GARDENA_URL_COMMANDF "https://api.smart.gardena.dev/v2/command/%s"

#define GARDENA_DEFAULT_POLL_S     1800   // 30 min -> ~336 requests/week
#define GARDENA_MIN_POLL_S         300
#define GARDENA_CACHE_MAX_AGE_S    (6UL * 3600UL)
#define GARDENA_TOKEN_MARGIN_S     300
#define GARDENA_BACKOFF_MIN_S      300
#define GARDENA_BACKOFF_MAX_S      3600

void gardena_check_opts();

class GardenaClient {
public:
	static GardenaClient &instance();

	// (Re)load credentials from SOPT_GARDENA_OPTS; call after /co changed them.
	void reloadConfig();
	bool configured() const { return apiKey.length() > 0 && (clientSecret.length() > 0 || refreshToken.length() > 0); }

	// Make sure a valid access token is available (authenticates if needed).
	bool ensureToken(bool force = false);
	// Refresh the cached service list if older than `poll` seconds (or forced).
	// Returns true when the cache holds usable data afterwards.
	bool refreshLocation(bool force = false);
	bool hasCache() const;
	ulong cacheAge() const;
	JsonArrayConst sensors() const { return cache["sensors"].as<JsonArrayConst>(); }
	JsonArrayConst valves() const { return cache["valves"].as<JsonArrayConst>(); }
	// Look up a sensor/valve entry (index as shown in the setup dialog).
	JsonObjectConst sensorByIndex(unsigned long index) const;
	JsonObjectConst valveByIndex(unsigned long index) const;

	// Uncached: list of locations of the account (setup dialog only).
	bool getLocationList(JsonDocument &doc);
	// Valve command by service index; uses the cached service list.
	bool sendValveCommand(unsigned long index, bool open, uint16_t seconds);

	// Diagnostics (reported by /gl)
	uint32_t requestCount = 0;   // API requests since boot
	int lastHttpStatus = 0;
	const char *lastError = "";
	ulong lastFetchTime = 0;
	ulong nextTryTime = 0;
	uint8_t failCount = 0;
	uint32_t pollSeconds = GARDENA_DEFAULT_POLL_S;
	const String &locationIdStr() const { return locationId; }
	bool tokenValid() const;

private:
	GardenaClient() {}
	bool authenticate();
	bool fetchLocation();
	void scheduleRetry(bool rateLimited);
	// Performs one HTTPS request. Returns HTTP status (or -1 on transport error)
	// and fills `response` with the body.
	int apiRequest(const char *method, const char *url, const char *body, const char *contentType, String &response, bool withAuth);
	void persistField(const char *key, const String &value);

	String apiKey;
	String clientId;
	String clientSecret;
	String refreshToken;
	String locationId;
	String accessToken;
	ulong tokenExpires = 0;
	JsonDocument cache;      // {"sensors":[...],"valves":[...]}
	ulong cacheTime = 0;
	bool loaded = false;
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
