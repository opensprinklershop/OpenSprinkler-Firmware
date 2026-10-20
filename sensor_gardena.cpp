#include "sensor_gardena.h"
#include "sensors.h"

#if defined(ESP32) || defined(OSPI)

#include <stdio.h>
#include <string.h>

using namespace ArduinoJson;

#if defined(OSPI)
static bool gardena_naett_init = false;
#endif

void gardena_check_opts() {
	ulong n = file_read_block(SOPTS_FILENAME, tmp_buffer, SOPT_GARDENA_OPTS * MAX_SOPTS_SIZE, MAX_SOPTS_SIZE);
	if (n == 0) tmp_buffer[0] = 0;
	tmp_buffer[MAX_SOPTS_SIZE - 1] = 0;
	if (tmp_buffer[0] != '{') {
		strcpy(tmp_buffer, "{}");
		file_write_block(SOPTS_FILENAME, tmp_buffer, SOPT_GARDENA_OPTS * MAX_SOPTS_SIZE, 3);
	}
}

static bool gardena_json_string(JsonObjectConst obj, const char *key, String &out) {
	if (!obj.containsKey(key) || !obj[key].is<const char *>()) return false;
	const char *v = obj[key].as<const char *>();
	out = v ? v : "";
	return true;
}

static String gardena_num_to_string(unsigned long v) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%lu", v);
	return String(buf);
}

// ---------------------------------------------------------------------------
// GardenaClient
// ---------------------------------------------------------------------------

GardenaClient &GardenaClient::instance() {
	static GardenaClient *inst = nullptr;
	if (!inst) {
		inst = new GardenaClient();
	}
	if (!inst->loaded) inst->reloadConfig();
	return *inst;
}

void GardenaClient::reloadConfig() {
	loaded = true;
	String oldKey = apiKey, oldSecret = clientSecret, oldRefresh = refreshToken, oldLocation = locationId;
	apiKey = "";
	clientId = "";
	clientSecret = "";
	refreshToken = "";
	locationId = "";
	pollSeconds = GARDENA_DEFAULT_POLL_S;

	JsonDocument doc;
	DeserializationError err = deserializeJson(doc, os.sopt_load(SOPT_GARDENA_OPTS));
	if (!err && doc.is<JsonObjectConst>()) {
		JsonObjectConst obj = doc.as<JsonObjectConst>();
		gardena_json_string(obj, "api_key", apiKey);
		gardena_json_string(obj, "client_id", clientId);
		gardena_json_string(obj, "client_secret", clientSecret);
		gardena_json_string(obj, "refresh_token", refreshToken);
		if (!gardena_json_string(obj, "location_id", locationId)) {
			gardena_json_string(obj, "locationId", locationId);
		}
		if (obj["poll"].is<unsigned long>()) {
			unsigned long p = obj["poll"].as<unsigned long>();
			if (p < GARDENA_MIN_POLL_S) p = GARDENA_MIN_POLL_S;
			pollSeconds = (uint32_t)p;
		}
	}
	// Husqvarna uses the application key as OAuth client_id; keep both in sync
	// so either field in the setup dialog works.
	if (apiKey.length() == 0 && clientId.length() > 0) apiKey = clientId;
	if (clientId.length() == 0) clientId = apiKey;

	bool credsChanged = (oldKey != apiKey) || (oldSecret != clientSecret) || (oldRefresh != refreshToken);
	if (credsChanged) {
		accessToken = "";
		tokenExpires = 0;
		failCount = 0;
		nextTryTime = 0;
		lastError = "";
	}
	if (oldLocation != locationId) {
		cache.clear();
		cacheTime = 0;
	}
}

bool GardenaClient::tokenValid() const {
	return accessToken.length() > 0 && tokenExpires > 0 && (ulong)os.now_tz() + GARDENA_TOKEN_MARGIN_S < tokenExpires;
}

bool GardenaClient::hasCache() const {
	if (cacheTime == 0) return false;
	ulong now = os.now_tz();
	return now >= cacheTime && now - cacheTime <= GARDENA_CACHE_MAX_AGE_S;
}

ulong GardenaClient::cacheAge() const {
	if (cacheTime == 0) return 0;
	ulong now = os.now_tz();
	return now > cacheTime ? now - cacheTime : 0;
}

