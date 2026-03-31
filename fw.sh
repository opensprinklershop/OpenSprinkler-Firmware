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
#   full-flash [matter|zigbee|all]       – Full flash (bootloader + partitions + firmware)
#   switch  [matter|zigbee|mode]         – Switch firmware variant via REST API
#                                          (automatically reboots the device)
#   reset                                 – Reset/reboot device via USB (RTS/DTR)
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
#   ./fw.sh full-flash zigbee     -> flash bootloader + partitions + firmware
#   ./fw.sh full-flash matter     -> flash bootloader + partitions + firmware + matter_kvs
#   ERASE_FLASH=1 ./fw.sh full-flash zigbee  -> erase flash first, then full flash
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

# ── Load .env (gitignored local config) ──────────────────────────────────────
# Resolve SCRIPT_DIR first so we can find .env relative to this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${SCRIPT_DIR}/.env" ]]; then
    # Export only lines that look like VAR=value (skip comments and blanks).
    # Variables already set in the environment take precedence.
    while IFS= read -r _line || [[ -n "$_line" ]]; do
        [[ "$_line" =~ ^[[:space:]]*# ]] && continue
        [[ -z "${_line// }" ]] && continue
        if [[ "$_line" =~ ^([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]]; then
            _key="${BASH_REMATCH[1]}"
            _val="${BASH_REMATCH[2]}"
            # Only set if not already exported from the calling environment
            [[ -z "${!_key+x}" ]] && export "${_key}=${_val}"
        fi
    done < "${SCRIPT_DIR}/.env"
    unset _line _key _val
fi

# ── Configuration ────────────────────────────────────────────────────────────
DEVICE_IP="${OS_IP:-192.168.0.151}"
PIO_BIN="platformio"

# ── OsPi remote build configuration ─────────────────────────────────────────
# Set OSPI_PI_PASS in .env, or configure SSH key auth (ssh-copy-id)
OSPI_PI_HOST="${OSPI_PI_HOST:-192.168.0.167}"
OSPI_PI_USER="${OSPI_PI_USER:-pi}"
OSPI_PI_PASS="${OSPI_PI_PASS:-}"
OSPI_PI_DIR="${OSPI_PI_DIR:-/home/pi/OpenSprinkler-Firmware}"
DEFINES_H="${SCRIPT_DIR}/defines.h"
UPGRADE_DIR="${OS_UPGRADE_DIR:-/srv/www/htdocs/upgrade}"
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

copy_to_dist() {
    local env="$1"
    local src="${SCRIPT_DIR}/.pio/build/${env}/firmware.bin"
    local dist="${SCRIPT_DIR}/.pio/dist"
    if [[ -f "$src" ]]; then
        mkdir -p "$dist"
        cp "$src" "${dist}/firmware_${env}.bin"
        ok "Dist copy → .pio/dist/firmware_${env}.bin"
    fi
}

# ── OsPi (Raspberry Pi) remote build helpers ────────────────────────────────

# Verify sshpass is available when a password is configured.
check_ospi_conn() {
    if [[ -n "$OSPI_PI_PASS" ]]; then
        if ! command -v sshpass &>/dev/null; then
            error "sshpass not found (needed when OSPI_PI_PASS is set)."
            error "Install: sudo apt install sshpass"
            error "Or use SSH key auth: ssh-copy-id ${OSPI_PI_USER}@${OSPI_PI_HOST}"
            exit 1
        fi
    fi
    if ! command -v rsync &>/dev/null; then
        error "rsync not found. Install: sudo apt install rsync"
        exit 1
    fi
}

# Run a command on the Pi as root via sudo.
_ospi_ssh() {
    if [[ -n "$OSPI_PI_PASS" ]]; then
        sshpass -p "$OSPI_PI_PASS" \
            ssh -o StrictHostKeyChecking=no -o BatchMode=no \
            "${OSPI_PI_USER}@${OSPI_PI_HOST}" \
            "sudo bash -c $(printf '%q' "$*")"
    else
        ssh -o StrictHostKeyChecking=no \
            "${OSPI_PI_USER}@${OSPI_PI_HOST}" \
            "sudo bash -c $(printf '%q' "$*")"
    fi
}

# rsync wrapper that injects the password via sshpass when set.
_ospi_rsync() {
    local ssh_cmd="ssh -o StrictHostKeyChecking=no"
    if [[ -n "$OSPI_PI_PASS" ]]; then
        SSHPASS="$OSPI_PI_PASS" sshpass -e rsync -e "$ssh_cmd" "$@"
    else
        rsync -e "$ssh_cmd" "$@"
    fi
}

# Rsync-exclude list shared by push and pull operations.
_OSPI_RSYNC_EXCLUDES=(
    --exclude='.git/'
    --exclude='.pio/'
    --exclude='node_modules/'
    --exclude='dist/'
    --exclude='managed_components/'
    --exclude='OpenSprinkler'       # compiled binary
    --exclude='*.o'
    --exclude='build_log.txt'
    --exclude='upload.log'
    --exclude='test.log'
)

# Push local workspace to the Pi.
ospi_push() {
    info "Syncing source → ${OSPI_PI_HOST}:${OSPI_PI_DIR} …"
    _ospi_rsync -az --info=progress2 \
        "${_OSPI_RSYNC_EXCLUDES[@]}" \
        "${SCRIPT_DIR}/" \
        "${OSPI_PI_USER}@${OSPI_PI_HOST}:${OSPI_PI_DIR}/"
    ok "Source synced to Pi."
}

# Pull Pi workspace back to local (to capture bug fixes made on the device).
ospi_pull_back() {
    info "Syncing ${OSPI_PI_HOST}:${OSPI_PI_DIR} → local …"
    _ospi_rsync -az --info=progress2 \
        "${_OSPI_RSYNC_EXCLUDES[@]}" \
        "${OSPI_PI_USER}@${OSPI_PI_HOST}:${OSPI_PI_DIR}/" \
        "${SCRIPT_DIR}/"
    ok "Sync-back complete. Review changes with: git diff"
}

# Build only: push source and compile via build2.sh.
build_ospi() {
    header "OsPi – remote build (${OSPI_PI_HOST})"
    check_ospi_conn
    ospi_push
    info "Running build2.sh on Pi …"
    _ospi_ssh "cd '${OSPI_PI_DIR}' && ./build2.sh"
    ok "OsPi build complete."
}

# Full deploy: clean Pi working tree, then run updater.sh (git pull + build +
# service install).  Any files previously pushed via ospi_push must be
# removed / stashed first so that 'git pull' inside updater.sh succeeds.
deploy_ospi() {
    header "OsPi – remote deploy (${OSPI_PI_HOST})"
    check_ospi_conn

    info "Cleaning Pi working tree to allow git pull …"
    _ospi_ssh "
        cd '${OSPI_PI_DIR}'
        git reset --hard HEAD 2>/dev/null || true
        git clean -fd         2>/dev/null || true
        git submodule foreach --recursive 'git reset --hard HEAD; git clean -fd' 2>/dev/null || true
    "
    ok "Pi working tree clean."

    info "Running ./updater.sh on Pi (git pull + build + service restart) …"
    _ospi_ssh "cd '${OSPI_PI_DIR}' && ./updater.sh"
    ok "OsPi deploy complete."
}

build_env() {
    local env="$1"
    header "Building firmware: ${env}"
    if "$PIO_BIN" run --environment "$env"; then
        ok "Build successful → .pio/build/${env}/firmware.bin"
        copy_to_dist "$env"
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

# ── esptool resolver ────────────────────────────────────────────────────────
# Prefer system esptool (>= 4.6 required for ESP32-C5 support) over the
# outdated version bundled by PlatformIO in .pio/packages/tool-esptoolpy/.
# Sets global ESPTOOL_CMD to the invocation string (e.g. "python3 -m esptool").
_find_esptool() {
    # 1. system esptool binary
    if command -v esptool.py &>/dev/null; then
        ESPTOOL_CMD=(esptool.py); return 0
    fi
    # 2. python3 -m esptool (pip-installed)
    if python3 -m esptool version &>/dev/null 2>&1; then
        ESPTOOL_CMD=(python3 -m esptool); return 0
    fi
    # 3. PlatformIO bundled (last resort — may be too old)
    local pio_tool="${SCRIPT_DIR}/.pio/packages/tool-esptoolpy/esptool.py"
    local pio_py="${HOME}/.platformio/penv/bin/python3"
    [[ ! -x "$pio_py" ]] && pio_py="python3"
    if [[ -f "$pio_tool" ]]; then
        warn "Using PlatformIO-bundled esptool — may not support ESP32-C5. Upgrade with: pip install -U esptool"
        ESPTOOL_CMD=("$pio_py" "$pio_tool"); return 0
    fi
    error "esptool not found. Install with: pip install esptool"
    exit 1
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

    _find_esptool

    local port="${UPLOAD_PORT:-/dev/ttyACM0}"
    local baud="${UPLOAD_SPEED:-460800}"

    info "Flashing ${MATTER_KVS_BIN} → ${MATTER_KVS_ADDR}"
    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
        --before default_reset --after hard_reset \
        write_flash "$MATTER_KVS_ADDR" "$MATTER_KVS_BIN" \
    || { error "Flash failed"; exit 1; }

    ok "Matter KVS partition flashed at ${MATTER_KVS_ADDR}"
}

# ── Full flash (bootloader + partition table + firmware) ─────────────────────
# ESP32-C5 flash layout addresses
BOOTLOADER_ADDR="0x2000"
PARTITION_TABLE_ADDR="0x8000"
FW_ADDR_ZIGBEE="0x10000"
FW_ADDR_MATTER="0x3A0000"

full_flash_env() {
    local env="$1"
    local fw_addr="$2"
    header "Full Flash: ${env}"

    local build_dir="${SCRIPT_DIR}/.pio/build/${env}"
    local bootloader="${build_dir}/bootloader.bin"
    local partitions="${build_dir}/partitions.bin"
    local firmware="${build_dir}/firmware.bin"

    # Build if artifacts are missing
    if [[ ! -f "$bootloader" ]] || [[ ! -f "$partitions" ]] || [[ ! -f "$firmware" ]]; then
        warn "Build artifacts missing — building first …"
        build_env "$env"
    fi

    # Validate all artifacts exist
    for f in "$bootloader" "$partitions" "$firmware"; do
        if [[ ! -f "$f" ]]; then
            error "Missing: $f"
            exit 1
        fi
    done

    _find_esptool

    local port="${UPLOAD_PORT:-/dev/ttyUSB2}"
    local baud="${UPLOAD_SPEED:-460800}"

    # Build flash arguments: bootloader + partitions + firmware
    local flash_args=(
        "$BOOTLOADER_ADDR"      "$bootloader"
        "$PARTITION_TABLE_ADDR"  "$partitions"
        "$fw_addr"               "$firmware"
    )

    # For matter, also flash matter_kvs if available
    if [[ "$env" == "$ENV_C5_MATTER" ]] && [[ -f "$MATTER_KVS_BIN" ]]; then
        flash_args+=("$MATTER_KVS_ADDR" "$MATTER_KVS_BIN")
        info "Including Matter KVS partition"
    fi

    info "Port: ${port}  Baud: ${baud}"
    info "Bootloader  → ${BOOTLOADER_ADDR}"
    info "Partitions  → ${PARTITION_TABLE_ADDR}"
    info "Firmware    → ${fw_addr}"

    # Optional: erase entire flash first (ERASE_FLASH=1 ./fw.sh full-flash zigbee)
    if [[ "${ERASE_FLASH:-0}" == "1" ]]; then
        warn "Erasing entire flash first …"
        "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
            erase_flash \
        || { error "Erase failed"; exit 1; }
        ok "Flash erased"
    fi

    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
        --before default_reset --after hard_reset \
        write_flash "${flash_args[@]}" \
    || { error "Full flash failed"; exit 1; }

    # Erase RainMaker factory credentials and NVS to ensure clean state.
    # fctry partition (0x7EA000, 0x6000): RainMaker node_id, certs, claiming data
    # nvs   partition (0x9000,   0x5000): user mapping, WiFi creds, runtime state
    info "Erasing RainMaker fctry partition (0x7EA000, 24K) …"
    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
        --before default_reset --after no_reset \
        erase_region 0x7EA000 0x6000 \
    || warn "fctry erase failed (non-fatal)"

    info "Erasing NVS partition (0x9000, 20K) …"
    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
        --before default_reset --after hard_reset \
        erase_region 0x9000 0x5000 \
    || warn "NVS erase failed (non-fatal)"

    ok "Full flash successful: ${env}"
}

# ── USB reset ───────────────────────────────────────────────────────────────
# Trigger a hardware reset on the connected ESP32-C5 via RTS/DTR signalling.
# esptool's "run" command pulses DTR/RTS exactly as it does before flashing,
# so no data is written to flash — the chip is simply rebooted.
do_reset() {
    header "USB Reset"
    _find_esptool

    local port="${UPLOAD_PORT:-/dev/ttyUSB2}"
    info "Resetting device on ${port} …"
    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" \
        --before default_reset --after hard_reset \
        run \
    || { error "Reset failed — is the device connected on ${port}?"; exit 1; }
    ok "Device reset."
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

# Disable ENABLE_DEBUG / ENABLE_MEMORY_DEBUG in all release environments.
# Currently the C5 variants (zigbee + matter) carry these flags; the ESP8266
# section does not, but the function is written to handle it as well should
# debug flags be added there in the future.
disable_release_debug() {
    info "Disabling debug flags for release builds …"
    for env_name in "esp32-c5-zigbee" "esp32-c5-matter" "os3x_esp8266"; do
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\)-D ENABLE_DEBUG\s*\$/\1;-D ENABLE_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\)-D ENABLE_MEMORY_DEBUG\s*\$/\1;-D ENABLE_MEMORY_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\)-DENABLE_DEBUG\s*\$/\1;-DENABLE_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\)-DENABLE_MEMORY_DEBUG\s*\$/\1;-DENABLE_MEMORY_DEBUG/}" "$PLATFORMIO_INI"
    done
    ok "Debug flags disabled in all release environments."
}

# Restore ENABLE_DEBUG / ENABLE_MEMORY_DEBUG in all release environments.
restore_release_debug() {
    info "Restoring debug flags …"
    for env_name in "esp32-c5-zigbee" "esp32-c5-matter" "os3x_esp8266"; do
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\);-D ENABLE_DEBUG\s*\$/\1-D ENABLE_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\);-D ENABLE_MEMORY_DEBUG\s*\$/\1-D ENABLE_MEMORY_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\);-DENABLE_DEBUG\s*\$/\1-DENABLE_DEBUG/}" "$PLATFORMIO_INI"
        sed -i "/^\[env:${env_name}\]/,/^\[env:/{s/^\(\s*\);-DENABLE_MEMORY_DEBUG\s*\$/\1-DENABLE_MEMORY_DEBUG/}" "$PLATFORMIO_INI"
    done
    ok "Debug flags restored in all environments."
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
    copy_to_dist "$env"
}

