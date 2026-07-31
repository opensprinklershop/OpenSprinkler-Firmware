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

// Run an ASH reset + EZSP version handshake. Blocks up to a few seconds.
// Fills `info` (may be nullptr). Returns true if the EZSP handshake succeeded.
bool mgm210p_probe(Mgm210pInfo *info);

// Diagnostic sweep used when the probe fails: passively listens and sends an ASH
// RST across several baud rates and both TX/RX orientations, dumping raw hex to
// the debug log. Helps identify the flashed image and detect swapped wiring.
void mgm210p_diagnose();

// Human-readable one-line summary of the last probe into `out`.
void mgm210p_status_line(char *out, size_t out_len);

#endif // ESP32C5 && OS_MGM210P
#endif // _MGM210P_TRANSPORT_H
