#!/usr/bin/env bash
# =============================================================================
# fw.sh – OpenSprinkler Firmware Manager
#
# Actions:
#   build   [matter|zigbee|esp8266|all]  – Build firmware
#   upload  [matter|zigbee|esp8266|all]  – Upload firmware (IP/REST if reachable, USB fallback)
#   deploy  [matter|zigbee|esp8266|all] [debug|monitor]  – Build + Upload + (optionally) Show live serial monitor
#   release [rebuild]                    – Bump version, release build, tag & publish
#                                          rebuild: build only, no version bump/git
#   release sync-tags                    – Create missing GitHub releases for existing tags
#   libs    [rebuild|deploy|rebuild-copy|deploy-copy]
#                                        – Rebuild/deploy custom ESP32 Arduino libs
#   online-deploy                        – Upload current upgrade/ tree to IONOS only
#   generate-mfg                         – Generate Matter manufacturing data (DAC, NVS)
#   flash-mfg                            – Flash matter_kvs partition only
#   full-flash [matter|zigbee|all]       – Full flash new device using pre-built binaries
#                                          (no compilation – always uses current release)
#   switch  [matter|zigbee|mode]         – Switch firmware variant via REST API
#                                          (automatically reboots the device)
#   install-ip [matter|zigbee|esp8266] [firmware.bin]
#                                        – Upload/install firmware via device IP (REST)
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
#   ./fw.sh deploy debug
#   ./fw.sh deploy zigbee debug
#   ./fw.sh deploy zigbee monitor   -> Build + Upload + Show live serial monitor + save logs to /tmp/zigbee_monitor_zigbee.log
#   ./fw.sh deploy matter monitor   -> Build + Upload + Show live serial monitor + save logs to /tmp/zigbee_monitor_matter.log
#                                      (View logs via MCP: get_monitor_log variant=zigbee)
#   ./fw.sh release
#   ./fw.sh release rebuild
#   ./fw.sh libs rebuild
#   ./fw.sh libs deploy
#   ./fw.sh generate-mfg
#   ./fw.sh flash-mfg
#   ./fw.sh full-flash zigbee     -> flash pre-built bootloader + partitions + firmware (no compile)
#   ./fw.sh full-flash matter     -> flash pre-built bootloader + partitions + firmware + matter_kvs
#                                      (auto-generates matter_kvs if missing)
#   ERASE_FLASH=1 ./fw.sh full-flash zigbee  -> erase flash first, then full flash
#   ./fw.sh switch matter        -> switch to Matter firmware
#   ./fw.sh switch zigbee        -> switch to ZigBee Gateway firmware
#   ./fw.sh switch zigbee-client -> switch to ZigBee Client firmware
#   ./fw.sh switch disabled      -> disable IEEE 802.15.4 radio
#   OS_IP=192.168.0.59 ./fw.sh install-ip zigbee
#   OS_IP=192.168.0.59 ./fw.sh install-ip matter /data/upgrade/firmware_matter.bin
#   FW_UPLOAD_METHOD=usb ./fw.sh deploy zigbee    -> force USB/serial upload
#   FW_UPLOAD_METHOD=ip  ./fw.sh upload zigbee    -> force IP/REST upload
#
# Environment variables (optional):
#   OS_IP           Device IP  (default: 192.168.0.151)
#   OS_PASSWORD     Admin password in plain text (will be MD5 hashed)
#   OS_HASH         Admin password already as MD5 hash
#   MONITOR_SPEED   Serial monitor baud rate (default: 115200)
#   MONITOR_LOGS    Enable saving monitor logs to /tmp/ (automatic with 'monitor' flag)
#   FW_UPLOAD_METHOD auto|ip|usb (default: auto; auto prefers IP for a reachable device)
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
SILENT="${SILENT:-true}"

# ── OsPi remote build configuration ─────────────────────────────────────────
# Set OSPI_PI_PASS in .env, or configure SSH key auth (ssh-copy-id)
OSPI_PI_HOST="${OSPI_PI_HOST:-192.168.0.167}"
OSPI_PI_USER="${OSPI_PI_USER:-pi}"
OSPI_PI_PASS="${OSPI_PI_PASS:-}"
OSPI_PI_DIR="${OSPI_PI_DIR:-/home/pi/OpenSprinkler-Firmware}"
OSPI_PI_TMP_DIR="${OSPI_PI_TMP_DIR:-/tmp/opensprinkler-fw-sync}"
DEFINES_H="${SCRIPT_DIR}/defines.h"
# Auto-migrate legacy upgrade path to the new location.
# This keeps old .env files working after moving upgrade/ to /data/upgrade.
if [[ "${OS_UPGRADE_DIR:-}" == "/srv/www/htdocs/upgrade" ]]; then
    OS_UPGRADE_DIR="/data/upgrade"
fi
UPGRADE_DIR="${OS_UPGRADE_DIR:-/data/upgrade}"
MANIFEST="${UPGRADE_DIR}/manifest.json"
VERSIONS_JSON="${UPGRADE_DIR}/versions.json"
CHANGELOG="${SCRIPT_DIR}/CHANGELOG.md"
PLATFORMIO_INI="${SCRIPT_DIR}/platformio.ini"
GITHUB_REPO="opensprinklershop/OpenSprinkler-Firmware"

# Path to the UI fw.sh script that handles the IONOS online deploy.
# Credentials (IONOS_SSH_*) live in ui/.env, so we delegate to that script.
UI_FW_SH="${UI_FW_SH:-/srv/www/htdocs/ui/fw.sh}"

# Matter manufacturing data
MATTER_MFG_DIR="${SCRIPT_DIR}/matter_mfg"
MATTER_KVS_BIN="${MATTER_MFG_DIR}/matter_kvs.bin"
UPGRADE_MATTER_KVS_BIN="${UPGRADE_DIR}/matter_kvs.bin"
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
FW_UPLOAD_METHOD="${FW_UPLOAD_METHOD:-auto}"

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

_variant_to_slot() {
    case "$1" in
        zigbee) echo "ota0" ;;
        matter) echo "ota1" ;;
        esp8266) echo "" ;;
        *) echo "" ;;
    esac
}

_env_to_variant() {
    case "$1" in
        "$ENV_C5_MATTER") echo "matter" ;;
        "$ENV_C5_ZIGBEE") echo "zigbee" ;;
        "$ENV_ESP8266")   echo "esp8266" ;;
        *)                  echo "" ;;
    esac
}

_variant_to_upgrade_name() {
    case "$1" in
        matter) echo "firmware_matter.bin" ;;
        zigbee) echo "firmware_zigbee.bin" ;;
        esp8266) echo "firmware_esp8266.bin" ;;
        *) echo "" ;;
    esac
}

_device_reachable_quiet() {
    local hash
    hash=$(_get_hash)
    curl -sf --connect-timeout 2 --max-time 4 "http://${DEVICE_IP}/jo?pw=${hash}" >/dev/null 2>&1 || \
        curl -sf --connect-timeout 2 --max-time 4 "http://${DEVICE_IP}/db?pw=${hash}" >/dev/null 2>&1
}

_select_ip_upload_bin() {
    local env="$1"
    local variant="$2"
    local upgrade_name
    upgrade_name=$(_variant_to_upgrade_name "$variant")

    local build_bin="${SCRIPT_DIR}/.pio/build/${env}/firmware.bin"
    local dist_bin="${SCRIPT_DIR}/.pio/dist/firmware_${env}.bin"
    local upgrade_bin="${UPGRADE_DIR}/${upgrade_name}"

    if [[ -f "$build_bin" ]]; then
        echo "$build_bin"
    elif [[ -f "$dist_bin" ]]; then
        echo "$dist_bin"
    else
        echo "$upgrade_bin"
    fi
}

