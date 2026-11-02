#!/usr/bin/env python3
"""Build and sign an on-device UI package (ui.tar) for ui_local.cpp.

Input is a UI snapshot directory (ui-live/www/<version> or ui-test/www/dev of
the OpenSprinkler-App repo). Output is an uncompressed ustar archive whose
text files are individually gzip-9 compressed (name.gz), plus a detached
signature:

    <out>          ustar: ui.json first, then css/ js/ vendor-js/ img/ fonts/ locale/ ...
    <out>.sig      base64(ECDSA P-256 signature over SHA-256(<out>)), raw r||s
    <out>.json     {"version","size","sha256","sig","files"}  (consumed by fw.sh)

The firmware verifies the signature with the public key compiled into
ui_local_pubkey.h (same key as the app's UI snapshot file lists:
App-Keys/ui-update-signing.pem). Nothing in this script needs more than the
python standard library, terser (optional, minification) and the openssl CLI.

Usage:
    mkuipack.py build <src_dir> <version> <out.tar> [--key PEM] [--locales de,en|all]
                      [--no-minify] [--no-bundle] [--source NAME]
    mkuipack.py verify <pack.tar> [--key PEM | --pub PEM]
    mkuipack.py list   <pack.tar>
"""
import argparse
import base64
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

DEFAULT_KEY = "/data/Workspace/App-Keys/ui-update-signing.pem"

# directories / files taken from the snapshot (relative to its root)
INCLUDE_DIRS = ["css", "js", "vendor-js", "img", "fonts", "locale", "res/ios-web"]
INCLUDE_FILES = ["index.html", "map.html", "manifest.json", "modules.json", "zigbeeclusterids.json"]
EXCLUDE_RE = [
    r"^cordova(_plugins)?\.js$", r"^plugins/", r"^platform\.js$", r"^exec\.js$", r"^confighelper\.js$",
    r"^config\.xml$", r"^sw\.js$", r"^firebase-messaging-sw\.js$", r"^filelist\.(json|sig)$",
    r"^js/ui-updater\.js$", r"\.(bak|orig|md|map|sav|sav\d*)$", r"~$", r"(^|/)\.", r"^irrigation-db-test\.html$",
    r"^index\.html\.bak$",
]
TEXT_EXT = {".js", ".css", ".html", ".json", ".svg", ".txt", ".map", ".xml"}


def log(msg):
    print(msg, file=sys.stderr)


def excluded(rel):
    return any(re.search(p, rel) for p in EXCLUDE_RE)


def walk(src):
    files = []
    for f in INCLUDE_FILES:
        p = os.path.join(src, f)
        if os.path.isfile(p):
            files.append(f)
    for d in INCLUDE_DIRS:
        base = os.path.join(src, d)
        if not os.path.isdir(base):
            continue
        for root, _dirs, names in os.walk(base):
            for n in sorted(names):
                full = os.path.join(root, n)
                rel = os.path.relpath(full, src).replace(os.sep, "/")
                if os.path.islink(full) or not os.path.isfile(full):
                    continue
                files.append(rel)
    out = []
    seen = set()
    for rel in files:
        if rel in seen or excluded(rel):
            continue
        seen.add(rel)
        out.append(rel)
    return out


def have(cmd):
    return shutil.which(cmd) is not None


def minify_js(data, name):
    if not have("terser"):
        return data
    try:
        r = subprocess.run(["terser", "--compress", "--mangle"], input=data, capture_output=True, timeout=120)
        if r.returncode == 0 and r.stdout:
            # make sure the output still parses (terser can be fooled by odd inputs)
            chk = subprocess.run(["node", "--check", "-"], input=r.stdout, capture_output=True, timeout=30) if have("node") else None
            if chk is None or chk.returncode == 0:
                return r.stdout
        log(f"  WARN: terser skipped {name}: {r.stderr.decode(errors='replace')[:120].strip()}")
    except Exception as e:  # noqa: BLE001
        log(f"  WARN: terser failed for {name}: {e}")
    return data


def minify_css(data, name):
    if not have("cleancss"):
        return data
    try:
        r = subprocess.run(["cleancss"], input=data, capture_output=True, timeout=120)
        if r.returncode == 0 and r.stdout:
            return r.stdout
        log(f"  WARN: cleancss skipped {name}")
    except Exception as e:  # noqa: BLE001
        log(f"  WARN: cleancss failed for {name}: {e}")
    return data


