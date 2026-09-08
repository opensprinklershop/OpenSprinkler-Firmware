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

bool mgm210p_has_reset_pin() {
    return (MGM210P_RST_PIN) >= 0;
}

void mgm210p_reset_pulse() {
    if ((MGM210P_RST_PIN) < 0) return;
    pinMode(MGM210P_RST_PIN, OUTPUT);
    digitalWrite(MGM210P_RST_PIN, LOW);   // assert RESETn
    delay(20);
    digitalWrite(MGM210P_RST_PIN, HIGH);  // release; hold HIGH so it runs
    delay(80);                            // allow SE/bootloader to come up
    DEBUG_PRINTF("[MGM210P] RESETn pulse on IO%d\n", (int)MGM210P_RST_PIN);
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

// ---------------------------------------------------------------------------
// Phase 2: AT-command exploration
// ---------------------------------------------------------------------------
static uint32_t g_baud = MGM210P_UART_BAUD;
static Mgm210pAtInfo g_at_last = {};

void mgm210p_set_baud(uint32_t baud) {
    g_uart.end();
    delay(10);
    g_uart.setRxBufferSize(512);
    g_uart.begin(baud, SERIAL_8N1, MGM210P_UART_RX_PIN, MGM210P_UART_TX_PIN);
    g_baud = baud;
    g_begun = true;
    delay(20);
    DEBUG_PRINTF("[MGM210P-AT] UART baud=%lu RX=IO%d TX=IO%d\n",
                 (unsigned long)baud, (int)MGM210P_UART_RX_PIN, (int)MGM210P_UART_TX_PIN);
}

// Heuristic: does a captured reply look like a genuine AT/text response rather
// than binary EZSP/ASH noise? Requires the "OK" token or a mostly-printable,
// alphanumeric-bearing buffer.
static bool at_reply_looks_valid(const char *buf, int n) {
    if (n <= 0) return false;
    if (strstr(buf, "OK") || strstr(buf, "ok")) return true;
    int printable = 0, alnum = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) printable++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) alnum++;
    }
    return alnum >= 2 && printable >= (n * 3) / 4;
}

int mgm210p_at_command(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!g_begun) mgm210p_transport_begin();
    if (resp && resp_len) resp[0] = 0;

    while (g_uart.read() >= 0) {} // drain stale RX

    size_t clen = cmd ? strlen(cmd) : 0;
    if (clen) g_uart.write((const uint8_t *)cmd, clen);
    if (clen == 0 || (cmd[clen - 1] != '\r' && cmd[clen - 1] != '\n')) {
        g_uart.write('\r');
        g_uart.write('\n');
    }
    g_uart.flush();

    size_t n = 0;
    uint32_t start = millis();
    uint32_t last = start;
    while ((millis() - start) < timeout_ms) {
        int c = g_uart.read();
        if (c < 0) {
            if (n > 0 && (millis() - last) > 150) break; // idle gap => reply done
            delay(2);
            continue;
        }
        last = millis();
        if (resp && n < resp_len - 1) resp[n++] = (char)c;
        // Early-out on an "OK"/"ERROR" terminator line.
        if (resp && n >= 2) {
            if (n >= 4 && resp[n-4]=='O' && resp[n-3]=='K' &&
                (resp[n-2]=='\r' || resp[n-2]=='\n')) break;
        }
    }
    if (resp) resp[n < resp_len ? n : resp_len - 1] = 0;
    return (int)n;
}

// Append "cmd => reply" to a fixed buffer (best-effort, truncating).
static void at_append_identity(char *dst, size_t cap, const char *cmd, const char *reply) {
    if (!dst || cap == 0) return;
    size_t used = strlen(dst);
    if (used + 4 >= cap) return;
    // Collapse CR/LF in the reply to spaces for a compact one-line identity.
    char clean[96];
    size_t j = 0;
    for (size_t i = 0; reply[i] && j < sizeof(clean) - 1; i++) {
        char c = reply[i];
        clean[j++] = (c == '\r' || c == '\n') ? ' ' : c;
    }
    clean[j] = 0;
    snprintf(dst + used, cap - used, "%s%s=>%s", used ? " | " : "", cmd, clean);
}

bool mgm210p_at_matter_test(Mgm210pAtInfo *info) {
    if (!info) info = &g_at_last;
    // Best-effort battery of possible "Matter over AT" commands. Unknown images
    // simply answer ERROR / nothing; we record whatever comes back.
    static const char *cmds[] = {
        "AT+MATTER?", "AT+MATTER", "AT+MATTERSTATUS", "AT+MATTERINFO",
        "AT+COMMISSION?", "AT+PAIRING?", "AT+CHIP?", "AT+FABRIC?", "matter"
    };
    char reply[MGM210P_AT_RESP_MAX];
    info->matter_resp[0] = 0;
    info->matter_at = false;
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        int n = mgm210p_at_command(cmds[i], reply, sizeof(reply), 600);
        if (n > 0 && at_reply_looks_valid(reply, n) && !strstr(reply, "ERROR")) {
            info->matter_at = true;
            at_append_identity(info->matter_resp, sizeof(info->matter_resp), cmds[i], reply);
            DEBUG_PRINTF("[MGM210P-AT] Matter probe %s => %s\n", cmds[i], reply);
        }
    }
    return info->matter_at;
}