# Copy firmware binaries to upgrade directory
copy_to_upgrade() {
    mkdir -p "$UPGRADE_DIR"
    # Use .pio/dist/ copies (which survive per-env build-dir cleans) rather than
    # .pio/build/<env>/firmware.bin which can disappear when platformio.ini is
    # modified or when a subsequent build env wipes the shared build directory.
    local zigbee_bin="${SCRIPT_DIR}/.pio/dist/firmware_${ENV_C5_ZIGBEE}.bin"
    local matter_bin="${SCRIPT_DIR}/.pio/dist/firmware_${ENV_C5_MATTER}.bin"
    local esp8266_bin="${SCRIPT_DIR}/.pio/dist/firmware_${ENV_ESP8266}.bin"

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
    sync_manifest_sha256
}

# Copy a single variant's firmware to upgrade directory (used by single-variant deploy)
copy_one_to_upgrade() {
    local env="$1"
    local dest_name="$2"
    mkdir -p "$UPGRADE_DIR"
    local src="${SCRIPT_DIR}/.pio/dist/firmware_${env}.bin"
    if [[ ! -f "$src" ]]; then
        error "Binary not found: ${src}"
        return 1
    fi
    cp "$src" "${UPGRADE_DIR}/${dest_name}"
    ok "upgrade/${dest_name} synced with current build"
    sync_manifest_sha256
}

