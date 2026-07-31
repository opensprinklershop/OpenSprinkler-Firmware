/* OpenSprinkler Unified Firmware
 * MGM210P co-processor transport (ASHv2 data-link + EZSP version handshake).
 * See mgm210p_transport.h for wiring and scope.
 *
 * 2026 @ OpenSprinklerShop
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mgm210p_transport.h"

#if defined(ESP32C5) && defined(OS_MGM210P)

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>
#include "defines.h"

// ---------------------------------------------------------------------------
// ASHv2 constants (UART Gateway Protocol, EmberZNet)
// ---------------------------------------------------------------------------
static const uint8_t ASH_FLAG   = 0x7E; // frame delimiter
static const uint8_t ASH_ESCAPE = 0x7D; // byte-stuffing escape
static const uint8_t ASH_XON    = 0x11; // sw flow control (ignored on RX)
static const uint8_t ASH_XOFF   = 0x13;
static const uint8_t ASH_SUBST  = 0x18; // substitute: frame in error until next flag
static const uint8_t ASH_CANCEL = 0x1A; // cancel: discard partial frame
static const uint8_t ASH_STUFF_MASK = 0x20;

// Control-byte encodings
static const uint8_t ASH_CTRL_RST    = 0xC0;
static const uint8_t ASH_CTRL_RSTACK = 0xC1;
static const uint8_t ASH_CTRL_ERROR  = 0xC2;

// Serial1 dedicated to the MGM210P link.
static HardwareSerial &g_uart = Serial1;
static bool g_begun = false;
static uint8_t g_tx_frmnum = 0;     // next DATA frame number to send
static uint8_t g_rx_ack = 0;        // ackNum to send back (next expected frmNum)
static Mgm210pInfo g_last = {};

// ---------------------------------------------------------------------------
// ASH helpers
// ---------------------------------------------------------------------------
static bool ash_is_reserved(uint8_t b) {
    return b == ASH_FLAG || b == ASH_ESCAPE || b == ASH_XON ||
           b == ASH_XOFF || b == ASH_SUBST || b == ASH_CANCEL;
}

static uint16_t ash_crc_update(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (uint8_t i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

// LFSR data-randomization mask sequence (seed 0x42). Advances `state`.
static uint8_t ash_rand_next(uint8_t &state) {
    uint8_t mask = state;
    state = (state & 0x01) ? (uint8_t)((state >> 1) ^ 0xB8) : (uint8_t)(state >> 1);
    return mask;
}

// Byte-stuff and emit a single raw byte on the wire.
static void ash_emit(uint8_t b) {
    if (ash_is_reserved(b)) {
        g_uart.write(ASH_ESCAPE);
        g_uart.write((uint8_t)(b ^ ASH_STUFF_MASK));
    } else {
        g_uart.write(b);
    }
}

// Send a full ASH frame: control byte + (optional) randomized data + CRC + flag.
// `randomize` applies only to DATA frames.
static void ash_send_frame(uint8_t control, const uint8_t *data, size_t len, bool randomize) {
    uint16_t crc = 0xFFFF;
    crc = ash_crc_update(crc, control);
    ash_emit(control);

    uint8_t rnd = 0x42;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (randomize) b ^= ash_rand_next(rnd);
        crc = ash_crc_update(crc, b);
        ash_emit(b);
    }
    ash_emit((uint8_t)(crc >> 8));
    ash_emit((uint8_t)(crc & 0xFF));
    g_uart.write(ASH_FLAG);
    g_uart.flush();
}

static void ash_send_rst() {
    g_uart.write(ASH_CANCEL); // flush any partial NCP state
    ash_send_frame(ASH_CTRL_RST, nullptr, 0, false);
}

static void ash_send_ack(uint8_t ack_num) {
    ash_send_frame((uint8_t)(0x80 | (ack_num & 0x07)), nullptr, 0, false);
}

// Send an EZSP payload inside an ASH DATA frame (frmNum/ackNum bookkeeping).
static void ash_send_data(const uint8_t *ezsp, size_t len) {
    uint8_t control = (uint8_t)((g_tx_frmnum & 0x07) << 4) | (g_rx_ack & 0x07);
    ash_send_frame(control, ezsp, len, true);
    g_tx_frmnum = (g_tx_frmnum + 1) & 0x07;
}

// Receive one complete ASH frame into `frame` (control+data+crc, de-stuffed).
// Returns frame length (>=3) on success, 0 on timeout. Verifies CRC.
static size_t ash_recv_frame(uint8_t *frame, size_t cap, uint32_t timeout_ms) {
    uint32_t start = millis();
    size_t n = 0;
    bool escaped = false;
    bool in_error = false;
    while ((millis() - start) < timeout_ms) {
        int c = g_uart.read();
        if (c < 0) { delay(1); continue; }
        uint8_t b = (uint8_t)c;

        if (b == ASH_CANCEL) { n = 0; escaped = false; in_error = false; continue; }
        if (b == ASH_XON || b == ASH_XOFF) continue;      // flow control bytes
        if (b == ASH_SUBST) { in_error = true; continue; } // frame invalid until flag
        if (b == ASH_FLAG) {
            if (in_error || n < 3) { n = 0; escaped = false; in_error = false; continue; }
            // verify CRC over frame minus trailing 2 CRC bytes
            uint16_t crc = 0xFFFF;
            for (size_t i = 0; i < n - 2; i++) crc = ash_crc_update(crc, frame[i]);
            uint16_t rx_crc = ((uint16_t)frame[n - 2] << 8) | frame[n - 1];
            size_t out = (crc == rx_crc) ? n : 0;
            n = 0; escaped = false; in_error = false;
            if (out) return out;
            continue;
        }
        if (b == ASH_ESCAPE) { escaped = true; continue; }
        if (escaped) { b ^= ASH_STUFF_MASK; escaped = false; }
        if (n < cap) frame[n++] = b;
        else in_error = true; // overflow -> drop until flag
    }
    return 0;
}

// De-randomize the DATA field of a received DATA frame in place.
static void ash_derandomize(uint8_t *data, size_t len) {
    uint8_t rnd = 0x42;
    for (size_t i = 0; i < len; i++) data[i] ^= ash_rand_next(rnd);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void mgm210p_transport_begin() {
    if (g_begun) return;
    g_uart.setRxBufferSize(512);
    g_uart.begin(MGM210P_UART_BAUD, SERIAL_8N1, MGM210P_UART_RX_PIN, MGM210P_UART_TX_PIN);
    g_begun = true;
    DEBUG_PRINTF("[MGM210P] UART begin: baud=%d RX=IO%d TX=IO%d\n",
                 (int)MGM210P_UART_BAUD, (int)MGM210P_UART_RX_PIN, (int)MGM210P_UART_TX_PIN);
}

bool mgm210p_probe(Mgm210pInfo *info) {
    mgm210p_transport_begin();
    memset(&g_last, 0, sizeof(g_last));
    g_tx_frmnum = 0;
    g_rx_ack = 0;

    uint8_t frame[256];

    // 1) Reset the NCP and wait for RSTACK (retry a few times).
    bool got_rstack = false;
    for (int attempt = 0; attempt < 3 && !got_rstack; attempt++) {
        while (g_uart.read() >= 0) {} // drain
        ash_send_rst();
        size_t n = ash_recv_frame(frame, sizeof(frame), 1000);
        while (n >= 3) {
            if (frame[0] == ASH_CTRL_RSTACK) {
                g_last.link_up   = true;
                g_last.ash_version = (n >= 3) ? frame[1] : 0;
                g_last.reset_code  = (n >= 4) ? frame[2] : 0;
                got_rstack = true;
                DEBUG_PRINTF("[MGM210P] RSTACK: ashVer=%u resetCode=0x%02X\n",
                             g_last.ash_version, g_last.reset_code);
                break;
            }
            if (frame[0] == ASH_CTRL_ERROR) {
                DEBUG_PRINTF("[MGM210P] ASH ERROR: code=0x%02X\n", (n >= 3) ? frame[1] : 0);
                break;
            }
            n = ash_recv_frame(frame, sizeof(frame), 300);
        }
        if (!got_rstack)
            DEBUG_PRINTF("[MGM210P] no RSTACK (attempt %d/3)\n", attempt + 1);
    }
    if (!got_rstack) {
        if (info) *info = g_last;
        DEBUG_PRINTLN(F("[MGM210P] link DOWN - no EZSP/ASH response on UART"));
        return false;
    }

    // 2) Send EZSP "version" command (legacy frame format).
    //    payload = [seq, frameControl=0x00, frameId=0x00(version), desiredVersion]
    const uint8_t desired = 0x0D; // request EZSP protocol v13; NCP replies with its own
    uint8_t ver_cmd[4] = { 0x00, 0x00, 0x00, desired };
    ash_send_data(ver_cmd, sizeof(ver_cmd));

    // 3) Read frames until we see the DATA response carrying the version reply.
    uint32_t deadline = millis() + 2000;
    while (millis() < deadline) {
        size_t n = ash_recv_frame(frame, sizeof(frame), 500);
        if (n < 3) continue;
        uint8_t control = frame[0];
        if ((control & 0x80) == 0) {
            // DATA frame: derandomize payload (between control and 2-byte CRC).
            size_t dlen = n - 3;
            uint8_t rxfrm = (control >> 4) & 0x07;
            g_rx_ack = (rxfrm + 1) & 0x07;
            uint8_t *payload = &frame[1];
            ash_derandomize(payload, dlen);
            ash_send_ack(g_rx_ack);
            // EZSP version response: [seq, frmCtrl, frameId(0x00), proto, stackType, verLo, verHi]
            if (dlen >= 4 && payload[2] == 0x00) {
                g_last.ezsp_ok       = true;
                g_last.ezsp_protocol = payload[3];
                g_last.stack_type    = (dlen >= 5) ? payload[4] : 0;
                g_last.stack_version = (dlen >= 7) ? (uint16_t)(payload[5] | (payload[6] << 8)) : 0;
                DEBUG_PRINTF("[MGM210P] EZSP v%u stackType=%u stackVer=0x%04X\n",
                             g_last.ezsp_protocol, g_last.stack_type, g_last.stack_version);
                if (info) *info = g_last;
                return true;
            }
        }
        // ACK/NAK/other control frames: ignore for probe.
    }

    if (info) *info = g_last;
    DEBUG_PRINTLN(F("[MGM210P] EZSP version reply not received"));
    return false;
}

// ---------------------------------------------------------------------------
// Diagnostic sweep
// ---------------------------------------------------------------------------
static void diag_hexdump(const char *tag, uint32_t baud, int rx, int tx,
                         const uint8_t *b, size_t n) {
    char hex[3 * 24 + 1];
    size_t m = n < 24 ? n : 24;
    int o = 0;
    for (size_t i = 0; i < m; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", b[i]);
    hex[o > 0 ? o : 0] = 0;
    DEBUG_PRINTF("[MGM210P-DIAG] %-13s baud=%6lu rx=IO%d tx=IO%d n=%u : %s\n",
                 tag, (unsigned long)baud, rx, tx, (unsigned)n, hex);
}

static size_t diag_listen(uint8_t *buf, size_t cap, uint32_t ms) {
    size_t n = 0;
    uint32_t t0 = millis();
    while ((millis() - t0) < ms && n < cap) {
        int c = g_uart.read();
        if (c >= 0) buf[n++] = (uint8_t)c;
        else delay(1);
    }
    return n;
}

void mgm210p_diagnose() {
    static const uint32_t bauds[] = { 115200, 230400, 460800, 57600, 38400, 9600 };
    const int RX = MGM210P_UART_RX_PIN, TX = MGM210P_UART_TX_PIN;
    uint8_t buf[96];

    DEBUG_PRINTLN(F("[MGM210P-DIAG] === UART sweep (baud x TX/RX orientation) ==="));
    for (int swap = 0; swap < 2; swap++) {
        int rx = swap ? TX : RX;   // swap==1 tries reversed wiring
        int tx = swap ? RX : TX;
        for (unsigned bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
            uint32_t baud = bauds[bi];
            g_uart.end();
            delay(20);
            g_uart.setRxBufferSize(512);
            g_uart.begin(baud, SERIAL_8N1, rx, tx);
            delay(40);
            while (g_uart.read() >= 0) {}

            // Passive: catch any unsolicited output (Spinel, banners, BGAPI).
            size_t n = diag_listen(buf, sizeof(buf), 250);
            if (n) diag_hexdump("passive", baud, rx, tx, buf, n);

            // Active: ASH RST -> expect RSTACK (0xC1) framed by 0x7E.
            while (g_uart.read() >= 0) {}
            ash_send_rst();
            n = diag_listen(buf, sizeof(buf), 400);
            bool flag = false, rstack = false;
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == ASH_FLAG) flag = true;
                if (buf[i] == ASH_CTRL_RSTACK) rstack = true;
            }
            const char *tag = rstack ? "RST->RSTACK!" : (flag ? "RST->ash?" : "RST->resp");
            diag_hexdump(tag, baud, rx, tx, buf, n);
        }
    }
    g_uart.end();
    delay(20);
    g_uart.setRxBufferSize(512);
    g_uart.begin(MGM210P_UART_BAUD, SERIAL_8N1, RX, TX);
    DEBUG_PRINTLN(F("[MGM210P-DIAG] === sweep done ==="));
}

void mgm210p_status_line(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (g_last.ezsp_ok) {
        snprintf(out, out_len,
                 "MGM210P EZSP v%u stackType=%u stackVer=0x%04X resetCode=0x%02X",
                 g_last.ezsp_protocol, g_last.stack_type, g_last.stack_version, g_last.reset_code);
    } else if (g_last.link_up) {
        snprintf(out, out_len, "MGM210P ASH up (v%u) but no EZSP reply", g_last.ash_version);
    } else {
        snprintf(out, out_len, "MGM210P link down");
    }
}

#endif // ESP32C5 && OS_MGM210P
