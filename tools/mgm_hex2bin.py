#!/usr/bin/env python3
"""Parse an Intel HEX firmware image, report its extent, and write a raw .bin
padded with 0xFF from the image start address. Used to prepare MGM210P/EFR32
firmware for direct SWD flashing from the ESP32-C5.

Usage: mgm_hex2bin.py <in.hex> [out.bin]
"""
import sys


def parse_hex(path):
    base = 0
    seg = {}
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        ln, off, typ, data = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
        if typ == 0x04:
            base = (data[0] << 24) | (data[1] << 16)
        elif typ == 0x02:
            base = ((data[0] << 8) | data[1]) << 4
        elif typ == 0x00:
            a = base + off
            for i, by in enumerate(data):
                seg[a + i] = by
    return seg


def main():
    if len(sys.argv) < 2:
        print("usage: mgm_hex2bin.py <in.hex> [out.bin]")
        return 1
    seg = parse_hex(sys.argv[1])
    lo, hi = min(seg), max(seg) + 1
    gaps = sum(1 for a in range(lo, hi) if a not in seg)
    print("start=0x%08X end=0x%08X size=%d (%.1f KB) data=%d fill0xFF=%d"
          % (lo, hi, hi - lo, (hi - lo) / 1024, len(seg), gaps))
    if len(sys.argv) >= 3:
        buf = bytearray(b"\xFF" * (hi - lo))
        for a, by in seg.items():
            buf[a - lo] = by
        open(sys.argv[2], "wb").write(buf)
        print("wrote %s (%d bytes) load_addr=0x%08X" % (sys.argv[2], len(buf), lo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
