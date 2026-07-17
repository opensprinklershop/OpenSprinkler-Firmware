# OpenSprinkler Shop Firmware und Dokumentation

Diese Seite enthält die Dokumentation zur OpenSprinkler-Firmware und zu den von OpenSprinklerShop gepflegten OpenSprinklerPro-Erweiterungen.

!!! info "OpenSprinklerShop-Firmwareversion"
    Die OpenSprinklerShop-Firmware inklusive OpenSprinklerPro verwendet eine eigene Firmware-Versionierung unabhängig von der Upstream-OpenSprinkler-Firmware. Die aktuelle OpenSprinklerShop-Firmwareversion ist **2.4.0(219)**.

!!! tip "Firmware-Upgrades & Portal"
    Firmware-Downloads, Archive, Changelogs und Upgrade-Richtlinien finden Sie auf dem [OpenSprinklerShop Upgrade-Portal](https://opensprinklershop.de/upgrade).

!!! note "Originale Upstream-Firmware"
    Die offizielle, originale OpenSprinkler Open-Source-Firmware und die dazugehörige Dokumentation finden Sie unter: [https://opensprinkler.github.io/OpenSprinkler-Firmware](https://opensprinkler.github.io/OpenSprinkler-Firmware)

* Benutzerhandbuch: [English](2.2.1/221_4_manual.md), [Deutsch](2.2.1/221_4_manual_de.md), [Français](2.2.1/221_4_manual_fr.md), [Italiano](2.2.1/221_4_manual_it.md), [Português](2.2.1/221_4_manual_pt.md), [Magyar](2.2.1/221_4_manual_hu.md), [Polski](2.2.1/221_4_manual_pl.md).
* OpenSprinklerPro-Erweiterungen: [English](opensprinklerpro.md), [Deutsch](opensprinklerpro_de.md), [Français](opensprinklerpro_fr.md), [Italiano](opensprinklerpro_it.md), [Português](opensprinklerpro_pt.md), [Magyar](opensprinklerpro_hu.md), [Polski](opensprinklerpro_pl.md).
* Firmware-Versionshinweise: [OpenSprinklerShop Firmware-Releases](https://opensprinklershop.de/upgrade/).

<hr class="double">

## Start-Kompass für neue Kunden

| Ziel | Empfohlener Einstieg |
|---|---|
| Neues Gerät anschließen | [Anschlussdiagramm für Zonenkabel](2.2.1/221_4_manual_de.md#anschlussdiagramm-fur-zonenkabel), danach [Installation und Hardware](2.2.1/221_4_manual_de.md#installation) für die WiFi-/Ethernet-Ersteinrichtung |
| Zonen und Bewässerungsprogramme anlegen | [Zonenattribute](2.2.1/221_4_manual_de.md#zonenattribute), [Programme](2.2.1/221_4_manual_de.md#programme) |
| Wetter, Sensoren und Durchfluss nutzen | [Wetter und Sensoren](2.2.1/221_4_manual_de.md#wetter-und-sensoren), [Sensor-Einrichtung](2.2.1/221_4_manual_de.md#sensor-einrichtung), [FYTA-Sensoren](fyta-sensors.md) |
| Fernzugriff und Benachrichtigungen einrichten | [Integrationen](2.2.1/221_4_manual_de.md#integrationen), [OpenSprinklerPro-Benachrichtigungen](opensprinklerpro_de.md#benachrichtigungsereignisse) |
| OpenSprinklerPro, Zigbee, Matter, BLE, RainMaker, MCP | [OpenSprinklerPro-Erweiterung](opensprinklerpro_de.md), [MCP-Server](mcp-server-de.md) |
| Fehleranalyse | [Troubleshooting](troubleshooting-de.md), Seitenmenü -> **System-Diagnose** |

## Produktfamilien und relevante Dokumentation

| Produkt | Bild | Kurzbeschreibung | Relevante Abschnitte |
|---|---|---|---|
| [OpenSprinkler Pro](https://opensprinklershop.de/product/opensprinkler-pro/) | ![OpenSprinkler Pro](https://osshop.b-cdn.net/wp-content/uploads/2026/04/16432-scaled-e1777199276933-247x247.avif){ width="120" } | ESP32-C5-Generation mit WiFi 6, BLE, Zigbee- oder Matter-Firmwarevarianten. | [OpenSprinklerPro](opensprinklerpro_de.md), [Hardware-Schnittstelle](2.2.1/221_4_manual_de.md#hardware-schnittstelle), [Firmware-Update](2.2.1/221_4_manual_de.md#firmware-update) |
| [OpenSprinkler 3.x](https://opensprinklershop.de/product/opensprinkler-3-0/) | ![OpenSprinkler 3.x](https://osshop.b-cdn.net/wp-content/uploads/2018/12/OpenSprinkerDC1-247x247.avif){ width="120" } | ESP8266-basierte Controller-Generation mit WiFi, Display und optionalem Ethernet. | [Installation](2.2.1/221_4_manual_de.md#installation), [Firmware-Update](2.2.1/221_4_manual_de.md#firmware-update) |
| [OpenSprinkler Pi / OSPi 2.0](https://opensprinklershop.de/product/ospi20/) | ![OpenSprinkler Pi 2.0](https://osshop.b-cdn.net/wp-content/uploads/2024/12/enclosure6-247x234.avif){ width="120" } | Raspberry-Pi-HAT mit OpenSprinkler-Firmware auf Linux/Raspberry Pi. | [OSPi-Update](2.2.1/221_4_manual_de.md#firmware-update), [Sensor-Einrichtung](2.2.1/221_4_manual_de.md#sensor-einrichtung) |
| [ESP32-C5 Upgrade Board](https://opensprinklershop.de/product/esp32-board-fuer-opensprinkler-3-3-upgrade/) | ![ESP32-C5 Upgrade Board](https://osshop.b-cdn.net/wp-content/uploads/2026/03/IMG_20260315_185118-scaled-e1773606716986-247x270.avif){ width="120" } | Upgrade-Board für OpenSprinkler 3.3 mit ESP32-C5, BLE, Zigbee/Matter und Online-Update. | [ESP32-Modus](opensprinklerpro_de.md#esp32-modus-und-funkverwaltung), [Online-OTA](opensprinklerpro_de.md#online-ota-update) |
| [Analog Sensor Board](https://opensprinklershop.de/product/analog-sensor-board/) / [Truebner RS485 Adapter](https://opensprinklershop.de/product/truebner-rs485-adapter/) | ![Analog Sensor Board](https://osshop.b-cdn.net/wp-content/uploads/2023/01/os3analogsensorboard2-247x247.avif){ width="120" } | Sensorerweiterungen für Bodenfeuchte, Temperatur, RS485/Modbus und Monitoring. | [Analog Sensor Konfiguration](analog-sensor-config-de.md), [Sensor-Automation](sensor-automation-de.md), [FYTA-Sensoren](fyta-sensors.md) |
