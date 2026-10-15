/* OpenSprinkler Unified Firmware
 *
 * W5500 PHY/MAC recovery over SPI for the ESP8266 (OpenSprinkler 3.x Ethernet
 * add-on). See eth_w5500.h. Register access mirrors the ESP8266 core's
 * Wiznet5500 driver (utility/w5500.cpp): 16-bit address, control byte
 * (block select | read/write), data; variable length mode, chip select on
 * PIN_ETHER_CS, SPI already configured by init_W5500().
 *
 * IMPORTANT: nothing in here may yield to the system while the PHY link is
 * down. Wiznet5500::sendFrame() busy-waits without timeout for SENDOK, and a
 * frame handed to the MAC while the PHY has no link never completes, so any
 * lwIP transmit (ARP, DHCP, TCP retransmit, MQTT keepalive) during the reset
 * window would hang the firmware until the watchdog fires. All waits below are
 * therefore busy waits that only feed the watchdog (no delay()/yield()).
 */
#include "defines.h"

#if defined(ESP8266)

#include <Arduino.h>
#include <SPI.h>
#include "eth_w5500.h"
#include "OpenSprinkler.h"

extern void gratuitousARPTask(); // main.cpp

namespace {

// Control byte fields (W5500 datasheet 2.2.1)
const uint8_t W5500_READ       = 0x00 << 2;
const uint8_t W5500_WRITE      = 0x01 << 2;
const uint8_t W5500_BLOCK_CREG = 0x00 << 3;  // common registers
const uint8_t W5500_BLOCK_S0   = 0x01 << 3;  // socket 0 registers

// Common registers
const uint16_t W5500_MR       = 0x0000;
const uint16_t W5500_SHAR     = 0x0009;  // 6 bytes
const uint16_t W5500_PHYCFGR  = 0x002E;
const uint16_t W5500_VERSIONR = 0x0039;
// Socket registers
const uint16_t W5500_Sn_MR         = 0x0000;
const uint16_t W5500_Sn_CR         = 0x0001;
const uint16_t W5500_Sn_IR         = 0x0002;
const uint16_t W5500_Sn_SR         = 0x0003;
const uint16_t W5500_Sn_RXBUF_SIZE = 0x001E;
const uint16_t W5500_Sn_TXBUF_SIZE = 0x001F;

const uint8_t MR_RST = 0x80;

// PHYCFGR bits: RST(7) OPMD(6) OPMDC(5:3) DPX(2) SPD(1) LNK(0)
const uint8_t PHYCFGR_RST        = 0x80;
const uint8_t PHYCFGR_OPMD       = 0x40;
const uint8_t PHYCFGR_OPMDC_ALLA = 0x38;  // all capable, auto-negotiation
const uint8_t PHYCFGR_OPMDC_PDWN = 0x30;  // power down
const uint8_t PHYCFGR_LNK        = 0x01;

const uint8_t Sn_MR_MACRAW  = 0x04;
const uint8_t Sn_CR_OPEN    = 0x01;
const uint8_t Sn_CR_CLOSE   = 0x10;
const uint8_t SOCK_MACRAW   = 0x42;

const uint32_t PHY_POWERDOWN_MS = 300;   // long enough for the link partner to see link-down

uint16_t s_recover_count = 0;

uint8_t w5500_read(uint8_t block, uint16_t addr) {
	digitalWrite(PIN_ETHER_CS, LOW);
	SPI.transfer(addr >> 8);
	SPI.transfer(addr & 0xFF);
	SPI.transfer(block | W5500_READ);
	uint8_t v = SPI.transfer(0);
	digitalWrite(PIN_ETHER_CS, HIGH);
	return v;
}

void w5500_write(uint8_t block, uint16_t addr, uint8_t v) {
	digitalWrite(PIN_ETHER_CS, LOW);
	SPI.transfer(addr >> 8);
	SPI.transfer(addr & 0xFF);
	SPI.transfer(block | W5500_WRITE);
	SPI.transfer(v);
	digitalWrite(PIN_ETHER_CS, HIGH);
}

// Busy wait that keeps the watchdog quiet but never yields (see file header).
void w5500_busy_wait_ms(uint32_t ms) {
	uint32_t t0 = millis();
	while (millis() - t0 < ms) {
		ESP.wdtFeed();
		delayMicroseconds(500);
	}
}

// Wait until the socket command register reads back 0 (command accepted).
bool w5500_wait_cmd(uint32_t timeout_ms) {
	uint32_t t0 = millis();
	while (w5500_read(W5500_BLOCK_S0, W5500_Sn_CR) != 0) {
		if (millis() - t0 > timeout_ms) return false;
		ESP.wdtFeed();
		delayMicroseconds(200);
	}
	return true;
}

// Pulse PHYCFGR.RST low with the given operation mode, then set RST again.
void w5500_phy_cfg_reset(uint8_t opmdc) {
	w5500_write(W5500_BLOCK_CREG, W5500_PHYCFGR, (uint8_t)(PHYCFGR_OPMD | opmdc));              // RST=0
	w5500_busy_wait_ms(2);
	w5500_write(W5500_BLOCK_CREG, W5500_PHYCFGR, (uint8_t)(PHYCFGR_RST | PHYCFGR_OPMD | opmdc)); // RST=1
}

bool w5500_wait_link(uint32_t timeout_ms, uint8_t *cfg_out) {
	uint32_t t0 = millis();
	uint8_t cfg = 0;
	while (millis() - t0 < timeout_ms) {
		cfg = w5500_read(W5500_BLOCK_CREG, W5500_PHYCFGR);
		if (cfg & PHYCFGR_LNK) break;
		w5500_busy_wait_ms(20);
	}
	if (cfg_out) *cfg_out = cfg;
	return (cfg & PHYCFGR_LNK) != 0;
}

} // namespace

