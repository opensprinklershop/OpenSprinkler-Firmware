/* OpenSprinkler Unified Firmware
 * MGM210P co-processor transport (ESP32-C5 host <-> Silicon Labs MGM210P/EFR32MG21)
 *
 * Experiment branch: MGM210P
 * Offloads Zigbee (EZSP NCP) / BLE from the ESP32-C5 native radios to an external
 * MGM210P module connected over UART.
 *
 * Wiring (confirmed):
 *   ESP32-C5 IO26 (TX) --> MGM210P PA02 (RX)
 *   ESP32-C5 IO25 (RX) <-- MGM210P PA01 (TX)
 *   115200 8N1, NO hardware flow control (2-wire link).
 *
 * Phase 1 scope: bring up the ASHv2 data-link and perform an EZSP "version"
 * handshake to prove the link and identify the flashed NCP image.
 *
 * 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef _MGM210P_TRANSPORT_H
#define _MGM210P_TRANSPORT_H

#if defined(ESP32C5) && defined(OS_MGM210P)

#include <stdint.h>
#include <stddef.h>

// Default wiring / link parameters (overridable via build flags).
#ifndef MGM210P_UART_TX_PIN
#define MGM210P_UART_TX_PIN 26   // ESP32-C5 TX -> MGM210P PA02
#endif
#ifndef MGM210P_UART_RX_PIN
#define MGM210P_UART_RX_PIN 25   // ESP32-C5 RX <- MGM210P PA01
#endif
#ifndef MGM210P_UART_BAUD
#define MGM210P_UART_BAUD 115200
#endif

// Optional C5 GPIO wired to the MGM210P RESETn line (module PIN27). -1 disables.
// When set, the probes pulse reset so a post-reset Gecko bootloader banner /
// XMODEM poll byte (emitted only right after reset) can actually be captured,
// and RESETn is deterministically driven HIGH so the module is not held in reset.
#ifndef MGM210P_RST_PIN
#define MGM210P_RST_PIN -1
#endif

// Result of the version handshake / last probe.
struct Mgm210pInfo {
    bool     link_up;              // ASH RSTACK received (module speaks EZSP/ASH)
    bool     ezsp_ok;              // EZSP version response received
    uint8_t  ash_version;          // from RSTACK
    uint8_t  reset_code;           // from RSTACK
    uint8_t  ezsp_protocol;        // EZSP protocol version (e.g. 8, 13)
    uint8_t  stack_type;           // EZSP stack type
    uint16_t stack_version;        // EZSP stack version
};

// Initialize the UART link to the MGM210P (idempotent).
void mgm210p_transport_begin();

// True if a C5 GPIO is configured to drive the MGM210P RESETn line.
bool mgm210p_has_reset_pin();

// Pulse the MGM210P RESETn line LOW->HIGH to restart it (into its bootloader).
// No-op if MGM210P_RST_PIN < 0. Leaves RESETn driven HIGH.
void mgm210p_reset_pulse();

// Run an ASH reset + EZSP version handshake. Blocks up to a few seconds.
// Fills `info` (may be nullptr). Returns true if the EZSP handshake succeeded.
bool mgm210p_probe(Mgm210pInfo *info);

// Diagnostic sweep used when the probe fails: passively listens and sends an ASH
// RST across several baud rates and both TX/RX orientations, dumping raw hex to
// the debug log. Helps identify the flashed image and detect swapped wiring.
void mgm210p_diagnose();

// Human-readable one-line summary of the last probe into `out`.
void mgm210p_status_line(char *out, size_t out_len);

// ---------------------------------------------------------------------------
// Phase 2: AT-command exploration API
// ---------------------------------------------------------------------------
// Some MGM210P images expose an ASCII "AT" command console (SiLabs BGX-style or
// a custom AT gateway) instead of / in addition to the binary EZSP/ASH NCP.
// These helpers probe for such an image, enumerate its identity, and let the
// host drive arbitrary AT commands (e.g. from the HTTP /mg endpoint) so every
// function of the co-processor can be exercised and a possible "Matter over AT"
// interface can be discovered.

// Longest AT reply captured per command.
#define MGM210P_AT_RESP_MAX 512

struct Mgm210pAtInfo {
    bool     at_ok;            // module answered "OK"/printable ASCII to "AT"
    bool     banner_seen;      // captured non-AT ASCII bytes (e.g. a boot banner)
    bool     matter_at;        // at least one Matter AT probe produced output
    uint32_t baud;             // baud at which AT traffic was detected
    char     identity[192];    // ATI/AT+GMR/banner identity text (truncated)
    char     matter_resp[256]; // responses to the Matter AT command battery
};

// Reconfigure the MGM210P UART to `baud` (re-begins Serial1).
void mgm210p_set_baud(uint32_t baud);

// Send a raw AT command (a CR/LF terminator is appended if missing) and collect
// the reply into `resp` (NUL-terminated, may span multiple lines) until an
// idle gap / "OK"/"ERROR" line or `timeout_ms` elapses. Returns the number of
// reply bytes captured.
int mgm210p_at_command(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

// Auto-detect an AT-capable firmware: sweeps common baud rates, probes with
// "AT", and on success queries identity + a battery of Matter AT commands.
// Fills `info` (may be nullptr). Returns true if any AT/banner traffic was seen.
bool mgm210p_at_autoprobe(Mgm210pAtInfo *info);

// Run only the Matter-related AT command battery at the current baud, replacing
// `info->matter_resp`. Returns true if any command produced output.
bool mgm210p_at_matter_test(Mgm210pAtInfo *info);

// Access the most recent AT probe result (never nullptr).
const Mgm210pAtInfo *mgm210p_at_last();

// ---------------------------------------------------------------------------
// Phase 3: Silicon Labs Gecko UART bootloader detection
// ---------------------------------------------------------------------------
// A factory MGM210P ships with only the Gecko UART XMODEM bootloader and NO
// application, so it answers neither EZSP/ASH nor AT. With no valid app it
// auto-enters the bootloader, whose "communication" plugin presents an ASCII
// menu ("Gecko Bootloader ...", "1. upload gbl / 2. run / 3. ebl info", "BL >").
// Detecting that menu confirms the module is alive and ready to be flashed
// (e.g. with an EZSP-NCP, RCP or Matter image) over XMODEM.

struct Mgm210pBootloaderInfo {
    bool     detected;      // Gecko bootloader menu/prompt recognized
    uint32_t baud;          // baud at which the menu was seen
    char     banner[256];   // captured menu/banner text
    char     info[192];     // "ebl info" (option 3) output, if queried
};

// Probe for the Gecko UART bootloader: sweeps common bauds, nudges the console
// with CR/LF, and matches the menu/prompt. On success it optionally queries
// "ebl info" (menu option '3'). Fills `info` (may be nullptr). Returns true if
// the bootloader menu/prompt was detected.
bool mgm210p_bootloader_probe(Mgm210pBootloaderInfo *info);

// Send a single Gecko bootloader menu key ('1'=upload, '2'=run, '3'=ebl info)
// at the current baud and capture the reply into `resp`. Returns bytes captured.
int mgm210p_bootloader_menu_key(char key, char *resp, size_t resp_len, uint32_t timeout_ms);

// Access the most recent bootloader probe result (never nullptr).
const Mgm210pBootloaderInfo *mgm210p_bootloader_last();

// ---------------------------------------------------------------------------
// Phase 4: active XMODEM handshake probe
// ---------------------------------------------------------------------------
// The Gecko UART bootloader is often non-interactive (no ASCII menu): once in
// upload mode it drives the XMODEM protocol by periodically sending the poll
// byte 'C' (0x43, XMODEM-CRC) or NAK (0x15, XMODEM-checksum) to request the
// first data packet. This probe tries to elicit and detect that poll byte:
// passively, then by selecting the upload menu key '1', an autobaud 'U', and a
// bare CR. Detecting the poll byte proves the bootloader is alive and ready to
// receive a .gbl/.ebl image over XMODEM.

struct Mgm210pXmodemInfo {
    bool     ready;        // XMODEM poll byte ('C' or NAK) detected
    uint32_t baud;         // baud at which it was detected
    char      mode[8];     // "CRC" (0x43) or "CHK" (0x15 NAK)
    int      poll_bytes;   // number of poll bytes captured in the window
};

// Probe for an XMODEM-ready bootloader across the baud sweep. Fills `info` (may
// be nullptr). Returns true if the XMODEM poll byte was detected.
bool mgm210p_xmodem_probe(Mgm210pXmodemInfo *info);

// Access the most recent XMODEM probe result (never nullptr).
const Mgm210pXmodemInfo *mgm210p_xmodem_last();

// ---------------------------------------------------------------------------
// Phase 5: SWD (Serial Wire Debug) probe
// ---------------------------------------------------------------------------
// The wired MGM210P pins PA01/PA02 are the EFR32 debug port (SWCLK/SWDIO), NOT
// a UART. This probe bit-bangs the ARM SWD protocol on those C5 GPIOs: line
// reset + JTAG-to-SWD switch, then reads the Debug Port IDCODE (DPIDR). A valid
// IDCODE with ACK=OK proves the debug link works and identifies the core -
// enabling firmware flashing over SWD (no bootloader needed).

// C5 GPIOs on the SWD lines. Defaults: SWCLK=UART_RX pin, SWDIO=UART_TX pin.
#ifndef MGM210P_SWCLK_PIN
#define MGM210P_SWCLK_PIN MGM210P_UART_RX_PIN   // IO26 <-> MGM PA01 (SWCLK)
#endif
#ifndef MGM210P_SWDIO_PIN
#define MGM210P_SWDIO_PIN MGM210P_UART_TX_PIN   // IO25 <-> MGM PA02 (SWDIO)
#endif

struct Mgm210pSwdInfo {
    bool     ok;        // valid IDCODE read with ACK=OK
    uint32_t idcode;    // DP IDCODE / DPIDR
    uint8_t  ack;       // SWD ACK (1=OK, 2=WAIT, 4=FAULT)
    int      swclk;     // C5 GPIO used as SWCLK
    int      swdio;     // C5 GPIO used as SWDIO
    uint32_t dpidr;     // (alias of idcode for clarity)
    bool     mem_ok;    // AHB-AP memory access working (debug powered up)
    uint32_t ap_idr;    // AHB-AP IDR
    uint32_t cpuid;     // SCB CPUID @0xE000ED00 (identifies the core)
    uint32_t devinfo;   // EFR32 DEVINFO word (part/rev), 0 if not read
    const char *chip;   // active chip profile name (EFR32MG21 / EFR32MG26)
};

// Bit-bang an SWD line reset + JTAG-to-SWD switch and read the DP IDCODE.
// Tries both SWCLK/SWDIO orientations. On success it also powers up the debug
// domain and reads the AHB-AP IDR + CPUID + DEVINFO via memory access. Fills
// `info` (may be nullptr). Returns true if a plausible IDCODE was read.
// Releases + restores the UART link.
bool mgm210p_swd_probe(Mgm210pSwdInfo *info);

// Access the most recent SWD probe result (never nullptr).
const Mgm210pSwdInfo *mgm210p_swd_last();

// Select the target chip profile at runtime ("mg21"/"mg210" or "mg26"/"mg260").
// One binary supports both MGM210P (EFR32MG21) and MGM260P (EFR32MG26).
void mgm210p_swd_set_chip(const char *which);

// Name of the active chip profile.
const char *mgm210p_swd_chip_name();

// Read `n` 32-bit words from MGM210P memory over SWD into `out` (n>=1).
// Re-establishes the SWD link (releases + restores the UART). Returns true on
// success. Use for DEVINFO/flash inspection and, later, the SWD mailbox.
bool mgm210p_swd_read_mem(uint32_t addr, uint32_t *out, int n);

// Result of the MSC flash write self-test.
struct Mgm210pFlashTest {
    bool     link;         // SWD link + debug power-up OK
    bool     halted;       // Cortex-M33 core halted
    uint32_t ipversion;    // MSC_IPVERSION (confirms the MSC base address; ~6)
    bool     erased;       // page erase reported not-busy
    bool     ok;           // written pattern read back correctly
    uint32_t addr;         // flash page address used
    uint32_t rd[4];        // words read back after write
};

// Prove the SWD flash-write path: halt the core, unlock the MSC, erase one
// flash page at `addr` (must be 8 KB-aligned and unused), write a known test
// pattern and read it back to verify. Safe/reversible on a blank device.
// Fills `out` (may be nullptr). Returns true if the readback matches.
bool mgm210p_swd_flash_test(uint32_t addr, Mgm210pFlashTest *out);

// ---------------------------------------------------------------------------
// SWD flash-programming session (stream a firmware image in chunks)
// ---------------------------------------------------------------------------
// EFR32MG21 main flash: base 0x00000000, 1 MB, 8 KB page. Usage:
//   begin() -> write(addr,chunk,len)* (ascending, page auto-erased) -> end(run)
// The SWD link + MSC unlock are held across write() calls; the UART is only
// released on begin() and restored on end().
#ifndef MGM210P_FLASH_PAGE
#define MGM210P_FLASH_PAGE 8192u
#endif

// Start a flash session: release UART, connect SWD, halt core, unlock MSC,
// enable write/erase. Returns true on success.
bool mgm210p_swd_flash_begin();

// Write `len` bytes to flash at `addr` (word-granular; a trailing partial word
// is padded with 0xFF). Pages are erased automatically the first time they are
// entered (ascending addresses assumed). If `verify`, each word is read back.
// Returns true on success. Call only between begin() and end().
bool mgm210p_swd_flash_write(uint32_t addr, const uint8_t *data, uint32_t len, bool verify);

// End the flash session: disable write/erase, optionally reset+run the target,
// and restore the UART link.
void mgm210p_swd_flash_end(bool run);

// True while a flash session is active.
bool mgm210p_swd_flash_active();

#endif // ESP32C5 && OS_MGM210P
#endif // _MGM210P_TRANSPORT_H