bool mgm210p_at_autoprobe(Mgm210pAtInfo *info) {
    static const uint32_t bauds[] = {
        115200, 921600, 460800, 230400, 57600, 38400, 19200, 9600
    };
    Mgm210pAtInfo r = {};
    char reply[MGM210P_AT_RESP_MAX];

    DEBUG_PRINTLN(F("[MGM210P-AT] === AT auto-probe (baud sweep) ==="));
    for (unsigned bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
        uint32_t baud = bauds[bi];
        mgm210p_set_baud(baud);
        while (g_uart.read() >= 0) {}

        int n = mgm210p_at_command("AT", reply, sizeof(reply), 500);
        if (n > 0 && at_reply_looks_valid(reply, n)) {
            r.at_ok = true;
            r.baud = baud;
            DEBUG_PRINTF("[MGM210P-AT] AT reply @%lu (%d bytes): %s\n",
                         (unsigned long)baud, n, reply);
            // Enumerate the module identity across common query variants.
            static const char *idcmds[] = { "ATI", "ATI13", "AT+GMR", "AT+VER", "?" };
            for (unsigned i = 0; i < sizeof(idcmds) / sizeof(idcmds[0]); i++) {
                int m = mgm210p_at_command(idcmds[i], reply, sizeof(reply), 600);
                if (m > 0 && at_reply_looks_valid(reply, m) && !strstr(reply, "ERROR"))
                    at_append_identity(r.identity, sizeof(r.identity), idcmds[i], reply);
            }
            mgm210p_at_matter_test(&r);
            break;
        } else if (n > 0) {
            // Non-AT bytes: keep the first capture as a boot-banner hint.
            if (!r.banner_seen) {
                r.banner_seen = true;
                r.baud = baud;
                strncpy(r.identity, reply, sizeof(r.identity) - 1);
                r.identity[sizeof(r.identity) - 1] = 0;
            }
            DEBUG_PRINTF("[MGM210P-AT] non-AT bytes @%lu (%d): %s\n",
                         (unsigned long)baud, n, reply);
        }
    }
    DEBUG_PRINTLN(F("[MGM210P-AT] === auto-probe done ==="));

    g_at_last = r;
    if (info) *info = r;
    // Leave the UART at the detected baud so follow-up HTTP AT commands work.
    mgm210p_set_baud(r.at_ok ? r.baud : (uint32_t)MGM210P_UART_BAUD);
    return r.at_ok || r.banner_seen;
}

const Mgm210pAtInfo *mgm210p_at_last() {
    return &g_at_last;
}

// ---------------------------------------------------------------------------
// Phase 3: Gecko UART bootloader detection
// ---------------------------------------------------------------------------
static Mgm210pBootloaderInfo g_btl_last = {};

// Does the captured text look like the Gecko/Ember bootloader menu or prompt?
static bool btl_banner_matches(const char *s) {
    return strstr(s, "Gecko Bootloader") || strstr(s, "Bootloader") ||
           strstr(s, "BL >") || strstr(s, "upload gbl") ||
           strstr(s, "ebl info") || strstr(s, "1. upload");
}

