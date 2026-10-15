# lwIP_w5500 (patched copy)

Copy of `libraries/lwIP_w5500` from the ESP8266 Arduino core (LGPL, see the
source headers) with bounded busy loops in `utility/w5500.cpp`:

- `sendFrame()`: returns -1 immediately while the PHY reports no link, waits
  at most 20 ms for TX buffer space and 50 ms for SENDOK/TIMEOUT, then returns
  -1 (frame dropped, lwIP retransmits). The original spins forever when the PHY
  has no link, which stalls the firmware until the watchdog resets it and can
  turn into a reboot loop (DHCP right after `begin()`).
- `setSn_CR()`: bounded wait (20 ms) for the command register to clear. This
  unbounded loop was the actual Soft-WDT location (lwIP timer -> sendFrame ->
  setSn_CR(SEND) with the link down).
- `end()`: bounded wait for `SOCK_CLOSED`.
- `getSn_TX_FSR()` / `getSn_RX_RSR()`: bounded read-until-stable loops.

PlatformIO's library finder prefers `lib/` over framework libraries, so this
copy replaces the core driver for the `os3x_esp8266` environment. Re-apply the
patch (search for "OpenSprinklerShop patch") when updating the copy from a
newer core release. Runtime recovery of a hung chip lives in `eth_w5500.cpp`.