# Recompute SHA-256 of whichever firmware binaries are present in upgrade/ and
# patch only the *_sha256 fields in manifest.json — all other fields are unchanged.
# Safe to call after every deploy so manifest always matches the served files.
sync_manifest_sha256() {
    if [[ ! -f "$MANIFEST" ]]; then
        return 0  # nothing to update
    fi
    python3 - "$MANIFEST" \
        "${UPGRADE_DIR}/firmware_zigbee.bin" \
        "${UPGRADE_DIR}/firmware_matter.bin" \
        "${UPGRADE_DIR}/firmware_esp8266.bin" <<'PYEOF'
import json, hashlib, sys, os

manifest_path = sys.argv[1]
bins = {"zigbee": sys.argv[2], "matter": sys.argv[3], "esp8266": sys.argv[4]}

def sha256file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

d = json.load(open(manifest_path))
changed = []
for variant, path in bins.items():
    key = f"{variant}_sha256"
    if os.path.isfile(path):
        new_hash = sha256file(path)
        if d.get(key) != new_hash:
            d[key] = new_hash
            changed.append(f"{key}={new_hash[:16]}…")
if changed:
    with open(manifest_path, "w") as f:
        json.dump(d, f, indent=4, ensure_ascii=False)
    print("  manifest.json SHA-256 updated:", ", ".join(changed))
PYEOF
    # Reload Squid config (cache bypass for /upgrade/ is enforced server-side via ACL + .htaccess no-cache)
    if command -v squid &>/dev/null; then
        sudo systemctl reload squid 2>/dev/null && info "Squid reloaded" || true
    fi
}