// Read UART for up to timeout_ms (with a short idle gap) into buf.
static int btl_collect(char *buf, size_t cap, uint32_t timeout_ms) {
    size_t n = 0;
    uint32_t start = millis();
    uint32_t last = start;
    while ((millis() - start) < timeout_ms) {
        int c = g_uart.read();
        if (c < 0) {
            if (n > 0 && (millis() - last) > 200) break;
            delay(2);
            continue;
        }
        last = millis();
        if (n < cap - 1) buf[n++] = (char)c;
    }
    buf[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

int mgm210p_bootloader_menu_key(char key, char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!g_begun) mgm210p_transport_begin();
    while (g_uart.read() >= 0) {}         // drain
    g_uart.write((uint8_t)key);           // menu reads a single keypress
    g_uart.flush();
    char tmp[MGM210P_AT_RESP_MAX];
    int n = btl_collect(tmp, sizeof(tmp), timeout_ms);
    if (resp && resp_len) {
        strncpy(resp, tmp, resp_len - 1);
        resp[resp_len - 1] = 0;
    }
    return n;
}

bool mgm210p_bootloader_probe(Mgm210pBootloaderInfo *info) {
    static const uint32_t bauds[] = { 115200, 57600, 38400, 921600, 230400, 9600 };
    Mgm210pBootloaderInfo r = {};
    char buf[MGM210P_AT_RESP_MAX];

    DEBUG_PRINTLN(F("[MGM210P-BTL] === Gecko bootloader probe (baud sweep) ==="));
    for (unsigned bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
        uint32_t baud = bauds[bi];
        mgm210p_set_baud(baud);
        while (g_uart.read() >= 0) {}
        mgm210p_reset_pulse();  // restart module so it re-emits its boot banner
        while (g_uart.read() >= 0) {}

        // Passive: some bootloaders echo the menu shortly after link-up.
        int n = btl_collect(buf, sizeof(buf), 300);
        // Active nudges: a bare CR, then LF, to elicit the "BL >" menu/prompt.
        if (!btl_banner_matches(buf)) {
            g_uart.write('\r'); g_uart.write('\n'); g_uart.flush();
            n = btl_collect(buf, sizeof(buf), 400);
        }
        if (n > 0)
            DEBUG_PRINTF("[MGM210P-BTL] @%lu (%d bytes): %s\n",
                         (unsigned long)baud, n, buf);

        if (btl_banner_matches(buf)) {
            r.detected = true;
            r.baud = baud;
            strncpy(r.banner, buf, sizeof(r.banner) - 1);
            r.banner[sizeof(r.banner) - 1] = 0;
            // Query "ebl info" (menu option '3') for bootloader/app details.
            int m = mgm210p_bootloader_menu_key('3', buf, sizeof(buf), 600);
            if (m > 0) {
                strncpy(r.info, buf, sizeof(r.info) - 1);
                r.info[sizeof(r.info) - 1] = 0;
            }
            DEBUG_PRINTF("[MGM210P-BTL] bootloader DETECTED @%lu baud\n",
                         (unsigned long)baud);
            break;
        }
    }
    DEBUG_PRINTLN(F("[MGM210P-BTL] === probe done ==="));

    g_btl_last = r;
    if (info) *info = r;
    mgm210p_set_baud(r.detected ? r.baud : (uint32_t)MGM210P_UART_BAUD);
    return r.detected;
}

const Mgm210pBootloaderInfo *mgm210p_bootloader_last() {
    return &g_btl_last;
}

// ---------------------------------------------------------------------------
// Phase 4: active XMODEM handshake probe
// ---------------------------------------------------------------------------
static const uint8_t XM_NAK = 0x15, XM_CAN = 0x18, XM_C = 0x43; // NAK / CAN / 'C'
static Mgm210pXmodemInfo g_xm_last = {};

// Listen up to timeout_ms for XMODEM poll bytes; count 'C' and NAK occurrences.
static void xm_count_poll(uint32_t timeout_ms, int &c_count, int &nak_count) {
    uint32_t t0 = millis();
    while ((millis() - t0) < timeout_ms) {
        int ch = g_uart.read();
        if (ch < 0) { delay(2); continue; }
        if ((uint8_t)ch == XM_C)   c_count++;
        if ((uint8_t)ch == XM_NAK) nak_count++;
    }
}

// Gecko bootloader autobaud: it locks its UART baud by measuring the 'U' (0x55)
// character, then starts polling with 'C'. Flood 'U' while watching for a reply.
static void xm_autobaud_flood(uint32_t window_ms, int &c_count, int &nak_count) {
    uint32_t t0 = millis();
    uint32_t last_tx = 0;
    while ((millis() - t0) < window_ms) {
        if (millis() - last_tx >= 15) {   // keep a steady 'U' stream
            g_uart.write((uint8_t)0x55);
            last_tx = millis();
        }
        int ch = g_uart.read();
        if (ch >= 0) {
            if ((uint8_t)ch == XM_C)   c_count++;
            if ((uint8_t)ch == XM_NAK) nak_count++;
        } else {
            delay(1);
        }
    }
}

bool mgm210p_xmodem_probe(Mgm210pXmodemInfo *info) {
    static const uint32_t bauds[] = { 115200, 57600, 38400, 921600, 230400, 9600 };
    Mgm210pXmodemInfo r = {};

    DEBUG_PRINTLN(F("[MGM210P-XM] === XMODEM handshake probe (baud sweep) ==="));
    for (unsigned bi = 0; bi < sizeof(bauds) / sizeof(bauds[0]); bi++) {
        uint32_t baud = bauds[bi];
        mgm210p_set_baud(baud);
        while (g_uart.read() >= 0) {}
        mgm210p_reset_pulse();  // restart module so the bootloader re-polls
        while (g_uart.read() >= 0) {}

        int c = 0, nak = 0;
        // 1) Autobaud: flood 'U' right after reset so the bootloader locks baud
        //    and starts polling with 'C'. This is the primary Gecko handshake.
        xm_autobaud_flood(1500, c, nak);
        // 2) Passive: bootloader may already poll in upload mode.
        if (!c && !nak) xm_count_poll(1000, c, nak);
        // 3) Nudge: select the "upload gbl" menu key '1'.
        if (!c && !nak) {
            g_uart.write('1'); g_uart.flush();
            xm_count_poll(1200, c, nak);
        }
        // 4) Bare CR (some menus need a keypress to appear) then upload select.
        if (!c && !nak) {
            g_uart.write('\r'); g_uart.write('\n'); g_uart.write('1'); g_uart.flush();
            xm_count_poll(1000, c, nak);
        }

        if (c || nak) {
            r.ready = true;
            r.baud = baud;
            r.poll_bytes = c + nak;
            strncpy(r.mode, c >= nak ? "CRC" : "CHK", sizeof(r.mode) - 1);
            r.mode[sizeof(r.mode) - 1] = 0;
            DEBUG_PRINTF("[MGM210P-XM] XMODEM %s poll @%lu baud (C=%d NAK=%d) - "
                         "bootloader ready to receive\n",
                         r.mode, (unsigned long)baud, c, nak);
            // Cancel any started transfer so we leave the bootloader idle.
            for (int i = 0; i < 4; i++) g_uart.write(XM_CAN);
            g_uart.write(0x08); g_uart.flush();
            break;
        }
    }
    DEBUG_PRINTLN(F("[MGM210P-XM] === probe done ==="));

    g_xm_last = r;
    if (info) *info = r;
    mgm210p_set_baud(r.ready ? r.baud : (uint32_t)MGM210P_UART_BAUD);
    return r.ready;
}

const Mgm210pXmodemInfo *mgm210p_xmodem_last() {
    return &g_xm_last;
}

// ---------------------------------------------------------------------------
// Phase 5: SWD (Serial Wire Debug) bit-bang probe
// ---------------------------------------------------------------------------
// Chip profile: per-part flash/MSC/DEVINFO parameters, selectable at RUNTIME so
// a single binary supports both MGM210P (EFR32MG21) and MGM260P (EFR32MG26).
struct MgmChipProfile {
    const char *name;
    uint32_t    msc_base;      // MSC peripheral base
    uint32_t    devinfo_addr;  // DEVINFO base
    uint32_t    flash_base;    // main flash base
    uint32_t    flash_page;    // flash page size (bytes)
};
// EFR32MG21 (MGM210P) - verified on hardware.
static const MgmChipProfile MGM_PROFILE_MG21 = {
    "EFR32MG21", 0x40030000u, 0x0FE08000u, 0x00000000u, 8192u
};
// EFR32MG26 (MGM260PD32VNA2) - 3200 KB flash, 8 KB page; TrustZone: MSC_NS base
// 0x50030000 (secure alias 0x40030000). swd_pick_msc_base() auto-selects the
// reachable alias at runtime. DEVINFO base to confirm on HW.
static const MgmChipProfile MGM_PROFILE_MG26 = {
    "EFR32MG26", 0x50030000u, 0x0FE08000u, 0x00000000u, 8192u
};
static const MgmChipProfile *g_chip = &MGM_PROFILE_MG21;
// Effective MSC base actually used (may be the sibling TrustZone alias).
static uint32_t g_msc_base = 0x40030000u;

static Mgm210pSwdInfo g_swd_last = {};
static int g_swclk = MGM210P_SWCLK_PIN;
static int g_swdio = MGM210P_SWDIO_PIN;

static inline void swd_delay() { delayMicroseconds(2); }          // ~100-200 kHz
static inline void swd_clk(int v) { digitalWrite(g_swclk, v); }
static inline void swd_dio_drive(int v) { digitalWrite(g_swdio, v); }
static inline void swd_dio_mode(int out) { pinMode(g_swdio, out ? OUTPUT : INPUT_PULLUP); }
static inline int  swd_dio_read() { return digitalRead(g_swdio); }

// Host drives SWDIO on the falling edge; target samples on the rising edge.
static void swd_wr_bit(int b) {
    swd_dio_drive(b); swd_clk(0); swd_delay(); swd_clk(1); swd_delay();
}
static int swd_rd_bit() {
    swd_clk(0); swd_delay(); int b = swd_dio_read(); swd_clk(1); swd_delay(); return b;
}
static void swd_wr_bits(uint32_t val, int n) {
    for (int i = 0; i < n; i++) { swd_wr_bit(val & 1); val >>= 1; }
}

static void swd_line_reset() {
    swd_dio_mode(1); swd_dio_drive(1);
    for (int i = 0; i < 56; i++) { swd_clk(0); swd_delay(); swd_clk(1); swd_delay(); }
}

// Read a DP/AP register with WAIT-retry. On ACK!=OK there is NO data phase
// (only a turnaround), so we must not clock 32 data bits then - otherwise the
// line protocol desyncs. Returns the final 3-bit ACK; on OK, *data holds the word.
static uint8_t swd_read_reg(bool APnDP, uint8_t a23, uint32_t *data) {
    uint8_t par = (uint8_t)((APnDP ? 1 : 0) ^ 1 /*RnW*/ ^ (a23 & 1) ^ ((a23 >> 1) & 1));
    for (int attempt = 0; attempt < 60; attempt++) {
        swd_dio_mode(1);
        swd_wr_bit(1);              // start
        swd_wr_bit(APnDP ? 1 : 0);
        swd_wr_bit(1);             // RnW = read
        swd_wr_bit(a23 & 1);       // A[2]
        swd_wr_bit((a23 >> 1) & 1);// A[3]
        swd_wr_bit(par & 1);
        swd_wr_bit(0);             // stop
        swd_wr_bit(1);             // park
        swd_dio_mode(0);           // release for turnaround + ACK
        swd_rd_bit();              // turnaround
        uint8_t ack = 0;
        for (int i = 0; i < 3; i++) ack |= (uint8_t)(swd_rd_bit() << i);
        if (ack == 0x1) {          // OK: read 32 data bits + parity
            uint32_t val = 0;
            for (int i = 0; i < 32; i++) val |= ((uint32_t)swd_rd_bit() << i);
            swd_rd_bit();          // data parity
            swd_dio_mode(1); swd_wr_bit(0); // turnaround back to host
            if (data) *data = val;
            return ack;
        }
        // WAIT/FAULT: single turnaround back to host, no data phase.
        swd_dio_mode(1); swd_wr_bit(0);
        if (ack == 0x2) continue;  // WAIT -> retry
        return ack;                // FAULT/other
    }
    return 0x2;
}

// Write a DP/AP register with WAIT-retry. Returns the final 3-bit ACK.
static uint8_t swd_write_reg(bool APnDP, uint8_t a23, uint32_t data) {
    uint8_t par = (uint8_t)((APnDP ? 1 : 0) ^ 0 /*RnW*/ ^ (a23 & 1) ^ ((a23 >> 1) & 1));
    for (int attempt = 0; attempt < 60; attempt++) {
        swd_dio_mode(1);
        swd_wr_bit(1);              // start
        swd_wr_bit(APnDP ? 1 : 0);
        swd_wr_bit(0);             // RnW = write
        swd_wr_bit(a23 & 1);       // A[2]
        swd_wr_bit((a23 >> 1) & 1);// A[3]
        swd_wr_bit(par & 1);
        swd_wr_bit(0);             // stop
        swd_wr_bit(1);             // park
        swd_dio_mode(0);           // turnaround (host releases for ACK)
        swd_rd_bit();              // trn
        uint8_t ack = 0;
        for (int i = 0; i < 3; i++) ack |= (uint8_t)(swd_rd_bit() << i);
        swd_dio_mode(1); swd_wr_bit(0); // trn back to host
        if (ack == 0x1) {          // OK: drive 32 data bits + parity
            uint32_t v = data; uint8_t p = 0;
            for (int i = 0; i < 32; i++) { int b = v & 1; swd_wr_bit(b); p ^= (uint8_t)b; v >>= 1; }
            swd_wr_bit(p & 1);
            return ack;
        }
        if (ack == 0x2) continue;  // WAIT -> retry
        return ack;                // FAULT/other
    }
    return 0x2;
}

// Power up the debug + system domains (required before AP/memory access).
static bool swd_dp_powerup() {
    swd_write_reg(false, 0x0, 0x0000001E);      // ABORT: clear sticky errors
    swd_write_reg(false, 0x1, 0x50000000);      // CTRL/STAT: CSYSPWRUPREQ|CDBGPWRUPREQ
    for (int i = 0; i < 50; i++) {
        uint32_t s = 0;
        swd_read_reg(false, 0x1, &s);
        if ((s & 0xA0000000) == 0xA0000000) return true; // CSYSPWRUPACK|CDBGPWRUPACK
        delay(1);
    }
    return false;
}

// Read the AHB-AP IDR (bank 0xF, reg 0xFC).
static bool swd_ap_idr(uint32_t *out) {
    swd_write_reg(false, 0x2, 0x000000F0);      // DP SELECT: AP0, APBANKSEL=0xF
    uint32_t d = 0;
    swd_read_reg(true, 0x3, &d);                // AP IDR (posted)
    uint8_t ack = swd_read_reg(false, 0x3, out);// DP RDBUFF
    swd_write_reg(false, 0x2, 0x00000000);      // restore bank 0
    return ack == 0x1;
}

// Read a 32-bit word from target memory via the AHB-AP.
static bool swd_mem_read32(uint32_t addr, uint32_t *out) {
    swd_write_reg(false, 0x2, 0x00000000);      // DP SELECT: AP0, bank 0 (CSW/TAR/DRW)
    swd_write_reg(true,  0x0, 0x23000052);      // CSW: 32-bit, addr auto-increment
    swd_write_reg(true,  0x1, addr);            // TAR
    uint32_t d = 0;
    swd_read_reg(true,  0x3, &d);               // DRW read (posted)
    uint8_t ack = swd_read_reg(false, 0x3, out);// DP RDBUFF
    return ack == 0x1;
}

// One orientation attempt: line reset + JTAG->SWD + line reset, read DPIDR.
static bool swd_try_read_idcode(uint32_t *idcode, uint8_t *ack_out) {
    pinMode(g_swclk, OUTPUT); swd_clk(1);
    swd_dio_mode(1); swd_dio_drive(1);
    swd_line_reset();
    swd_dio_mode(1); swd_wr_bits(0xE79E, 16); // JTAG-to-SWD switch sequence
    swd_line_reset();
    swd_dio_mode(1); swd_wr_bits(0, 8);       // >=2 idle clocks
    uint32_t id = 0;
    uint8_t ack = swd_read_reg(false, 0x0, &id); // DP reg 0 = DPIDR/IDCODE
    if (ack_out) *ack_out = ack;
    if (idcode) *idcode = id;
    return ack == 0x1 && id != 0 && id != 0xFFFFFFFF;
}

bool mgm210p_swd_probe(Mgm210pSwdInfo *info) {
    // Release the UART so the pins can be bit-banged as SWD.
    g_uart.end();
    delay(10);
    g_begun = false;

    Mgm210pSwdInfo r = {};
    r.chip = g_chip->name;
    DEBUG_PRINTLN(F("[MGM210P-SWD] === SWD IDCODE probe ==="));
    mgm210p_reset_pulse(); // bring the target up cleanly (DAP stays alive)

    for (int swap = 0; swap < 2 && !r.ok; swap++) {
        g_swclk = swap ? MGM210P_SWDIO_PIN : MGM210P_SWCLK_PIN;
        g_swdio = swap ? MGM210P_SWCLK_PIN : MGM210P_SWDIO_PIN;
        uint32_t id = 0; uint8_t ack = 0;
        bool ok = swd_try_read_idcode(&id, &ack);
        DEBUG_PRINTF("[MGM210P-SWD] SWCLK=IO%d SWDIO=IO%d ack=%u idcode=0x%08X\n",
                     g_swclk, g_swdio, ack, (unsigned)id);
        if (ok) {
            r.ok = true; r.idcode = id; r.dpidr = id; r.ack = ack;
            r.swclk = g_swclk; r.swdio = g_swdio;
            // Power up the debug domain and exercise memory access.
            if (swd_dp_powerup()) {
                swd_ap_idr(&r.ap_idr);
                r.mem_ok = swd_mem_read32(0xE000ED00, &r.cpuid); // SCB CPUID
                swd_mem_read32(g_chip->devinfo_addr, &r.devinfo);// EFR32 DEVINFO base
                DEBUG_PRINTF("[MGM210P-SWD] AP_IDR=0x%08X CPUID=0x%08X DEVINFO=0x%08X mem=%s\n",
                             (unsigned)r.ap_idr, (unsigned)r.cpuid, (unsigned)r.devinfo,
                             r.mem_ok ? "OK" : "FAIL");
            } else {
                DEBUG_PRINTLN(F("[MGM210P-SWD] debug power-up FAILED"));
            }
        }
    }
    if (r.ok) {
        DEBUG_PRINTF("[MGM210P-SWD] LINK UP - DPIDR=0x%08X (SWCLK=IO%d SWDIO=IO%d)\n",
                     (unsigned)r.idcode, r.swclk, r.swdio);
    } else {
        DEBUG_PRINTLN(F("[MGM210P-SWD] no valid IDCODE (link down / wrong pins)"));
    }
    DEBUG_PRINTLN(F("[MGM210P-SWD] === probe done ==="));

    g_swd_last = r;
    if (info) *info = r;
    // Restore the UART link for the other probes.
    mgm210p_set_baud(MGM210P_UART_BAUD);
    return r.ok;
}

const Mgm210pSwdInfo *mgm210p_swd_last() {
    return &g_swd_last;
}

void mgm210p_swd_set_chip(const char *which) {
    if (which && (strstr(which, "26") || strstr(which, "MG26") || strstr(which, "mg26")))
        g_chip = &MGM_PROFILE_MG26;
    else
        g_chip = &MGM_PROFILE_MG21;
    g_msc_base = g_chip->msc_base;
    DEBUG_PRINTF("[MGM210P-SWD] chip profile = %s (msc_base=0x%08X)\n",
                 g_chip->name, (unsigned)g_chip->msc_base);
}

const char *mgm210p_swd_chip_name() {
    return g_chip->name;
}

// Establish the SWD link on the correct orientation and power up the debug
// domain. Assumes the UART is already released. Returns true on success.
static bool swd_connect() {
    mgm210p_reset_pulse();
    for (int swap = 0; swap < 2; swap++) {
        g_swclk = swap ? MGM210P_SWDIO_PIN : MGM210P_SWCLK_PIN;
        g_swdio = swap ? MGM210P_SWCLK_PIN : MGM210P_SWDIO_PIN;
        uint32_t id = 0; uint8_t ack = 0;
        if (swd_try_read_idcode(&id, &ack) && swd_dp_powerup())
            return true;
    }
    return false;
}

bool mgm210p_swd_read_mem(uint32_t addr, uint32_t *out, int n) {
    if (!out || n <= 0) return false;
    g_uart.end();
    delay(10);
    g_begun = false;

    bool ok = false;
    if (swd_connect()) {
        ok = true;
        for (int i = 0; i < n; i++) {
            if (!swd_mem_read32(addr + 4u * (uint32_t)i, &out[i])) { ok = false; break; }
        }
    }
    mgm210p_set_baud(MGM210P_UART_BAUD);
    return ok;
}

// ---------------------------------------------------------------------------
// MSC flash programming (Series-2 register map). Register base = g_msc_base, the
// runtime-selected alias (see swd_pick_msc_base) so MG21 (0x40030000) and MG26
// (0x50030000 NS / 0x40030000 S) both work.
// ---------------------------------------------------------------------------
#define MSC_IPVERSION_REG  (g_msc_base + 0x000u)
#define MSC_WRITECTRL_REG  (g_msc_base + 0x00Cu)
#define MSC_WRITECMD_REG   (g_msc_base + 0x010u)
#define MSC_ADDRB_REG      (g_msc_base + 0x014u)
#define MSC_WDATA_REG      (g_msc_base + 0x018u)
#define MSC_STATUS_REG     (g_msc_base + 0x01Cu)
#define MSC_LOCK_REG       (g_msc_base + 0x03Cu)
#define MSC_WREN_BIT       0x1u
#define MSC_ERASEPAGE_BIT  0x2u
#define MSC_WRITEEND_BIT   0x4u
#define MSC_STAT_BUSY      0x1u
#define MSC_STAT_WDATARDY  0x8u
#define MSC_UNLOCK_KEY     0x1B71u
#define CM_DHCSR           0xE000EDF0u
#define CM_DHCSR_HALT      0xA05F0003u   // DBGKEY|C_HALT|C_DEBUGEN
#define CM_DHCSR_S_HALT    0x00020000u   // read: core halted

// Write one 32-bit word to target memory via the AHB-AP.
static bool swd_mem_write32(uint32_t addr, uint32_t val) {
    swd_write_reg(false, 0x2, 0x00000000);      // DP SELECT: AP0, bank 0
    swd_write_reg(true,  0x0, 0x23000052);      // CSW: 32-bit, auto-increment
    swd_write_reg(true,  0x1, addr);            // TAR
    return swd_write_reg(true, 0x3, val) == 0x1;// DRW write
}

static bool swd_halt_core() {
    swd_mem_write32(CM_DHCSR, CM_DHCSR_HALT);
    uint32_t v = 0;
    for (int i = 0; i < 30; i++) {
        if (swd_mem_read32(CM_DHCSR, &v) && (v & CM_DHCSR_S_HALT)) return true;
        delay(1);
    }
    return false;
}

static bool msc_wait_notbusy(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while ((millis() - t0) < timeout_ms) {
        uint32_t s = 0;
        if (swd_mem_read32(MSC_STATUS_REG, &s) && !(s & MSC_STAT_BUSY)) return true;
    }
    return false;
}

// Select the reachable MSC alias for the active chip. MG26 (TrustZone) exposes
// MSC at 0x50030000 (NS) / 0x40030000 (S); MG21 only at 0x40030000. Try the
// profile base and its bit-28 sibling; keep whichever STATUS reads plausibly.
static void swd_pick_msc_base() {
    uint32_t cands[2] = { g_chip->msc_base, g_chip->msc_base ^ 0x10000000u };
    for (int i = 0; i < 2; i++) {
        swd_write_reg(false, 0x0, 0x0000001Eu); // clear sticky errors (ABORT)
        uint32_t s = 0;
        if (swd_mem_read32(cands[i] + 0x01Cu, &s) && s != 0xFFFFFFFFu) {
            g_msc_base = cands[i];
            DEBUG_PRINTF("[MGM210P-SWD] MSC base 0x%08X (STATUS=0x%08X)\n",
                         (unsigned)g_msc_base, (unsigned)s);
            return;
        }
    }
    g_msc_base = g_chip->msc_base;
}

bool mgm210p_swd_flash_test(uint32_t addr, Mgm210pFlashTest *out) {
    static const uint32_t pat[4] = { 0xDEADBEEF, 0x12345678, 0xCAFEBABE, 0xA5A5A5A5 };
    Mgm210pFlashTest r = {};
    r.addr = addr;

    g_uart.end();
    delay(10);
    g_begun = false;

    if (swd_connect()) {
        r.link = true;
        r.halted = swd_halt_core();
        swd_pick_msc_base();                             // handle MG26 TZ alias
        swd_mem_read32(MSC_IPVERSION_REG, &r.ipversion); // confirm MSC base
        if (r.halted) {
            swd_mem_write32(MSC_LOCK_REG, MSC_UNLOCK_KEY);
            swd_mem_write32(MSC_WRITECTRL_REG, MSC_WREN_BIT);
            // Erase the target page.
            swd_mem_write32(MSC_ADDRB_REG, addr);
            swd_mem_write32(MSC_WRITECMD_REG, MSC_ERASEPAGE_BIT);
            r.erased = msc_wait_notbusy(1000);
            // Write the 4-word test pattern (WDATA auto-increments from ADDRB).
            swd_mem_write32(MSC_ADDRB_REG, addr);
            for (int i = 0; i < 4; i++) {
                uint32_t s = 0, t0 = millis();
                while ((millis() - t0) < 200) {
                    if (swd_mem_read32(MSC_STATUS_REG, &s) && (s & MSC_STAT_WDATARDY)) break;
                }
                swd_mem_write32(MSC_WDATA_REG, pat[i]);
            }
            swd_mem_write32(MSC_WRITECMD_REG, MSC_WRITEEND_BIT);
            msc_wait_notbusy(1000);
            swd_mem_write32(MSC_WRITECTRL_REG, 0); // WREN off
            // Verify by reading the words back.
            r.ok = true;
            for (int i = 0; i < 4; i++) {
                uint32_t v = 0;
                swd_mem_read32(addr + 4u * (uint32_t)i, &v);
                r.rd[i] = v;
                if (v != pat[i]) r.ok = false;
            }
            DEBUG_PRINTF("[MGM210P-SWD] flash test @0x%08X: IPVER=0x%08X erased=%d "
                         "rd=%08X %08X %08X %08X -> %s\n",
                         (unsigned)addr, (unsigned)r.ipversion, r.erased ? 1 : 0,
                         (unsigned)r.rd[0], (unsigned)r.rd[1], (unsigned)r.rd[2],
                         (unsigned)r.rd[3], r.ok ? "PASS" : "FAIL");
        } else {
            DEBUG_PRINTLN(F("[MGM210P-SWD] flash test: core halt FAILED"));
        }
    }
    mgm210p_set_baud(MGM210P_UART_BAUD);
    if (out) *out = r;
    return r.ok;
}

// ---------------------------------------------------------------------------
// SWD flash-programming session
// ---------------------------------------------------------------------------
static bool     g_flash_active = false;
static uint32_t g_flash_last_page = 0xFFFFFFFFu; // highest page already erased

static bool msc_erase_page(uint32_t page_addr) {
    swd_mem_write32(MSC_ADDRB_REG, page_addr);
    swd_mem_write32(MSC_WRITECMD_REG, MSC_ERASEPAGE_BIT);
    return msc_wait_notbusy(1000);
}

bool mgm210p_swd_flash_begin() {
    g_uart.end();
    delay(10);
    g_begun = false;
    g_flash_active = false;
    g_flash_last_page = 0xFFFFFFFFu;

    if (!swd_connect() || !swd_halt_core()) {
        mgm210p_set_baud(MGM210P_UART_BAUD);
        DEBUG_PRINTLN(F("[MGM210P-SWD] flash begin FAILED (connect/halt)"));
        return false;
    }
    swd_pick_msc_base();                        // handle MG26 TrustZone alias
    swd_mem_write32(MSC_LOCK_REG, MSC_UNLOCK_KEY);
    swd_mem_write32(MSC_WRITECTRL_REG, MSC_WREN_BIT);
    g_flash_active = true;
    DEBUG_PRINTLN(F("[MGM210P-SWD] flash session begin"));
    return true;
}

bool mgm210p_swd_flash_write(uint32_t addr, const uint8_t *data, uint32_t len, bool verify) {
    if (!g_flash_active || !data) return false;
    uint32_t off = 0;
    while (off < len) {
        uint32_t cur = addr + off;
        uint32_t page = cur & ~(g_chip->flash_page - 1u);
        if (page != g_flash_last_page) {
            if (!msc_erase_page(page)) {
                DEBUG_PRINTF("[MGM210P-SWD] erase FAILED @0x%08X\n", (unsigned)page);
                return false;
            }
            g_flash_last_page = page;
        }
        // Bytes until the next page boundary.
        uint32_t seg = (page + g_chip->flash_page) - cur;
        if (seg > len - off) seg = len - off;

        swd_mem_write32(MSC_ADDRB_REG, cur);
        uint32_t w = 0;
        while (w < seg) {
            uint32_t word = 0xFFFFFFFFu;
            for (int b = 0; b < 4 && (w + (uint32_t)b) < seg; b++)
                word = (word & ~(0xFFu << (8 * b))) | ((uint32_t)data[off + w + b] << (8 * b));
            // Wait for the write buffer, then push the word (auto-increments).
            uint32_t s = 0, t0 = millis();
            while ((millis() - t0) < 50) {
                if (swd_mem_read32(MSC_STATUS_REG, &s) && (s & MSC_STAT_WDATARDY)) break;
            }
            swd_mem_write32(MSC_WDATA_REG, word);
            w += 4;
        }
        swd_mem_write32(MSC_WRITECMD_REG, MSC_WRITEEND_BIT);
        msc_wait_notbusy(1000);

        if (verify) {
            for (uint32_t v = 0; v < seg; v += 4) {
                uint32_t expect = 0xFFFFFFFFu, got = 0;
                for (int b = 0; b < 4 && (v + (uint32_t)b) < seg; b++)
                    expect = (expect & ~(0xFFu << (8 * b))) | ((uint32_t)data[off + v + b] << (8 * b));
                swd_mem_read32(cur + v, &got);
                if (got != expect) {
                    DEBUG_PRINTF("[MGM210P-SWD] verify MISMATCH @0x%08X got=%08X exp=%08X\n",
                                 (unsigned)(cur + v), (unsigned)got, (unsigned)expect);
                    return false;
                }
            }
        }
        off += seg;
    }
    return true;
}

void mgm210p_swd_flash_end(bool run) {
    if (g_flash_active) {
        swd_mem_write32(MSC_WRITECTRL_REG, 0); // WREN off
        if (run) {
            // Deassert halt + request a system reset so the new image runs.
            swd_mem_write32(0xE000ED0Cu, 0x05FA0004u); // AIRCR SYSRESETREQ
        }
    }
    g_flash_active = false;
    g_flash_last_page = 0xFFFFFFFFu;
    mgm210p_set_baud(MGM210P_UART_BAUD);
    DEBUG_PRINTLN(F("[MGM210P-SWD] flash session end"));
}

bool mgm210p_swd_flash_active() {
    return g_flash_active;
}

// ---------------------------------------------------------------------------
// SWD RAM-mailbox host (C5 side of osmb_t). See osmb.h for the shared contract.
// Word-aligned framing: [u32 len][ceil(len/4) payload words]. Rings are SPSC,
// power-of-two byte sizes; head/tail are byte indices kept multiples of 4 so
// every access is a single 32-bit AHB-AP transfer (no read-modify-write).
// ---------------------------------------------------------------------------
#define OSMB_ADDR       0x20000000u
#define OSMB_MAGIC      0x424D534Fu   // 'O','S','M','B' little-endian
#define OSMB_H2N_SIZE   2048u
#define OSMB_N2H_SIZE   2048u
#define OSMB_O_MAGIC    0x00u
#define OSMB_O_VERSION  0x04u
#define OSMB_O_H2NSZ    0x08u
#define OSMB_O_N2HSZ    0x0Cu
#define OSMB_O_H2NHEAD  0x10u
#define OSMB_O_H2NTAIL  0x14u
#define OSMB_O_N2HHEAD  0x18u
#define OSMB_O_N2HTAIL  0x1Cu
#define OSMB_O_N2HIRQ   0x20u
#define OSMB_O_FLAGS    0x24u
#define OSMB_HDR_LEN    0x40u

static uint32_t g_mb_h2n_size = OSMB_H2N_SIZE;
static uint32_t g_mb_n2h_size = OSMB_N2H_SIZE;

static inline uint32_t osmb_h2n_base() { return OSMB_ADDR + OSMB_HDR_LEN; }
static inline uint32_t osmb_n2h_base() { return OSMB_ADDR + OSMB_HDR_LEN + g_mb_h2n_size; }
static inline uint32_t osmb_slot(uint16_t len) { return 4u + ((len + 3u) & ~3u); }

// Push a word-framed message into a ring (producer side). `hd_off`/`tl_off` are
// the head/tail header offsets; `base`/`size` describe the buffer. No wrap
// straddle: size is a power of two multiple of 4 so every word stays aligned.
static int osmb_ring_put(uint32_t base, uint32_t size, uint32_t hd_off, uint32_t tl_off,
                         const uint8_t *frame, uint16_t len) {
    uint32_t head = 0, tail = 0;
    swd_mem_read32(OSMB_ADDR + hd_off, &head);
    swd_mem_read32(OSMB_ADDR + tl_off, &tail);
    uint32_t used = (head - tail) & (size - 1u);
    uint32_t slot = osmb_slot(len);
    if ((size - 1u - used) < slot) return 0;             // full
    uint32_t pos = head;
    swd_mem_write32(base + (pos & (size - 1u)), len);    // length word
    pos += 4u;
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t w = 0xFFFFFFFFu;
        for (int b = 0; b < 4 && (i + (uint32_t)b) < len; b++)
            w = (w & ~(0xFFu << (8 * b))) | ((uint32_t)frame[i + b] << (8 * b));
        swd_mem_write32(base + (pos & (size - 1u)), w);
        pos += 4u;
    }
    swd_mem_write32(OSMB_ADDR + hd_off, (head + slot) & (size - 1u)); // publish
    return 1;
}