void GardenaClient::scheduleRetry(bool rateLimited) {
	ulong now = os.now_tz();
	if (failCount < 10) failCount++;
	ulong wait = GARDENA_BACKOFF_MIN_S;
	for (uint8_t i = 1; i < failCount && wait < GARDENA_BACKOFF_MAX_S; i++) wait *= 2;
	if (wait > GARDENA_BACKOFF_MAX_S || rateLimited) wait = GARDENA_BACKOFF_MAX_S;
	nextTryTime = now + wait;
	DEBUG_PRINTF("[GARDENA] %s (http %d), retry in %lu s\n", lastError, lastHttpStatus, wait);
}

void GardenaClient::persistField(const char *key, const String &value) {
	JsonDocument doc;
	DeserializationError err = deserializeJson(doc, os.sopt_load(SOPT_GARDENA_OPTS));
	if (err || !doc.is<JsonObject>()) {
		doc.clear();
		doc.to<JsonObject>();
	}
	doc[key] = value.c_str();
	doc.remove("access_token");
	size_t n = serializeJson(doc, tmp_buffer, MAX_SOPTS_SIZE);
	if (n >= (size_t)MAX_SOPTS_SIZE - 1) {
		DEBUG_PRINTLN(F("[GARDENA] options too long, not saved"));
		return;
	}
	os.sopt_save(SOPT_GARDENA_OPTS, tmp_buffer);
}

// ---------------------------------------------------------------------------
// HTTP transport
// ---------------------------------------------------------------------------