def der_to_raw(der):
    """ECDSA-Sig-Value SEQUENCE { INTEGER r, INTEGER s } -> 64 raw bytes."""
    i = 0
    assert der[i] == 0x30
    i += 1
    ln = der[i]
    i += 1
    if ln & 0x80:
        i += ln & 0x7F
    out = b""
    for _ in range(2):
        assert der[i] == 0x02
        i += 1
        l2 = der[i]
        i += 1
        v = der[i:i + l2]
        i += l2
        v = v.lstrip(b"\x00")
        assert len(v) <= 32
        out += v.rjust(32, b"\x00")
    return out


def raw_to_der(raw):
    def integer(b):
        b = b.lstrip(b"\x00") or b"\x00"
        if b[0] & 0x80:
            b = b"\x00" + b
        return b"\x02" + bytes([len(b)]) + b
    body = integer(raw[:32]) + integer(raw[32:])
    return b"\x30" + bytes([len(body)]) + body


def sign(data, key):
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, "in.bin")
        sig = os.path.join(td, "sig.der")
        with open(inp, "wb") as f:
            f.write(data)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key, "-out", sig, inp], capture_output=True)
        if r.returncode != 0:
            raise SystemExit(f"openssl sign failed: {r.stderr.decode(errors='replace')}")
        with open(sig, "rb") as f:
            der = f.read()
        pub = subprocess.run(["openssl", "ec", "-in", key, "-pubout"], capture_output=True, check=True).stdout
        pubf = os.path.join(td, "pub.pem")
        with open(pubf, "wb") as f:
            f.write(pub)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pubf, "-signature", sig, inp], capture_output=True)
        if r.returncode != 0:
            raise SystemExit("signature self-check failed")
    return der_to_raw(der)


def verify(data, raw_sig, pub_pem):
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, "in.bin")
        sig = os.path.join(td, "sig.der")
        pubf = os.path.join(td, "pub.pem")
        with open(inp, "wb") as f:
            f.write(data)
        with open(sig, "wb") as f:
            f.write(raw_to_der(raw_sig))
        with open(pubf, "wb") as f:
            f.write(pub_pem)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pubf, "-signature", sig, inp], capture_output=True)
        return r.returncode == 0