# Archive the previous release binaries into a versioned subdirectory
archive_previous_version() {
    if [[ ! -f "$MANIFEST" ]]; then
        info "No existing manifest.json — skipping archive of previous version."
        return 0
    fi

    local prev_version prev_minor
    prev_version=$(python3 -c "
import json
try:
    d=json.load(open('$MANIFEST')); print(d.get('fw_version',0))
except Exception:
    print(0)
")
    prev_minor=$(python3 -c "
import json
try:
    d=json.load(open('$MANIFEST')); print(d.get('fw_minor',0))
except Exception:
    print(0)
")

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

# Archive the current (just-built) release into its own versioned subdirectory.
# Must be called AFTER copy_to_upgrade and update_manifest so the binaries and
# manifest in upgrade/ are already the authoritative, consistent post-build files.
archive_current_version() {
    local archive_dir="${UPGRADE_DIR}/archive/v${OS_FW_VERSION}_${OS_FW_MINOR}"
    mkdir -p "$archive_dir"

    for bin_name in firmware_zigbee.bin firmware_matter.bin firmware_esp8266.bin; do
        if [[ -f "${UPGRADE_DIR}/${bin_name}" ]]; then
            cp "${UPGRADE_DIR}/${bin_name}" "${archive_dir}/${bin_name}"
        fi
    done
    cp "$MANIFEST" "${archive_dir}/manifest.json"

    ok "Current version v${OS_FW_VERSION} (build ${OS_FW_MINOR}) self-archived to ${archive_dir}/"
}

# Update (or create) the versions.json catalog with all known releases
update_versions_catalog() {
    local changelog_text="$1"
    local date_str
    date_str=$(date +%Y-%m-%d)

    # Compute SHA-256 checksums for this release
    local zigbee_sha256 matter_sha256 esp8266_sha256
    zigbee_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_zigbee.bin" | awk '{print $1}')
    matter_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_matter.bin" | awk '{print $1}')
    esp8266_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_esp8266.bin" | awk '{print $1}')

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
    'zigbee_sha256': sys.argv[5],
    'matter_sha256': sys.argv[6],
    'esp8266_sha256': sys.argv[7],
    'changelog': sys.argv[4]
}
print(json.dumps(entry))
" "$OS_FW_VERSION" "$OS_FW_MINOR" "$date_str" "$changelog_text" "$zigbee_sha256" "$matter_sha256" "$esp8266_sha256")

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
    # Compute SHA-256 checksums for integrity verification
    local zigbee_sha256 matter_sha256 esp8266_sha256
    zigbee_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_zigbee.bin" | awk '{print $1}')
    matter_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_matter.bin" | awk '{print $1}')
    esp8266_sha256=$(sha256sum "${UPGRADE_DIR}/firmware_esp8266.bin" | awk '{print $1}')
    cat > "$MANIFEST" <<EOFM
{
	"fw_version": ${OS_FW_VERSION},
	"fw_minor": ${OS_FW_MINOR},
	"zigbee_url": "https://opensprinklershop.de/upgrade/firmware_zigbee.bin",
	"matter_url": "https://opensprinklershop.de/upgrade/firmware_matter.bin",
	"esp8266_url": "https://opensprinklershop.de/upgrade/firmware_esp8266.bin",
	"zigbee_sha256": "${zigbee_sha256}",
	"matter_sha256": "${matter_sha256}",
	"esp8266_sha256": "${esp8266_sha256}",
	"versions_url": "https://opensprinklershop.de/upgrade/versions.json",
	"releases_url": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$releases_url"),
	"prev_release_url": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$prev_release_url"),
	"changelog": $(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$changelog_text")
}
EOFM
    ok "manifest.json updated (with SHA-256)."
    # Reload Squid config (cache bypass for /upgrade/ is enforced server-side via ACL + .htaccess no-cache)
    if command -v squid &>/dev/null; then
        sudo systemctl reload squid 2>/dev/null && info "Squid reloaded" || true
    fi
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

    # 2. Build all firmware variants WITHOUT debug flags.
    #    Two PlatformIO quirks we work around here:
    #    a) `pio run -e ENV --target clean` wipes the *entire* .pio/build/
    #       directory, not just that environment's subdirectory.  We therefore
    #       stash the ESP8266 binary in a temp file before running C5 builds so
    #       their per-environment cleans cannot destroy it.
    #    b) Restoring debug flags (modifying platformio.ini) between builds
    #       causes PlatformIO to detect the checksum change and implicitly clean
    #       all build directories.  We therefore keep platformio.ini unchanged
    #       throughout all three builds and restore only afterwards.
    disable_release_debug
    trap 'restore_release_debug' EXIT
    build_release_env "$ENV_ESP8266" || exit 1

    # Stash ESP8266 binary — C5 per-env cleans will wipe .pio/build/ entirely.
    local esp8266_stash
    esp8266_stash=$(mktemp --suffix=_esp8266_firmware.bin)
    cp "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}/firmware.bin" "$esp8266_stash"

    # 3. Build C5 firmware variants — platformio.ini unchanged (debug still
    #    disabled only in the esp8266 section, no effect on C5 build flags).
    build_release_env "$ENV_C5_ZIGBEE" || { rm -f "$esp8266_stash"; exit 1; }

    # Stash ZigBee binary — C5 matter build will wipe .pio/build/ entirely.
    local zigbee_stash
    zigbee_stash=$(mktemp --suffix=_zigbee_firmware.bin)
    cp "${SCRIPT_DIR}/.pio/build/${ENV_C5_ZIGBEE}/firmware.bin" "$zigbee_stash"

    build_release_env "$ENV_C5_MATTER" || { rm -f "$esp8266_stash" "$zigbee_stash"; exit 1; }
    restore_release_debug
    trap - EXIT

    # Restore ESP8266 and ZigBee binaries that were wiped by per-env cleans.
    mkdir -p "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}"
    cp "$esp8266_stash" "${SCRIPT_DIR}/.pio/build/${ENV_ESP8266}/firmware.bin"
    rm -f "$esp8266_stash"
    mkdir -p "${SCRIPT_DIR}/.pio/build/${ENV_C5_ZIGBEE}"
    cp "$zigbee_stash" "${SCRIPT_DIR}/.pio/build/${ENV_C5_ZIGBEE}/firmware.bin"
    rm -f "$zigbee_stash"
    ok "ESP8266 and ZigBee firmwares restored from stash."

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
    # Ensure the archive directory for this version contains the same consistent
    # post-build binaries and manifest that are now in upgrade/.  This must run
    # AFTER update_manifest so the archive manifest carries the correct SHA-256.
    archive_current_version
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
  build   [matter|zigbee|esp8266|ospi|all]  Build firmware (default: all)
  upload  [matter|zigbee|esp8266|all]       Upload firmware via USB (default: all)
  deploy  [matter|zigbee|esp8266|ospi|all]  Build + Upload/Deploy (default: all)
  sync-back                                 Pull OsPi changes back to local workspace
  release                                   Bump version, build all firmwares,
                                            copy to upgrade dir, git tag & push,
                                            create GitHub release
  release rebuild                           Rebuild all firmwares without version
                                            bump, no git tag/release — build only
  generate-mfg                              Generate Matter manufacturing data
                                            (test PAA → PAI → DAC chain + NVS binary)
  flash-mfg                                 Flash matter_kvs partition to device
                                            (standalone, without firmware)
  full-flash [matter|zigbee|all]            Full flash: bootloader + partition table
                                            + firmware via esptool (ESP32-C5 only)
  reset                                     Reset/reboot device via USB (RTS/DTR)
  switch <matter|zigbee|zigbee-client|disabled>
                                            Switch firmware via REST API + reboot
  status                                    Show device status

${BOLD}Variants (build/upload/deploy):${NC}
  matter        ESP32-C5 Matter firmware  (OTA slot 1 @ 0x3A0000)
  zigbee        ESP32-C5 ZigBee firmware  (OTA slot 0 @ 0x10000)
  esp8266       OS 3.x ESP8266 firmware   (env: os3x_esp8266)
  ospi          OsPi / Raspberry Pi firmware — remote build (see OsPi vars below)
  all           All firmwares (matter + zigbee + esp8266, not ospi)

${BOLD}OsPi workflow:${NC}
  build ospi        Push local source to Pi, compile with build2.sh (dev/test)
  deploy ospi       Clean Pi working tree, git pull + build + service restart
  sync-back         Pull Pi changes (bug fixes) back to local workspace

${BOLD}Variants (switch – ESP32-C5 only):${NC}
  matter         → Matter mode
  zigbee         → ZigBee Gateway mode
  zigbee-client  → ZigBee Client mode
  disabled       → Disable IEEE 802.15.4 radio

${BOLD}Environment variables:${NC}
  OS_IP=<IP>              Device IP        (default: 192.168.0.151)
  OS_PASSWORD=<password>  Admin password   (will be MD5 hashed)
  OS_HASH=<md5>           MD5 hash direct  (overrides OS_PASSWORD)
  UPLOAD_PORT=<port>      Serial port      (default: /dev/ttyUSB2)
  UPLOAD_SPEED=<baud>     Upload baud rate  (default: 460800)
  ERASE_FLASH=1           Erase entire flash before full-flash

${BOLD}OsPi environment variables:${NC}
  OSPI_PI_HOST=<IP>       OsPi host        (default: 192.168.0.167)
  OSPI_PI_USER=<user>     OsPi SSH user    (default: pi)
  OSPI_PI_PASS=<password> OsPi SSH password (leave empty to use SSH keys)
  OSPI_PI_DIR=<path>      OsPi firmware dir (default: /home/pi/OpenSprinkler-Firmware)

${BOLD}Examples:${NC}
  ./fw.sh build
  ./fw.sh build matter
  ./fw.sh build esp8266
  ./fw.sh build ospi
  ./fw.sh deploy zigbee
  ./fw.sh deploy ospi
  ./fw.sh sync-back
  ./fw.sh switch matter
  ./fw.sh switch zigbee
  ./fw.sh release
  ./fw.sh release rebuild
  ./fw.sh generate-mfg
  ./fw.sh flash-mfg
  ./fw.sh full-flash zigbee
  ./fw.sh full-flash matter
  ERASE_FLASH=1 ./fw.sh full-flash zigbee
  ./fw.sh reset
  UPLOAD_PORT=/dev/ttyACM0 ./fw.sh reset
  OS_IP=192.168.0.151 OS_PASSWORD=opendoor ./fw.sh switch zigbee
  OS_IP=192.168.0.86  OS_HASH=a6d82bced638de3def1e9bbb4983225c ./fw.sh status
  OSPI_PI_PASS=secret ./fw.sh build ospi
  OSPI_PI_PASS=secret ./fw.sh deploy ospi

${BOLD}Environment variables (release):${NC}
  GITHUB_TOKEN=<token>    GitHub personal access token (optional, for release upload)
EOF
}

# ── Main logic ───────────────────────────────────────────────────────────────
ACTION="${1:-help}"
VARIANT="${2:-all}"

case "$ACTION" in
    build)
        case "$VARIANT" in
            ospi)     build_ospi ;;
            *)
                check_pio
                case "$VARIANT" in
                matter)   build_env "$ENV_C5_MATTER" ;;
                zigbee)   build_env "$ENV_C5_ZIGBEE" ;;
                esp8266)  build_env "$ENV_ESP8266" ;;
                all|"")
                    build_env "$ENV_C5_MATTER"
                    build_env "$ENV_C5_ZIGBEE"
                    build_env "$ENV_ESP8266"
                    header "All builds successful"
                    ok "Matter:   .pio/build/${ENV_C5_MATTER}/firmware.bin"
                    ok "ZigBee:   .pio/build/${ENV_C5_ZIGBEE}/firmware.bin"
                    ok "ESP8266:  .pio/build/${ENV_ESP8266}/firmware.bin"
                    ;;
                *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|ospi|all)"; exit 1 ;;
                esac
                ;;
        esac
        ;;

    upload)
        check_pio
        case "$VARIANT" in
            matter)   upload_env "$ENV_C5_MATTER" ;;
            zigbee)   upload_env "$ENV_C5_ZIGBEE" ;;
            esp8266)  upload_env "$ENV_ESP8266" ;;
            all|"")
                warn "Uploading all firmwares requires switching between devices."
                upload_env "$ENV_C5_MATTER"
                upload_env "$ENV_C5_ZIGBEE"
                upload_env "$ENV_ESP8266"
                ;;
            *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|all)"; exit 1 ;;
        esac
        ;;

    deploy)
        case "$VARIANT" in
            ospi)
                deploy_ospi
                ;;
            *)
                check_pio
                case "$VARIANT" in
                matter)
                    build_env "$ENV_C5_MATTER"
                    copy_one_to_upgrade "$ENV_C5_MATTER" "firmware_matter.bin"
                    upload_env "$ENV_C5_MATTER"
                    ;;
                zigbee)
                    build_env "$ENV_C5_ZIGBEE"
                    copy_one_to_upgrade "$ENV_C5_ZIGBEE" "firmware_zigbee.bin"
                    upload_env "$ENV_C5_ZIGBEE"
                    ;;
                esp8266)
                    build_env "$ENV_ESP8266"
                    copy_one_to_upgrade "$ENV_ESP8266" "firmware_esp8266.bin"
                    upload_env "$ENV_ESP8266"
                    ;;
                all|"")
                    build_env "$ENV_C5_MATTER"
                    build_env "$ENV_C5_ZIGBEE"
                    build_env "$ENV_ESP8266"
                    copy_to_upgrade
                    upload_env "$ENV_C5_MATTER"
                    upload_env "$ENV_C5_ZIGBEE"
                    upload_env "$ENV_ESP8266"
                    header "Deploy complete"
                    ok "Matter:   flashed to ota_1 (0x3A0000)"
                    ok "ZigBee:   flashed to ota_0 (0x10000)"
                    ok "ESP8266:  flashed"
                    ok "upgrade/  synced with current build"
                    ;;
                *) error "Unknown variant: $VARIANT (matter|zigbee|esp8266|ospi|all)"; exit 1 ;;
                esac
                ;;
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

    full-flash)
        check_pio
        case "$VARIANT" in
            matter)
                full_flash_env "$ENV_C5_MATTER" "$FW_ADDR_MATTER"
                ;;
            zigbee)
                full_flash_env "$ENV_C5_ZIGBEE" "$FW_ADDR_ZIGBEE"
                ;;
            all|"")
                full_flash_env "$ENV_C5_ZIGBEE" "$FW_ADDR_ZIGBEE"
                full_flash_env "$ENV_C5_MATTER" "$FW_ADDR_MATTER"
                header "Full flash complete (both variants)"
                ;;
            *) error "Unknown variant: $VARIANT (matter|zigbee|all)"; exit 1 ;;
        esac
        ;;

    reset)
        do_reset
        ;;

    switch)
        switch_firmware "$VARIANT"
        ;;

    sync-back)
        # Pull changes made directly on the OsPi back to the local workspace
        # (e.g., compiler-error fixes applied on the device).
        check_ospi_conn
        ospi_pull_back
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
