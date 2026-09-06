#!/usr/bin/env python3
"""Flash a raw firmware image onto the external MGM210P via the ESP32-C5's SWD
flasher, over HTTP. The C5 exposes /mg?flash=begin|data|end.

Usage:
  mgm_flash.py <ip> <pwhash> <image.bin> <load_addr> [chunk_bytes] [--no-verify]

- <load_addr> e.g. 0x00004000 (where the .bin's first byte lives in flash).
- chunk_bytes must be a multiple of 4 (default 1024).
"""
import sys
import time
import urllib.parse
import urllib.request


def call(base, pw, params, body=None):
    q = dict(params)
    q["pw"] = pw
    url = base + "?" + urllib.parse.urlencode(q)
    req = urllib.request.Request(url, data=body,
                                 method="POST" if body is not None else "GET")
    if body is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode("utf-8", "replace")


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1
    ip, pw, binf, addr_s = sys.argv[1:5]
    addr = int(addr_s, 0)
    chunk = 1024
    verify = 1
    for a in sys.argv[5:]:
        if a == "--no-verify":
            verify = 0
        else:
            chunk = int(a, 0)
    if chunk % 4:
        print("chunk_bytes must be a multiple of 4")
        return 1

    data = open(binf, "rb").read()
    base = "http://%s/mg" % ip
    print("image %s: %d bytes -> flash 0x%08X, chunk=%d, verify=%d"
          % (binf, len(data), addr, chunk, verify))

    print("begin:", call(base, pw, {"flash": "begin"}))
    t0 = time.time()
    for i in range(0, len(data), chunk):
        seg = data[i:i + chunk]
        resp = call(base, pw, {"flash": "data", "addr": hex(addr + i),
                               "verify": verify}, body=seg)
        if '"ok":1' not in resp:
            print("  FAILED @0x%08X: %s" % (addr + i, resp))
            call(base, pw, {"flash": "end"})
            return 1
        done = i + len(seg)
        pct = 100.0 * done / len(data)
        sys.stdout.write("\r  %6.1f%%  0x%08X  %d/%d bytes" % (pct, addr + i, done, len(data)))
        sys.stdout.flush()
    dt = time.time() - t0
    print("\nend:", call(base, pw, {"flash": "end", "run": "1"}))
    print("done in %.1fs (%.1f KB/s)" % (dt, len(data) / 1024 / max(dt, 0.001)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
