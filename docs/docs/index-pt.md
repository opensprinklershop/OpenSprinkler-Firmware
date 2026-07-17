# Firmware e Documentação do OpenSprinkler Shop

Este site contém documentação para o firmware do OpenSprinkler e as extensões OpenSprinklerPro mantidas pelo OpenSprinklerShop.

!!! info "Versão do firmware do OpenSprinklerShop"
    O firmware do OpenSprinklerShop, incluindo o OpenSprinklerPro, usa sua própria numeração de versão de firmware independente da linha de firmware oficial do OpenSprinkler. A versão atual do firmware do OpenSprinklerShop é **2.4.0(219)**.

!!! tip "Atualizações de Firmware e Portal"
    Os downloads de firmware, arquivos, changelogs e diretrizes de atualização estão hospedados no [Portal de Atualização OpenSprinklerShop](https://opensprinklershop.de/upgrade).

!!! note "Firmware Upstream Original"
    O firmware oficial, open-source e a documentação original do OpenSprinkler podem ser encontrados em: [https://opensprinkler.github.io/OpenSprinkler-Firmware](https://opensprinkler.github.io/OpenSprinkler-Firmware)

* Manual do usuário: [Português](2.2.1/221_4_manual_pt.md), [English](2.2.1/221_4_manual.md), [Deutsch](2.2.1/221_4_manual_de.md), [Français](2.2.1/221_4_manual_fr.md), [Italiano](2.2.1/221_4_manual_it.md), [Magyar](2.2.1/221_4_manual_hu.md), [Polski](2.2.1/221_4_manual_pl.md).
* Extensões OpenSprinklerPro: [Português](opensprinklerpro_pt.md), [English](opensprinklerpro.md), [Deutsch](opensprinklerpro_de.md), [Français](opensprinklerpro_fr.md), [Italiano](opensprinklerpro_it.md), [Magyar](opensprinklerpro_hu.md), [Polski](opensprinklerpro_pl.md).
* Notas de lançamento do firmware: [Versões de firmware do OpenSprinklerShop](https://opensprinklershop.de/upgrade/).

<hr class="double">

## Guia de Início para Novos Clientes

| Objetivo | Ponto de entrada recomendado |
|---|---|
| Conectar um novo dispositivo | [Diagrama de Fiação](2.2.1/221_4_manual_pt.md#zone-wiring-diagram), depois [Instalação e Hardware](2.2.1/221_4_manual_pt.md#installation) para primeira configuração WiFi/Ethernet |
| Criar zonas e programas de irrigação | [Cartões de Estação](2.2.1/221_4_manual_pt.md#station-cards), [Programas](2.2.1/221_4_manual_pt.md#programs) |
| Usar clima, sensores e fluxo | [Ajuste de Clima](2.2.1/221_4_manual_pt.md#weather-adjustment), [Configuração do Sensor](2.2.1/221_4_manual_pt.md#sensor-setup), [Sensores FYTA](fyta-sensors-pt.md) |
| Acesso remoto e notificações | [Integração](2.2.1/221_4_manual_pt.md#integration), [Notificações do OpenSprinklerPro](opensprinklerpro_pt.md#notification-events) |
| OpenSprinklerPro, Zigbee, Matter, BLE, RainMaker, MCP | [Extensões OpenSprinklerPro](opensprinklerpro_pt.md), [Servidor MCP](mcp-server-pt.md) |
| Solução de problemas | [Solução de problemas](troubleshooting-pt.md), menu lateral -> **Diagnósticos do Sistema** |

## Famílias de Produtos e Documentação Relevante

| Produto | Imagem | Resumo | Seções relevantes |
|---|---|---|---|
| [OpenSprinkler Pro](https://opensprinklershop.de/product/opensprinkler-pro/) | ![OpenSprinkler Pro](https://osshop.b-cdn.net/wp-content/uploads/2026/04/16432-scaled-e1777199276933-247x247.avif){ width="120" } | Geração ESP32-C5 com variantes de firmware WiFi 6, BLE, Zigbee ou Matter. | [OpenSprinklerPro](opensprinklerpro_pt.md), [Interface de Hardware](2.2.1/221_4_manual_pt.md#hardware-interface), [Atualização de Firmware](2.2.1/221_4_manual_pt.md#firmware-update) |
| [OpenSprinkler 3.x](https://opensprinklershop.de/product/opensprinkler-3-0/) | ![OpenSprinkler 3.x](https://osshop.b-cdn.net/wp-content/uploads/2018/12/OpenSprinkerDC1-247x247.avif){ width="120" } | Geração de controlador ESP8266 com WiFi, display e Ethernet opcional. | [Instalação](2.2.1/221_4_manual_pt.md#installation), [Atualização de Firmware](2.2.1/221_4_manual_pt.md#firmware-update) |
| [OpenSprinkler Pi / OSPi 2.0](https://opensprinklershop.de/product/ospi20/) | ![OpenSprinkler Pi 2.0](https://osshop.b-cdn.net/wp-content/uploads/2024/12/enclosure6-247x234.avif){ width="120" } | Raspberry Pi HAT executando firmware OpenSprinkler no Linux/Raspberry Pi. | [Atualização OSPi](2.2.1/221_4_manual_pt.md#firmware-update), [Configuração do Sensor](2.2.1/221_4_manual_pt.md#sensor-setup) |
| [ESP32-C5 Upgrade Board](https://opensprinklershop.de/product/esp32-board-fuer-opensprinkler-3-3-upgrade/) | ![ESP32-C5 Upgrade Board](https://osshop.b-cdn.net/wp-content/uploads/2026/03/IMG_20260315_185118-scaled-e1773606716986-247x270.avif){ width="120" } | Placa de upgrade para OpenSprinkler 3.3 com ESP32-C5, BLE, Zigbee/Matter e atualização online. | [Modo ESP32](opensprinklerpro_pt.md#esp32-mode-and-radio-management), [Online OTA](opensprinklerpro_pt.md#online-ota-update) |
| [Analog Sensor Board](https://opensprinklershop.de/product/analog-sensor-board/) / [Truebner RS485 Adapter](https://opensprinklershop.de/product/truebner-rs485-adapter/) | ![Analog Sensor Board](https://osshop.b-cdn.net/wp-content/uploads/2023/01/os3analogsensorboard2-247x247.avif){ width="120" } | Extensões de sensor para umidade do solo, temperatura, RS485/Modbus e monitoramento. | [Configuração de Sensor Analógico](analog-sensor-config-pt.md), [Automação de Sensores](sensor-automation-pt.md), [Sensores FYTA](fyta-sensors-pt.md) |
