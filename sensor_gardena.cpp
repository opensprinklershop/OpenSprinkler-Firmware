#include "sensor_gardena.h"
#include "sensors.h"

#if defined(ESP32)

using namespace ArduinoJson;

void gardena_check_opts() {
	ulong n = file_read_block(SOPTS_FILENAME, tmp_buffer, SOPT_GARDENA_OPTS * MAX_SOPTS_SIZE, MAX_SOPTS_SIZE);
	if (n == 0) tmp_buffer[0] = 0;
	if (tmp_buffer[0] != '{') {
		strcpy(tmp_buffer, "{\"api_key\":\"\",\"client_id\":\"\",\"client_secret\":\"\",\"refresh_token\":\"\",\"access_token\":\"\",\"location_id\":\"\"}");
		file_write_block(SOPTS_FILENAME, tmp_buffer, SOPT_GARDENA_OPTS * MAX_SOPTS_SIZE, MAX_SOPTS_SIZE);
	}
}

static bool gardena_json_has_string(JsonObjectConst obj, const char *key) {
	return obj.containsKey(key) && obj[key].is<const char *>();
}

void GardenaApi::init() {
	client.setInsecure();
}

bool GardenaApi::authenticate(const String &auth) {
	JsonDocument doc;
	DeserializationError error = deserializeJson(doc, auth);
	if (error) {
		return false;
	}

	JsonObject obj = doc.as<JsonObject>();
	if (gardena_json_has_string(obj, "api_key")) {
#if defined(ESP8266) || defined(ESP32)
		apiKey = obj["api_key"].as<String>();
#else
		apiKey = obj["api_key"].as<std::string>();
#endif
	}

	if (gardena_json_has_string(obj, "access_token")) {
#if defined(ESP8266) || defined(ESP32)
		authToken = obj["access_token"].as<String>();
#else
		authToken = obj["access_token"].as<std::string>();
#endif
		return authToken.length() > 0;
	}

	String grantType = obj["grant_type"] | "refresh_token";
	String clientId = obj["client_id"] | "";
	String clientSecret = obj["client_secret"] | "";
	String refreshToken = obj["refresh_token"] | "";
	String scope = obj["scope"] | "";

	if (clientId.length() == 0 || clientSecret.length() == 0) {
		return false;
	}

	String body = "grant_type=" + grantType + "&client_id=" + clientId + "&client_secret=" + clientSecret;
	if (refreshToken.length() > 0) {
		body += "&refresh_token=" + refreshToken;
	}
	if (scope.length() > 0) {
		body += "&scope=" + scope;
	}

#if defined(ESP8266) || defined(ESP32)
	http.begin(client, GARDENA_URL_AUTH);
	http.addHeader("Content-Type", "application/x-www-form-urlencoded");
	http.addHeader("accept", "application/json");
	int res = http.POST(body);
	if (res == 200) {
		JsonDocument responseDoc;
		error = deserializeJson(responseDoc, http.getStream());
		if (!error && responseDoc.containsKey("access_token")) {
			authToken = responseDoc["access_token"].as<String>();
			return true;
		}
	}
	return false;
#else
	naettReq *req = naettRequest(GARDENA_URL_AUTH,
		naettMethod("POST"),
		naettHeader("accept", "application/json"),
		naettHeader("Content-Type", "application/x-www-form-urlencoded"),
		naettBody(body.c_str(), body.length()));

	naettRes *res = naettMake(req);
	while (!naettComplete(res)) {
		usleep(100 * 1000);
	}

	if (naettGetStatus(res) < 0) {
		naettFree(req);
		return false;
	}

	int bodyLength = 0;
	const char *responseBody = (char *)naettGetBody(res, &bodyLength);
	JsonDocument responseDoc;
	error = deserializeJson(responseDoc, responseBody, bodyLength);
	if (!error && responseDoc.containsKey("access_token")) {
		authToken = responseDoc["access_token"].as<std::string>();
		naettClose(res);
		naettFree(req);
		return true;
	}
	naettClose(res);
	naettFree(req);
	return false;
#endif
}

static bool gardena_extract_location_id(JsonDocument &input, String &locationId) {
	JsonObject obj = input.as<JsonObject>();
	if (obj.containsKey("location_id")) {
		locationId = obj["location_id"].as<String>();
		return locationId.length() > 0;
	}
	if (obj.containsKey("locationId")) {
		locationId = obj["locationId"].as<String>();
		return locationId.length() > 0;
	}
	return false;
}