# ── Install firmware via IP (REST API) ─────────────────────────────────────
install_ip() {
    local variant="$1"
    local bin_file="${2:-}"
    local slot=""

    if [[ -z "$variant" ]]; then
        error "Usage: ./fw.sh install-ip <matter|zigbee|esp8266> [firmware.bin]"
        exit 1
    fi

    case "$variant" in
        zigbee|matter|esp8266) slot=$(_variant_to_slot "$variant") ;;
        *)
            error "Unknown variant: $variant"
            error "Allowed: matter | zigbee | esp8266"
            exit 1
            ;;
    esac

    if [[ -z "$bin_file" ]]; then
        bin_file="${UPGRADE_DIR}/$(_variant_to_upgrade_name "$variant")"
    fi

    if [[ ! -f "$bin_file" ]]; then
        error "Firmware binary not found: $bin_file"
        error "Usage: ./fw.sh install-ip <matter|zigbee|esp8266> [firmware.bin]"
        exit 1
    fi

    check_device
    local hash
    hash=$(_get_hash)

    header "Firmware-Upload via IP: $bin_file → $DEVICE_IP ($variant)"

    local url="http://${DEVICE_IP}:8080/update"
    if [[ -n "$slot" ]]; then
        url+="?slot=${slot}"
    fi

    info "Uploading firmware (multipart/form-data) to ${url} ..."
    local resp
    resp=$(curl -sS -X POST --connect-timeout 10 --max-time 600 \
        -F "pw=${hash}" \
        -F "slot=${slot}" \
        -F "file=@${bin_file}" \
        "$url") || {
        error "Upload failed: ${url}"
        exit 1
    }

    echo "Response: $resp"
    if echo "$resp" | python3 -c "import sys,json; d=json.load(sys.stdin); r=d.get('result'); sys.exit(0 if r in (0,1,'0','1') else 1)" 2>/dev/null; then
        ok "Firmware upload successful. Device reboot is triggered by firmware."
        info "Firmware update in progress. Device will be unavailable for 30–60s."
    else
        error "Firmware upload failed."
        local err_msg
        err_msg=$(echo "$resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('error', d.get('result','?')))" 2>/dev/null || echo "Unknown error")
        error "Error: ${err_msg}"
        exit 1
    fi
}

upload_ip_env() {
    local env="$1"
    local variant
    variant=$(_env_to_variant "$env")
    if [[ -z "$variant" ]]; then
        error "IP upload is not supported for environment: ${env}"
        exit 1
    fi

    local bin_file
    bin_file=$(_select_ip_upload_bin "$env" "$variant")
    install_ip "$variant" "$bin_file"
}

upload_env_auto() {
    local env="$1"
    local method="${FW_UPLOAD_METHOD,,}"

    case "$method" in
        auto|ip|usb) ;;
        *)
            error "Unknown FW_UPLOAD_METHOD=${FW_UPLOAD_METHOD}"
            error "Allowed: auto | ip | usb"
            exit 1
            ;;
    esac

    if [[ "$method" == "ip" ]]; then
        upload_ip_env "$env"
        return
    fi

    if [[ "$method" == "auto" && -n "${OS_IP:-}" ]]; then
        if _device_reachable_quiet; then
            info "Device reachable at ${DEVICE_IP}; using fast IP upload (FW_UPLOAD_METHOD=usb to force serial)."
            upload_ip_env "$env"
            return
        fi
        info "Device not reachable at ${DEVICE_IP}; falling back to USB upload."
    fi

    upload_env "$env"
}

upload_env_fast_debug() {
    local env="$1"

    # Debug deploy should be as fast as possible: prefer IP upload first,
    # then fall back to USB if device is currently not reachable.
    if _device_reachable_quiet; then
        info "Debug deploy: device reachable at ${DEVICE_IP}; using fast IP upload."
        upload_ip_env "$env"
        return
    fi

    warn "Debug deploy: fast IP upload unavailable (device not reachable), falling back to USB upload."
    upload_env "$env"
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
    local hash
    hash=$(_get_hash)
    if ! curl -sf --connect-timeout 3 --max-time 6 "http://${DEVICE_IP}/jo?pw=${hash}" &>/dev/null && \
       ! curl -sf --connect-timeout 3 --max-time 6 "http://${DEVICE_IP}/db?pw=${hash}" &>/dev/null; then
        error "Device not reachable at http://${DEVICE_IP}"
        error "Hint: set OS_IP=<IP>, or use FW_UPLOAD_METHOD=usb for serial upload."
        exit 1
    fi
    ok "Device reachable."
}

copy_to_dist() {
    local env="$1"
    local build_dir="${SCRIPT_DIR}/.pio/build/${env}"
    local dist="${SCRIPT_DIR}/.pio/dist"
    local src="${build_dir}/firmware.bin"
    if [[ -f "$src" ]]; then
        mkdir -p "$dist"
        cp "$src" "${dist}/firmware_${env}.bin"
        ok "Dist copy → .pio/dist/firmware_${env}.bin"
    fi
    # Preserve bootloader and partitions for full-flash (new-device provisioning).
    # These are stored in .pio/dist/ so they survive subsequent per-env build cleans.
    for artifact in bootloader partitions; do
        local art_src="${build_dir}/${artifact}.bin"
        if [[ -f "$art_src" ]]; then
            mkdir -p "$dist"
            cp "$art_src" "${dist}/${artifact}_${env}.bin"
        fi
    done
}

