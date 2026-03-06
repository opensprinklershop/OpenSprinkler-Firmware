#include <Arduino.h>
#ifdef ESP32
#include <esp_ieee802154.h>

void test_disable() {
    esp_ieee802154_sleep();
}
#endif