// Pop a word-framed message from a ring (consumer side).
static int osmb_ring_get(uint32_t base, uint32_t size, uint32_t hd_off, uint32_t tl_off,
                         uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    uint32_t head = 0, tail = 0;
    swd_mem_read32(OSMB_ADDR + hd_off, &head);
    swd_mem_read32(OSMB_ADDR + tl_off, &tail);
    uint32_t used = (head - tail) & (size - 1u);
    if (used < 4u) return 0;                              // no length word yet
    uint32_t pos = tail, lenw = 0;
    swd_mem_read32(base + (pos & (size - 1u)), &lenw);
    uint16_t len = (uint16_t)(lenw & 0xFFFFu);
    uint32_t slot = osmb_slot(len);
    if (len == 0 || len > cap) {                          // bad frame: drop len word
        swd_mem_write32(OSMB_ADDR + tl_off, (tail + 4u) & (size - 1u));
        return 0;
    }
    if (used < slot) return 0;                            // frame not fully arrived
    pos += 4u;
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t w = 0;
        swd_mem_read32(base + (pos & (size - 1u)), &w);
        pos += 4u;
        for (int b = 0; b < 4 && (i + (uint32_t)b) < len; b++)
            buf[i + b] = (uint8_t)(w >> (8 * b));
    }
    swd_mem_write32(OSMB_ADDR + tl_off, (tail + slot) & (size - 1u)); // consume
    if (out_len) *out_len = len;
    return 1;
}