# Keep the locally rebuilt ESP32-C5 framework-libs package in the state required
# by the Wi-Fi/Ethernet-only Matter build. PlatformIO/ComponentManager may
# rewrite the installed package metadata during a build, so validate the live
# package before every C5 build.
ensure_c5_framework_libs() {
    local env_name="${1:-}"
    case "$env_name" in
        "$ENV_C5_MATTER"|"$ENV_C5_ZIGBEE") ;;
        *) return 0 ;;
    esac

    local pkg_dir="${SCRIPT_DIR}/.pio/packages/framework-arduinoespressif32-libs/esp32c5"
    local src_dir="/data/Workspace/framework-arduinoespressif32-libs/esp32c5"
    local build_py="${pkg_dir}/pioarduino-build.py"

    _matter_archive_has_openthread() {
        local archive="$1"
        [[ -f "$archive" ]] || return 1
        strings "$archive" | grep -Eq 'esp_openthread_init|otInstanceInitSingle|OpenthreadLauncher'
    }

    _restore_openthread_free_matter_archive() {
        local target_archive="$1"
        local clean_archive=""
        local candidate=""

        if [[ -f "${src_dir}/lib.bootloop.1778288323/libespressif__esp_matter.a" ]] && \
           ! _matter_archive_has_openthread "${src_dir}/lib.bootloop.1778288323/libespressif__esp_matter.a"; then
            clean_archive="${src_dir}/lib.bootloop.1778288323/libespressif__esp_matter.a"
        else
            while IFS= read -r candidate; do
                if ! _matter_archive_has_openthread "$candidate"; then
                    clean_archive="$candidate"
                    break
                fi
            done < <(find "$src_dir" -path '*/libespressif__esp_matter.a' -type f | sort -r)
        fi

        if [[ -z "$clean_archive" ]]; then
            error "No OpenThread-free ESP Matter archive found under ${src_dir}."
            error "Rebuild the local ESP32-C5 framework-libs package without Matter-over-Thread first."
            exit 1
        fi

        mkdir -p "$(dirname "$target_archive")"
        cp -f "$clean_archive" "$target_archive"
        ok "Restored OpenThread-free Matter archive → ${target_archive}"
    }

    if [[ ! -d "${src_dir}/lib" ]]; then
        error "Local ESP32-C5 framework-libs source missing: ${src_dir}/lib"
        exit 1
    fi

    if [[ "$env_name" == "$ENV_C5_MATTER" ]]; then
        local src_matter_archive="${src_dir}/lib/libespressif__esp_matter.a"
        if _matter_archive_has_openthread "$src_matter_archive"; then
            _restore_openthread_free_matter_archive "$src_matter_archive"
        fi
    fi

    # The OpenThread-free Matter rebuild kept the full IDF libs but can leave
    # the closed ZBOSS archives out of the live lib/ directory. Restore them
    # from the newest package backup so `deploy all` can still build ZigBee.
    local -a zigbee_libs=(
        libesp_zb_api.ed.a
        libesp_zb_api.gpd.a
        libesp_zb_api.zczr.a
        libzboss_port.native.a
        libzboss_port.remote.a
        libzboss_stack.ed.a
        libzboss_stack.gpd.a
        libzboss_stack.zczr.a
    )
    local zigbee_backup=""
    if [[ ! -f "${src_dir}/lib/libesp_zb_api.zczr.a" || ! -f "${src_dir}/lib/libzboss_stack.zczr.a" ]]; then
        zigbee_backup=$(find "$src_dir" -path '*/libesp_zb_api.zczr.a' -printf '%h\n' | sort -r | head -1)
        if [[ -z "$zigbee_backup" ]]; then
            error "Missing ZBOSS ZigBee archives in ${src_dir}/lib and no backup was found."
            exit 1
        fi
        for lib in "${zigbee_libs[@]}"; do
            [[ -f "${zigbee_backup}/${lib}" ]] && cp -f "${zigbee_backup}/${lib}" "${src_dir}/lib/"
        done
    fi

    # If PlatformIO has not installed the local file package yet, there is
    # nothing to patch. The next build will install it from the source package.
    [[ -d "$pkg_dir" ]] || return 0

    if [[ ! -f "${pkg_dir}/lib/libespressif__esp_daylight.a" ]]; then
        if [[ -f "${src_dir}/lib/libespressif__esp_daylight.a" ]]; then
            cp -f "${src_dir}/lib/libespressif__esp_daylight.a" "${pkg_dir}/lib/"
        else
            error "Missing ESP daylight library: ${pkg_dir}/lib/libespressif__esp_daylight.a"
            error "Rebuild/install the local framework-libs package first."
            exit 1
        fi
    fi

    for lib in "${zigbee_libs[@]}"; do
        if [[ ! -f "${pkg_dir}/lib/${lib}" && -f "${src_dir}/lib/${lib}" ]]; then
            cp -f "${src_dir}/lib/${lib}" "${pkg_dir}/lib/"
        fi
    done

    if [[ "$env_name" == "$ENV_C5_MATTER" ]]; then
        local pkg_matter_archive="${pkg_dir}/lib/libespressif__esp_matter.a"
        if _matter_archive_has_openthread "$pkg_matter_archive"; then
            _restore_openthread_free_matter_archive "$pkg_matter_archive"
        fi
    fi

    if [[ -f "${src_dir}/include/esp_matter/esp_matter_endpoint.h" ]]; then
        mkdir -p "${pkg_dir}/include/esp_matter"
        cp -f "${src_dir}/include/esp_matter/esp_matter_endpoint.h" \
              "${pkg_dir}/include/esp_matter/esp_matter_endpoint.h"
    fi

    if [[ -f "${src_dir}/bin/bootloader_qio_80m.elf" ]]; then
        mkdir -p "${pkg_dir}/bin"
        cp -f "${src_dir}/bin/bootloader_qio_80m.elf" \
              "${pkg_dir}/bin/bootloader_qio_80m.elf"
    fi

    if [[ -f "$build_py" ]]; then
        perl -0pi -e '
            s/"-lopenthread",\s*//g;
            s/"-lespressif__esp_schedule", "-lespressif__network_provisioning"/"-lespressif__esp_schedule", "-lespressif__esp_daylight", "-lespressif__network_provisioning"/g;
            s/"-lespressif__esp_schedule", "-lespressif__rmaker_common"/"-lespressif__esp_schedule", "-lespressif__esp_daylight", "-lespressif__rmaker_common"/g;
        ' "$build_py"

        if ! grep -q '"-lespressif__esp_daylight"' "$build_py"; then
            error "Framework link list still misses -lespressif__esp_daylight: ${build_py}"
            exit 1
        fi
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

