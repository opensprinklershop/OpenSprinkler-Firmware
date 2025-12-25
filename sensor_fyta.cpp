#include "sensor_fyta.h"
#include "sensors.h"

#if defined(ESP8266) || defined(ESP32) || defined(OSPI) 

using namespace ArduinoJson;

#if defined(OSPI)
static bool fyta_init = false;
#endif

void fyta_check_opts() {
    file_read_block(SOPTS_FILENAME, tmp_buffer, SOPT_FYTA_OPTS*MAX_SOPTS_SIZE, MAX_SOPTS_SIZE);
    if (tmp_buffer[0] != '{') {
      strcpy(tmp_buffer, "{\"token\":\"\"}");
      file_write_block(SOPTS_FILENAME, tmp_buffer, SOPT_FYTA_OPTS*MAX_SOPTS_SIZE, MAX_SOPTS_SIZE);
    }
}


/**
 * @brief FYTA Public API Client
 * https://fyta-io.notion.site/FYTA-Public-API-d2f4c30306f74504924c9a40402a3afd
 *
 * https://arduinojson.org/v6/how-to/use-arduinojson-with-httpclient/
 *
 */
bool FytaApi::authenticate(const String &auth) {
    DEBUG_PRINTLN("FYTA AUTH");

#if defined(ESP8266) || defined(ESP32)
    if (auth.indexOf("token") >= 0) {
#else
    if (auth.find("token", 0) >= 0) {
#endif
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, auth);
        if (!error && doc.containsKey("token")) {
            authToken = doc["token"].as<String>();
            if (authToken.length() > 10) {
                DEBUG_PRINTLN("AUTH-TOKEN:");
                DEBUG_PRINTLN(authToken.c_str());
                return true;
            }
            authToken = "";
        }   
    }

#if defined(ESP8266) || defined(ESP32)
    http.begin(client, FYTA_URL_LOGIN);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("accept", "application/json");

    int res = http.POST(auth.c_str());
    if (res == 200) {
        JsonDocument responseDoc;
        DeserializationError error = deserializeJson(responseDoc, http.getStream());
        if (!error && responseDoc.containsKey("access_token")) {
            authToken = responseDoc["access_token"].as<String>();
            return true;
        }
    }
    return false;
#elif defined(OSPI)
    naettReq* req =
        naettRequest(FYTA_URL_LOGIN,
            naettMethod("POST"),
            naettHeader("accept", "application/json"),
            naettHeader("Content-Type", "application/json"),
            naettBody(auth.c_str(), auth.length()));

    naettRes* res = naettMake(req);
    while (!naettComplete(res)) {
        usleep(100 * 1000);
    }

    if (naettGetStatus(res) < 0) {
        DEBUG_PRINTLN("Request failed");
        naettFree(req);
        return false;
    }

    int bodyLength = 0;
    const char* body = (char*)naettGetBody(res, &bodyLength);
    JsonDocument responseDoc;
    DeserializationError error = deserializeJson(responseDoc, body, bodyLength);
    if (!error && responseDoc.containsKey("access_token")) {
        authToken = responseDoc["access_token"].as<String>();
    }
    naettClose(res);
    naettFree(req);
#endif
    DEBUG_PRINTLN("AUTH-TOKEN:");
    DEBUG_PRINTLN(authToken.c_str());
    return true;
}

// Query sensor values
bool FytaApi::getSensorData(ulong plantId, JsonDocument& doc) {
    DEBUG_PRINTLN("FYTA getSensorData");
#if defined(ESP8266) || defined(ESP32)
    if (authToken.isEmpty()) return false;
    char url[50];
    sprintf(url, FYTA_URL_USER_PLANTF, plantId);
    DEBUG_PRINTLN(url);
    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + authToken);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("accept", "application/json");
    int httpCode = http.GET();
    if (httpCode == 200) {
        JsonDocument filter;
        filter["plant"]["temperature_unit"] = true;
        filter["plant"]["measurements"]["temperature"]["values"]["current"] = true;
        filter["plant"]["measurements"]["moisture"]["values"]["current"] = true;

        DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        doc["error"] = error.c_str();
        return !error;
    }
    doc["error"] = httpCode;
    return false;
#elif defined(OSPI)
    if (authToken.empty()) return false;
    std::string auth = "Bearer " + authToken;
    char url[50];
    sprintf(url, FYTA_URL_USER_PLANTF, plantId);
    DEBUG_PRINTLN(url);
    naettReq* req =
        naettRequest(url,
            naettMethod("GET"),
            naettHeader("accept", "application/json"),
            naettHeader("Content-Type", "application/json"),
            naettHeader("Authorization", auth.c_str()));

    naettRes* res = naettMake(req);
    while (!naettComplete(res)) {
        usleep(100 * 1000);
    }

    int bodyLength = 0;
    const char* body = (char*)naettGetBody(res, &bodyLength);

    JsonDocument filter;
    filter["plant"]["temperature_unit"] = true;
    filter["plant"]["measurements"]["temperature"]["values"]["current"] = true;
    filter["plant"]["measurements"]["moisture"]["values"]["current"] = true;

    DeserializationError error = deserializeJson(doc, body, bodyLength, DeserializationOption::Filter(filter));
    if (naettGetStatus(res) < 0 || !body || !bodyLength || error) {
        DEBUG_PRINTLN("FYTA Request failed");
        naettClose(res);
        naettFree(req);
        return false;
    }
    DEBUG_PRINTLN("FYTA getSensorData OK");
    naettClose(res);
    naettFree(req);
    return true;