bool eth_w5500_link_up() {
	pinMode(PIN_ETHER_CS, OUTPUT);
	return (w5500_read(W5500_BLOCK_CREG, W5500_PHYCFGR) & PHYCFGR_LNK) != 0;
}

bool eth_w5500_phy_reset(bool power_cycle) {
	pinMode(PIN_ETHER_CS, OUTPUT);
	uint8_t before = w5500_read(W5500_BLOCK_CREG, W5500_PHYCFGR);
	DEBUG_PRINTF("[ETH] W5500 PHY reset (power_cycle=%d) PHYCFGR=0x%02X\n", power_cycle ? 1 : 0, before);

	if (power_cycle) {
		// Power the PHY down so the link partner sees the link drop and clears
		// its port state, then bring it back with auto-negotiation.
		w5500_phy_cfg_reset(PHYCFGR_OPMDC_PDWN);
		w5500_busy_wait_ms(PHY_POWERDOWN_MS);
	}
	w5500_phy_cfg_reset(PHYCFGR_OPMDC_ALLA);

	uint32_t t0 = millis();
	uint8_t cfg = 0;
	bool link = w5500_wait_link(ETH_W5500_LINK_TIMEOUT_MS, &cfg);
	DEBUG_PRINTF("[ETH] W5500 PHY after reset: PHYCFGR=0x%02X link=%d (%lu ms)\n", cfg, link ? 1 : 0, (unsigned long)(millis() - t0));
	return link;
}

