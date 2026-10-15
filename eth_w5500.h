/* OpenSprinkler Unified Firmware
 *
 * W5500 Ethernet recovery for OpenSprinkler 3.x (ESP8266) with the W5500
 * Ethernet add-on. The module's RESET pin is not wired to a GPIO, so the chip
 * can only be reset over SPI: a software reset (MR.RST) re-initialises the MAC
 * but not the PHY, which is why a plain reboot does not cure a PHY that stopped
 * passing frames while still reporting link-up. These helpers reset the PHY via
 * PHYCFGR (optionally with a power-down cycle so the link partner sees the link
 * drop) and re-open socket 0 in MACRAW mode, mirroring Wiznet5500::begin().
 */
#ifndef _ETH_W5500_H
#define _ETH_W5500_H

#include <stdint.h>

#if defined(ESP8266) || defined(ESP32)
/** Full runtime recovery of the W5500 Ethernet link (see per-platform notes
 *  below). Increments the recovery counter. ESP8266: runs synchronously.
 *  ESP32: schedules the restart for the main loop (eth_w5500_loop()) because it
 *  may be called from a ping or HTTP task; returns the link state at call time. */
bool eth_w5500_recover();

/** Number of runtime recoveries since boot (reported as "ethrst" in /jc). */
uint16_t eth_w5500_recover_count();

/** PHY link status (ESP8266: PHYCFGR.LNK over SPI, ESP32: driver link state). */
bool eth_w5500_link_up();

/** Call from the main loop (ESP32: executes a pending recovery). */
void eth_w5500_loop();
#else
inline bool eth_w5500_recover() { return false; }
inline uint16_t eth_w5500_recover_count() { return 0; }
inline bool eth_w5500_link_up() { return false; }
inline void eth_w5500_loop() {}
#endif

#if defined(ESP8266)

/** Reset the W5500 PHY (PHYCFGR.RST) and re-enable auto-negotiation with all
 *  capabilities. With power_cycle the PHY is first put into power-down for a
 *  moment so the switch sees a real link-down/link-up. Returns true when the
 *  PHY reports link-up within ETH_W5500_LINK_TIMEOUT_MS. */
bool eth_w5500_phy_reset(bool power_cycle);

/** Re-initialise the MAC exactly like Wiznet5500::begin(): software reset,
 *  16 KB socket-0 buffers, MAC address (read back from the chip), MACRAW mode,
 *  open. The lwIP driver keeps no chip state of its own, so this is transparent
 *  to it. Returns true when socket 0 reports SOCK_MACRAW afterwards. */
bool eth_w5500_mac_reinit();

/* ESP8266 eth_w5500_recover(): MAC re-init, PHY power-cycle reset and a
 * gratuitous ARP so the router relearns our MAC. Blocks (busy wait, watchdog
 * fed) for up to ~0.3 s + ETH_W5500_LINK_TIMEOUT_MS because no lwIP transmit
 * may happen while the link is down. Returns true when link and socket are
 * back. No-op (false) when the active Ethernet controller is not a W5500. */

#if defined(ENABLE_DEBUG)
/** Debug builds only: hold the PHY in power-down for ms while the system keeps
 *  running (lwIP transmits into the dead link), then recover. Exercises the
 *  sendFrame() timeouts of lib/lwIP_w5500. Triggered via /cv?ethpd=<ms>. */
void eth_w5500_test_linkdown(uint32_t ms);
#endif

#define ETH_W5500_LINK_TIMEOUT_MS 3000UL

#endif // ESP8266
#endif // _ETH_W5500_H