#endif
}

bool FytaApi::getPlantList(JsonDocument& doc) {
    DEBUG_PRINTLN("FYTA getPlantList");
#if defined(ESP8266) || defined(ESP32)
    if (authToken.isEmpty()) return false;
    http.begin(client, FYTA_URL_USER_PLANT);
    http.addHeader("Authorization", "Bearer " + authToken);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("accept", "application/json");

    int httpCode = http.GET();
    if (httpCode == 200) {
        JsonDocument filter;
        filter["plants"][0]["id"] = true;
        filter["plants"][0]["nickname"] = true;
        filter["plants"][0]["scientific_name"] = true;
        filter["plants"][0]["thumb_path"] = true;
        filter["plants"][0]["sensor"]["has_sensor"] = true;

        DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        doc["error"] = error.c_str();
        return !error;
    }
    doc["error"] = httpCode;
    return false;
#elif defined(OSPI)
    if (authToken.empty()) return false;
    std::string auth = "Bearer " + authToken;
    naettReq* req =
        naettRequest(FYTA_URL_USER_PLANT,
            naettMethod("GET"),
            naettHeader("accept", "application/json"),
            naettHeader("Content-Type", "application/json"),
            naettHeader("Authorization", auth.c_str()));

    naettRes* res = naettMake(req);
    while (!naettComplete(res)) {
        usleep(100 * 1000);
    }

    int bodyLength = 0;
    const char* body = (char*)naettGetBody(res, &bodyLength);

    JsonDocument filter;
    filter["plants"][0]["id"] = true;
    filter["plants"][0]["nickname"] = true;
    filter["plants"][0]["scientific_name"] = true;
    filter["plants"][0]["thumb_path"] = true;
    filter["plants"][0]["sensor"]["has_sensor"] = true;

    DeserializationError error = deserializeJson(doc, body, bodyLength, DeserializationOption::Filter(filter));
    if (naettGetStatus(res) < 0 || !body || !bodyLength || error) {
        DEBUG_PRINTLN("FYTA Request failed!");
        naettClose(res);
        naettFree(req);
        return false;
    }

    DEBUG_PRINTLN("FYTA getPlantList OK");

    naettClose(res);
    naettFree(req);

    return true;
#endif
}

void FytaApi::init() {
#if defined(ESP32)
    client.setInsecure();
#elif defined(OSPI)
    if (!fyta_init) {
        fyta_init = true;
        naettInit(NULL);
    }
#endif
}

// Implementation of the C++ wrapper read() method
int FytaSensor::read(unsigned long time) {
    SensorBase *data_ = this;
    if (!data_) return HTTP_RQT_NOT_RECEIVED;
    if (time >= data_->last_read + data_->read_interval) {
        data_->last_read = time;

        FytaApi fytaapi(os.sopt_load(SOPT_FYTA_OPTS));

        JsonDocument doc;
        if (!fytaapi.getSensorData(data_->id, doc)) {
            DEBUG_PRINTLN(F("Fyta Sensor not found!"));
            data_->flags.data_ok = false;
            return HTTP_RQT_NOT_RECEIVED;
        }

        if (!doc.containsKey("plant")) {
            data_->flags.data_ok = false;
            return HTTP_RQT_NOT_RECEIVED;
        }
        int unit = doc["plant"]["temperature_unit"]; //1=Celsius, 2=Fahrenheit

        if (data_->type == SENSOR_FYTA_TEMPERATURE) {
            data_->last_data = doc["plant"]["measurements"]["temperature"]["values"]["current"].as<double>();
            if (unit == 1 && data_->assigned_unitid != UNIT_DEGREE) {
                data_->assigned_unitid = UNIT_DEGREE;
                data_->unitid = UNIT_DEGREE;
                sensor_save_all();
            }
            else if (unit == 2 && data_->assigned_unitid != UNIT_FAHRENHEIT) {
                data_->assigned_unitid = UNIT_FAHRENHEIT;
                data_->unitid = UNIT_FAHRENHEIT;
                sensor_save_all();
            }
            data_->flags.data_ok = true;
            return HTTP_RQT_SUCCESS;
        }
        else if (data_->type == SENSOR_FYTA_MOISTURE) {
            data_->last_data = doc["plant"]["measurements"]["moisture"]["values"]["current"].as<double>();
            if (data_->assigned_unitid != UNIT_PERCENT) {
                data_->assigned_unitid = UNIT_PERCENT;
                data_->unitid = UNIT_PERCENT;
                sensor_save_all();
            }
            data_->flags.data_ok = true;
            return HTTP_RQT_SUCCESS;
        }
    }
    return HTTP_RQT_NOT_RECEIVED;
}

#endif // defined(ESP8266) || defined(ESP32) || defined(OSPI)
