#!/usr/bin/env bash
# =============================================================================
# fw.sh – OpenSprinkler Firmware Manager
#
# Actions:
#   build   [matter|zigbee|esp8266|all]  – Build firmware
#   upload  [matter|zigbee|esp8266|all]  – Upload firmware (via USB/Serial)
#   deploy  [matter|zigbee|esp8266|all]  – Build + Upload
#   release [rebuild]                    – Bump version, release build, tag & publish
#                                          rebuild: build only, no version bump/git
#   generate-mfg                         – Generate Matter manufacturing data (DAC, NVS)
#   flash-mfg                            – Flash matter_kvs partition only
#   switch  [matter|zigbee|mode]         – Switch firmware variant via REST API
#                                          (automatically reboots the device)
#
# Usage:
#   ./fw.sh build
#   ./fw.sh build matter
#   ./fw.sh build zigbee
#   ./fw.sh build esp8266
#   ./fw.sh build all
#   ./fw.sh upload
#   ./fw.sh upload matter
#   ./fw.sh upload zigbee
#   ./fw.sh upload esp8266
#   ./fw.sh deploy
#   ./fw.sh deploy matter
#   ./fw.sh release
#   ./fw.sh release rebuild
#   ./fw.sh generate-mfg
#   ./fw.sh flash-mfg
#   ./fw.sh switch matter        -> switch to Matter firmware
#   ./fw.sh switch zigbee        -> switch to ZigBee Gateway firmware
#   ./fw.sh switch zigbee-client -> switch to ZigBee Client firmware
#   ./fw.sh switch disabled      -> disable IEEE 802.15.4 radio
#
# Environment variables (optional):
#   OS_IP           Device IP  (default: 192.168.0.151)
#   OS_PASSWORD     Admin password in plain text (will be MD5 hashed)
#   OS_HASH         Admin password already as MD5 hash
# =============================================================================

set -euo pipefail

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header()  { echo -e "\n${BOLD}${BLUE}══════════════════════════════════════${NC}"; \
            echo -e "${BOLD}${BLUE}  $*${NC}"; \
            echo -e "${BOLD}${BLUE}══════════════════════════════════════${NC}"; }

# ── Configuration ────────────────────────────────────────────────────────────
DEVICE_IP="${OS_IP:-192.168.0.151}"
PIO_BIN="platformio"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFINES_H="${SCRIPT_DIR}/defines.h"
UPGRADE_DIR="${SCRIPT_DIR}/docs/upgrade"
MANIFEST="${UPGRADE_DIR}/manifest.json"
VERSIONS_JSON="${UPGRADE_DIR}/versions.json"
CHANGELOG="${SCRIPT_DIR}/CHANGELOG.md"
PLATFORMIO_INI="${SCRIPT_DIR}/platformio.ini"
GITHUB_REPO="opensprinklershop/OpenSprinkler-Firmware"

# Matter manufacturing data
MATTER_MFG_DIR="${SCRIPT_DIR}/matter_mfg"
MATTER_KVS_BIN="${MATTER_MFG_DIR}/matter_kvs.bin"
MATTER_PAA_CERT="${MATTER_MFG_DIR}/paa/paa_cert.pem"
MATTER_PAA_KEY="${MATTER_MFG_DIR}/paa/paa_key.pem"
MATTER_KVS_ADDR="0x730000"
MATTER_KVS_SIZE="0x10000"
MATTER_VENDOR_ID="0xFFF1"
MATTER_PRODUCT_ID="0x8000"

# PlatformIO environment names
ENV_C5_MATTER="esp32-c5-matter"
ENV_C5_ZIGBEE="esp32-c5-zigbee"
ENV_ESP8266="os3x_esp8266"

# Determine password hash
_get_hash() {
    if [[ -n "${OS_HASH:-}" ]]; then
        echo "$OS_HASH"
    elif [[ -n "${OS_PASSWORD:-}" ]]; then
        echo -n "$OS_PASSWORD" | md5sum | awk '{print $1}'
    else
        echo ""   # no password → empty hash (public endpoints)
    fi
}

# ── Helper functions ─────────────────────────────────────────────────────────
check_pio() {
    if ! command -v "$PIO_BIN" &>/dev/null; then
        error "PlatformIO not found. Please install: https://platformio.org"
        exit 1
    fi
}

check_device() {
    info "Checking connection to ${DEVICE_IP} …"
    if ! curl -sf --connect-timeout 3 "http://${DEVICE_IP}/db?pw=" &>/dev/null; then
        error "Device not reachable at http://${DEVICE_IP}"
        error "Hint: OS_IP=<IP> ./fw.sh switch <variant>"
        exit 1
    fi
    ok "Device reachable."
}