bool eth_w5500_mac_reinit() {
	pinMode(PIN_ETHER_CS, OUTPUT);
	// Keep the MAC address the driver programmed (Wiznet5500::begin -> setSHAR).
	uint8_t mac[6];
	for (uint8_t i = 0; i < 6; i++) mac[i] = w5500_read(W5500_BLOCK_CREG, W5500_SHAR + i);
	uint8_t ver = w5500_read(W5500_BLOCK_CREG, W5500_VERSIONR);
	DEBUG_PRINTF("[ETH] W5500 MAC reinit: VERSIONR=0x%02X mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
		ver, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	if (ver != 0x04) {
		DEBUG_PRINTLN(F("[ETH] W5500 not responding on SPI"));
		return false;
	}

	// Same sequence as Wiznet5500::begin(): software reset, full 16 KB buffers
	// for socket 0, MAC address, MACRAW mode, open.
	w5500_write(W5500_BLOCK_CREG, W5500_MR, MR_RST);
	uint32_t t0 = millis();
	while (w5500_read(W5500_BLOCK_CREG, W5500_MR) & MR_RST) {   // RST self-clears
		if (millis() - t0 > 50) break;
		ESP.wdtFeed();
		delayMicroseconds(100);
	}
	w5500_write(W5500_BLOCK_S0, W5500_Sn_RXBUF_SIZE, 16);
	w5500_write(W5500_BLOCK_S0, W5500_Sn_TXBUF_SIZE, 16);
	for (uint8_t i = 0; i < 6; i++) w5500_write(W5500_BLOCK_CREG, W5500_SHAR + i, mac[i]);
	w5500_write(W5500_BLOCK_S0, W5500_Sn_IR, 0xFF);
	w5500_write(W5500_BLOCK_S0, W5500_Sn_MR, Sn_MR_MACRAW);
	w5500_write(W5500_BLOCK_S0, W5500_Sn_CR, Sn_CR_OPEN);
	// Sn_SR is the authoritative result (Sn_CR read-back right after the MR
	// reset was seen to stay non-zero although the socket did open).
	uint8_t sr = 0;
	t0 = millis();
	while (millis() - t0 < 100) {
		sr = w5500_read(W5500_BLOCK_S0, W5500_Sn_SR);
		if (sr == SOCK_MACRAW) break;
		ESP.wdtFeed();
		delayMicroseconds(200);
	}
	DEBUG_PRINTF("[ETH] W5500 socket 0: Sn_SR=0x%02X Sn_CR=0x%02X\n", sr, w5500_read(W5500_BLOCK_S0, W5500_Sn_CR));
	return sr == SOCK_MACRAW;
}

bool eth_w5500_recover() {
	if (!useEth || !eth.isW5500) return false;
	s_recover_count++;
	DEBUG_PRINTF("[ETH] W5500 recovery #%u: connected=%d ip=%s\n", (unsigned)s_recover_count,
		eth.connected() ? 1 : 0, eth.localIP().toString().c_str());
	// Order: PHY first (power-cycle reset, wait for link), then the MAC. The
	// W5500 does not process a socket OPEN while the PHY is powered down, and
	// the MR reset in the MAC re-init leaves the PHY alone anyway (it only
	// returns PHYCFGR to hardware-pin mode, the link stays up).
	// No yield from here until the link is back (see file header).
	bool link = eth_w5500_phy_reset(true);
	bool sock = eth_w5500_mac_reinit();
	if (link && sock) {
		// The router's ARP entry for us has usually expired/failed by now.
		gratuitousARPTask();
	}
	DEBUG_PRINTF("[ETH] W5500 recovery %s (link=%d sock=%d)\n", (link && sock) ? "done" : "FAILED", link ? 1 : 0, sock ? 1 : 0);
	return link && sock;
}

uint16_t eth_w5500_recover_count() {
	return s_recover_count;
}

void eth_w5500_loop() {}

#if defined(ENABLE_DEBUG)
void eth_w5500_test_linkdown(uint32_t ms) {
	// Diagnostic only (debug builds): PHY power-down with the system running,
	// so lwIP keeps transmitting into a dead link. Proves that the patched
	// Wiznet5500::sendFrame() (lib/lwIP_w5500) times out instead of hanging.
	pinMode(PIN_ETHER_CS, OUTPUT);
	DEBUG_PRINTF("[ETH] TEST: PHY power-down for %lu ms with yield\n", (unsigned long)ms);
	w5500_phy_cfg_reset(PHYCFGR_OPMDC_PDWN);
	uint32_t t0 = millis();
	while (millis() - t0 < ms) {
		delay(50);   // yields: lwIP timers, MQTT, HTTP all keep running
	}
	DEBUG_PRINTLN(F("[ETH] TEST: power-down window over, recovering"));
	eth_w5500_recover();
}
#endif

#elif defined(ESP32)

/* ESP32-C5: the W5500 is driven by the ESP-IDF esp_eth driver (own task, SPI
 * and SEND waits with timeouts, PHY reset in phy->init at driver start). What
 * is missing there is a recovery without reboot when the PHY still reports
 * link-up but passes no frames. esp_eth_stop() forces the link down,
 * esp_eth_start() restarts auto-negotiation, which the W5500 PHY driver
 * implements as a PHYCFGR reset pulse; lwIP then re-runs DHCP on link-up. The
 * restart is executed from the main loop (eth_w5500_loop()) so it never runs
 * inside a ping or HTTP task. */
#include <Arduino.h>
#include "eth_w5500.h"
#include "OpenSprinkler.h"
#include <esp_eth.h>

namespace {
uint16_t s_recover_count = 0;
volatile bool s_recover_pending = false;
}

bool eth_w5500_link_up() {
	return useEth && eth.linkUp();
}

bool eth_w5500_recover() {
	if (!useEth) return false;
	bool link = eth.linkUp();
	if (!s_recover_pending) {
		s_recover_pending = true;
		DEBUG_PRINTF("[ETH] W5500 recovery scheduled (link=%d ip=%s)\n", link ? 1 : 0, eth.localIP().toString().c_str());
	}
	return link;
}

uint16_t eth_w5500_recover_count() {
	return s_recover_count;
}

void eth_w5500_loop() {
	if (!s_recover_pending) return;
	s_recover_pending = false;
	esp_eth_handle_t h = eth.handle();
	if (!h) {
		DEBUG_PRINTLN(F("[ETH] W5500 recovery: no driver handle"));
		return;
	}
	s_recover_count++;
	DEBUG_PRINTF("[ETH] W5500 recovery #%u: esp_eth_stop/start\n", (unsigned)s_recover_count);
	esp_err_t rs = esp_eth_stop(h);
	delay(300);
	esp_err_t rr = esp_eth_start(h);
	uint32_t t0 = millis();
	while (millis() - t0 < 5000 && !eth.linkUp()) delay(100);
	DEBUG_PRINTF("[ETH] W5500 recovery %s (stop=%d start=%d link=%d, %lu ms)\n",
		eth.linkUp() ? "done" : "FAILED", (int)rs, (int)rr, eth.linkUp() ? 1 : 0, (unsigned long)(millis() - t0));
}

#endif // ESP8266 / ESP32
