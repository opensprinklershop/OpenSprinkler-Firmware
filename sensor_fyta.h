#ifndef _SENSOR_FYTA_H
#define _SENSOR_FYTA_H

#if defined(ESP8266) || defined(ESP32) || defined(OSPI) 

#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
//#include <WiFiClientSecure.h>
#elif defined(ESP32)
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#elif defined(ESP32)
#include <HTTPClient.h>
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
 * @brief Check and validate FYTA configuration options
 * @note Ensures FYTA credentials are properly configured before API access
 */
void fyta_check_opts();

/**
 * @brief FYTA Public API Client
 * @note API documentation: https://fyta-io.notion.site/FYTA-Public-API-d2f4c30306f74504924c9a40402a3afd
 * @note ESP8266 uses HTTP due to memory constraints, ESP32/OSPI use HTTPS
 */
class FytaApi {
public:
    /**
     * @brief Constructor with authentication credentials
     * @param auth Authentication string (email:password format)
     */
    FytaApi(const String& auth) {
            init();
            authenticate(auth);
        }
    /**
     * @brief Destructor - cleans up HTTP client resources
     */
    ~FytaApi() {
#if defined(ESP8266) || defined(ESP32)
        http.end();
#endif
    }

    /**
     * @brief Authenticate with FYTA API and store access token
     * @param auth Authentication string in format "email:password"
     * @return true if authentication successful, false on failure
     * @note Stores JWT token in authToken member for subsequent API calls
     */
    bool authenticate(const String &auth);
    
    /**
     * @brief Query sensor data for a specific plant
     * @param plantId FYTA plant ID
     * @param doc JSON document to populate with sensor data
     * @return true if data retrieved successfully, false on error
     */
    bool getSensorData(ulong plantId, JsonDocument& doc);
    
    /**
     * @brief Get list of all plants associated with account
     * @param doc JSON document to populate with plant list
     * @return true if list retrieved successfully, false on error
     */
    bool getPlantList(JsonDocument& doc);
#if defined(ESP8266) || defined(ESP32) 
    String authToken;
#else
    std::string authToken;
#endif
    
private:
    /**
     * @brief Initialize HTTP client for platform-specific implementation
     * @note Sets up WiFi client (ESP8266/ESP32) or stores credentials (OSPI)
     */
    void init();
#if defined(ESP8266)
    WiFiClient client;
    HTTPClient http;
#elif defined(ESP32)
    WiFiClientSecure client;    
    HTTPClient http;
#else
    std::string userEmail;
    std::string userPassword;
#endif
};

// New C++ wrapper class for FYTA sensors (incremental migration)
#include "SensorBase.hpp"

/**
 * @brief FYTA plant sensor integration
 * @note Retrieves moisture, temperature, light and nutrient data from FYTA cloud service
 */
class FytaSensor : public SensorBase {
public:
    /**
     * @brief Constructor
     * @param type Sensor type identifier
     */
    explicit FytaSensor(uint type) : SensorBase(type) {}
    virtual ~FytaSensor() {}

    /**
     * @brief Read sensor value from FYTA cloud API
     * @param time Current timestamp
     * @return HTTP_RQT_SUCCESS on successful read, error code on failure
     * @note Fetches data from FYTA API using stored plant ID and authentication
     */
    virtual int read(unsigned long time) override;
    
    /**
     * @brief Get measurement unit identifier
     * @return Unit ID from assigned_unitid field
     */
    virtual unsigned char getUnitId() const override;
};

#endif // defined(ESP8266) || defined(ESP32) || defined(OSPI)
#endif // _SENSOR_FYTA_H
