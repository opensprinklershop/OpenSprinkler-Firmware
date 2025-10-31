#include "sensor_fyta.h"

#if defined(ESP8266) || defined(OSPI)

using namespace ArduinoJson;

#if defined(OSPI)
static bool fyta_init = false;
#endif

/**
 * @brief FYTA Public API Client
 * https://fyta-io.notion.site/FYTA-Public-API-d2f4c30306f74504924c9a40402a3afd
 *
 * https://arduinojson.org/v6/how-to/use-arduinojson-with-httpclient/
 *
 */
bool FytaApi::authenticate(const String &auth) {
    DEBUG_PRINTLN("FYTA AUTH");

#if defined(ESP8266)
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

#if defined(ESP8266)
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
#if defined(ESP8266)
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
#if defined(ESP8266)
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
#if defined(ESP8266)
    //client.setInsecure();
    //client.setBufferSizes(512, 512);

#elif defined(OSPI)
    if (!fyta_init) {
        fyta_init = true;
        naettInit(NULL);
    }
#endif
}

#endif // defined(ESP8266) || defined(OSPI)