# Run a command on the Pi as the configured SSH user (without sudo).
_ospi_ssh_user() {
    if [[ -n "$OSPI_PI_PASS" ]]; then
        sshpass -p "$OSPI_PI_PASS" \
            ssh -o StrictHostKeyChecking=no -o BatchMode=no \
            "${OSPI_PI_USER}@${OSPI_PI_HOST}" \
            "$*"
    else
        ssh -o StrictHostKeyChecking=no \
            "${OSPI_PI_USER}@${OSPI_PI_HOST}" \
            "$*"
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
    info "Syncing source → ${OSPI_PI_HOST}:${OSPI_PI_TMP_DIR} …"
    _ospi_ssh_user "mkdir -p '${OSPI_PI_TMP_DIR}/src'"
    _ospi_rsync -az --info=progress2 \
        --delete --no-group --omit-dir-times \
        "${_OSPI_RSYNC_EXCLUDES[@]}" \
        "${SCRIPT_DIR}/" \
        "${OSPI_PI_USER}@${OSPI_PI_HOST}:${OSPI_PI_TMP_DIR}/src/"

    info "Applying staged source to ${OSPI_PI_DIR} with sudo …"
    _ospi_ssh "
        mkdir -p '${OSPI_PI_DIR}'
        rsync -a --delete --omit-dir-times '${OSPI_PI_TMP_DIR}/src/' '${OSPI_PI_DIR}/'
    "
    ok "Source staged + synced to Pi target."
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

# Build only: push source and compile via build.sh.
build_ospi() {
    header "OsPi – remote build (${OSPI_PI_HOST})"
    check_ospi_conn
    ospi_push

    local silent_flag=""
    if [[ "${SILENT:-}" == "true" || "${SILENT:-}" == "1" ]]; then
        silent_flag="-s"
        info "Running build.sh on Pi (silent) …"
    else
        info "Running build.sh on Pi …"
    fi

    _ospi_ssh "cd '${OSPI_PI_DIR}' && ./build.sh ${silent_flag}"
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
    ensure_c5_framework_libs "$env"
    if ! "$PIO_BIN" run --environment "$env"; then
        # PlatformIO/SCons can sporadically fail on the first run with missing
        # intermediate build directories; retry once before failing hard.
        warn "First build attempt failed for ${env} — retrying once …"
        if ! "$PIO_BIN" run --environment "$env"; then
            error "Build failed: ${env}"
            exit 1
        fi
    fi
    ok "Build successful → .pio/build/${env}/firmware.bin"
    copy_to_dist "$env"
}

upload_env() {
    local env="$1"
    _find_usb_port "$(_env_to_chip "$env")"
    header "Uploading firmware: ${env} → ${USB_PORT}"
    if "$PIO_BIN" run --environment "$env" --target upload --upload-port "$USB_PORT"; then
        ok "Upload successful: ${env}"
    else
        error "Upload failed: ${env}"
        exit 1
    fi
}

ensure_zigbee_boot_partition() {
    info "Erasing otadata partition to guarantee OTA0 (zigbee) boot …"
    _find_usb_port "esp32c5"
    _find_esptool
    "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$USB_PORT" --baud 460800 \
        --before default_reset --after hard_reset \
        erase-region 0xe000 0x2000 \
        && ok "otadata erased — device will boot zigbee (OTA0) on next reset" \
        || warn "otadata erase failed — device may boot matter instead of zigbee"
}

# Monitor only: Start serial monitor on the specified port
monitor_only() {
    local port="${1:-}"
    if [[ -z "$port" ]]; then
        error "Usage: ./fw.sh monitor <port>"
        error "Example: ./fw.sh monitor /dev/ttyUSB1"
        exit 1
    fi

    if [[ ! -e "$port" ]]; then
        error "Port not found: ${port}"
        exit 1
    fi

    local baud="${MONITOR_SPEED:-115200}"
    local log_file="/tmp/monitor_$(basename "$port").log"

    # Clear old log file
    > "$log_file"

    header "Serial Monitor: ${port} @ ${baud} bps"
    info "Log file: ${log_file}"
    info "Press Ctrl+C to exit monitor"

    # Open the serial port with reset awareness
    if ! python3 - "$port" "$baud" 2>&1 <<'PY' | tee -a "$log_file"; then
import signal
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is required for reset-aware monitor (install: pip install pyserial)", file=sys.stderr)
    sys.exit(1)

port = sys.argv[1]
baud = int(sys.argv[2])
running = True

def stop(_signum, _frame):
    global running
    running = False

# signal.signal(signal.SIGINT, stop)
# signal.signal(signal.SIGTERM, stop)

ser = serial.Serial(port, baud, timeout=0.2)
# print(f"--- Reset-aware monitor on {port} | {baud} 8-N-1")
# print("--- Resetting target now to capture boot logs")
# sys.stdout.flush()

# ser.dtr = False
# ser.rts = True
# time.sleep(0.1)
# ser.rts = False

try:
    while running:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
finally:
    ser.close()
PY
        warn "Monitor exited"
    fi

    info "Monitor logs saved to: ${log_file}"
}

# Deploy + monitor: Build, upload, and start serial monitor
deploy_monitor_env() {
    local env="$1"
    local debug_mode="$2"
    
    header "Deploy & Monitor: ${env}"
    
    # Debug mode handling: if debug_mode is "debug", keep debug flags ENABLED
    local restore_debug=false
    if [[ "$debug_mode" != "debug" ]]; then
        disable_release_debug
        restore_debug=true
    else
        info "Debug mode enabled (ENABLE_DEBUG flags active)."
    fi
    
    # Build with debug flags
    info "Building firmware with debug output enabled..."
    build_env "$env"
    copy_one_to_upgrade "$env" "firmware_${env##*-}.bin" 2>/dev/null || true
    
    # Upload
    info "Uploading to device..."
    upload_env_auto "$env"
    if [[ "$env" == "$ENV_C5_ZIGBEE" && "${FW_UPLOAD_METHOD,,}" == "usb" ]]; then
        ensure_zigbee_boot_partition
    fi
    
    # Find port and start monitor
    _find_usb_port "$(_env_to_chip "$env")"
    local baud="${MONITOR_SPEED:-115200}"
    local variant="${env##*-}"
    local log_file="/tmp/zigbee_monitor_${variant}.log"
    
    # Clear old log file
    > "$log_file"
    
    header "Starting serial monitor: ${USB_PORT} @ ${baud} bps"
    info "Log file: ${log_file}"
    info "View logs via MCP: get_monitor_log variant=${variant}"
    info "Press Ctrl+C to exit monitor"

    # Open the serial port first, then reset the ESP32-C5 so early boot logs are not missed.
    if ! python3 - "$USB_PORT" "$baud" 2>&1 <<'PY' | tee -a "$log_file"; then
import signal
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is required for reset-aware monitor (install: pip install pyserial)", file=sys.stderr)
    sys.exit(1)

port = sys.argv[1]
baud = int(sys.argv[2])
running = True

def stop(_signum, _frame):
    global running
    running = False

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

ser = serial.Serial(port, baud, timeout=0.2)
print(f"--- Reset-aware monitor on {port} | {baud} 8-N-1")
print("--- Resetting target now to capture boot logs")
sys.stdout.flush()

ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False

try:
    while running:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
finally:
    ser.close()
PY
        warn "Monitor exited"
    fi
    
    info "Monitor logs saved to: ${log_file}"
    
    # Restore debug flags if needed
    if $restore_debug; then
        restore_release_debug
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

# ── USB port auto-detection ──────────────────────────────────────────────────
# Map PlatformIO environment name → expected chip string (as reported by esptool).
_env_to_chip() {
    case "$1" in
        "$ENV_C5_MATTER"|"$ENV_C5_ZIGBEE") echo "esp32c5" ;;
        "$ENV_ESP8266")                     echo "esp8266" ;;
        *)                                  echo "" ;;
    esac
}

# Probe the chip type on a serial port via esptool.
# Returns the lowercase chip description (e.g. "esp32c5", "esp8266") or empty.
_probe_chip() {
    local port="$1"
    # Use chip_id which is fast and prints the chip type.  Capture stderr+stdout.
    local out
    out=$("${ESPTOOL_CMD[@]}" --port "$port" --before default_reset --after no_reset \
        chip_id 2>&1) || true
    # esptool v5+ prints "Chip type:          ESP32-C5 (revision …)" or "ESP8266EX"
    # older versions print "Chip is ESP32-C5 …"
    local chip
    chip=$(echo "$out" | grep -ioP '(?:Chip is|Chip type:\s*|Connected to )\K[A-Za-z0-9-]+' | head -1)
    # Normalise: lowercase, strip hyphens, strip suffixes like "EX" → "esp32c5", "esp8266"
    chip="${chip,,}"
    chip="${chip//-/}"
    chip="${chip%ex}"
    echo "$chip"
}