bool mgm_mailbox_attach() {
    g_uart.end();
    delay(10);
    g_begun = false;
    if (!swd_connect()) {
        mgm210p_set_baud(MGM210P_UART_BAUD);
        return false;
    }
    uint32_t magic = 0;
    swd_mem_read32(OSMB_ADDR + OSMB_O_MAGIC, &magic);
    if (magic != OSMB_MAGIC) {
        DEBUG_PRINTF("[MGM-MB] no mailbox (magic=0x%08X, want 0x%08X)\n",
                     (unsigned)magic, (unsigned)OSMB_MAGIC);
        mgm210p_set_baud(MGM210P_UART_BAUD);
        return false;
    }
    swd_mem_read32(OSMB_ADDR + OSMB_O_H2NSZ, &g_mb_h2n_size);
    swd_mem_read32(OSMB_ADDR + OSMB_O_N2HSZ, &g_mb_n2h_size);
    if (!g_mb_h2n_size) g_mb_h2n_size = OSMB_H2N_SIZE;
    if (!g_mb_n2h_size) g_mb_n2h_size = OSMB_N2H_SIZE;
    DEBUG_PRINTF("[MGM-MB] attached: h2n=%u n2h=%u\n",
                 (unsigned)g_mb_h2n_size, (unsigned)g_mb_n2h_size);
    return true;
}

