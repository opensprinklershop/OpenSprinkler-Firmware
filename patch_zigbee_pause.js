const fs = require('fs');

// Patch sensor_zigbee.cpp
let file = '/data/Workspace/OpenSprinkler-Firmware/sensor_zigbee.cpp';
let content = fs.readFileSync(file, 'utf8');

const newFuncs = `
void sensor_zigbee_pause() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY || mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        Zigbee.stop();
        esp_ieee802154_sleep();
    }
}

void sensor_zigbee_resume() {
    IEEE802154Mode mode = ieee802154_get_mode();
    if (mode == IEEE802154Mode::IEEE_ZIGBEE_GATEWAY || mode == IEEE802154Mode::IEEE_ZIGBEE_CLIENT) {
        esp_ieee802154_receive();
        Zigbee.start();
    }
}

void sensor_zigbee_stop() {`;

content = content.replace('void sensor_zigbee_stop() {', newFuncs);
fs.writeFileSync(file, content, 'utf8');

// Patch sensor_zigbee.h
file = '/data/Workspace/OpenSprinkler-Firmware/sensor_zigbee.h';
content = fs.readFileSync(file, 'utf8');

const newH = `void sensor_zigbee_init();
void sensor_zigbee_loop();
void sensor_zigbee_pause();
void sensor_zigbee_resume();
void sensor_zigbee_stop();`;

content = content.replace('void sensor_zigbee_init();\nvoid sensor_zigbee_loop();\nvoid sensor_zigbee_stop();', newH);
fs.writeFileSync(file, content, 'utf8');

