# OpenSprinkler Shop Firmware and Documentation

This site contains documentation for OpenSprinkler firmware and the OpenSprinklerPro extensions maintained by OpenSprinklerShop.

!!! info "OpenSprinklerShop firmware version"
    The OpenSprinklerShop firmware, including OpenSprinklerPro, uses its own firmware version numbering independent of the upstream OpenSprinkler firmware line. The current OpenSprinklerShop firmware version is **2.4.0(208)**.

* User manual: [English](2.2.1/221_4_manual.md), [Deutsch](2.2.1/221_4_manual_de.md), [Français](2.2.1/221_4_manual_fr.md), [Italiano](2.2.1/221_4_manual_it.md).
* OpenSprinklerPro extensions: [English](opensprinklerpro.md), [Deutsch](opensprinklerpro_de.md), [Français](opensprinklerpro_fr.md), [Italiano](opensprinklerpro_it.md).
* Firmware release notes: [OpenSprinklerShop Firmware Releases](https://github.com/opensprinklershop/OpenSprinkler-Firmware/releases).

<hr class="double">

## Start Compass for New Customers

| Goal | Recommended entry point |
|---|---|
| Connect a new device | [Zone Wiring Diagram](2.2.1/221_4_manual.md#zone-wiring-diagram), then [Installation and Hardware](2.2.1/221_4_manual.md#installation) for WiFi/Ethernet first setup |
| Create zones and watering programs | [Station Cards](2.2.1/221_4_manual.md#station-cards), [Programs](2.2.1/221_4_manual.md#programs) |
| Use weather, sensors and flow | [Weather Adjustment](2.2.1/221_4_manual.md#weather-adjustment), [Sensor Setup](2.2.1/221_4_manual.md#sensor-setup), [FYTA Sensors](fyta-sensors.md) |
| Remote access and notifications | [Integration](2.2.1/221_4_manual.md#integration), [OpenSprinklerPro notifications](opensprinklerpro.md#notification-events) |
| OpenSprinklerPro, Zigbee, Matter, BLE, RainMaker, MCP | [OpenSprinklerPro Extensions](opensprinklerpro.md), [MCP Server](mcp-server.md) |
| Troubleshooting | [Troubleshooting](troubleshooting.md), side menu -> **System Diagnostics** |

## Product Families and Relevant Documentation

| Product | Image | Summary | Relevant sections |
|---|---|---|---|
| [OpenSprinkler Pro](https://opensprinklershop.de/product/opensprinkler-pro/) | ![OpenSprinkler Pro](https://osshop.b-cdn.net/wp-content/uploads/2026/04/16432-scaled-e1777199276933-247x247.avif){ width="120" } | ESP32-C5 generation with WiFi 6, BLE and Zigbee or Matter firmware variants. | [OpenSprinklerPro](opensprinklerpro.md), [Hardware Interface](2.2.1/221_4_manual.md#hardware-interface), [Firmware Update](2.2.1/221_4_manual.md#firmware-update) |
| [OpenSprinkler 3.x](https://opensprinklershop.de/product/opensprinkler-3-0/) | ![OpenSprinkler 3.x](https://osshop.b-cdn.net/wp-content/uploads/2018/12/OpenSprinkerDC1-247x247.avif){ width="120" } | ESP8266 controller generation with WiFi, display and optional Ethernet. | [Installation](2.2.1/221_4_manual.md#installation), [Firmware Update](2.2.1/221_4_manual.md#firmware-update) |
| [OpenSprinkler Pi / OSPi 2.0](https://opensprinklershop.de/product/ospi20/) | ![OpenSprinkler Pi 2.0](https://osshop.b-cdn.net/wp-content/uploads/2024/12/enclosure6-247x234.avif){ width="120" } | Raspberry Pi HAT running OpenSprinkler firmware on Linux/Raspberry Pi. | [OSPi update](2.2.1/221_4_manual.md#firmware-update), [Sensor Setup](2.2.1/221_4_manual.md#sensor-setup) |
| [ESP32-C5 Upgrade Board](https://opensprinklershop.de/product/esp32-board-fuer-opensprinkler-3-3-upgrade/) | ![ESP32-C5 Upgrade Board](https://osshop.b-cdn.net/wp-content/uploads/2026/03/IMG_20260315_185118-scaled-e1773606716986-247x270.avif){ width="120" } | Upgrade board for OpenSprinkler 3.3 with ESP32-C5, BLE, Zigbee/Matter and online update. | [ESP32 mode](opensprinklerpro.md#esp32-mode-and-radio-management), [Online OTA](opensprinklerpro.md#online-ota-update) |
| [Analog Sensor Board](https://opensprinklershop.de/product/analog-sensor-board/) / [Truebner RS485 Adapter](https://opensprinklershop.de/product/truebner-rs485-adapter/) | ![Analog Sensor Board](https://osshop.b-cdn.net/wp-content/uploads/2023/01/os3analogsensorboard2-247x247.avif){ width="120" } | Sensor extensions for soil moisture, temperature, RS485/Modbus and monitoring. | [Analog Sensor Configuration](analog-sensor-config.md), [Sensor Automation](sensor-automation.md), [FYTA Sensors](fyta-sensors.md) |