def cmd_build(a):
    src = os.path.abspath(a.src)
    if not os.path.isdir(src):
        raise SystemExit(f"source dir not found: {src}")
    if not re.match(r"^\d+\.\d+\.\d+(\.\d+)?$", a.version):
        raise SystemExit(f"bad version: {a.version}")
    if not os.path.isfile(a.key):
        raise SystemExit(f"signing key not found: {a.key}")
    locales = None if a.locales == "all" else set(x.strip() for x in a.locales.split(",") if x.strip())

    rels = walk(src)
    modules = []
    mj = os.path.join(src, "modules.json")
    if os.path.isfile(mj):
        with open(mj, encoding="utf-8") as f:
            modules = [m for m in json.load(f) if isinstance(m, str)]

    entries = []   # (name_in_tar, bytes)
    raw_total = 0
    bundle_parts = []
    for rel in rels:
        if rel.startswith("locale/"):
            if not rel.endswith(".js"):
                continue                       # .po sources etc.
            code = os.path.splitext(os.path.basename(rel))[0]
            if locales is not None and code not in locales:
                continue
        with open(os.path.join(src, rel), "rb") as f:
            data = f.read()
        raw_total += len(data)
        ext = os.path.splitext(rel)[1].lower()
        if a.bundle and rel.startswith("js/modules/") and os.path.basename(rel) in modules:
            bundle_parts.append((modules.index(os.path.basename(rel)), rel, data))
            continue
        if rel == "modules.json" and a.bundle and modules:
            data = json.dumps(["bundle.js"]).encode()
        if a.minify and ext == ".js" and not rel.startswith(("vendor-js/", "locale/")) and ".min." not in rel:
            data = minify_js(data, rel)
        elif a.minify and ext == ".css" and ".min." not in rel:
            data = minify_css(data, rel)
        if ext in TEXT_EXT:
            entries.append((rel + ".gz", gzip.compress(data, 9, mtime=0)))
        else:
            entries.append((rel, data))

    if a.bundle and bundle_parts:
        bundle_parts.sort()
        if len(bundle_parts) != len(modules):
            missing = sorted(set(modules) - {os.path.basename(r) for _, r, _ in bundle_parts})
            log(f"  WARN: modules missing from snapshot: {missing}")
        blob = b""
        for _, rel, data in bundle_parts:
            blob += b"\n/* " + rel.encode() + b" */\n;" + (minify_js(data, rel) if a.minify else data) + b"\n"
        entries.append(("js/modules/bundle.js.gz", gzip.compress(blob, 9, mtime=0)))

    stored = sum(len(d) for _, d in entries)
    meta = {
        "version": a.version,
        "build": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "files": len(entries),
        "bytes": stored,
        "source": a.source or os.path.basename(src.rstrip("/")),
    }
    meta_bytes = (json.dumps(meta, separators=(",", ":")) + "\n").encode()

    buf = io.BytesIO()
    mtime = int(time.time())
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        def add(name, data):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mtime = mtime
            ti.mode = 0o644
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            tar.addfile(ti, io.BytesIO(data))
        add("ui.json", meta_bytes)
        for name, data in sorted(entries):
            add(name, data)
    pack = buf.getvalue()

    raw_sig = sign(pack, a.key)
    sig_b64 = base64.b64encode(raw_sig).decode()
    sha = hashlib.sha256(pack).hexdigest()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(pack)
    with open(out + ".sig", "w") as f:
        f.write(sig_b64 + "\n")
    info = {"version": a.version, "size": len(pack), "sha256": sha, "sig": sig_b64, "files": len(entries), "stored": stored, "raw": raw_total}
    with open(out + ".json", "w") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    log(f"  {out}: {len(pack)/1024:.0f} kB, {len(entries)} files (raw {raw_total/1024:.0f} kB), sha256 {sha[:16]}…")
    print(json.dumps(info))


def read_pack(path):
    with open(path, "rb") as f:
        data = f.read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tar:
        members = tar.getmembers()
        meta = None
        if members and members[0].name == "ui.json":
            meta = json.loads(tar.extractfile(members[0]).read().decode())
    return data, members, meta


def cmd_verify(a):
    data, members, meta = read_pack(a.pack)
    if not meta:
        raise SystemExit("ui.json missing or not the first entry")
    with open(a.pack + ".sig") as f:
        raw = base64.b64decode(f.read().strip())
    if len(raw) != 64:
        raise SystemExit("signature is not 64 raw bytes")
    if a.pub:
        with open(a.pub, "rb") as f:
            pub = f.read()
    else:
        pub = subprocess.run(["openssl", "ec", "-in", a.key, "-pubout"], capture_output=True, check=True).stdout
    ok = verify(data, raw, pub)
    print(f"version {meta['version']}, {len(members)} entries, {len(data)} bytes, signature {'OK' if ok else 'INVALID'}")
    sys.exit(0 if ok else 1)


def cmd_list(a):
    data, members, meta = read_pack(a.pack)
    print(json.dumps(meta))
    for m in members:
        print(f"{m.size:9d}  {m.name}")
    print(f"{len(data):9d}  total")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("src")
    b.add_argument("version")
    b.add_argument("out")
    b.add_argument("--key", default=os.environ.get("UI_UPDATE_KEY", DEFAULT_KEY))
    b.add_argument("--locales", default="all", help="all or comma separated codes (de,en,fr)")
    b.add_argument("--no-minify", dest="minify", action="store_false")
    b.add_argument("--no-bundle", dest="bundle", action="store_false")
    b.add_argument("--source", default="")
    b.set_defaults(func=cmd_build)
    v = sub.add_parser("verify")
    v.add_argument("pack")
    v.add_argument("--key", default=os.environ.get("UI_UPDATE_KEY", DEFAULT_KEY))
    v.add_argument("--pub", default="")
    v.set_defaults(func=cmd_verify)
    ls = sub.add_parser("list")
    ls.add_argument("pack")
    ls.set_defaults(func=cmd_list)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