void mgm_mailbox_detach() {
    mgm210p_set_baud(MGM210P_UART_BAUD);
}

int mgm_mailbox_send(const uint8_t *frame, uint16_t len) {
    return osmb_ring_put(osmb_h2n_base(), g_mb_h2n_size,
                         OSMB_O_H2NHEAD, OSMB_O_H2NTAIL, frame, len);
}

int mgm_mailbox_recv(uint8_t *buf, uint16_t cap, uint16_t *len) {
    return osmb_ring_get(osmb_n2h_base(), g_mb_n2h_size,
                         OSMB_O_N2HHEAD, OSMB_O_N2HTAIL, buf, cap, len);
}

bool mgm_mailbox_selftest(char *out, size_t out_len) {
    g_uart.end();
    delay(10);
    g_begun = false;
    if (!swd_connect()) {
        if (out) snprintf(out, out_len, "SWD connect failed");
        mgm210p_set_baud(MGM210P_UART_BAUD);
        return false;
    }
    // Initialise a mailbox header in target RAM (as the NCP's osmb_init would).
    g_mb_h2n_size = OSMB_H2N_SIZE;
    g_mb_n2h_size = OSMB_N2H_SIZE;
    swd_mem_write32(OSMB_ADDR + OSMB_O_H2NSZ, OSMB_H2N_SIZE);
    swd_mem_write32(OSMB_ADDR + OSMB_O_N2HSZ, OSMB_N2H_SIZE);
    swd_mem_write32(OSMB_ADDR + OSMB_O_H2NHEAD, 0);
    swd_mem_write32(OSMB_ADDR + OSMB_O_H2NTAIL, 0);
    swd_mem_write32(OSMB_ADDR + OSMB_O_N2HHEAD, 0);
    swd_mem_write32(OSMB_ADDR + OSMB_O_N2HTAIL, 0);
    swd_mem_write32(OSMB_ADDR + OSMB_O_VERSION, 1);
    swd_mem_write32(OSMB_ADDR + OSMB_O_MAGIC, OSMB_MAGIC);

    uint32_t rb_magic = 0;
    swd_mem_read32(OSMB_ADDR + OSMB_O_MAGIC, &rb_magic);

    // Produce a test frame into the n2h ring (simulating the NCP), then recv it.
    const uint8_t test[8] = { 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
    int put = osmb_ring_put(osmb_n2h_base(), g_mb_n2h_size,
                            OSMB_O_N2HHEAD, OSMB_O_N2HTAIL, test, sizeof(test));
    uint8_t rx[16];
    uint16_t rl = 0;
    int got = mgm_mailbox_recv(rx, sizeof(rx), &rl);
    bool match = (got == 1) && (rl == sizeof(test)) && (memcmp(rx, test, sizeof(test)) == 0);
    bool ok = (rb_magic == OSMB_MAGIC) && (put == 1) && match;
    if (out)
        snprintf(out, out_len, "magic=%s put=%d recv=%d len=%u match=%d",
                 rb_magic == OSMB_MAGIC ? "OK" : "BAD", put, got, (unsigned)rl, match ? 1 : 0);
    DEBUG_PRINTF("[MGM-MB] selftest: %s\n", out ? out : "");
    mgm210p_set_baud(MGM210P_UART_BAUD);
    return ok;
}

#endif // ESP32C5 && OS_MGM210P
