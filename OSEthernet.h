/* OpenSprinkler Unified Firmware
 *
 * Standalone OSEthernet adapter definition for unified Ethernet access
 * on ESP8266 and ESP32 platforms.
 */

#ifndef _OSETHERNET_H
#define _OSETHERNET_H

#include <Arduino.h>
#include "defines.h"

#if defined(ESP8266)
	#include <ENC28J60lwIP.h>
	#include <W5500lwIP.h>
	#if OS_ETH_TOE
		#include <EthernetCompat.h>
		extern ArduinoENC28J60lwIP enc28j60;
		extern ArduinoWiznet5500lwIP w5500;
	#else
		extern ENC28J60lwIP enc28j60;
		extern Wiznet5500lwIP w5500;
	#endif

	struct OSEthernet {
		bool isW5500 = false;
		inline boolean config(const IPAddress& local_ip, const IPAddress& arg1, const IPAddress& arg2, const IPAddress& arg3 = IPADDR_NONE, const IPAddress& dns2 = IPADDR_NONE) {
			return (isW5500)?w5500.config(local_ip, arg1, arg2, arg3, dns2) : enc28j60.config(local_ip, arg1, arg2, arg3, dns2);
		}
		inline boolean begin(const uint8_t *macAddress = nullptr) {
			return (isW5500)?w5500.begin(macAddress):enc28j60.begin(macAddress);
		}
		inline IPAddress localIP() {
			return (isW5500)?w5500.localIP():enc28j60.localIP();
		}
		inline IPAddress subnetMask() {
			return (isW5500)?w5500.subnetMask():enc28j60.subnetMask();
		}
		inline IPAddress gatewayIP() {
			return (isW5500)?w5500.gatewayIP():enc28j60.gatewayIP();
		}
		inline void setDefault() {
			(isW5500)?w5500.setDefault():enc28j60.setDefault();
		}
		inline bool connected() {
			return (isW5500)?w5500.connected():enc28j60.connected();
		}
		inline wl_status_t status() {
			return (isW5500)?w5500.status():enc28j60.status();
		}
	};

#elif defined(ESP32)
	#include <ETH.h>

	struct OSEthernet {
		bool isW5500 = true;

		inline boolean config(const IPAddress& local_ip, const IPAddress& arg1, const IPAddress& arg2, const IPAddress& arg3 = IPADDR_NONE, const IPAddress& dns2 = IPADDR_NONE) {
			return backend.config(local_ip, arg1, arg2, arg3, dns2);
		}

		inline bool begin(
			eth_phy_type_t type,
			int32_t phy_addr,
			int cs,
			int irq,
			int rst,
			spi_host_device_t spi_host,
			int sck = -1,
			int miso = -1,
			int mosi = -1,
			uint8_t spi_freq_mhz = ETH_PHY_SPI_FREQ_MHZ
		) {
			return backend.begin(type, phy_addr, cs, irq, rst, spi_host, sck, miso, mosi, spi_freq_mhz);
		}

		inline IPAddress localIP() { return backend.localIP(); }
		inline IPAddress subnetMask() { return backend.subnetMask(); }
		inline IPAddress gatewayIP() { return backend.gatewayIP(); }
		// Mirror ETHClass semantics: connected() reflects link/interface state,
		// not necessarily DHCP completion at that exact moment.
		inline bool connected() { return backend.connected(); }
		inline bool linkUp() { return backend.linkUp(); }
		inline wl_status_t status() { return connected() ? WL_CONNECTED : WL_DISCONNECTED; }
		inline bool setHostname(const char* name) { return backend.setHostname(name); }

	private:
		ETHClass backend;
	};
#endif

#endif // _OSETHERNET_H