int GardenaClient::apiRequest(const char *method, const char *url, const char *body, const char *contentType, String &response, bool withAuth) {
	requestCount++;
	response = "";
#if defined(ESP32)
	WiFiClientSecure client;
	client.setInsecure();
	HTTPClient http;
	http.setTimeout(15000);
	http.setReuse(false);
	if (!http.begin(client, url)) {
		lastHttpStatus = -1;
		return -1;
	}
	http.addHeader("accept", body && contentType && strstr(contentType, "x-www-form") ? "application/json" : "application/vnd.api+json");
	if (contentType) http.addHeader("Content-Type", contentType);
	if (withAuth) {
		http.addHeader("Authorization", String("Bearer ") + accessToken);
		http.addHeader("Authorization-Provider", "husqvarna");
		http.addHeader("X-Api-Key", apiKey);
	}
	int status;
	if (strcmp(method, "GET") == 0) {
		status = http.GET();
	} else {
		status = http.sendRequest(method, (uint8_t *)(body ? body : ""), body ? strlen(body) : 0);
	}
	if (status > 0) {
		response = http.getString();
	}
	http.end();
	lastHttpStatus = status;
	return status;
#else
	if (!gardena_naett_init) {
		gardena_naett_init = true;
		naettInit(NULL);
	}
	std::string auth = "Bearer " + accessToken;
	const naettOption *options[8];
	int n = 0;
	options[n++] = naettMethod(method);
	options[n++] = naettHeader("accept", (body && contentType && strstr(contentType, "x-www-form")) ? "application/json" : "application/vnd.api+json");
	options[n++] = naettTimeout(15000);
	if (contentType) options[n++] = naettHeader("Content-Type", contentType);
	if (withAuth) {
		options[n++] = naettHeader("Authorization", auth.c_str());
		options[n++] = naettHeader("Authorization-Provider", "husqvarna");
		options[n++] = naettHeader("X-Api-Key", apiKey.c_str());
	}
	if (body) options[n++] = naettBody(body, (int)strlen(body));
	naettReq *req = naettRequestWithOptions(url, n, options);
	naettRes *res = naettMake(req);
	while (!naettComplete(res)) {
		usleep(50 * 1000);
	}
	int status = naettGetStatus(res);
	if (status > 0) {
		int bodyLength = 0;
		const char *responseBody = (const char *)naettGetBody(res, &bodyLength);
		if (responseBody && bodyLength > 0) response.assign(responseBody, bodyLength);
	}
	naettClose(res);
	naettFree(req);
	lastHttpStatus = status;
	return status;
#endif
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

bool GardenaClient::authenticate() {
	if (apiKey.length() == 0) {
		lastError = "no application key";
		return false;
	}
	String body;
	bool useRefresh = false;
	if (clientSecret.length() > 0) {
		// Preferred: client_credentials, exactly what the Husqvarna portal hands
		// out (Application key + Application secret), no browser login needed.
		body = "grant_type=client_credentials&client_id=" + clientId + "&client_secret=" + clientSecret;
	} else if (refreshToken.length() > 0) {
		body = "grant_type=refresh_token&client_id=" + clientId + "&refresh_token=" + refreshToken;
		useRefresh = true;
	} else {
		lastError = "no application secret";
		return false;
	}

	String response;
	int status = apiRequest("POST", GARDENA_URL_AUTH, body.c_str(), "application/x-www-form-urlencoded", response, false);
	if (status != 200) {
		lastError = (status == 400 || status == 401 || status == 403) ? "authentication rejected (check application key/secret)" : "authentication failed";
		accessToken = "";
		tokenExpires = 0;
		return false;
	}

	JsonDocument doc;
	DeserializationError err = deserializeJson(doc, response);
	if (err || !doc.containsKey("access_token")) {
		lastError = "authentication: unexpected response";
		return false;
	}
	{
		const char *tok = doc["access_token"] | "";
		accessToken = tok;
	}
	unsigned long expiresIn = doc["expires_in"] | 3600UL;
	tokenExpires = (ulong)os.now_tz() + expiresIn;
	if (useRefresh && doc["refresh_token"].is<const char *>()) {
		const char *nr = doc["refresh_token"] | "";
		String newRefresh = nr;
		if (newRefresh.length() > 0 && newRefresh != refreshToken) {
			refreshToken = newRefresh;
			persistField("refresh_token", refreshToken);
		}
	}
	lastError = "";
	DEBUG_PRINTF("[GARDENA] token ok, expires in %lu s\n", expiresIn);
	return accessToken.length() > 0;
}

bool GardenaClient::ensureToken(bool force) {
	if (!force && tokenValid()) return true;
	return authenticate();
}

// ---------------------------------------------------------------------------
// Location data
// ---------------------------------------------------------------------------

static bool gardena_fill_services(JsonDocument &raw, JsonDocument &out) {
	JsonArrayConst included = raw["included"].as<JsonArrayConst>();
	if (included.isNull()) {
		return false;
	}

	out.clear();
	JsonArray sensors = out["sensors"].to<JsonArray>();
	JsonArray valves = out["valves"].to<JsonArray>();
	uint16_t sensorIndex = 0;
	uint16_t valveIndex = 0;

	for (JsonVariantConst variant : included) {
		JsonObjectConst service = variant.as<JsonObjectConst>();
		if (service.isNull()) continue;

		const char *type = service["type"] | "";
		const char *serviceId = service["id"] | "";
		JsonObjectConst attrs = service["attributes"].as<JsonObjectConst>();
		const char *name = "";
		if (!attrs.isNull() && attrs["name"].is<JsonObjectConst>()) {
			name = attrs["name"]["value"] | "";
		}

		if (strcmp(type, "SENSOR") == 0) {
			JsonObject entry = sensors.add<JsonObject>();
			entry["id"] = sensorIndex++;
			entry["serviceId"] = serviceId;
			entry["name"] = name;
			// Assign the variant directly: `| nullptr` always evaluated to null in
			// ArduinoJson 7, so no Gardena value ever reached the sensors.
			entry["soilHumidity"] = attrs["soilHumidity"]["value"];
			entry["soilTemperature"] = attrs["soilTemperature"]["value"];
			entry["ambientTemperature"] = attrs["ambientTemperature"]["value"];
			entry["lightIntensity"] = attrs["lightIntensity"]["value"];
		} else if (strcmp(type, "VALVE") == 0) {
			JsonObject entry = valves.add<JsonObject>();
			entry["id"] = valveIndex++;
			entry["serviceId"] = serviceId;
			entry["name"] = name;
			entry["state"] = attrs["state"]["value"];
			entry["activity"] = attrs["activity"]["value"];
		}
	}

	return true;
}

static void gardena_location_filter(JsonDocument &filter) {
	JsonObject inc = filter["included"][0].to<JsonObject>();
	inc["type"] = true;
	inc["id"] = true;
	JsonObject attrs = inc["attributes"].to<JsonObject>();
	attrs["name"]["value"] = true;
	attrs["soilHumidity"]["value"] = true;
	attrs["soilTemperature"]["value"] = true;
	attrs["ambientTemperature"]["value"] = true;
	attrs["lightIntensity"]["value"] = true;
	attrs["state"]["value"] = true;
	attrs["activity"]["value"] = true;
}

bool GardenaClient::getLocationList(JsonDocument &doc) {
	if (!ensureToken()) return false;
	String response;
	int status = apiRequest("GET", GARDENA_URL_LOCATIONS, NULL, NULL, response, true);
	if (status == 401 || status == 403) {
		if (!ensureToken(true)) return false;
		status = apiRequest("GET", GARDENA_URL_LOCATIONS, NULL, NULL, response, true);
	}
	if (status != 200) {
		lastError = (status == 403) ? "forbidden: connect the 'GARDENA smart system API' to the application" : "location list unavailable";
		return false;
	}
	DeserializationError err = deserializeJson(doc, response);
	if (err) {
		lastError = "location list: invalid response";
		return false;
	}
	lastError = "";
	return true;
}

bool GardenaClient::fetchLocation() {
	if (locationId.length() == 0) {
		JsonDocument locations;
		if (!getLocationList(locations)) return false;
		JsonArrayConst data = locations["data"].as<JsonArrayConst>();
		if (data.isNull() || data.size() == 0) {
			lastError = "no location in the Gardena account";
			return false;
		}
		const char *lid = data[0]["id"] | "";
		locationId = lid;
		if (locationId.length() == 0) {
			lastError = "location without id";
			return false;
		}
		persistField("location_id", locationId);
		DEBUG_PRINTF("[GARDENA] using location %s\n", locationId.c_str());
	}

	char url[180];
	snprintf(url, sizeof(url), GARDENA_URL_LOCATIONF, locationId.c_str());
	String response;
	int status = apiRequest("GET", url, NULL, NULL, response, true);
	if (status == 401 || status == 403) {
		if (!ensureToken(true)) return false;
		status = apiRequest("GET", url, NULL, NULL, response, true);
	}
	if (status == 429) {
		lastError = "rate limited by Husqvarna (weekly quota)";
		return false;
	}
	if (status == 404) {
		// Location vanished (account change): rediscover next time.
		locationId = "";
		persistField("location_id", locationId);
		lastError = "location not found";
		return false;
	}
	if (status != 200) {
		lastError = (status == 403) ? "forbidden: connect the 'GARDENA smart system API' to the application" : "location data unavailable";
		return false;
	}

	JsonDocument filter;
	gardena_location_filter(filter);
	JsonDocument raw;
	DeserializationError err = deserializeJson(raw, response, DeserializationOption::Filter(filter));
	if (err) {
		lastError = "location data: invalid response";
		return false;
	}
	JsonDocument fresh;
	if (!gardena_fill_services(raw, fresh)) {
		lastError = "location data: no services";
		return false;
	}
	cache = fresh;
	cacheTime = os.now_tz();
	lastFetchTime = cacheTime;
	lastError = "";
	DEBUG_PRINTF("[GARDENA] location fetched: %u sensors, %u valves (requests since boot: %lu)\n",
	             (unsigned)sensors().size(), (unsigned)valves().size(), (unsigned long)requestCount);
	return true;
}

bool GardenaClient::refreshLocation(bool force) {
	if (!configured()) {
		lastError = "not configured";
		return false;
	}
	ulong now = os.now_tz();
	if (!force) {
		if (cacheTime && now >= cacheTime && now - cacheTime < pollSeconds) return true;
		if (nextTryTime && now < nextTryTime) return hasCache();
	}
	if (!ensureToken()) {
		scheduleRetry(lastHttpStatus == 429);
		return hasCache();
	}
	if (!fetchLocation()) {
		scheduleRetry(lastHttpStatus == 429);
		return hasCache();
	}
	failCount = 0;
	nextTryTime = 0;
	return true;
}

JsonObjectConst GardenaClient::sensorByIndex(unsigned long index) const {
	for (JsonVariantConst v : sensors()) {
		JsonObjectConst s = v.as<JsonObjectConst>();
		if (!s.isNull() && s["id"].as<unsigned long>() == index) return s;
	}
	return JsonObjectConst();
}

JsonObjectConst GardenaClient::valveByIndex(unsigned long index) const {
	for (JsonVariantConst v : valves()) {
		JsonObjectConst s = v.as<JsonObjectConst>();
		if (!s.isNull() && s["id"].as<unsigned long>() == index) return s;
	}
	return JsonObjectConst();
}

bool GardenaClient::sendValveCommand(unsigned long index, bool open, uint16_t seconds) {
	if (!refreshLocation(false) && !hasCache()) return false;
	JsonObjectConst valve = valveByIndex(index);
	if (valve.isNull()) {
		lastError = "valve index not found";
		return false;
	}
	const char *sid = valve["serviceId"] | "";
	String serviceId = sid;
	if (serviceId.length() == 0) return false;
	if (!ensureToken()) return false;

	// Husqvarna accepts the duration in whole minutes only.
	unsigned long secs = seconds;
	if (secs < 60) secs = 60;
	secs = ((secs + 59) / 60) * 60;

	char url[180];
	snprintf(url, sizeof(url), GARDENA_URL_COMMANDF, serviceId.c_str());
	String body = "{\"data\":{\"id\":\"os-";
	body += gardena_num_to_string((unsigned long)os.now_tz());
	body += "\",\"type\":\"VALVE_CONTROL\",\"attributes\":{\"command\":\"";
	body += open ? "START_SECONDS_TO_OVERRIDE" : "STOP_UNTIL_NEXT_TASK";
	body += "\"";
	if (open) {
		body += ",\"seconds\":";
		body += gardena_num_to_string(secs);
	}
	body += "}}}";

	String response;
	int status = apiRequest("PUT", url, body.c_str(), "application/vnd.api+json", response, true);
	if (status == 401 || status == 403) {
		if (!ensureToken(true)) return false;
		status = apiRequest("PUT", url, body.c_str(), "application/vnd.api+json", response, true);
	}
	bool ok = (status == 202 || status == 200);
	if (!ok) {
		lastError = (status == 429) ? "rate limited by Husqvarna (weekly quota)" : "valve command rejected";
		DEBUG_PRINTF("[GARDENA] valve %lu %s failed (http %d)\n", index, open ? "START" : "STOP", status);
	} else {
		DEBUG_PRINTF("[GARDENA] valve %lu %s%s ok\n", index, open ? "START " : "STOP", open ? gardena_num_to_string(secs).c_str() : "");
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Sensor
// ---------------------------------------------------------------------------

int GardenaSensor::read(unsigned long time) {
	(void)time;
	GardenaClient &g = GardenaClient::instance();
	if (!g.configured()) {
		flags.data_ok = false;
		return HTTP_RQT_CONNECT_ERR;
	}
	if (!g.refreshLocation(false)) {
		// API failed and no usable cache: retry with the regular backoff.
		flags.data_ok = false;
		return HTTP_RQT_CONNECT_ERR;
	}

	JsonObjectConst current = g.sensorByIndex(id);
	if (current.isNull()) {
		flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	if (type == SENSOR_GARDENA_MOISTURE) {
		if (current.containsKey("soilHumidity") && !current["soilHumidity"].isNull()) {
			last_data = current["soilHumidity"].as<float>();
			last_native_data = (uint32_t)last_data;
			flags.data_ok = true;
			return HTTP_RQT_SUCCESS;
		}
	} else if (type == SENSOR_GARDENA_TEMPERATURE) {
		if (current.containsKey("soilTemperature") && !current["soilTemperature"].isNull()) {
			last_data = current["soilTemperature"].as<float>();
			last_native_data = (uint32_t)(last_data * 100);
			flags.data_ok = true;
			return HTTP_RQT_SUCCESS;
		}
		if (current.containsKey("ambientTemperature") && !current["ambientTemperature"].isNull()) {
			last_data = current["ambientTemperature"].as<float>();
			last_native_data = (uint32_t)(last_data * 100);
			flags.data_ok = true;
			return HTTP_RQT_SUCCESS;
		}
	}

	flags.data_ok = false;
	return HTTP_RQT_NOT_RECEIVED;
}

unsigned char GardenaSensor::getUnitId() const {
	if (this->type == SENSOR_GARDENA_TEMPERATURE) return UNIT_DEGREE;
	return UNIT_PERCENT;
}

#endif
