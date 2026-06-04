# Firmware et documentation OpenSprinkler Shop

Ce site contient la documentation du firmware OpenSprinkler et des extensions OpenSprinklerPro maintenues par OpenSprinklerShop.

!!! info "Version du firmware OpenSprinklerShop"
    Le firmware OpenSprinklerShop, y compris OpenSprinklerPro, utilise sa propre numérotation de version indépendamment de la ligne upstream du firmware OpenSprinkler. La version actuelle du firmware OpenSprinklerShop est **2.4.0(208)**.

!!! tip "Mises à jour du firmware & Portail"
    Les téléchargements de firmware, les archives, les changelogs et les directives de mise à niveau sont hébergés sur le [portail de mise à niveau OpenSprinklerShop](https://opensprinklershop.de/upgrade).

!!! note "Firmware original en amont (Upstream)"
    Le firmware open-source officiel d'OpenSprinkler et sa documentation se trouvent sur : [https://opensprinkler.github.io/OpenSprinkler-Firmware](https://opensprinkler.github.io/OpenSprinkler-Firmware)

* Manuel utilisateur : [English](2.2.1/221_4_manual.md), [Deutsch](2.2.1/221_4_manual_de.md), [Français](2.2.1/221_4_manual_fr.md), [Italiano](2.2.1/221_4_manual_it.md), [Português](2.2.1/221_4_manual_pt.md), [Magyar](2.2.1/221_4_manual_hu.md), [Polski](2.2.1/221_4_manual_pl.md).
* Extensions OpenSprinklerPro : [English](opensprinklerpro.md), [Deutsch](opensprinklerpro_de.md), [Français](opensprinklerpro_fr.md), [Italiano](opensprinklerpro_it.md), [Português](opensprinklerpro_pt.md), [Magyar](opensprinklerpro_hu.md), [Polski](opensprinklerpro_pl.md).
* Notes de version du firmware : [OpenSprinklerShop Firmware Releases](https://opensprinklershop.de/upgrade/).

<hr class="double">

## Boussole de départ pour nouveaux clients

| Objectif | Point d'entrée recommandé |
|---|---|
| Brancher un nouvel appareil | [Schéma de câblage des zones](2.2.1/221_4_manual_fr.md#zone-wiring-diagram), puis [Installation et matériel](2.2.1/221_4_manual_fr.md#installation) pour la première configuration WiFi/Ethernet |
| Créer zones et programmes | [Cartes de stations](2.2.1/221_4_manual_fr.md#station-cards), [Programmes](2.2.1/221_4_manual_fr.md#programs) |
| Utiliser météo, capteurs et débit | [Ajustement météo](2.2.1/221_4_manual_fr.md#weather-adjustment), [Capteurs](2.2.1/221_4_manual_fr.md#sensor-setup), [FYTA](fyta-sensors.md) |
| Accès distant et notifications | [Intégration](2.2.1/221_4_manual_fr.md#integration), [notifications Pro](opensprinklerpro_fr.md#evenements-de-notification) |
| OpenSprinklerPro, Zigbee, Matter, BLE, RainMaker, MCP | [Extension OpenSprinklerPro](opensprinklerpro_fr.md), [Serveur MCP](mcp-server-fr.md) |
| Diagnostic | [Troubleshooting](troubleshooting-fr.md), menu latéral -> **Diagnostics du système** |

## Familles de produits et documentation

| Produit | Image | Résumé | Sections utiles |
|---|---|---|---|
| [OpenSprinkler Pro](https://opensprinklershop.de/product/opensprinkler-pro/) | ![OpenSprinkler Pro](https://osshop.b-cdn.net/wp-content/uploads/2026/04/16432-scaled-e1777199276933-247x247.avif){ width="120" } | Génération ESP32-C5 avec WiFi 6, BLE et variantes Zigbee ou Matter. | [OpenSprinklerPro](opensprinklerpro_fr.md), [Interface matérielle](2.2.1/221_4_manual_fr.md#hardware-interface), [Mise à jour](2.2.1/221_4_manual_fr.md#mise-a-jour-du-firmware) |
| [OpenSprinkler 3.x](https://opensprinklershop.de/product/opensprinkler-3-0/) | ![OpenSprinkler 3.x](https://osshop.b-cdn.net/wp-content/uploads/2018/12/OpenSprinkerDC1-247x247.avif){ width="120" } | Contrôleur ESP8266 avec WiFi, écran et Ethernet optionnel. | [Installation](2.2.1/221_4_manual_fr.md#installation), [Mise à jour](2.2.1/221_4_manual_fr.md#mise-a-jour-du-firmware) |
| [OpenSprinkler Pi / OSPi 2.0](https://opensprinklershop.de/product/ospi20/) | ![OpenSprinkler Pi 2.0](https://osshop.b-cdn.net/wp-content/uploads/2024/12/enclosure6-247x234.avif){ width="120" } | HAT Raspberry Pi exécutant OpenSprinkler sous Linux/Raspberry Pi. | [Update OSPi](2.2.1/221_4_manual_fr.md#mise-a-jour-du-firmware), [Capteurs](2.2.1/221_4_manual_fr.md#sensor-setup) |
| [Carte de mise à niveau ESP32-C5](https://opensprinklershop.de/product/esp32-board-fuer-opensprinkler-3-3-upgrade/) | ![Carte ESP32-C5](https://osshop.b-cdn.net/wp-content/uploads/2026/03/IMG_20260315_185118-scaled-e1773606716986-247x270.avif){ width="120" } | Carte de mise à niveau pour OpenSprinkler 3.3 avec ESP32-C5, BLE, Zigbee/Matter et mise à jour en ligne. | [Mode ESP32](opensprinklerpro_fr.md#mode-esp32-et-gestion-radio), [OTA en ligne](opensprinklerpro_fr.md#mise-a-jour-ota-en-ligne) |
| [Analog Sensor Board](https://opensprinklershop.de/product/analog-sensor-board/) / [Adaptateur Truebner RS485](https://opensprinklershop.de/product/truebner-rs485-adapter/) | ![Analog Sensor Board](https://osshop.b-cdn.net/wp-content/uploads/2023/01/os3analogsensorboard2-247x247.avif){ width="120" } | Extensions de capteurs pour humidité du sol, température, RS485/Modbus et monitoring. | [Configuration des capteurs analogiques](analog-sensor-config-fr.md), [Automatisation capteurs](sensor-automation-fr.md), [FYTA](fyta-sensors.md) |
