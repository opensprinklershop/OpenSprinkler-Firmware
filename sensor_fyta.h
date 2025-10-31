#ifndef _SENSOR_FYTA_H
#define _SENSOR_FYTA_H

#if defined(ESP8266) || defined(OSPI)

#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
//#include <WiFiClientSecure.h>
#elif defined(OSPI)
#include "naett.h"
#endif

#include "ArduinoJson.hpp"
#include "OpenSprinkler.h"

using namespace ArduinoJson;

#if defined(ESP8266)
#define FYTA_URL_LOGIN "http://web.fyta.de/api/auth/login"
#define FYTA_URL_USER_PLANT "http://web.fyta.de/api/user-plant"
#define FYTA_URL_USER_PLANTF "http://web.fyta.de/api/user-plant/%lu"
#else
#define FYTA_URL_LOGIN "https://web.fyta.de/api/auth/login"
#define FYTA_URL_USER_PLANT "https://web.fyta.de/api/user-plant"
#define FYTA_URL_USER_PLANTF "https://web.fyta.de/api/user-plant/%lu"
#endif
/**
 * @brief FYTA Public API Client
 * https://fyta-io.notion.site/FYTA-Public-API-d2f4c30306f74504924c9a40402a3afd
 * 
 */
class FytaApi {
public:
    FytaApi(const String& auth) {
            init();
            authenticate(auth);
        }
    ~FytaApi() {
#if defined(ESP8266)
        http.end();
#endif
    }

    // Authenticate and store token
    bool authenticate(const String &auth);
    // Query sensor values
    bool getSensorData(ulong plantId, JsonDocument& doc);
    // Get plant list
    bool getPlantList(JsonDocument& doc);
#if defined(ESP8266)
    String authToken;
#else
    std::string authToken;
#endif
    
private:
    void init();
#if defined(ESP8266)
    WiFiClient client;
    HTTPClient http;
#else
    std::string userEmail;
    std::string userPassword;
#endif
};

#endif // defined(ESP8266) || defined(OSPI)
#endif // _SENSOR_FYTA_H