# Auto-detect the USB serial port for flashing/monitoring.
# Usage: _find_usb_port [expected_chip]
#   expected_chip – e.g. "esp32c5" or "esp8266".  When set and multiple ports
#                   exist, probes each port and picks the matching one.
# Prefers UPLOAD_PORT if already set.  Sets global USB_PORT.
# When expected_chip is given, verifies the chip even on an explicit UPLOAD_PORT.
_find_usb_port() {
    local expected_chip="${1:-}"

    if [[ -n "${UPLOAD_PORT:-}" ]]; then
        USB_PORT="$UPLOAD_PORT"
        # Verify chip type if expected_chip was requested
        if [[ -n "$expected_chip" ]]; then
            if [[ -z "${ESPTOOL_CMD+x}" ]]; then
                _find_esptool
            fi
            local detected
            detected=$(_probe_chip "$USB_PORT")
            if [[ -n "$detected" && "$detected" != "$expected_chip" ]]; then
                error "UPLOAD_PORT=${USB_PORT} has chip '${detected}', expected '${expected_chip}'."
                error "Hint: unset UPLOAD_PORT to auto-detect, or point it to the correct device."
                exit 1
            fi
        fi
        info "Using UPLOAD_PORT=${USB_PORT}"
        return 0
    fi

    local -a candidates=()
    for dev in /dev/ttyUSB* /dev/ttyACM*; do
        [[ -e "$dev" ]] && candidates+=("$dev")
    done

    if [[ ${#candidates[@]} -eq 0 ]]; then
        error "No USB serial device found (/dev/ttyUSB* or /dev/ttyACM*)."
        error "Hint: connect the device or set UPLOAD_PORT=/dev/…"
        exit 1
    fi

    # Single device — verify chip if expected
    if [[ ${#candidates[@]} -eq 1 ]]; then
        USB_PORT="${candidates[0]}"
        if [[ -n "$expected_chip" ]]; then
            if [[ -z "${ESPTOOL_CMD+x}" ]]; then
                _find_esptool
            fi
            local detected
            detected=$(_probe_chip "$USB_PORT")
            if [[ -n "$detected" && "$detected" != "$expected_chip" ]]; then
                error "${USB_PORT} has chip '${detected}', expected '${expected_chip}'."
                error "Hint: connect the right device or set UPLOAD_PORT=/dev/…"
                exit 1
            fi
        fi
        info "Auto-detected USB port: ${USB_PORT}"
        return 0
    fi

    # Multiple devices — probe chip type to find the right one
    if [[ -n "$expected_chip" ]]; then
        # Ensure esptool is available for probing
        if [[ -z "${ESPTOOL_CMD+x}" ]]; then
            _find_esptool
        fi
        info "Multiple USB ports found — probing for ${expected_chip} …"
        for dev in "${candidates[@]}"; do
            local detected
            detected=$(_probe_chip "$dev")
            if [[ -n "$detected" ]]; then
                info "  ${dev} → ${detected}"
                if [[ "$detected" == "$expected_chip" ]]; then
                    USB_PORT="$dev"
                    ok "Matched ${expected_chip} on ${USB_PORT}"
                    return 0
                fi
            else
                info "  ${dev} → (no response)"
            fi
        done
        error "No port with chip '${expected_chip}' found among: ${candidates[*]}"
        error "Hint: set UPLOAD_PORT=/dev/… to override"
        exit 1
    fi

    # Multiple devices, no expected chip — pick first with a warning
    warn "Multiple USB serial devices found:"
    for dev in "${candidates[@]}"; do
        echo "    $dev"
    done
    USB_PORT="${candidates[0]}"
    warn "Using first: ${USB_PORT}  (override with UPLOAD_PORT=…)"
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

    # Keep upgrade directory in sync so full-flash/release artifacts are complete.
    mkdir -p "$UPGRADE_DIR"
    cp "$MATTER_KVS_BIN" "$UPGRADE_MATTER_KVS_BIN"
    ok "Matter KVS copied to ${UPGRADE_MATTER_KVS_BIN}"

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
    _find_usb_port "esp32c5"

    local port="$USB_PORT"
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
    header "Full Flash: ${env} (pre-built binaries)"

    # Matter full-flash always needs manufacturing KVS at 0x730000.
    # If it is missing, generate it automatically.
    if [[ "$env" == "$ENV_C5_MATTER" ]] && [[ ! -f "$MATTER_KVS_BIN" ]]; then
        warn "Matter KVS binary missing: ${MATTER_KVS_BIN}"
        info "Generating Matter manufacturing data automatically …"
        generate_matter_mfg
    fi

    # Determine the variant name used in the upgrade directory.
    local variant
    case "$env" in
        "$ENV_C5_MATTER") variant="matter" ;;
        "$ENV_C5_ZIGBEE") variant="zigbee" ;;
        *) error "Unknown env for full-flash: ${env}"; exit 1 ;;
    esac

    # All binaries are taken from the upgrade directory (current released version).
    local firmware="${UPGRADE_DIR}/firmware_${variant}.bin"
    local bootloader="${UPGRADE_DIR}/bootloader_${variant}.bin"
    local partitions="${UPGRADE_DIR}/partitions_${variant}.bin"

    # Validate — never compile automatically, never fall back to local build.
    local missing=false
    for f in "$bootloader" "$partitions" "$firmware"; do
        if [[ ! -f "$f" ]]; then
            error "Missing: $f"
            missing=true
        fi
    done
    if $missing; then
        echo ""
        error "full-flash requires release binaries in ${UPGRADE_DIR}/."
        error "Populate them by running a release build:"
        error "  ./fw.sh release rebuild       # rebuild all variants"
        error "  ./fw.sh release               # full release build (with version bump)"
        echo ""
        error "Or copy from the last local build manually:"
        local dist="${SCRIPT_DIR}/.pio/dist"
        error "  cp ${dist}/bootloader_${env}.bin ${UPGRADE_DIR}/bootloader_${variant}.bin"
        error "  cp ${dist}/partitions_${env}.bin ${UPGRADE_DIR}/partitions_${variant}.bin"
        exit 1
    fi

    info "Using pre-built binaries:"
    info "  Firmware:   ${firmware}"
    info "  Bootloader: ${bootloader}"
    info "  Partitions: ${partitions}"

    _find_esptool
    _find_usb_port "esp32c5"

    local port="$USB_PORT"
    local baud="${UPLOAD_SPEED:-460800}"

    # Build flash arguments: bootloader + partitions + firmware
    local flash_args=(
        "$BOOTLOADER_ADDR"      "$bootloader"
        "$PARTITION_TABLE_ADDR"  "$partitions"
        "$fw_addr"               "$firmware"
    )

    # For matter, always include matter_kvs.
    if [[ "$env" == "$ENV_C5_MATTER" ]]; then
        if [[ ! -f "$MATTER_KVS_BIN" ]]; then
            error "Matter KVS binary still missing after auto-generation: ${MATTER_KVS_BIN}"
            exit 1
        fi
        flash_args+=("$MATTER_KVS_ADDR" "$MATTER_KVS_BIN")
        info "Including Matter KVS partition (${MATTER_KVS_ADDR})"
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

    # Erase RainMaker factory credentials and NVS only when explicitly requested.
    # Use FACTORY_RESET=1 to wipe WiFi credentials + RainMaker certs (new device provisioning).
    # fctry partition (0x7EA000, 0x6000): RainMaker node_id, certs, claiming data
    # nvs   partition (0x9000,   0x5000): user mapping, WiFi creds, runtime state
    if [[ "${FACTORY_RESET:-0}" == "1" ]]; then
        info "FACTORY_RESET=1: Erasing RainMaker fctry partition (0x7EA000, 24K) …"
        "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
            --before default_reset --after no_reset \
            erase_region 0x7EA000 0x6000 \
        || warn "fctry erase failed (non-fatal)"

        info "FACTORY_RESET=1: Erasing NVS partition (0x9000, 20K) …"
        "${ESPTOOL_CMD[@]}" --chip esp32c5 --port "$port" --baud "$baud" \
            --before default_reset --after hard_reset \
            erase_region 0x9000 0x5000 \
        || warn "NVS erase failed (non-fatal)"
    fi

    ok "Full flash successful: ${env}"
}

# ── USB reset ───────────────────────────────────────────────────────────────
# Trigger a hardware reset on the connected ESP32-C5 via RTS/DTR signalling.
# esptool's "run" command pulses DTR/RTS exactly as it does before flashing,
# so no data is written to flash — the chip is simply rebooted.
do_reset() {
    header "USB Reset"
    _find_esptool
    _find_usb_port "esp32c5"

    local port="$USB_PORT"
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

    # Sync Matter manufacturing partition binary if present.
    if [[ -f "$MATTER_KVS_BIN" ]]; then
        cp "$MATTER_KVS_BIN" "$UPGRADE_MATTER_KVS_BIN"
    fi

    ok "Binaries copied to ${UPGRADE_DIR}/"

    # Copy bootloader/partitions for full-flash (new-device provisioning)
    local dist="${SCRIPT_DIR}/.pio/dist"
    for artifact in bootloader partitions; do
        [[ -f "${dist}/${artifact}_${ENV_C5_ZIGBEE}.bin" ]] && \
            cp "${dist}/${artifact}_${ENV_C5_ZIGBEE}.bin" "${UPGRADE_DIR}/${artifact}_zigbee.bin"
        [[ -f "${dist}/${artifact}_${ENV_C5_MATTER}.bin" ]] && \
            cp "${dist}/${artifact}_${ENV_C5_MATTER}.bin" "${UPGRADE_DIR}/${artifact}_matter.bin"
    done

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
# patch the *_sha256 fields AND fw_version/fw_minor in manifest.json.
# fw_version and fw_minor are read from defines.h so deploy always keeps
# the manifest in sync with what was actually compiled and served.
sync_manifest_sha256() {
    if [[ ! -f "$MANIFEST" ]]; then
        return 0  # nothing to update
    fi
    # Re-read version from defines.h in case read_version() was not yet called
    local fwv fwm
    fwv=$(grep -oP '^\s*#define\s+OS_FW_VERSION\s+\K[0-9]+' "$DEFINES_H")
    fwm=$(grep -oP '^\s*#define\s+OS_FW_MINOR\s+\K[0-9]+'   "$DEFINES_H")
    python3 - "$MANIFEST" \
        "${UPGRADE_DIR}/firmware_zigbee.bin" \
        "${UPGRADE_DIR}/firmware_matter.bin" \
        "${UPGRADE_DIR}/firmware_esp8266.bin" \
        "$fwv" "$fwm" <<'PYEOF'
import json, hashlib, sys, os

manifest_path = sys.argv[1]
bins = {"zigbee": sys.argv[2], "matter": sys.argv[3], "esp8266": sys.argv[4]}
fw_version = int(sys.argv[5])
fw_minor   = int(sys.argv[6])

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
if d.get("fw_version") != fw_version:
    d["fw_version"] = fw_version
    changed.append(f"fw_version={fw_version}")
if d.get("fw_minor") != fw_minor:
    d["fw_minor"] = fw_minor
    changed.append(f"fw_minor={fw_minor}")
if changed:
    with open(manifest_path, "w") as f:
        json.dump(d, f, indent=4, ensure_ascii=False)
    print("  manifest.json updated:", ", ".join(changed))
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

# Ensure versions.json contains an up-to-date entry for the version referenced
# by manifest.json. This is used by online-deploy to keep IONOS metadata
# consistent even when no full release pipeline is run.
sync_versions_from_manifest() {
    if [[ ! -f "$MANIFEST" ]]; then
        warn "versions sync skipped — ${MANIFEST} not found."
        return 0
    fi

    mkdir -p "$(dirname "$VERSIONS_JSON")"

    python3 - "$MANIFEST" "$VERSIONS_JSON" <<'PYEOF'
import json
import os
import sys
from datetime import date

manifest_path = sys.argv[1]
versions_path = sys.argv[2]

with open(manifest_path, "r", encoding="utf-8") as f:
    manifest = json.load(f)

fw_version = int(manifest.get("fw_version", 0) or 0)
fw_minor = int(manifest.get("fw_minor", 0) or 0)
if fw_version <= 0:
    print("  versions.json sync skipped: manifest has no valid fw_version")
    sys.exit(0)

catalog = []
if os.path.isfile(versions_path):
    try:
        with open(versions_path, "r", encoding="utf-8") as f:
            loaded = json.load(f)
            if isinstance(loaded, list):
                catalog = loaded
    except Exception:
        catalog = []

today = date.today().isoformat()
entry_date = today
for item in catalog:
    if int(item.get("fw_version", 0) or 0) == fw_version and int(item.get("fw_minor", 0) or 0) == fw_minor:
        entry_date = item.get("date") or today
        break

entry = {
    "fw_version": fw_version,
    "fw_minor": fw_minor,
    "date": entry_date,
    "zigbee_url": f"https://opensprinklershop.de/upgrade/archive/v{fw_version}_{fw_minor}/firmware_zigbee.bin",
    "matter_url": f"https://opensprinklershop.de/upgrade/archive/v{fw_version}_{fw_minor}/firmware_matter.bin",
    "esp8266_url": f"https://opensprinklershop.de/upgrade/archive/v{fw_version}_{fw_minor}/firmware_esp8266.bin",
    "zigbee_sha256": manifest.get("zigbee_sha256", ""),
    "matter_sha256": manifest.get("matter_sha256", ""),
    "esp8266_sha256": manifest.get("esp8266_sha256", ""),
    "changelog": manifest.get("changelog", "")
}

catalog = [
    item for item in catalog
    if not (
        int(item.get("fw_version", 0) or 0) == fw_version
        and int(item.get("fw_minor", 0) or 0) == fw_minor
    )
]
catalog.insert(0, entry)
catalog.sort(key=lambda e: (int(e.get("fw_version", 0) or 0), int(e.get("fw_minor", 0) or 0)), reverse=True)

with open(versions_path, "w", encoding="utf-8") as f:
    json.dump(catalog, f, indent=2, ensure_ascii=False)

print(f"  versions.json updated: v{fw_version}_{fw_minor}")
PYEOF

    ok "versions.json synced from manifest.json"
}

prepare_online_upgrade_metadata() {
    read_version
    sync_manifest_sha256
    sync_versions_from_manifest
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

    # Only add files that are tracked within this repository.
    # MANIFEST, VERSIONS_JSON and binary files in UPGRADE_DIR may live outside
    # the repo root (e.g. /data/upgrade/) — git add on those paths
    # would produce a fatal error.  We only commit defines.h and CHANGELOG.md
    # which are always inside the repo.
    local files_to_add=("$DEFINES_H" "$CHANGELOG")
    # Add upgrade files only if they are inside the repo tree
    for f in "$MANIFEST" "$VERSIONS_JSON" \
              "${UPGRADE_DIR}/firmware_zigbee.bin" \
              "${UPGRADE_DIR}/firmware_matter.bin" \
              "${UPGRADE_DIR}/firmware_esp8266.bin" \
              "${UPGRADE_DIR}/archive/"; do
        if [[ "$f" == "${SCRIPT_DIR}/"* ]]; then
            files_to_add+=("$f")
        fi
    done

    git add "${files_to_add[@]}"

    git commit -m "Release ${version_str} (build ${OS_FW_MINOR})"
    ok "Committed."

    git tag -a "$tag" -m "Release ${version_str} (build ${OS_FW_MINOR})"
    ok "Tag created: ${tag}"

    git push origin HEAD
    git push origin "$tag"
    ok "Pushed to origin."
}

# Create a GitHub release using gh CLI
github_create_release() {
    local tag="$1"
    local version_str="$2"
    local notes="$3"

    header "GitHub: create release"

    if ! command -v gh &>/dev/null; then
        warn "gh CLI not installed — skipping GitHub release creation."
        warn "Install: https://cli.github.com/ then run: gh auth login"
        return 0
    fi

    if ! gh auth status &>/dev/null; then
        warn "gh CLI not authenticated — skipping GitHub release creation."
        warn "Run: gh auth login"
        return 0
    fi

    local release_name="v${version_str} (build ${OS_FW_MINOR})"

    gh release create "$tag" \
        --repo "$GITHUB_REPO" \
        --title "$release_name" \
        --notes "$notes" \
        "${UPGRADE_DIR}/firmware_zigbee.bin" \
        "${UPGRADE_DIR}/firmware_matter.bin" \
        "${UPGRADE_DIR}/firmware_esp8266.bin" \
    || {
        error "Failed to create GitHub release."
        return 1
    }

    ok "GitHub release created: ${tag}"
}

github_release_title_from_tag() {
    local tag="$1"

    if [[ "$tag" =~ ^v([0-9]+)_([0-9]+)$ ]]; then
        local fw_version="${BASH_REMATCH[1]}"
        local fw_minor="${BASH_REMATCH[2]}"
        echo "v$(format_version "$fw_version") (build ${fw_minor})"
        return 0
    fi

    echo "$tag"
}

github_release_exists() {
    local tag="$1"
    gh release view "$tag" --repo "$GITHUB_REPO" &>/dev/null
}

github_create_release_for_existing_tag() {
    local tag="$1"
    local release_name
    local notes

    release_name="$(github_release_title_from_tag "$tag")"
    notes="$(git for-each-ref "refs/tags/${tag}" --format='%(contents)')"

    if [[ -z "${notes//[$'\t\r\n ']/}" ]]; then
        notes="Release created from existing Git tag ${tag}."
    fi

    gh release create "$tag" \
        --repo "$GITHUB_REPO" \
        --title "$release_name" \
        --notes "$notes" \
    || {
        error "Failed to create GitHub release for ${tag}."
        return 1
    }

    ok "Release created for ${tag}"
}

github_sync_tag_releases() {
    header "GitHub: sync tags to releases"

    if ! command -v gh &>/dev/null; then
        error "gh CLI not installed — cannot create GitHub releases from tags."
        error "Install: https://cli.github.com/ then run: gh auth login"
        return 1
    fi

    if ! gh auth status &>/dev/null; then
        error "gh CLI not authenticated — cannot create GitHub releases."
        error "Run: gh auth login"
        return 1
    fi

    local -a tags
    mapfile -t tags < <(git tag -l 'v*' --sort=version:refname)
    if [[ ${#tags[@]} -eq 0 ]]; then
        warn "No matching tags found (pattern: v*)."
        return 0
    fi

    local created=0
    local skipped=0
    local failed=0
    local tag

    for tag in "${tags[@]}"; do
        if github_release_exists "$tag"; then
            info "Release already exists for ${tag} — skipping."
            ((skipped+=1))
            continue
        fi

        info "Creating GitHub release for ${tag} …"
        if github_create_release_for_existing_tag "$tag"; then
            ((created+=1))
        else
            ((failed+=1))
        fi
    done

    echo ""
    ok "Created: ${created}"
    ok "Skipped: ${skipped}"

    if (( failed > 0 )); then
        error "Failed:  ${failed}"
        return 1
    fi

    ok "All matching tags are now represented as GitHub releases."
}

# ── Online (IONOS) deploy ───────────────────────────────────────────────────
# Delegates to ui/fw.sh which reads IONOS credentials from its own .env and
# rsyncs the entire local upgrade/ tree to the IONOS server.
online_deploy() {
    header "Online deploy → IONOS"
    local ui_env="$(dirname "$UI_FW_SH")/.env"
    local -a deploy_cmd=("$UI_FW_SH")

    info "Preparing upgrade metadata before upload (manifest + versions) …"
    prepare_online_upgrade_metadata

    if [[ ! -f "$UI_FW_SH" ]]; then
        warn "Online deploy skipped — ${UI_FW_SH} not found."
        warn "Set UI_FW_SH=<path> in .env to configure."
        return 0
    fi
    if [[ ! -x "$UI_FW_SH" ]]; then
        warn "Online deploy skipped — ${UI_FW_SH} is not executable (chmod +x it)."
        return 0
    fi

    if ! command -v sshpass &>/dev/null; then
        warn "Online deploy skipped — sshpass not found."
        warn "Install: sudo apt install sshpass"
        return 0
    fi

    if [[ -f "$ui_env" && ! -r "$ui_env" ]]; then
        if ! command -v sudo &>/dev/null; then
            error "Online deploy credentials are not readable: ${ui_env}"
            error "Run as root, fix file permissions, or install sudo."
            exit 1
        fi
        warn "${ui_env} is not readable by $(id -un); running UI deploy via sudo."
        deploy_cmd=(sudo --preserve-env=LOCAL_UPGRADE_SRC "$UI_FW_SH")
    fi

    info "Syncing ${UPGRADE_DIR}/ → IONOS via ${UI_FW_SH} …"
    LOCAL_UPGRADE_SRC="${UPGRADE_DIR%/}/" "${deploy_cmd[@]}" \
    || { error "Online deploy failed."; exit 1; }
    ok "Online deploy complete."
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

        # 9. GitHub release (optional, requires gh CLI authenticated)
        github_create_release "$tag" "$version_str" "$release_notes"
    fi

    # 10. Online deploy to IONOS (upgrade/ → remote server)
    online_deploy

    # 11. Summary
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
    upload  [matter|zigbee|esp8266|all]       Upload firmware (IP/REST if reachable,
                                                                                        USB fallback; default: all)
    deploy  [matter|zigbee|esp8266|ospi|all] [debug]
                                            Build + Upload/Deploy (default: all)
                                            Optional 'debug' keeps ENABLE_DEBUG
                                            flags enabled for all variants
                                            Examples:
                                              ./fw.sh deploy
                                              ./fw.sh deploy zigbee
                                              ./fw.sh deploy debug (all platforms)
                                              ./fw.sh deploy zigbee debug
  deploy  [matter|zigbee|esp8266] monitor   Build + Upload + Monitor with debug
                                            No monitor with "all" (use one platform)
                                            Recommended for device debugging
                                            Example: ./fw.sh deploy zigbee monitor
  monitor <port>                            Start serial monitor on specified port
                                            No build or upload — just monitor
                                            Example: ./fw.sh monitor /dev/ttyUSB1
  sync-back                                 Pull OsPi changes back to local workspace
  online-deploy                            Upload current upgrade/ tree to IONOS only
  release                                   Bump version, build all firmwares,
                                            copy to upgrade dir, git tag & push,
                                            create GitHub release, deploy to IONOS
  release rebuild                           Rebuild all firmwares without version
                                            bump, no git tag/release — build only,
                                            then deploy to IONOS
    release sync-tags                         Create missing GitHub releases for
                                                                                        existing v* tags
    libs [rebuild|deploy|rebuild-copy|deploy-copy]
                                                                                        Rebuild/deploy custom ESP32-C5 Arduino libs
                                                                                        rebuild      = compile + deploy to workspace/.pio
                                                                                        deploy       = deploy only (skip compile)
                                                                                        *-copy       = additionally copy to ~/.platformio
  generate-mfg                              Generate Matter manufacturing data
                                            (test PAA → PAI → DAC chain + NVS binary)
  flash-mfg                                 Flash matter_kvs partition to device
                                            (standalone, without firmware)
  full-flash [matter|zigbee|all]            Full flash for new devices: bootloader +
                                            partition table + firmware using pre-built
                                            binaries (no compilation — uses current
                                            release from upgrade/ directory);
                                            matter auto-generates matter_kvs if missing
  reset                                     Reset/reboot device via USB (RTS/DTR)
  switch <matter|zigbee|zigbee-client|disabled>
                                            Switch firmware via REST API + reboot
    install-ip <matter|zigbee|esp8266> [firmware.bin]
                                                                                        Install firmware via device IP.
                                                                                        If firmware.bin is omitted, defaults to:
                                                                                        ${UPGRADE_DIR}/firmware_<variant>.bin
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
    FW_UPLOAD_METHOD=<mode> Upload mode: auto | ip | usb  (default: auto)
                                                    auto prefers IP/REST when OS_IP is reachable
    UPLOAD_PORT=<port>      Serial port      (auto-detected if not set)
  UPLOAD_SPEED=<baud>     Upload baud rate  (default: 460800)
  ERASE_FLASH=1           Erase entire flash before full-flash
  FACTORY_RESET=1         Also erase NVS + fctry after full-flash (wipes WiFi credentials
                          and RainMaker certs — only for new-device provisioning)

${BOLD}OsPi environment variables:${NC}
  OSPI_PI_HOST=<IP>       OsPi host        (default: 192.168.0.167)
  OSPI_PI_USER=<user>     OsPi SSH user    (default: pi)
  OSPI_PI_PASS=<password> OsPi SSH password (leave empty to use SSH keys)
  OSPI_PI_DIR=<path>      OsPi firmware dir (default: /home/pi/OpenSprinkler-Firmware)
  SILENT=<true|false>     Run remote build silently on Pi target (default: true)

${BOLD}Examples:${NC}
  ./fw.sh build
  ./fw.sh build matter
  ./fw.sh build esp8266
  ./fw.sh build ospi
    ./fw.sh deploy zigbee
    ./fw.sh upload zigbee              # fast IP upload if OS_IP is reachable
    FW_UPLOAD_METHOD=usb ./fw.sh deploy zigbee
    FW_UPLOAD_METHOD=ip ./fw.sh upload zigbee
  ./fw.sh deploy debug
  ./fw.sh deploy zigbee debug
  ./fw.sh deploy ospi
  ./fw.sh deploy zigbee monitor      # Deploy + Monitor with debug (Zigbee Client debug)
  ./fw.sh deploy matter monitor      # Deploy + Monitor with debug (Matter debug)
  ./fw.sh sync-back
  ./fw.sh switch matter
  ./fw.sh switch zigbee
  ./fw.sh release
  ./fw.sh release rebuild
    ./fw.sh libs rebuild
    ./fw.sh libs deploy
    ./fw.sh libs rebuild-copy
    ./fw.sh libs deploy-copy
    ./fw.sh online-deploy
  ./fw.sh release sync-tags
  ./fw.sh generate-mfg
  ./fw.sh flash-mfg
  ./fw.sh full-flash zigbee
  ./fw.sh full-flash matter
  ERASE_FLASH=1 ./fw.sh full-flash zigbee
  FACTORY_RESET=1 ./fw.sh full-flash zigbee   # new device: also wipe WiFi credentials
  ./fw.sh reset
  UPLOAD_PORT=/dev/ttyACM0 ./fw.sh reset
  OS_IP=192.168.0.151 OS_PASSWORD=opendoor ./fw.sh switch zigbee
    OS_IP=192.168.0.59 ./fw.sh install-ip zigbee
    OS_IP=192.168.0.59 ./fw.sh install-ip matter /data/upgrade/firmware_matter.bin
  OS_IP=192.168.0.86  OS_HASH=a6d82bced638de3def1e9bbb4983225c ./fw.sh status
  OSPI_PI_PASS=secret ./fw.sh build ospi
  OSPI_PI_PASS=secret ./fw.sh deploy ospi

${BOLD}Prerequisites (release):${NC}
  gh auth login           Authenticate gh CLI for GitHub release creation (optional)
EOF
}

run_libs() {
    local mode="${1:-rebuild}"
    local libs_script="${SCRIPT_DIR}/build_and_deploy_libs.sh"

    if [[ ! -x "$libs_script" ]]; then
        error "Library build script not found or not executable: $libs_script"
        exit 1
    fi

    case "$mode" in
        ""|all|rebuild)
            info "Rebuilding custom ESP32-C5 Arduino libs (workspace + .pio/packages) ..."
            "$libs_script"
            ;;
        deploy)
            info "Deploying existing custom libs only (skip build) ..."
            "$libs_script" --skip-build
            ;;
        rebuild-copy)
            info "Rebuilding custom libs and copying to ~/.platformio ..."
            "$libs_script" --copy-pio
            ;;
        deploy-copy)
            info "Deploying existing custom libs and copying to ~/.platformio ..."
            "$libs_script" --skip-build --copy-pio
            ;;
        *)
            error "Unknown libs mode: $mode"
            error "Usage: ./fw.sh libs [rebuild|deploy|rebuild-copy|deploy-copy]"
            exit 1
            ;;
    esac
}

# ── Main logic ───────────────────────────────────────────────────────────────
ACTION="${1:-help}"
VARIANT="${2:-all}"
MODE_ARG="${3:-}"

case "$ACTION" in
    install-ip)
        install_ip "$VARIANT" "$MODE_ARG" ;;
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
            matter)   upload_env_auto "$ENV_C5_MATTER" ;;
            zigbee)   upload_env_auto "$ENV_C5_ZIGBEE" ;;
            esp8266)  upload_env_auto "$ENV_ESP8266" ;;
            all|"")
                warn "Uploading all firmwares can reboot the same device multiple times; single-variant upload is recommended."
                upload_env_auto "$ENV_C5_MATTER"
                upload_env_auto "$ENV_C5_ZIGBEE"
                upload_env_auto "$ENV_ESP8266"
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
                deploy_variant="all"
                deploy_debug=false
                deploy_with_monitor=false

                # Parse up to two optional args in any order:
                #   ./fw.sh deploy <variant> [debug|monitor]
                #   ./fw.sh deploy [debug|monitor] <variant>
                for token in "$VARIANT" "$MODE_ARG"; do
                    case "$token" in
                    ""|all)
                        ;;
                    matter|zigbee|esp8266|ospi)
                        deploy_variant="$token"
                        ;;
                    debug)
                        deploy_debug=true
                        ;;
                    monitor)
                        deploy_with_monitor=true
                        deploy_debug=true
                        ;;
                    *)
                        error "Unknown deploy argument: $token"
                        error "Allowed: matter|zigbee|esp8266|ospi|all|debug|monitor"
                        exit 1
                        ;;
                    esac
                done

                if $deploy_with_monitor; then
                    # Monitor mode: deploy + monitor (with debug enabled)
                    info "Deploy + Monitor mode enabled (debug + serial monitor)"
                    case "$deploy_variant" in
                    zigbee|matter)
                        deploy_monitor_env "esp32-c5-${deploy_variant}" "debug"
                        ;;
                    all)
                        warn "Monitor mode only supports: zigbee, matter (single platform at a time)"
                        info "Using default: zigbee"
                        deploy_monitor_env "esp32-c5-zigbee" "debug"
                        ;;
                    *)
                        error "Monitor mode only supports: zigbee, matter"
                        exit 1
                        ;;
                    esac
                else
                    # Standard deploy mode (with or without debug)
                    if ! $deploy_debug; then
                        disable_release_debug
                        trap 'restore_release_debug' EXIT
                    else
                        info "Deploy debug mode enabled (keeping ENABLE_DEBUG flags active)."
                    fi

                    case "$deploy_variant" in
                    matter)
                        build_env "$ENV_C5_MATTER"
                        copy_one_to_upgrade "$ENV_C5_MATTER" "firmware_matter.bin"
                        upload_env_auto "$ENV_C5_MATTER"
                        ;;
                    zigbee)
                        build_env "$ENV_C5_ZIGBEE"
                        copy_one_to_upgrade "$ENV_C5_ZIGBEE" "firmware_zigbee.bin"
                        if $deploy_debug; then
                            upload_env_fast_debug "$ENV_C5_ZIGBEE"
                        else
                            upload_env_auto "$ENV_C5_ZIGBEE"
                        fi
                        if [[ "${FW_UPLOAD_METHOD,,}" == "usb" ]]; then
                            ensure_zigbee_boot_partition
                        fi
                        ;;
                    esp8266)
                        build_env "$ENV_ESP8266"
                        copy_one_to_upgrade "$ENV_ESP8266" "firmware_esp8266.bin"
                        upload_env_auto "$ENV_ESP8266"
                        ;;
                    ospi)
                        if $deploy_debug; then
                            info "Debug build for OSPI (OsPi does not support ENABLE_DEBUG flags)"
                        fi
                        build_ospi
                        ;;
                    all|"")
                        build_env "$ENV_C5_MATTER"
                        build_env "$ENV_C5_ZIGBEE"
                        build_env "$ENV_ESP8266"
                        if [[ "$deploy_variant" == "all" ]]; then
                            build_ospi
                        fi
                        copy_to_upgrade
                        upload_env_auto "$ENV_C5_MATTER"
                        upload_env_auto "$ENV_C5_ZIGBEE"
                        upload_env_auto "$ENV_ESP8266"
                        if [[ "${FW_UPLOAD_METHOD,,}" == "usb" ]]; then
                            ensure_zigbee_boot_partition
                        fi
                        header "Deploy complete"
                        ok "Matter:   flashed to ota_1 (0x3A0000)"
                        ok "ZigBee:   flashed to ota_0 (0x10000)"
                        ok "ESP8266:  flashed"
                        ok "upgrade/  synced with current build"
                        ;;
                    *) error "Unknown variant: $deploy_variant (matter|zigbee|esp8266|ospi|all|debug|monitor)"; exit 1 ;;
                    esac

                    if ! $deploy_debug; then
                        restore_release_debug
                        trap - EXIT
                    fi
                fi
                ;;
        esac
        ;;

    monitor)
        # For monitor, VARIANT is the port (not a firmware variant)
        # Detect if the user forgot to provide a port
        if [[ "$VARIANT" == "all" ]]; then
            error "Usage: ./fw.sh monitor <port>"
            error "Example: ./fw.sh monitor /dev/ttyUSB1"
            exit 1
        fi
        monitor_only "$VARIANT"
        ;;

    release)
        case "$VARIANT" in
            rebuild)  do_release rebuild ;;
            sync-tags) github_sync_tag_releases ;;
            all|"")   do_release full ;;
            *) error "Unknown release mode: $VARIANT (rebuild|sync-tags)"; exit 1 ;;
        esac
        ;;

    libs)
        # Optional second argument selects libs mode.
        run_libs "$VARIANT"
        ;;

    online-deploy|upload-online)
        online_deploy
        ;;

    generate-mfg)
        generate_matter_mfg
        ;;

    flash-mfg)
        flash_matter_kvs
        ;;

    full-flash)
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