build_env() {
    local env="$1"
    header "Building firmware: ${env}"
    if "$PIO_BIN" run --environment "$env"; then
        ok "Build successful → .pio/build/${env}/firmware.bin"
    else
        error "Build failed: ${env}"
        exit 1
    fi
}

upload_env() {
    local env="$1"
    header "Uploading firmware: ${env}"
    if "$PIO_BIN" run --environment "$env" --target upload; then
        ok "Upload successful: ${env}"
    else
        error "Upload failed: ${env}"
        exit 1
    fi
}

# Mode codes per IEEE802154Mode enum in firmware code:
#   0 = disabled, 1 = matter, 2 = zigbee-gateway, 3 = zigbee-client
_mode_code() {
    case "${1,,}" in
        disabled)      echo 0 ;;
        matter)        echo 1 ;;
        zigbee|zigbee-gateway|zb-gw) echo 2 ;;
        zigbee-client|zb-client)     echo 3 ;;
        *)
            error "Unknown variant: '$1'"
            error "Allowed: disabled | matter | zigbee | zigbee-client"
            exit 1
            ;;
    esac
}

_mode_name() {
    case "$1" in
        0) echo "Disabled" ;;
        1) echo "Matter" ;;
        2) echo "ZigBee Gateway" ;;
        3) echo "ZigBee Client" ;;
        *) echo "Unknown" ;;
    esac
}

switch_firmware() {
    local target="${1:-}"
    if [[ -z "$target" ]]; then
        error "Usage: ./fw.sh switch <matter|zigbee|zigbee-client|disabled>"
        exit 1
    fi

    local code
    code=$(_mode_code "$target")
    local name
    name=$(_mode_name "$code")

    check_device
    local hash
    hash=$(_get_hash)

    header "Switching firmware → ${name} (mode=${code})"

    local url="http://${DEVICE_IP}/iw?pw=${hash}&mode=${code}"
    local response
    response=$(curl -sf --connect-timeout 5 "$url" 2>&1) || {
        error "REST request failed: ${url}"
        exit 1
    }

    echo "Response: $response"

    # Check if result=1
    if echo "$response" | python3 -c "import sys,json; d=json.load(sys.stdin); sys.exit(0 if d.get('result')==1 else 1)" 2>/dev/null; then
        ok "Firmware switch successful. Device is rebooting …"
        local new_variant
        new_variant=$(echo "$response" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('bootVariantName','?'))" 2>/dev/null || echo "?")
        ok "New boot variant: ${new_variant}"
        info "Please wait 10–20 seconds for the device to become reachable after reboot."
    else
        error "Firmware switch failed."
        local err_msg
        err_msg=$(echo "$response" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('error','?'))" 2>/dev/null || echo "Unknown error")
        error "Error: ${err_msg}"
        exit 1
    fi
}

