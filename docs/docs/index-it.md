# Firmware e documentazione OpenSprinkler Shop

Questo sito contiene la documentazione del firmware OpenSprinkler e delle estensioni OpenSprinklerPro mantenute da OpenSprinklerShop.

!!! info "Versione firmware OpenSprinklerShop"
    Il firmware OpenSprinklerShop, incluso OpenSprinklerPro, usa una propria numerazione di versione indipendente dalla linea firmware OpenSprinkler upstream. La versione firmware OpenSprinklerShop attuale è **2.4.0(219)**.

!!! tip "Aggiornamenti del firmware e Portale"
    I download del firmware, gli archivi, i changelog e le linee guida per l'aggiornamento sono ospitati sul [portale di aggiornamento OpenSprinklerShop](https://opensprinklershop.de/upgrade).

!!! note "Firmware originale a monte (Upstream)"
    Il firmware open-source ufficiale di OpenSprinkler e la relativa documentazione si trovano su: [https://opensprinkler.github.io/OpenSprinkler-Firmware](https://opensprinkler.github.io/OpenSprinkler-Firmware)

* Manuale utente: [English](2.2.1/221_4_manual.md), [Deutsch](2.2.1/221_4_manual_de.md), [Français](2.2.1/221_4_manual_fr.md), [Italiano](2.2.1/221_4_manual_it.md), [Português](2.2.1/221_4_manual_pt.md), [Magyar](2.2.1/221_4_manual_hu.md), [Polski](2.2.1/221_4_manual_pl.md).
* Estensioni OpenSprinklerPro: [English](opensprinklerpro.md), [Deutsch](opensprinklerpro_de.md), [Français](opensprinklerpro_fr.md), [Italiano](opensprinklerpro_it.md), [Português](opensprinklerpro_pt.md), [Magyar](opensprinklerpro_hu.md), [Polski](opensprinklerpro_pl.md).
* Note di rilascio firmware: [OpenSprinklerShop Firmware Releases](https://opensprinklershop.de/upgrade/).

<hr class="double">

## Bussola iniziale per nuovi clienti

| Obiettivo | Punto di partenza consigliato |
|---|---|
| Collegare un nuovo dispositivo | [Schema di cablaggio delle zone](2.2.1/221_4_manual_it.md#zone-wiring-diagram), poi [Installazione e hardware](2.2.1/221_4_manual_it.md#installazione) per la prima configurazione WiFi/Ethernet |
| Creare zone e programmi | [Schede stazione](2.2.1/221_4_manual_it.md#station-cards), [Programmi](2.2.1/221_4_manual_it.md#programs) |
| Usare meteo, sensori e flusso | [Regolazione meteo](2.2.1/221_4_manual_it.md#weather-adjustment), [Sensori](2.2.1/221_4_manual_it.md#sensor-setup), [FYTA](fyta-sensors.md) |
| Accesso remoto e notifiche | [Integrazione](2.2.1/221_4_manual_it.md#integration), [notifiche Pro](opensprinklerpro_it.md#eventi-di-notifica) |
| OpenSprinklerPro, Zigbee, Matter, BLE, RainMaker, MCP | [Estensione OpenSprinklerPro](opensprinklerpro_it.md), [Server MCP](mcp-server-it.md) |
| Diagnostica | [Troubleshooting](troubleshooting-it.md), menu laterale -> **Diagnostica di sistema** |

## Famiglie prodotto e documentazione

| Prodotto | Immagine | Sintesi | Sezioni utili |
|---|---|---|---|
| [OpenSprinkler Pro](https://opensprinklershop.de/product/opensprinkler-pro/) | ![OpenSprinkler Pro](https://osshop.b-cdn.net/wp-content/uploads/2026/04/16432-scaled-e1777199276933-247x247.avif){ width="120" } | Generazione ESP32-C5 con WiFi 6, BLE e varianti Zigbee o Matter. | [OpenSprinklerPro](opensprinklerpro_it.md), [Interfaccia hardware](2.2.1/221_4_manual_it.md#hardware-interface), [Aggiornamento firmware](2.2.1/221_4_manual_it.md#aggiornamento-firmware) |
| [OpenSprinkler 3.x](https://opensprinklershop.de/product/opensprinkler-3-0/) | ![OpenSprinkler 3.x](https://osshop.b-cdn.net/wp-content/uploads/2018/12/OpenSprinkerDC1-247x247.avif){ width="120" } | Controller ESP8266 con WiFi, display ed Ethernet opzionale. | [Installazione](2.2.1/221_4_manual_it.md#installazione), [Aggiornamento firmware](2.2.1/221_4_manual_it.md#aggiornamento-firmware) |
| [OpenSprinkler Pi / OSPi 2.0](https://opensprinklershop.de/product/ospi20/) | ![OpenSprinkler Pi 2.0](https://osshop.b-cdn.net/wp-content/uploads/2024/12/enclosure6-247x234.avif){ width="120" } | HAT Raspberry Pi con firmware OpenSprinkler su Linux/Raspberry Pi. | [Update OSPi](2.2.1/221_4_manual_it.md#aggiornamento-firmware), [Sensori](2.2.1/221_4_manual_it.md#sensor-setup) |
| [Scheda upgrade ESP32-C5](https://opensprinklershop.de/product/esp32-board-fuer-opensprinkler-3-3-upgrade/) | ![Scheda ESP32-C5](https://osshop.b-cdn.net/wp-content/uploads/2026/03/IMG_20260315_185118-scaled-e1773606716986-247x270.avif){ width="120" } | Scheda upgrade per OpenSprinkler 3.3 con ESP32-C5, BLE, Zigbee/Matter e aggiornamento online. | [Modalità ESP32](opensprinklerpro_it.md#modalita-esp32-e-gestione-radio), [OTA online](opensprinklerpro_it.md#aggiornamento-ota-online) |
| [Analog Sensor Board](https://opensprinklershop.de/product/analog-sensor-board/) / [Adattatore Truebner RS485](https://opensprinklershop.de/product/truebner-rs485-adapter/) | ![Analog Sensor Board](https://osshop.b-cdn.net/wp-content/uploads/2023/01/os3analogsensorboard2-247x247.avif){ width="120" } | Estensioni sensori per umidità del suolo, temperatura, RS485/Modbus e monitoraggio. | [Configurazione sensori analogici](analog-sensor-config-it.md), [Automazione sensori](sensor-automation-it.md), [FYTA](fyta-sensors.md) |