static bool gardena_fill_services(JsonDocument &raw, JsonDocument &out) {
	JsonArrayConst included = raw["included"].as<JsonArrayConst>();
	if (included.isNull()) {
		return false;
	}

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
			entry["soilHumidity"] = attrs["soilHumidity"]["value"] | nullptr;
			entry["soilTemperature"] = attrs["soilTemperature"]["value"] | nullptr;
			entry["ambientTemperature"] = attrs["ambientTemperature"]["value"] | nullptr;
			entry["lightIntensity"] = attrs["lightIntensity"]["value"] | nullptr;
		} else if (strcmp(type, "VALVE") == 0) {
			JsonObject entry = valves.add<JsonObject>();
			entry["id"] = valveIndex++;
			entry["serviceId"] = serviceId;
			entry["name"] = name;
			entry["state"] = attrs["state"]["value"] | nullptr;
			entry["activity"] = attrs["activity"]["value"] | nullptr;
		}
	}

	return true;
}

bool GardenaApi::getLocationList(JsonDocument &doc) {
	if (authToken.length() == 0 || apiKey.length() == 0) return false;

#if defined(ESP8266) || defined(ESP32)
	http.begin(client, GARDENA_URL_LOCATIONS);
	http.addHeader("Authorization", "Bearer " + authToken);
	http.addHeader("X-Api-Key", apiKey);
	http.addHeader("accept", "application/vnd.api+json");
	int httpCode = http.GET();
	if (httpCode == 200) {
		DeserializationError error = deserializeJson(doc, http.getStream());
		return !error;
	}
	return false;
#else
	std::string auth = "Bearer " + authToken;
	naettReq *req = naettRequest(GARDENA_URL_LOCATIONS,
		naettMethod("GET"),
		naettHeader("accept", "application/vnd.api+json"),
		naettHeader("Authorization", auth.c_str()),
		naettHeader("X-Api-Key", apiKey.c_str()));

	naettRes *res = naettMake(req);
	while (!naettComplete(res)) {
		usleep(100 * 1000);
	}

	if (naettGetStatus(res) != 200) {
		naettClose(res);
		naettFree(req);
		return false;
	}

	int bodyLength = 0;
	const char *body = (char *)naettGetBody(res, &bodyLength);
	DeserializationError error = deserializeJson(doc, body, bodyLength);
	naettClose(res);
	naettFree(req);
	return !error;
#endif
}

bool GardenaApi::getLocationData(const String &locationId, JsonDocument &doc) {
	if (authToken.length() == 0 || apiKey.length() == 0 || locationId.length() == 0) return false;

	char url[180];
	snprintf(url, sizeof(url), GARDENA_URL_LOCATIONF, locationId.c_str());

#if defined(ESP8266) || defined(ESP32)
	http.begin(client, url);
	http.addHeader("Authorization", "Bearer " + authToken);
	http.addHeader("X-Api-Key", apiKey);
	http.addHeader("accept", "application/vnd.api+json");
	int httpCode = http.GET();
	if (httpCode == 200) {
		JsonDocument raw;
		DeserializationError error = deserializeJson(raw, http.getStream());
		if (error) return false;
		return gardena_fill_services(raw, doc);
	}
	return false;
#else
	std::string auth = "Bearer " + authToken;
	naettReq *req = naettRequest(url,
		naettMethod("GET"),
		naettHeader("accept", "application/vnd.api+json"),
		naettHeader("Authorization", auth.c_str()),
		naettHeader("X-Api-Key", apiKey.c_str()));

	naettRes *res = naettMake(req);
	while (!naettComplete(res)) {
		usleep(100 * 1000);
	}

	if (naettGetStatus(res) != 200) {
		naettClose(res);
		naettFree(req);
		return false;
	}

	int bodyLength = 0;
	const char *body = (char *)naettGetBody(res, &bodyLength);
	JsonDocument raw;
	DeserializationError error = deserializeJson(raw, body, bodyLength);
	naettClose(res);
	naettFree(req);
	if (error) return false;
	return gardena_fill_services(raw, doc);
#endif
}