show_status() {
    check_device
    local hash
    hash=$(_get_hash)

    header "Device status: ${DEVICE_IP}"

    local db_resp
    db_resp=$(curl -sf --connect-timeout 5 "http://${DEVICE_IP}/db?pw=${hash}" 2>/dev/null || echo "{}")
    echo "Debug: $db_resp" | python3 -c "
import sys, json
raw = sys.stdin.read().replace('Debug: ','')
try:
    d = json.loads(raw)
    print(f\"  Heap:          {d.get('heap','?')} bytes free\")
    print(f\"  Flash used:    {d.get('used','?')} / {d.get('flash','?')} bytes\")
    print(f\"  Ethernet:      {'Yes' if d.get('ETH') else 'No'}\")
    print(f\"  BLE active:    {'Yes' if d.get('ble_ok') else 'No'}\")
    print(f\"  Sensors:       {d.get('sensor_count','?')} loaded, file {d.get('sensor_file_sz','?')} bytes\")
except Exception as e:
    print('  (Parsing failed)', e)
" 2>/dev/null || true

    # IEEE802154 status (only when authenticated)
    if [[ -n "$hash" ]]; then
        local ir_resp
        ir_resp=$(curl -sf --connect-timeout 5 "http://${DEVICE_IP}/ir?pw=${hash}" 2>/dev/null || echo "{}")
        echo "$ir_resp" | python3 -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    mode = d.get('mode_name', d.get('mode', '?'))
    variant = d.get('bootVariantName', d.get('boot_variant', '?'))
    print(f'  Mode:          {mode}')
    print(f'  Boot variant:  {variant}')
except:
    pass
" 2>/dev/null || true
    fi
    echo ""
}

# ── Matter manufacturing data helpers ─────────────────────────────────────────

# Generate Matter manufacturing data (PAA → PAI → DAC chain + NVS partition)
generate_matter_mfg() {
    header "Generating Matter manufacturing data"

    if ! command -v esp-matter-mfg-tool &>/dev/null; then
        error "esp-matter-mfg-tool not found."
        error "Install: pip3 install esp-matter-mfg-tool"
        exit 1
    fi

    mkdir -p "${MATTER_MFG_DIR}/paa"

    # Generate test PAA if not present
    if [[ ! -f "$MATTER_PAA_CERT" || ! -f "$MATTER_PAA_KEY" ]]; then
        info "Generating test PAA certificate …"
        python3 -c "
from cryptography import x509
from cryptography.x509.oid import NameOID, ObjectIdentifier
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
import datetime

paa_key = ec.generate_private_key(ec.SECP256R1())
VID_OID = ObjectIdentifier('1.3.6.1.4.1.37244.2.1')
subject = issuer = x509.Name([
    x509.NameAttribute(NameOID.COMMON_NAME, 'OpenSprinkler Test PAA'),
    x509.NameAttribute(VID_OID, 'FFF1'),
])
paa_cert = (
    x509.CertificateBuilder()
    .subject_name(subject).issuer_name(issuer)
    .public_key(paa_key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(datetime.datetime.utcnow())
    .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=36500))
    .add_extension(x509.BasicConstraints(ca=True, path_length=1), critical=True)
    .add_extension(x509.KeyUsage(
        digital_signature=False, key_encipherment=False,
        content_commitment=False, data_encipherment=False,
        key_agreement=False, key_cert_sign=True, crl_sign=True,
        encipher_only=False, decipher_only=False), critical=True)
    .add_extension(x509.SubjectKeyIdentifier.from_public_key(paa_key.public_key()), critical=False)
    .sign(paa_key, hashes.SHA256())
)
with open('${MATTER_PAA_CERT}', 'wb') as f:
    f.write(paa_cert.public_bytes(serialization.Encoding.PEM))
with open('${MATTER_PAA_KEY}', 'wb') as f:
    f.write(paa_key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption()))
"
        ok "Test PAA certificate generated."
    else
        info "Using existing PAA certificate."
    fi

    # Remove previous output
    rm -rf "${MATTER_MFG_DIR}/fff1_8000"

    info "Running esp-matter-mfg-tool …"
    esp-matter-mfg-tool \
        -v "$MATTER_VENDOR_ID" \
        -p "$MATTER_PRODUCT_ID" \
        --vendor-name "OpenSprinkler" \
        --product-name "OpenSprinkler-C5" \
        --hw-ver 1 \
        --hw-ver-str "1.0" \
        --target esp32c5 \
        -s "$MATTER_KVS_SIZE" \
        --paa \
        -c "$MATTER_PAA_CERT" \
        -k "$MATTER_PAA_KEY" \
        -lt 3650 \
        --passcode 20202021 \
        --discriminator 3840 \
        -dm 6 \
        --outdir "$MATTER_MFG_DIR" \
    || { error "esp-matter-mfg-tool failed"; exit 1; }

    # Find the generated partition binary (UUID-based directory)
    local gen_bin
    gen_bin=$(find "${MATTER_MFG_DIR}/fff1_8000" -name "*-partition.bin" -type f | head -1)
    if [[ -z "$gen_bin" ]]; then
        error "No partition binary found in output."
        exit 1
    fi

    cp "$gen_bin" "$MATTER_KVS_BIN"
    ok "Matter KVS binary: ${MATTER_KVS_BIN} ($(stat -c%s "$MATTER_KVS_BIN") bytes)"

    # Show onboarding codes
    local onb_csv
    onb_csv=$(find "${MATTER_MFG_DIR}/fff1_8000" -name "*-onb_codes.csv" -type f | head -1)
    if [[ -n "$onb_csv" ]]; then
        echo ""
        info "Matter onboarding codes:"
        tail -1 "$onb_csv" | awk -F',' '{
            printf "  QR Code:     %s\n", $1
            printf "  Manual Code: %s\n", $2
            printf "  Discriminator: %s\n", $3
            printf "  Passcode:    %s\n", $4
        }'
        echo ""
    fi
}

# Flash only the matter_kvs partition (standalone)
flash_matter_kvs() {
    header "Flashing Matter KVS partition"

    if [[ ! -f "$MATTER_KVS_BIN" ]]; then
        error "Matter KVS binary not found: ${MATTER_KVS_BIN}"
        error "Run: ./fw.sh generate-mfg"
        exit 1
    fi

    local esptool="${SCRIPT_DIR}/.pio/packages/tool-esptoolpy/esptool.py"
    if [[ ! -f "$esptool" ]]; then
        error "esptool.py not found. Run a PlatformIO build first."
        exit 1
    fi

    local port="${UPLOAD_PORT:-/dev/ttyACM0}"
    local baud="${UPLOAD_SPEED:-460800}"

    info "Flashing ${MATTER_KVS_BIN} → ${MATTER_KVS_ADDR}"
    "$esptool" --chip esp32c5 --port "$port" --baud "$baud" \
        --before default_reset --after hard_reset \
        write_flash "$MATTER_KVS_ADDR" "$MATTER_KVS_BIN" \
    || { error "Flash failed"; exit 1; }

    ok "Matter KVS partition flashed at ${MATTER_KVS_ADDR}"
}

# ── Release helpers ──────────────────────────────────────────────────────────

# Read current version numbers from defines.h
read_version() {
    OS_FW_VERSION=$(grep -oP '^\s*#define\s+OS_FW_VERSION\s+\K[0-9]+' "$DEFINES_H")
    OS_FW_MINOR=$(grep -oP '^\s*#define\s+OS_FW_MINOR\s+\K[0-9]+' "$DEFINES_H")
    if [[ -z "$OS_FW_VERSION" || -z "$OS_FW_MINOR" ]]; then
        error "Could not read OS_FW_VERSION / OS_FW_MINOR from ${DEFINES_H}"
        exit 1
    fi
}

# Format version for display: 233 → "2.3.3"
format_version() {
    local v="$1"
    local major=$((v / 100))
    local mid=$(( (v % 100) / 10 ))
    local patch=$((v % 10))
    echo "${major}.${mid}.${patch}"
}

# Bump OS_FW_MINOR in defines.h
bump_minor_version() {
    read_version
    local new_minor=$((OS_FW_MINOR + 1))
    info "Bumping OS_FW_MINOR: ${OS_FW_MINOR} → ${new_minor}"
    sed -i "s/^\(#define OS_FW_MINOR\s*\)[0-9]\+/\1${new_minor}/" "$DEFINES_H"
    OS_FW_MINOR="$new_minor"
    ok "defines.h updated: OS_FW_VERSION=${OS_FW_VERSION}, OS_FW_MINOR=${OS_FW_MINOR}"
}

# Disable ENABLE_DEBUG in the ESP8266 section of platformio.ini
disable_esp8266_debug() {
    info "Disabling debug flags for ESP8266 release build …"
    # Only modify lines within the [env:os3x_esp8266] section
    sed -i '/^\[env:os3x_esp8266\]/,/^\[env:/{s/^\(\s*\)-DENABLE_DEBUG\s*$/\1;-DENABLE_DEBUG/}' "$PLATFORMIO_INI"
    ok "ESP8266 debug flags disabled."
}

# Restore ENABLE_DEBUG in the ESP8266 section of platformio.ini
restore_esp8266_debug() {
    info "Restoring debug flags for ESP8266 …"
    sed -i '/^\[env:os3x_esp8266\]/,/^\[env:/{s/^\(\s*\);-DENABLE_DEBUG$/\1-DENABLE_DEBUG/}' "$PLATFORMIO_INI"
    ok "ESP8266 debug flags restored."
}

# Build a clean release for a given env
build_release_env() {
    local env="$1"
    local bin="${SCRIPT_DIR}/.pio/build/${env}/firmware.bin"
    header "Release build: ${env}"
    if ! "$PIO_BIN" run --environment "$env"; then
        # PlatformIO SCons can fail on the first run after a clean build dir
        # is encountered with a new flag set (directory-ordering quirk).
        # Retry once; the second run always succeeds in this case.
        warn "First attempt failed — retrying …"
        if ! "$PIO_BIN" run --environment "$env"; then
            error "Release build failed: ${env}"
            return 1
        fi
    fi
    if [[ ! -f "$bin" ]]; then
        error "Build reported success but binary not found: ${bin}"
        return 1
    fi
    ok "Release build successful → .pio/build/${env}/firmware.bin"
}

# Copy firmware binaries to upgrade directory
copy_to_upgrade() {
    mkdir -p "$UPGRADE_DIR"
    local zigbee_bin="${SCRIPT_DIR}/.pio/build/${ENV_C5_ZIGBEE}/firmware.bin"
    local matter_bin="${SCRIPT_DIR}/.pio/build/${ENV_C5_MATTER}/firmware.bin"
    local esp8266_bin="${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}/firmware.bin"

    for f in "$zigbee_bin" "$matter_bin" "$esp8266_bin"; do
        if [[ ! -f "$f" ]]; then
            error "Binary not found: ${f}"
            return 1
        fi
    done

    cp "$zigbee_bin" "${UPGRADE_DIR}/firmware_zigbee.bin"
    cp "$matter_bin" "${UPGRADE_DIR}/firmware_matter.bin"
    cp "$esp8266_bin" "${UPGRADE_DIR}/firmware_esp8266.bin"
    ok "Binaries copied to ${UPGRADE_DIR}/"
}

# Archive the previous release binaries into a versioned subdirectory
archive_previous_version() {
    if [[ ! -f "$MANIFEST" ]]; then
        info "No existing manifest.json — skipping archive of previous version."
        return 0
    fi

    local prev_version prev_minor
    prev_version=$(python3 -c "import json; d=json.load(open('$MANIFEST')); print(d.get('fw_version',0))")
    prev_minor=$(python3 -c "import json; d=json.load(open('$MANIFEST')); print(d.get('fw_minor',0))")

    if [[ "$prev_version" == "0" || -z "$prev_version" ]]; then
        info "Previous manifest has no fw_version — skipping archive."
        return 0
    fi

    local archive_dir="${UPGRADE_DIR}/archive/v${prev_version}_${prev_minor}"
    mkdir -p "$archive_dir"

    # Copy current binaries to archive
    for bin_name in firmware_zigbee.bin firmware_matter.bin firmware_esp8266.bin; do
        if [[ -f "${UPGRADE_DIR}/${bin_name}" ]]; then
            cp "${UPGRADE_DIR}/${bin_name}" "${archive_dir}/${bin_name}"
        fi
    done

    # Copy current manifest as archive metadata
    cp "$MANIFEST" "${archive_dir}/manifest.json"

    ok "Previous version v${prev_version} (build ${prev_minor}) archived to ${archive_dir}/"
}

# Update (or create) the versions.json catalog with all known releases
update_versions_catalog() {
    local changelog_text="$1"
    local date_str
    date_str=$(date +%Y-%m-%d)

    # Build a new entry for the current version
    local new_entry
    new_entry=$(python3 -c "
import json, sys
entry = {
    'fw_version': int(sys.argv[1]),
    'fw_minor': int(sys.argv[2]),
    'date': sys.argv[3],
    'zigbee_url': 'https://opensprinklershop.de/upgrade/archive/v' + sys.argv[1] + '_' + sys.argv[2] + '/firmware_zigbee.bin',
    'matter_url': 'https://opensprinklershop.de/upgrade/archive/v' + sys.argv[1] + '_' + sys.argv[2] + '/firmware_matter.bin',
    'esp8266_url': 'https://opensprinklershop.de/upgrade/archive/v' + sys.argv[1] + '_' + sys.argv[2] + '/firmware_esp8266.bin',
    'changelog': sys.argv[4]
}
print(json.dumps(entry))
" "$OS_FW_VERSION" "$OS_FW_MINOR" "$date_str" "$changelog_text")

    # Merge into existing catalog or create new one
    if [[ -f "$VERSIONS_JSON" ]]; then
        python3 -c "
import json, sys
catalog = json.load(open(sys.argv[1]))
new_entry = json.loads(sys.argv[2])
# Remove existing entry with same version if present (rebuild case)
catalog = [e for e in catalog if not (e['fw_version'] == new_entry['fw_version'] and e['fw_minor'] == new_entry['fw_minor'])]
catalog.insert(0, new_entry)
# Sort newest first
catalog.sort(key=lambda e: (e['fw_version'], e['fw_minor']), reverse=True)
with open(sys.argv[1], 'w') as f:
    json.dump(catalog, f, indent=2)
" "$VERSIONS_JSON" "$new_entry"
    else
        python3 -c "
import json, sys
new_entry = json.loads(sys.argv[2])
with open(sys.argv[1], 'w') as f:
    json.dump([new_entry], f, indent=2)
" "$VERSIONS_JSON" "$new_entry"
    fi

    ok "versions.json catalog updated."
}

# Update manifest.json with new version and changelog entry
update_manifest() {
    local changelog_text="$1"
    local releases_url="https://github.com/${GITHUB_REPO}/releases"
    local prev_tag
    prev_tag=$(git tag -l 'v*' --sort=-version:refname | head -1)
    local prev_release_url=""
    if [[ -n "$prev_tag" ]]; then
        prev_release_url="https://github.com/${GITHUB_REPO}/releases/tag/${prev_tag}"
    fi
    cat > "$MANIFEST" <<EOFM
{
	"fw_version": ${OS_FW_VERSION},
	"fw_minor": ${OS_FW_MINOR},
	"zigbee_url": "https://opensprinklershop.de/upgrade/firmware_zigbee.bin",
	"matter_url": "https://opensprinklershop.de/upgrade/firmware_matter.bin",
	"esp8266_url": "https://opensprinklershop.de/upgrade/firmware_esp8266.bin",
	"versions_url": "https://opensprinklershop.de/upgrade/versions.json",
	"releases_url": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$releases_url"),
	"prev_release_url": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$prev_release_url"),
	"changelog": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$changelog_text")
}
EOFM
    ok "manifest.json updated."
}

# Update CHANGELOG.md header for the new release
update_changelog() {
    local version_str="$1"
    local today
    today=$(date +%Y-%m-%d)
    # Replace "unveröffentlicht" marker with release date
    if grep -q "unveröffentlicht" "$CHANGELOG"; then
        sed -i "s/unveröffentlicht[^)]*)/veröffentlicht ${today}/" "$CHANGELOG"
        ok "CHANGELOG.md updated with release date ${today}."
    else
        warn "No 'unveröffentlicht' marker found in CHANGELOG.md — skipping update."
    fi
}

# Extract the changelog text for the current version section
extract_changelog_section() {
    # Get text between the first ## heading and the next ## heading (or EOF)
    awk '/^## \[/{if(found) exit; found=1; next} found{print}' "$CHANGELOG" \
        | sed '/^$/d' | head -50
}

# Git commit, tag, and push
git_tag_and_push() {
    local tag="$1"
    local version_str="$2"

    header "Git: commit, tag & push"

    cd "$SCRIPT_DIR"

    git add "$DEFINES_H" "$MANIFEST" "$VERSIONS_JSON" "$CHANGELOG" \
        "${UPGRADE_DIR}/firmware_zigbee.bin" "${UPGRADE_DIR}/firmware_matter.bin" \
        "${UPGRADE_DIR}/firmware_esp8266.bin" "${UPGRADE_DIR}/archive/"

    git commit -m "Release ${version_str} (build ${OS_FW_MINOR})"
    ok "Committed."

    git tag -a "$tag" -m "Release ${version_str} (build ${OS_FW_MINOR})"
    ok "Tag created: ${tag}"

    git push origin HEAD
    git push origin "$tag"
    ok "Pushed to origin."
}

# Create a GitHub release via REST API
github_create_release() {
    local tag="$1"
    local version_str="$2"
    local notes="$3"

    header "GitHub: create release"

    local token="${GITHUB_TOKEN:-}"
    if [[ -z "$token" ]]; then
        warn "GITHUB_TOKEN not set — skipping GitHub release creation."
        warn "You can create it manually or re-run with:"
        warn "  GITHUB_TOKEN=<token> ./fw.sh release"
        return 0
    fi

    local zigbee_bin="${UPGRADE_DIR}/firmware_zigbee.bin"
    local matter_bin="${UPGRADE_DIR}/firmware_matter.bin"
    local release_name="v${version_str} (build ${OS_FW_MINOR})"

    # Create release
    local response
    response=$(curl -sf -X POST \
        -H "Authorization: token ${token}" \
        -H "Content-Type: application/json" \
        "https://api.github.com/repos/${GITHUB_REPO}/releases" \
        -d "$(python3 -c "
import json, sys
print(json.dumps({
    'tag_name': sys.argv[1],
    'name': sys.argv[2],
    'body': sys.argv[3],
    'draft': False,
    'prerelease': False
}))
" "$tag" "$release_name" "$notes")") || {
        error "Failed to create GitHub release."
        return 1
    }

    local upload_url
    upload_url=$(echo "$response" | python3 -c "import sys,json; print(json.load(sys.stdin)['upload_url'].replace('{?name,label}',''))")
    local release_url
    release_url=$(echo "$response" | python3 -c "import sys,json; print(json.load(sys.stdin)['html_url'])")
    ok "Release created: ${release_url}"

    # Upload assets
    for bin_name in "firmware_zigbee.bin" "firmware_matter.bin" "firmware_esp8266.bin"; do
        local bin_path="${UPGRADE_DIR}/${bin_name}"
        info "Uploading ${bin_name} …"
        curl -sf -X POST \
            -H "Authorization: token ${token}" \
            -H "Content-Type: application/octet-stream" \
            "${upload_url}?name=${bin_name}" \
            --data-binary "@${bin_path}" >/dev/null || {
            error "Failed to upload ${bin_name}"
            return 1
        }
        ok "Uploaded: ${bin_name}"
    done

    ok "GitHub release complete: ${release_url}"
}

# ── Main release workflow ────────────────────────────────────────────────────
# Usage: do_release [rebuild]
#   rebuild  — build only, no version bump, no git tag/release
do_release() {
    local mode="${1:-full}"
    local is_rebuild=false
    if [[ "$mode" == "rebuild" ]]; then
        is_rebuild=true
    fi

    if $is_rebuild; then
        header "OpenSprinkler Firmware Rebuild"
    else
        header "OpenSprinkler Firmware Release"
    fi
    check_pio

    # 1. Bump version (skip for rebuild)
    if $is_rebuild; then
        read_version
        info "Rebuild mode — keeping version OS_FW_VERSION=${OS_FW_VERSION}, OS_FW_MINOR=${OS_FW_MINOR}"
    else
        bump_minor_version
    fi
    local version_str
    version_str="$(format_version "$OS_FW_VERSION")"
    local tag="v${OS_FW_VERSION}_${OS_FW_MINOR}"

    if $is_rebuild; then
        info "Rebuilding: ${version_str} build ${OS_FW_MINOR}"
    else
        info "Releasing: ${version_str} build ${OS_FW_MINOR} (tag: ${tag})"
    fi

    # 2. Build ESP8266 firmware (without debug), then C5 variants.
    #    Two PlatformIO quirks we work around here:
    #    a) `pio run -e ENV --target clean` wipes the *entire* .pio/build/
    #       directory, not just that environment's subdirectory.  We therefore
    #       stash the ESP8266 binary in a temp file before running C5 builds so
    #       their per-environment cleans cannot destroy it.
    #    b) Restoring debug flags (modifying platformio.ini) between builds
    #       causes PlatformIO to detect the checksum change and implicitly clean
    #       all build directories.  We therefore keep platformio.ini unchanged
    #       throughout all three builds and restore only afterwards.
    disable_esp8266_debug
    trap 'restore_esp8266_debug' EXIT
    build_release_env "$ENV_ESP8266" || exit 1

    # Stash ESP8266 binary — C5 per-env cleans will wipe .pio/build/ entirely.
    local esp8266_stash
    esp8266_stash=$(mktemp --suffix=_esp8266_firmware.bin)
    cp "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}/firmware.bin" "$esp8266_stash"

    # 3. Build C5 firmware variants — platformio.ini unchanged (debug still
    #    disabled only in the esp8266 section, no effect on C5 build flags).
    build_release_env "$ENV_C5_ZIGBEE" || { rm -f "$esp8266_stash"; exit 1; }
    build_release_env "$ENV_C5_MATTER" || { rm -f "$esp8266_stash"; exit 1; }
    restore_esp8266_debug
    trap - EXIT

    # Restore ESP8266 binary that was wiped by C5 per-env cleans.
    mkdir -p "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}"
    cp "$esp8266_stash" "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}/firmware.bin"
    rm -f "$esp8266_stash"
    ok "ESP8266 firmware restored from stash."

    # 5. Archive previous version before overwriting
    archive_previous_version

    # 6. Copy binaries to upgrade directory
    copy_to_upgrade || exit 1

    # 7. Generate release notes & update manifest + versions catalog
    local changelog_section
    changelog_section=$(extract_changelog_section)
    local release_notes="Release ${version_str} (build ${OS_FW_MINOR})

Firmware binaries:
- firmware_zigbee.bin – ESP32-C5 ZigBee + BLE variant
- firmware_matter.bin – ESP32-C5 Matter + BLE variant
- firmware_esp8266.bin – ESP8266 (OS 3.x)

All releases: https://github.com/${GITHUB_REPO}/releases

${changelog_section}"

    update_manifest "$release_notes"
    update_versions_catalog "$release_notes"

    if ! $is_rebuild; then
        # 7. Update CHANGELOG.md
        update_changelog "$version_str"

        # 8. Git commit, tag, push
        git_tag_and_push "$tag" "$version_str"

        # 9. GitHub release (optional, requires GITHUB_TOKEN)
        github_create_release "$tag" "$version_str" "$release_notes"
    fi

    # 10. Summary
    if $is_rebuild; then
        header "Rebuild complete!"
    else
        header "Release complete!"
    fi
    echo ""
    ok "Version:  ${version_str} (build ${OS_FW_MINOR})"
    if ! $is_rebuild; then
        ok "Tag:      ${tag}"
    fi
    ok "Binaries:"
    ok "  ${UPGRADE_DIR}/firmware_zigbee.bin"
    ok "  ${UPGRADE_DIR}/firmware_matter.bin"
    ok "  ${UPGRADE_DIR}/firmware_esp8266.bin"
    echo ""
    info "Firmware v${version_str} build ${OS_FW_MINOR} is now available for OTA upgrade."
}

usage() {
    cat <<EOF
${BOLD}fw.sh – OpenSprinkler Firmware Manager${NC}

${BOLD}Usage:${NC}
  ./fw.sh <action> [variant]

${BOLD}Actions:${NC}
  build   [matter|zigbee|esp8266|all]  Build firmware (default: all)
  upload  [matter|zigbee|esp8266|all]  Upload firmware via USB (default: all)
  deploy  [matter|zigbee|esp8266|all]  Build + Upload (default: all)
  release                              Bump version, build all firmwares,
                                       copy to upgrade dir, git tag & push,
                                       create GitHub release
  release rebuild                      Rebuild all firmwares without version
                                       bump, no git tag/release — build only
  generate-mfg                         Generate Matter manufacturing data
                                       (test PAA → PAI → DAC chain + NVS binary)
  flash-mfg                            Flash matter_kvs partition to device
                                       (standalone, without firmware)
  switch <matter|zigbee|zigbee-client|disabled>
                                       Switch firmware via REST API + reboot
  status                               Show device status

${BOLD}Variants (build/upload/deploy):${NC}
  matter        ESP32-C5 Matter firmware  (OTA slot 1 @ 0x3A0000)
  zigbee        ESP32-C5 ZigBee firmware  (OTA slot 0 @ 0x10000)
  esp8266       OS 3.x ESP8266 firmware   (env: os3x_esp8266)
  all           All C5 firmwares (matter + zigbee)

${BOLD}Variants (switch – ESP32-C5 only):${NC}
  matter         → Matter mode
  zigbee         → ZigBee Gateway mode
  zigbee-client  → ZigBee Client mode
  disabled       → Disable IEEE 802.15.4 radio

${BOLD}Environment variables:${NC}
  OS_IP=<IP>              Device IP        (default: 192.168.0.151)
  OS_PASSWORD=<password>  Admin password   (will be MD5 hashed)
  OS_HASH=<md5>           MD5 hash direct  (overrides OS_PASSWORD)

${BOLD}Examples:${NC}
  ./fw.sh build
  ./fw.sh build matter
  ./fw.sh build esp8266
  ./fw.sh deploy zigbee
  ./fw.sh deploy esp8266
  ./fw.sh switch matter
  ./fw.sh switch zigbee
  ./fw.sh release
  ./fw.sh release rebuild
  ./fw.sh generate-mfg
  ./fw.sh flash-mfg
  OS_IP=192.168.0.151 OS_PASSWORD=opendoor ./fw.sh switch zigbee
  OS_IP=192.168.0.86  OS_HASH=a6d82bced638de3def1e9bbb4983225c ./fw.sh status

${BOLD}Environment variables (release):${NC}
  GITHUB_TOKEN=<token>    GitHub personal access token (optional, for release upload)
EOF
}

# ── Main logic ───────────────────────────────────────────────────────────────
ACTION="${1:-help}"
VARIANT="${2:-all}"

case "$ACTION" in
    build)
        check_pio
        case "$VARIANT" in
            matter)   build_env "$ENV_C5_MATTER" ;;
            zigbee)   build_env "$ENV_C5_ZIGBEE" ;;
            esp8266)  build_env "$ENV_ESP8266" ;;
            all|"")
                build_env "$ENV_C5_MATTER"
                build_env "$ENV_C5_ZIGBEE"
                header "All builds successful"
                ok "Matter:  .pio/build/${ENV_C5_MATTER}/firmware.bin"
                ok "ZigBee:  .pio/build/${ENV_C5_ZIGBEE}/firmware.bin"
                ;;
            *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|all)"; exit 1 ;;
        esac
        ;;

    upload)
        check_pio
        case "$VARIANT" in
            matter)   upload_env "$ENV_C5_MATTER" ;;
            zigbee)   upload_env "$ENV_C5_ZIGBEE" ;;
            esp8266)  upload_env "$ENV_ESP8266" ;;
            all|"")
                warn "Uploading both C5 firmwares requires the device to be reconnected/flashed TWICE."
                warn "For sequential upload, please run 'upload matter' then 'upload zigbee' separately."
                upload_env "$ENV_C5_MATTER"
                upload_env "$ENV_C5_ZIGBEE"
                ;;
            *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|all)"; exit 1 ;;
        esac
        ;;

    deploy)
        check_pio
        case "$VARIANT" in
            matter)
                build_env "$ENV_C5_MATTER"
                upload_env "$ENV_C5_MATTER"
                ;;
            zigbee)
                build_env "$ENV_C5_ZIGBEE"
                upload_env "$ENV_C5_ZIGBEE"
                ;;
            esp8266)
                build_env "$ENV_ESP8266"
                upload_env "$ENV_ESP8266"
                ;;
            all|"")
                build_env "$ENV_C5_MATTER"
                build_env "$ENV_C5_ZIGBEE"
                upload_env "$ENV_C5_MATTER"
                upload_env "$ENV_C5_ZIGBEE"
                header "Deploy complete"
                ok "ZigBee:  flashed to ota_0 (0x10000)"
                ok "Matter:  flashed to ota_1 (0x3A0000)"
                ;;
            *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|all)"; exit 1 ;;
        esac
        ;;

    release)
        case "$VARIANT" in
            rebuild)  do_release rebuild ;;
            all|"")   do_release full ;;
            *) error "Unknown release mode: $VARIANT (rebuild)"; exit 1 ;;
        esac
        ;;

    generate-mfg)
        generate_matter_mfg
        ;;

    flash-mfg)
        flash_matter_kvs
        ;;

    switch)
        switch_firmware "$VARIANT"
        ;;

    status)
        show_status
        ;;

    help|--help|-h|"")
        usage
        ;;

    *)
        error "Unknown action: $ACTION"
        usage
        exit 1
        ;;
esac