bool GardenaApi::sendValveCommand(const String &serviceId, bool open, uint16_t seconds) {
	if (authToken.length() == 0 || apiKey.length() == 0 || serviceId.length() == 0) return false;

	char url[180];
	snprintf(url, sizeof(url), GARDENA_URL_COMMANDF, serviceId.c_str());
	String body = "{\"data\":{\"id\":\"request-1\",\"type\":\"VALVE_CONTROL\",\"attributes\":{\"command\":\"";
	body += open ? "START_SECONDS_TO_OVERRIDE" : "STOP_UNTIL_NEXT_TASK";
	body += "\"";
	if (open) {
		body += ",\"seconds\":" + String(seconds);
	}
	body += "}}}";

#if defined(ESP8266) || defined(ESP32)
	http.begin(client, url);
	http.addHeader("Authorization", "Bearer " + authToken);
	http.addHeader("X-Api-Key", apiKey);
	http.addHeader("Content-Type", "application/vnd.api+json");
	http.addHeader("accept", "application/vnd.api+json");
	int httpCode = http.sendRequest("PUT", body);
	return httpCode == 202 || httpCode == 200;
#else
	std::string auth = "Bearer " + authToken;
	naettReq *req = naettRequest(url,
		naettMethod("PUT"),
		naettHeader("accept", "application/vnd.api+json"),
		naettHeader("Authorization", auth.c_str()),
		naettHeader("X-Api-Key", apiKey.c_str()),
		naettHeader("Content-Type", "application/vnd.api+json"),
		naettBody(body.c_str(), body.length()));

	naettRes *res = naettMake(req);
	while (!naettComplete(res)) {
		usleep(100 * 1000);
	}
	int status = naettGetStatus(res);
	naettClose(res);
	naettFree(req);
	return status == 202 || status == 200;
#endif
}

int GardenaSensor::read(unsigned long time) {
	SensorBase *data_ = this;
	if (!data_) return HTTP_RQT_NOT_RECEIVED;
	if (time < data_->last_read + data_->read_interval) {
		return HTTP_RQT_SUCCESS;
	}

	data_->last_read = time;
	GardenaApi gardenaapi(os.sopt_load(SOPT_GARDENA_OPTS));
	JsonDocument settings;
	if (!gardenaapi.getLocationList(settings)) {
		data_->flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	String locationId = "";
	JsonObject root = settings.as<JsonObject>();
	if (root.containsKey("location_id")) {
		locationId = root["location_id"].as<String>();
	} else if (root.containsKey("locationId")) {
		locationId = root["locationId"].as<String>();
	}
	if (locationId.length() == 0) {
		JsonArrayConst locations = root["data"].as<JsonArrayConst>();
		if (!locations.isNull() && locations.size() > 0) {
			locationId = locations[0]["id"].as<String>();
		}
	}

	if (locationId.length() == 0) {
		data_->flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	JsonDocument doc;
	if (!gardenaapi.getLocationData(locationId, doc)) {
		data_->flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	JsonArrayConst sensors = doc["sensors"].as<JsonArrayConst>();
	if (sensors.isNull()) {
		data_->flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	JsonObjectConst current = JsonObjectConst();
	for (JsonVariantConst variant : sensors) {
		JsonObjectConst sensor = variant.as<JsonObjectConst>();
		if (!sensor.isNull() && sensor["id"].as<unsigned long>() == data_->id) {
			current = sensor;
			break;
		}
	}

	if (current.isNull()) {
		data_->flags.data_ok = false;
		return HTTP_RQT_NOT_RECEIVED;
	}

	data_->flags.data_ok = true;
	if (this->type == SENSOR_GARDENA_MOISTURE) {
		if (current.containsKey("soilHumidity") && !current["soilHumidity"].isNull()) {
			data_->last_data = current["soilHumidity"].as<float>();
			data_->last_native_data = data_->last_data;
			return HTTP_RQT_SUCCESS;
		}
	} else if (this->type == SENSOR_GARDENA_TEMPERATURE) {
		if (current.containsKey("soilTemperature") && !current["soilTemperature"].isNull()) {
			data_->last_data = current["soilTemperature"].as<float>();
			data_->last_native_data = data_->last_data;
			return HTTP_RQT_SUCCESS;
		}
		if (current.containsKey("ambientTemperature") && !current["ambientTemperature"].isNull()) {
			data_->last_data = current["ambientTemperature"].as<float>();
			data_->last_native_data = data_->last_data;
			return HTTP_RQT_SUCCESS;
		}
	}

	data_->flags.data_ok = false;
	return HTTP_RQT_NOT_RECEIVED;
}

unsigned char GardenaSensor::getUnitId() const {
	if (this->type == SENSOR_GARDENA_TEMPERATURE) return UNIT_DEGREE;
	return UNIT_PERCENT;
}

#endif
