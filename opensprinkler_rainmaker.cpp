/* OpenSprinkler Firmware
 * Copyright (C) 2026 by OpenSprinkler Shop Ltd.
 *
 * ESP RainMaker Integration — Alexa & Google Home Smart Home
 *
 * This module registers OpenSprinkler irrigation zones as RainMaker
 * "Switch" devices (Alexa/Google Home compatible) and exposes sensor
 * data (rain sensor, flow sensor, temperature, soil moisture) as
 * RainMaker "Temperature-Sensor" / custom devices.
 *
 * Uses the Arduino RainMaker wrapper (RMaker, WiFiProv) for lifecycle
 * management and raw ESP-IDF APIs for device/param creation.
 *
 * CRITICAL: The prebuilt RainMaker library does NOT auto-start
 * esp_local_ctrl (CONFIG_ESP_RMAKER_LOCAL_CTRL_ENABLE is not set).
 * We start it manually after RMaker.start() to enable "On Network"
 * provisioning in the ESP RainMaker phone app.
 */

#include "defines.h"

#if defined(ESP32) && defined(ENABLE_RAINMAKER)

#include "opensprinkler_rainmaker.h"
#include "OpenSprinkler.h"
#include "program.h"
#include "sensors.h"
#include "SensorBase.hpp"
#if defined(OS_ENABLE_BLE)
#include "sensor_ble.h"
#endif

// Arduino RainMaker wrapper (lifecycle + events)
#include "RMaker.h"

extern "C" {
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_standard_services.h>
#include <esp_rmaker_mqtt.h>
#include <esp_rmaker_user_mapping.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_mac.h>
#include <esp_local_ctrl.h>
#include <esp_wifi.h>
#include <nvs.h>

// Exported by prebuilt RainMaker library — handles CmdSetUserMapping protobuf
// and calls esp_rmaker_start_user_node_mapping() internally.
esp_err_t esp_rmaker_user_mapping_handler(uint32_t session_id,
                                          const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen,
                                          void *priv_data);

// Exported by prebuilt RainMaker library (CONFIG_ESP_RMAKER_FACTORY_RESET_REPORTING).
// Publishes {"node_id":"...","user_id":"esp-rmaker","secret_key":"failed","reset":true}
// via MQTT to tell the cloud to disassociate this node from its user.
esp_err_t esp_rmaker_reset_user_node_mapping(void);
}

#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <mdns.h>
#include <esp_heap_caps.h>

#include <new>       // placement new for PSRAM-allocated struct
#include <ctype.h>

static const char *TAG = "OSRainMaker";

extern OpenSprinkler os;
extern ProgramData pd;
extern ulong flow_count;

// OTF web server — must be stopped before SoftAP provisioning (port 80 conflict)
#if defined(USE_OTF)
#include "OpenThingsFramework.h"
extern OTF::OpenThingsFramework *otf;
#endif

// Forward declarations from main.cpp (use correct types)
extern void schedule_all_stations(time_os_t curr_time, unsigned char req_option);
extern void turn_off_station(unsigned char sid, time_os_t curr_time, unsigned char shift);
extern bool useEth;  // true when connected via Ethernet

// ─── PSRAM-allocated state ───────────────────────────────────────────────────
//
// All mutable RainMaker data lives in a single heap block allocated in PSRAM
// (SPIRAM) to free the scarce internal SRAM.  Zone and sensor device arrays
// are sized dynamically at init (no fixed RMAKER_MAX_* limits).

struct OSRainMakerData {
  // ── State flags ──
  bool initialized = false;
  bool unlinking = false;
  bool use_ethernet = false;
  bool local_ctrl_active = false;
  bool mqtt_connected = false;
  int  user_mapping_state = 0;
  // Set true by unlink() so that the RMAKER_EVENT_USER_NODE_MAPPING_RESET event
  // (fired when the cloud MQTT reset PUBACK is received) triggers an immediate
  // factory reset instead of waiting for the 20-second fallback timer.
  bool unlink_factory_reset_pending = false;

  // ── Provisioning ──
  char prov_pop[12] = {};
  char prov_service_name[32] = {};

  // ── Zones (dynamically allocated in PSRAM) ──
  esp_rmaker_device_t **zone_devices = nullptr;
  uint8_t             *zone_sid_map  = nullptr;
  uint8_t              zone_count    = 0;

  // ── Controller ──
  esp_rmaker_device_t *controller_device = nullptr;
  esp_rmaker_param_t  *param_enabled     = nullptr;
  esp_rmaker_param_t  *param_rain_delay  = nullptr;
  esp_rmaker_param_t  *param_rain_sensor = nullptr;
  esp_rmaker_param_t  *param_water_level = nullptr;

  // ── Sensors (dynamically allocated in PSRAM) ──
  esp_rmaker_device_t **sensor_devices = nullptr;
  uint8_t              sensor_count    = 0;

  // ── Timing ──
  unsigned long last_sensor_update_ms = 0;

  // ── Internal flags ──
  bool rmaker_handler_registered  = false;
  bool local_ctrl_started         = false;   // set when esp_local_ctrl is running
  bool local_chal_resp_enabled    = false;
  bool chal_resp_disabled         = false;

  // ── Local-ctrl config (must persist while service runs) ──
  esp_local_ctrl_handlers_t         lc_handlers  = {};
  httpd_ssl_config_t                lc_httpd_cfg = {};
  protocomm_security1_params_t      lc_sec1      = {};
};

// File-scope pointer — set once by OSRainMaker::ensure_data(), used by
// free-standing callbacks and helpers that cannot take a this pointer.
static OSRainMakerData *D = nullptr;

// Allocate zeroed memory, preferring PSRAM when available.
static inline void *rmaker_calloc_psram(size_t nmemb, size_t size) {
#if defined(BOARD_HAS_PSRAM)
  void *p = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return calloc(nmemb, size);
}

static const unsigned long SENSOR_UPDATE_INTERVAL_MS = 30000; // 30 seconds

static const char *CHAL_RESP_ENDPOINT = "ch_resp";
static const char *CHAL_RESP_MDNS_TYPE = "_esp_rmaker_chal_resp";
static const char *CHAL_RESP_MDNS_PROTO = "_tcp";
static const uint16_t CHAL_RESP_LOCAL_CTRL_PORT = 12312;
static const uint32_t CHAL_RESP_SEC_VERSION = 1;

enum ChalRespMsgType {
  CHAL_RESP_MSG_CMD_CHALLENGE = 0,
  CHAL_RESP_MSG_RESP_CHALLENGE = 1,
  CHAL_RESP_MSG_CMD_GET_NODE_ID = 2,
  CHAL_RESP_MSG_RESP_GET_NODE_ID = 3,
  CHAL_RESP_MSG_CMD_DISABLE = 4,
  CHAL_RESP_MSG_RESP_DISABLE = 5,
};

enum ChalRespStatus {
  CHAL_RESP_STATUS_SUCCESS = 0,
  CHAL_RESP_STATUS_FAIL = 1,
  CHAL_RESP_STATUS_INVALID_PARAM = 2,
  CHAL_RESP_STATUS_DISABLED = 3,
};

static esp_err_t enable_local_ctrl_chal_resp();
static bool local_ctrl_started_runtime();

static bool read_varint32(const uint8_t *buf, size_t len, size_t *index, uint32_t *value) {
  if (!buf || !index || !value) return false;

  uint32_t result = 0;
  uint32_t shift = 0;
  while (*index < len && shift < 32) {
    uint8_t byte = buf[(*index)++];
    result |= (uint32_t)(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

static bool skip_field(const uint8_t *buf, size_t len, size_t *index, uint32_t wire_type) {
  uint32_t ignored = 0;
  switch (wire_type) {
    case 0:
      return read_varint32(buf, len, index, &ignored);
    case 2: {
      uint32_t field_len = 0;
      if (!read_varint32(buf, len, index, &field_len)) return false;
      if (*index + field_len > len) return false;
      *index += field_len;
      return true;
    }
    default:
      return false;
  }
}

static bool read_length_delimited(const uint8_t *buf, size_t len, size_t *index,
                                  const uint8_t **data, size_t *data_len) {
  uint32_t field_len = 0;
  if (!read_varint32(buf, len, index, &field_len)) return false;
  if (*index + field_len > len) return false;
  *data = &buf[*index];
  *data_len = field_len;
  *index += field_len;
  return true;
}

static bool parse_cmd_cr_payload(const uint8_t *buf, size_t len,
                                 const uint8_t **payload, size_t *payload_len) {
  size_t index = 0;
  while (index < len) {
    uint32_t key = 0;
    if (!read_varint32(buf, len, &index, &key)) return false;

    uint32_t field_no = key >> 3;
    uint32_t wire_type = key & 0x07;
    if (field_no == 1 && wire_type == 2) {
      return read_length_delimited(buf, len, &index, payload, payload_len);
    }
    if (!skip_field(buf, len, &index, wire_type)) return false;
  }
  return false;
}

static bool parse_chal_resp_request(const uint8_t *buf, size_t len,
                                    ChalRespMsgType *msg_type,
                                    const uint8_t **payload,
                                    size_t *payload_len) {
  if (!buf || len == 0 || !msg_type || !payload || !payload_len) return false;

  size_t index = 0;
  bool msg_seen = false;
  *payload = nullptr;
  *payload_len = 0;

  while (index < len) {
    uint32_t key = 0;
    if (!read_varint32(buf, len, &index, &key)) return false;

    uint32_t field_no = key >> 3;
    uint32_t wire_type = key & 0x07;

    if (field_no == 1 && wire_type == 0) {
      uint32_t msg = 0;
      if (!read_varint32(buf, len, &index, &msg)) return false;
      *msg_type = (ChalRespMsgType)msg;
      msg_seen = true;
      continue;
    }

    if (field_no == 10 && wire_type == 2) {
      const uint8_t *nested = nullptr;
      size_t nested_len = 0;
      if (!read_length_delimited(buf, len, &index, &nested, &nested_len)) return false;
      if (!parse_cmd_cr_payload(nested, nested_len, payload, payload_len)) return false;
      continue;
    }

    if (!skip_field(buf, len, &index, wire_type)) return false;
  }

  return msg_seen;
}

static size_t write_varint32(uint32_t value, uint8_t *out) {
  size_t written = 0;
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value) byte |= 0x80;
    out[written++] = byte;
  } while (value);
  return written;
}

static size_t append_varint_field(uint8_t *out, uint32_t field_no, uint32_t value) {
  size_t index = 0;
  index += write_varint32((field_no << 3) | 0, out + index);
  index += write_varint32(value, out + index);
  return index;
}

static size_t append_bytes_field(uint8_t *out, uint32_t field_no, const uint8_t *data, size_t len) {
  size_t index = 0;
  index += write_varint32((field_no << 3) | 2, out + index);
  index += write_varint32((uint32_t)len, out + index);
  if (len > 0 && data) {
    memcpy(out + index, data, len);
    index += len;
  }
  return index;
}

static size_t append_string_field(uint8_t *out, uint32_t field_no, const char *value) {
  const size_t len = value ? strlen(value) : 0;
  return append_bytes_field(out, field_no, (const uint8_t *)value, len);
}

static esp_err_t build_simple_response(ChalRespMsgType msg_type, ChalRespStatus status,
                                       uint8_t **outbuf, ssize_t *outlen) {
  uint8_t *buf = (uint8_t *)calloc(1, 16);
  if (!buf) return ESP_ERR_NO_MEM;

  size_t index = 0;
  index += append_varint_field(buf + index, 1, (uint32_t)msg_type);
  index += append_varint_field(buf + index, 2, (uint32_t)status);

  *outbuf = buf;
  *outlen = (ssize_t)index;
  return ESP_OK;
}

static esp_err_t build_get_node_id_response(ChalRespStatus status,
                                            uint8_t **outbuf, ssize_t *outlen) {
  const char *node_id = esp_rmaker_get_node_id();
  const size_t node_id_len = node_id ? strlen(node_id) : 0;
  uint8_t nested[128] = {};
  size_t nested_len = append_string_field(nested, 1, node_id ? node_id : "");

  uint8_t *buf = (uint8_t *)calloc(1, nested_len + node_id_len + 32);
  if (!buf) return ESP_ERR_NO_MEM;

  size_t index = 0;
  index += append_varint_field(buf + index, 1, CHAL_RESP_MSG_RESP_GET_NODE_ID);
  index += append_varint_field(buf + index, 2, (uint32_t)status);
  index += append_bytes_field(buf + index, 13, nested, nested_len);

  *outbuf = buf;
  *outlen = (ssize_t)index;
  return ESP_OK;
}

static esp_err_t build_signed_challenge_response(ChalRespStatus status,
                                                 const uint8_t *signature, size_t signature_len,
                                                 uint8_t **outbuf, ssize_t *outlen) {
  const char *node_id = esp_rmaker_get_node_id();
  const size_t node_id_len = node_id ? strlen(node_id) : 0;
  const size_t nested_cap = signature_len + node_id_len + 24;
  uint8_t *nested = (uint8_t *)calloc(1, nested_cap);
  if (!nested) return ESP_ERR_NO_MEM;

  size_t nested_len = 0;
  nested_len += append_bytes_field(nested + nested_len, 1, signature, signature_len);
  nested_len += append_string_field(nested + nested_len, 2, node_id ? node_id : "");

  uint8_t *buf = (uint8_t *)calloc(1, nested_len + 32);
  if (!buf) {
    free(nested);
    return ESP_ERR_NO_MEM;
  }

  size_t index = 0;
  index += append_varint_field(buf + index, 1, CHAL_RESP_MSG_RESP_CHALLENGE);
  index += append_varint_field(buf + index, 2, (uint32_t)status);
  index += append_bytes_field(buf + index, 11, nested, nested_len);

  free(nested);
  *outbuf = buf;
  *outlen = (ssize_t)index;
  return ESP_OK;
}

static esp_err_t local_ctrl_chal_resp_handler(uint32_t session_id, const uint8_t *inbuf,
                                              ssize_t inlen, uint8_t **outbuf,
                                              ssize_t *outlen, void *priv_data) {
  (void)session_id;
  (void)priv_data;

  if (!inbuf || inlen <= 0 || !outbuf || !outlen) return ESP_ERR_INVALID_ARG;

  const uint8_t *payload = nullptr;
  size_t payload_len = 0;
  ChalRespMsgType msg_type = CHAL_RESP_MSG_CMD_CHALLENGE;
  if (!parse_chal_resp_request(inbuf, (size_t)inlen, &msg_type, &payload, &payload_len)) {
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_INVALID_PARAM,
                                 outbuf, outlen);
  }

  if (D->chal_resp_disabled) {
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_DISABLED,
                                 outbuf, outlen);
  }

  if (msg_type == CHAL_RESP_MSG_CMD_GET_NODE_ID) {
    return build_get_node_id_response(CHAL_RESP_STATUS_SUCCESS, outbuf, outlen);
  }

  if (msg_type == CHAL_RESP_MSG_CMD_DISABLE) {
    D->chal_resp_disabled = true;
    mdns_service_remove(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO);
    D->local_chal_resp_enabled = false;
    return build_simple_response(CHAL_RESP_MSG_RESP_DISABLE,
                                 CHAL_RESP_STATUS_SUCCESS,
                                 outbuf, outlen);
  }

  if (msg_type != CHAL_RESP_MSG_CMD_CHALLENGE || !payload || payload_len == 0) {
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_INVALID_PARAM,
                                 outbuf, outlen);
  }

  void *signed_data = nullptr;
  size_t signed_len = 0;
  esp_err_t err = esp_rmaker_node_auth_sign_msg(payload, payload_len, &signed_data, &signed_len);
  if (err != ESP_OK) {
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_FAIL,
                                 outbuf, outlen);
  }

  // esp_rmaker_node_auth_sign_msg returns a hex-encoded string.
  // The RainMaker app expects binary bytes in the protobuf signature field —
  // just like the official esp_rmaker_chal_resp.c which calls hex_str_to_bin() first.
  if (signed_len % 2 != 0) {
    ESP_LOGE(TAG, "Odd-length hex signature (%u), dropping", (unsigned)signed_len);
    free(signed_data);
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_FAIL, outbuf, outlen);
  }
  size_t binary_len = signed_len / 2;
  uint8_t *binary_sig = (uint8_t *)malloc(binary_len);
  if (!binary_sig) {
    free(signed_data);
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_FAIL, outbuf, outlen);
  }
  const char *hex = (const char *)signed_data;
  auto hval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  bool hex_ok = true;
  for (size_t i = 0; i < binary_len; i++) {
    int hi = hval(hex[2 * i]);
    int lo = hval(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) { hex_ok = false; break; }
    binary_sig[i] = (uint8_t)((hi << 4) | lo);
  }
  free(signed_data);
  if (!hex_ok) {
    ESP_LOGE(TAG, "Invalid hex character in signature, dropping");
    free(binary_sig);
    return build_simple_response(CHAL_RESP_MSG_RESP_CHALLENGE,
                                 CHAL_RESP_STATUS_FAIL, outbuf, outlen);
  }
  err = build_signed_challenge_response(CHAL_RESP_STATUS_SUCCESS,
                                        binary_sig, binary_len,
                                        outbuf, outlen);
  free(binary_sig);
  return err;
}

static esp_err_t enable_local_ctrl_chal_resp() {
  if (!local_ctrl_started_runtime()) return ESP_ERR_INVALID_STATE;

  D->chal_resp_disabled = false;

  esp_err_t err = esp_local_ctrl_set_handler(CHAL_RESP_ENDPOINT,
                                             local_ctrl_chal_resp_handler,
                                             nullptr);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register local chall-resp handler: %s", esp_err_to_name(err));
    return err;
  }

  err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mDNS init failed for chall-resp: %s", esp_err_to_name(err));
  }

  // Set mDNS hostname (required for service announcements to be visible)
  const char *node_id = esp_rmaker_get_node_id();
  if (node_id && node_id[0]) {
    mdns_hostname_set(node_id);
    ESP_LOGI(TAG, "mDNS hostname set to: %s", node_id);
  }

  // mdns_init() was called AFTER WiFi already obtained an IP, so the mDNS
  // stack missed the IP_EVENT_STA_GOT_IP event and never enabled the STA
  // interface.  Manually trigger enable + announce on the STA netif.
  esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta_netif) {
    err = mdns_netif_action(sta_netif,
            (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "mdns_netif_action(STA, ENABLE|ANNOUNCE) failed: %s", esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "mDNS STA interface enabled and announced");
    }
  } else {
    ESP_LOGW(TAG, "Could not get WIFI_STA_DEF netif for mDNS");
  }

  const char *instance_name = OSRainMaker::instance().get_prov_service_name();
  if (!instance_name || !instance_name[0]) {
    instance_name = node_id;
  }

  // ── 1. Register _esp_local_ctrl._tcp (what the RainMaker app discovers) ──
  mdns_service_remove("_esp_local_ctrl", "_tcp");
  err = mdns_service_add(instance_name, "_esp_local_ctrl", "_tcp",
                         CHAL_RESP_LOCAL_CTRL_PORT, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to advertise _esp_local_ctrl mDNS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Registered _esp_local_ctrl._tcp on port %u", CHAL_RESP_LOCAL_CTRL_PORT);
    if (node_id && node_id[0]) {
      mdns_service_txt_item_set("_esp_local_ctrl", "_tcp", "node_id", node_id);
    }
    // Re-add the standard esp_local_ctrl endpoint TXT records that the HTTPD
    // transport would have set, but were lost when we removed and re-added the service.
    mdns_service_txt_item_set("_esp_local_ctrl", "_tcp", "version_endpoint", "/esp_local_ctrl/version");
    mdns_service_txt_item_set("_esp_local_ctrl", "_tcp", "session_endpoint", "/esp_local_ctrl/session");
    mdns_service_txt_item_set("_esp_local_ctrl", "_tcp", "control_endpoint", "/esp_local_ctrl/control");
  }

  // ── 2. Register _esp_rmaker_chal_resp._tcp (challenge-response endpoint) ──
  mdns_service_remove(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO);
  err = mdns_service_add(instance_name, CHAL_RESP_MDNS_TYPE,
                         CHAL_RESP_MDNS_PROTO, CHAL_RESP_LOCAL_CTRL_PORT,
                         nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to advertise chall-resp mDNS service: %s", esp_err_to_name(err));
    D->local_chal_resp_enabled = true;
    return ESP_OK;
  }

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", CHAL_RESP_LOCAL_CTRL_PORT);
  char sec_ver_str[4];
  snprintf(sec_ver_str, sizeof(sec_ver_str), "%u", (unsigned)CHAL_RESP_SEC_VERSION);

  const char *pop = OSRainMaker::instance().get_pop();
  const char *pop_required = (CHAL_RESP_SEC_VERSION != 0 && pop && pop[0]) ? "true" : "false";

  if (node_id && node_id[0]) {
    mdns_service_txt_item_set(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO, "node_id", node_id);
  }
  mdns_service_txt_item_set(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO, "port", port_str);
  mdns_service_txt_item_set(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO, "sec_version", sec_ver_str);
  mdns_service_txt_item_set(CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO, "pop_required", pop_required);

  D->local_chal_resp_enabled = true;
  ESP_LOGI(TAG, "Enabled on-network chall-resp via local control: %s.%s port=%u",
           CHAL_RESP_MDNS_TYPE, CHAL_RESP_MDNS_PROTO, CHAL_RESP_LOCAL_CTRL_PORT);
  return ESP_OK;
}

static bool local_ctrl_started_runtime() {
  return D && D->local_ctrl_started;
}

static const char *wifi_mode_to_str(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL: return "off";
    case WIFI_MODE_STA: return "sta";
    case WIFI_MODE_AP: return "ap";
    case WIFI_MODE_APSTA: return "apsta";
    default: return "unknown";
  }
}

static const char *rmaker_event_to_str(int32_t id) {
  switch (id) {
    case RMAKER_EVENT_INIT_DONE: return "INIT_DONE";
    case RMAKER_EVENT_CLAIM_STARTED: return "CLAIM_STARTED";
    case RMAKER_EVENT_CLAIM_SUCCESSFUL: return "CLAIM_SUCCESSFUL";
    case RMAKER_EVENT_CLAIM_FAILED: return "CLAIM_FAILED";
    case RMAKER_EVENT_USER_NODE_MAPPING_DONE: return "USER_NODE_MAPPING_DONE";
    case RMAKER_EVENT_LOCAL_CTRL_STARTED: return "LOCAL_CTRL_STARTED";
    case RMAKER_EVENT_USER_NODE_MAPPING_RESET: return "USER_NODE_MAPPING_RESET";
    case RMAKER_EVENT_LOCAL_CTRL_STOPPED: return "LOCAL_CTRL_STOPPED";
    default: return "UNKNOWN";
  }
}

static void log_runtime_snapshot(const char *reason, bool prov_active) {
  wifi_mode_t wifi_mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&wifi_mode);

  const char *svc = OSRainMaker::instance().get_prov_service_name();
  const char *pop = OSRainMaker::instance().get_pop();

  ESP_LOGI(TAG,
           "[STATE] %s | eth=%d wifi=%s prov=%d local_ctrl=%d mqtt=%d map=%d svc=%s pop=%s",
           reason ? reason : "(none)",
           useEth ? 1 : 0,
           wifi_mode_to_str(wifi_mode),
           prov_active ? 1 : 0,
           local_ctrl_started_runtime() ? 1 : 0,
           esp_rmaker_is_mqtt_connected() ? 1 : 0,
           (int)esp_rmaker_user_node_mapping_get_state(),
           (svc && svc[0]) ? svc : "-",
           (pop && pop[0]) ? pop : "-");
}

static void rmaker_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data) {
  if (base != RMAKER_EVENT) return;

  const char *event_name = rmaker_event_to_str(id);
  if (id == RMAKER_EVENT_LOCAL_CTRL_STARTED && data) {
    ESP_LOGI(TAG, "[RMAKER] Event %s service=%s", event_name, (const char *)data);
  } else if (id == RMAKER_EVENT_USER_NODE_MAPPING_DONE && data) {
    ESP_LOGI(TAG, "[RMAKER] Event %s user=%s", event_name, (const char *)data);
  } else {
    ESP_LOGI(TAG, "[RMAKER] Event %s (%ld)", event_name, (long)id);
  }

  if (id == RMAKER_EVENT_LOCAL_CTRL_STARTED) {
    // This event only fires if the prebuilt lib has LOCAL_CTRL enabled.
    // We start local_ctrl manually, so this is just a fallback.
    if (!D->local_ctrl_started) {
      D->local_ctrl_started = true;
      enable_local_ctrl_chal_resp();
    }
  } else if (id == RMAKER_EVENT_LOCAL_CTRL_STOPPED) {
    D->local_ctrl_started = false;
    D->local_chal_resp_enabled = false;
  } else if (id == RMAKER_EVENT_USER_NODE_MAPPING_RESET) {
    // The cloud PUBACK for the user-mapping reset MQTT message has been received.
    // If we are in the middle of an unlink() flow, trigger the factory reset NOW
    // (instead of waiting for the 20-second fallback timer).  This guarantees the
    // reset message actually reached the RainMaker cloud before we erase NVS.
    if (D->unlink_factory_reset_pending) {
      D->unlink_factory_reset_pending = false;
      ESP_LOGI(TAG, "[UNLINK] Cloud mapping reset confirmed (MQTT PUBACK received). "
                    "Factory-resetting NVS immediately.");
      // reset_seconds=0 → erase NVS immediately; reboot_seconds=2 → reboot 2 s later.
      // The 20-second factory-reset fallback timer (started in unlink()) will fire
      // after the reboot and is harmless.
      esp_rmaker_factory_reset(0, 2);
    }
  }

  log_runtime_snapshot(event_name, false);
}

// ─── Write callback: zone on/off from Alexa / Google Home (Arduino style) ────

static esp_err_t zone_write_cb(const esp_rmaker_device_t *device,
                               const esp_rmaker_param_t *param,
                               const esp_rmaker_param_val_t val,
                               void *priv_data,
                               esp_rmaker_write_ctx_t *ctx)
{
  // Get parameter type
  const char *param_type = esp_rmaker_param_get_type(const_cast<esp_rmaker_param_t*>(param));
  if (!param_type || strcmp(param_type, ESP_RMAKER_PARAM_POWER) != 0) {
    return ESP_OK;  // Only handle Power parameter
  }

  uint8_t sid = (uint8_t)(uintptr_t)priv_data;
  if (sid >= os.nstations) return ESP_ERR_INVALID_ARG;

  bool power_on = val.val.b;
  unsigned long curr_time = os.now_tz();

  if (power_on) {
    // Turn on for 10 minutes (default manual run)
    if ((os.status.mas == (unsigned char)(sid + 1)) ||
        (os.status.mas2 == (unsigned char)(sid + 1))) {
      ESP_LOGW(TAG, "Refusing to turn on master station %d", sid);
      return ESP_ERR_NOT_ALLOWED;
    }

    RuntimeQueueStruct *q = pd.enqueue();
    if (!q) return ESP_ERR_NO_MEM;

    q->st  = 0;
    q->dur = 600;  // 10 minutes
    q->sid = sid;
    q->pid = 99;   // manual
    schedule_all_stations(curr_time, 0);
    ESP_LOGI(TAG, "Zone %d ON via RainMaker", sid);
  } else {
    // Turn off
    if (pd.station_qid[sid] != 255) {
      turn_off_station(sid, curr_time, 0);
      ESP_LOGI(TAG, "Zone %d OFF via RainMaker", sid);
    }
  }

  // Update and report to cloud
  esp_rmaker_param_update_and_report(const_cast<esp_rmaker_param_t*>(param), val);
  return ESP_OK;
}

// ─── Write callback: controller params (Arduino style) ───────────────────────

static esp_err_t controller_write_cb(const esp_rmaker_device_t *device,
                                     const esp_rmaker_param_t *param,
                                     const esp_rmaker_param_val_t val,
                                     void *priv_data,
                                     esp_rmaker_write_ctx_t *ctx)
{
  const char *param_name = esp_rmaker_param_get_name(const_cast<esp_rmaker_param_t*>(param));
  if (!param_name) return ESP_FAIL;

  if (strcmp(param_name, "Enabled") == 0) {
    // Controller enabled/disabled toggle
    os.status.enabled = val.val.b ? 1 : 0;
    os.iopts[IOPT_DEVICE_ENABLE] = os.status.enabled;
    os.iopts_save();
    ESP_LOGI(TAG, "Controller %s", val.val.b ? "enabled" : "disabled");
  } 
  else if (strcmp(param_name, "Rain Delay") == 0) {
    // Rain delay in hours (0 = clear)
    int hours = val.val.i;
    if (hours > 0) {
      os.nvdata.rd_stop_time = os.now_tz() + (unsigned long)hours * 3600UL;
      os.raindelay_start();
    } else {
      os.nvdata.rd_stop_time = 0;
      os.raindelay_stop();
    }
    ESP_LOGI(TAG, "Rain delay: %d hours", hours);
  }

  // Report updated value to cloud
  esp_rmaker_param_update_and_report(const_cast<esp_rmaker_param_t*>(param), val);
  return ESP_OK;
}

// ─── Create zone devices ─────────────────────────────────────────────────────

static void create_zone_devices(esp_rmaker_node_t *node) {
  // Pass 1: count eligible zones
  uint8_t eligible = 0;
  for (uint8_t sid = 0; sid < os.nstations; sid++) {
    uint8_t bid = sid >> 3;
    uint8_t s   = sid & 0x07;
    if (os.attrib_dis[bid] & (1 << s)) continue;
    if (os.is_master_station(sid)) continue;
    eligible++;
  }
  if (eligible == 0) { D->zone_count = 0; return; }

  // Allocate exact-size arrays in PSRAM
  D->zone_devices = (esp_rmaker_device_t **)rmaker_calloc_psram(eligible, sizeof(esp_rmaker_device_t *));
  D->zone_sid_map = (uint8_t *)rmaker_calloc_psram(eligible, sizeof(uint8_t));
  if (!D->zone_devices || !D->zone_sid_map) {
    ESP_LOGE(TAG, "Failed to allocate zone arrays (%d entries)", eligible);
    D->zone_count = 0;
    return;
  }

  // Pass 2: create devices
  char name_buf[40];
  char stn_name[32];
  uint8_t count = 0;
  for (uint8_t sid = 0; sid < os.nstations && count < eligible; sid++) {
    uint8_t bid = sid >> 3;
    uint8_t s   = sid & 0x07;
    if (os.attrib_dis[bid] & (1 << s)) continue;
    if (os.is_master_station(sid)) continue;

    os.get_station_name(sid, stn_name);
    if (stn_name[0] == '\0') {
      snprintf(name_buf, sizeof(name_buf), "Zone %d", sid + 1);
    } else {
      snprintf(name_buf, sizeof(name_buf), "%.31s", stn_name);
    }

    esp_rmaker_device_t *dev = esp_rmaker_switch_device_create(
        name_buf, (void*)(uintptr_t)sid, os.is_running(sid) ? true : false);
    if (!dev) {
      ESP_LOGE(TAG, "Failed to create zone device for sid=%d", sid);
      continue;
    }

    esp_rmaker_device_add_cb(dev, zone_write_cb, nullptr);
    esp_rmaker_device_add_subtype(dev, "esp.subtype.irrigation-valve");

    char sid_str[8];
    snprintf(sid_str, sizeof(sid_str), "%d", sid);
    esp_rmaker_device_add_attribute(dev, "station_id", sid_str);

    esp_rmaker_node_add_device(node, dev);

    D->zone_devices[count] = dev;
    D->zone_sid_map[count]  = sid;
    count++;
  }
  D->zone_count = count;
  ESP_LOGI(TAG, "Created %d zone devices (dynamic, PSRAM)", count);
}

// ─── Create controller device ────────────────────────────────────────────────

static void create_controller_device(esp_rmaker_node_t *node) {
  // Use "Other" device type with custom params for controller-level controls
  D->controller_device = esp_rmaker_device_create(
      "Controller", ESP_RMAKER_DEVICE_OTHER, nullptr);
  if (!D->controller_device) {
    ESP_LOGE(TAG, "Failed to create controller device");
    return;
  }

  esp_rmaker_device_add_cb(D->controller_device, controller_write_cb, nullptr);
  esp_rmaker_device_add_subtype(D->controller_device, "esp.subtype.irrigation-controller");

  // Name param (standard, read-only display name)
  esp_rmaker_param_t *p_name = esp_rmaker_name_param_create(
      ESP_RMAKER_DEF_NAME_PARAM, "Controller");
  esp_rmaker_device_add_param(D->controller_device, p_name);

  // Enabled toggle (esp.param.toggle + esp.ui.toggle)
  D->param_enabled = esp_rmaker_param_create(
      "Enabled", ESP_RMAKER_PARAM_TOGGLE,
      esp_rmaker_bool(os.status.enabled ? true : false),
      PROP_FLAG_READ | PROP_FLAG_WRITE | PROP_FLAG_PERSIST);
  esp_rmaker_param_add_ui_type(D->param_enabled, ESP_RMAKER_UI_TOGGLE);
  esp_rmaker_device_add_param(D->controller_device, D->param_enabled);

  // Rain Delay (hours, slider 0–96)
  D->param_rain_delay = esp_rmaker_param_create(
      "Rain Delay", ESP_RMAKER_PARAM_RANGE,
      esp_rmaker_int(0),
      PROP_FLAG_READ | PROP_FLAG_WRITE);
  esp_rmaker_param_add_ui_type(D->param_rain_delay, ESP_RMAKER_UI_SLIDER);
  esp_rmaker_param_add_bounds(D->param_rain_delay,
      esp_rmaker_int(0), esp_rmaker_int(96), esp_rmaker_int(1));
  esp_rmaker_device_add_param(D->controller_device, D->param_rain_delay);

  // Rain Sensor (read-only)
  D->param_rain_sensor = esp_rmaker_param_create(
      "Rain Sensor", ESP_RMAKER_PARAM_TOGGLE,
      esp_rmaker_bool(os.status.sensor1_active ? true : false),
      PROP_FLAG_READ);
  esp_rmaker_param_add_ui_type(D->param_rain_sensor, ESP_RMAKER_UI_TOGGLE);
  esp_rmaker_device_add_param(D->controller_device, D->param_rain_sensor);

  // Water Level (percentage, read-only)
  D->param_water_level = esp_rmaker_param_create(
      "Water Level", ESP_RMAKER_PARAM_RANGE,
      esp_rmaker_int(os.iopts[IOPT_WATER_PERCENTAGE]),
      PROP_FLAG_READ);
  esp_rmaker_param_add_ui_type(D->param_water_level, ESP_RMAKER_UI_SLIDER);
  esp_rmaker_param_add_bounds(D->param_water_level,
      esp_rmaker_int(0), esp_rmaker_int(250), esp_rmaker_int(1));
  esp_rmaker_device_add_param(D->controller_device, D->param_water_level);

  esp_rmaker_device_assign_primary_param(D->controller_device, D->param_enabled);
  esp_rmaker_node_add_device(node, D->controller_device);

  ESP_LOGI(TAG, "Created controller device");
}

// ─── Create sensor devices from the sensor API ──────────────────────────────

static void create_sensor_devices(esp_rmaker_node_t *node) {
  // Pass 1: count eligible sensors
  uint8_t eligible = 0;
  {
    SensorIterator it = sensors_iterate_begin();
    for (;;) {
      SensorBase *s = sensors_iterate_next(it);
      if (!s) break;
      if (!s->flags.enable || s->nr == 0) continue;
      eligible++;
    }
  }
  if (eligible == 0) { D->sensor_count = 0; return; }

  // Allocate exact-size array in PSRAM
  D->sensor_devices = (esp_rmaker_device_t **)rmaker_calloc_psram(eligible, sizeof(esp_rmaker_device_t *));
  if (!D->sensor_devices) {
    ESP_LOGE(TAG, "Failed to allocate sensor device array (%d entries)", eligible);
    D->sensor_count = 0;
    return;
  }

  // Pass 2: create devices
  uint8_t count = 0;
  SensorIterator it = sensors_iterate_begin();

  while (count < eligible) {
    SensorBase *s = sensors_iterate_next(it);
    if (!s) break;
    if (!s->flags.enable) continue;
    if (s->nr == 0) continue;  // deleted sensor

    char dev_name[40];
    if (s->name[0]) {
      snprintf(dev_name, sizeof(dev_name), "%.31s", s->name);
    } else {
      snprintf(dev_name, sizeof(dev_name), "Sensor %u", s->nr);
    }

    uint8_t uid = getSensorUnitId(s);
    esp_rmaker_device_t *dev = nullptr;

    // Map sensor unit to appropriate RainMaker device type
    switch (uid) {
      case UNIT_DEGREE:
      case UNIT_FAHRENHEIT: {
        // Use standard Temperature Sensor device
        dev = esp_rmaker_temp_sensor_device_create(
            dev_name, nullptr, (float)s->last_data);
        break;
      }
      case UNIT_PERCENT:
      case UNIT_HUM_PERCENT: {
        // Soil moisture / humidity — use custom device with range param
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              (uid == UNIT_HUM_PERCENT) ? "Humidity" : "Moisture",
              ESP_RMAKER_PARAM_RANGE,
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_SLIDER);
          esp_rmaker_param_add_bounds(pv,
              esp_rmaker_float(0.0f), esp_rmaker_float(100.0f), esp_rmaker_float(0.1f));
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
          esp_rmaker_device_add_subtype(dev, "esp.subtype.soil-sensor");
        }
        break;
      }
      case UNIT_LX:
      case UNIT_LM: {
        // Light sensor
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              "Illuminance", ESP_RMAKER_PARAM_TEMPERATURE, // closest standard param
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_TEXT);
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
        }
        break;
      }
      default: {
        // Generic sensor — expose value as text
        dev = esp_rmaker_device_create(dev_name, ESP_RMAKER_DEVICE_OTHER, nullptr);
        if (dev) {
          esp_rmaker_param_t *pn = esp_rmaker_name_param_create(
              ESP_RMAKER_DEF_NAME_PARAM, dev_name);
          esp_rmaker_device_add_param(dev, pn);

          const char *unit = getSensorUnit(s);
          char val_str[32];
          snprintf(val_str, sizeof(val_str), "%.2f %s", s->last_data, unit ? unit : "");

          esp_rmaker_param_t *pv = esp_rmaker_param_create(
              "Value", ESP_RMAKER_PARAM_TEMPERATURE,
              esp_rmaker_float((float)s->last_data),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
          esp_rmaker_param_add_ui_type(pv, ESP_RMAKER_UI_TEXT);
          esp_rmaker_device_add_param(dev, pv);
          esp_rmaker_device_assign_primary_param(dev, pv);
        }
        break;
      }
    }

    if (dev) {
      // Add sensor nr as attribute
      char nr_str[8];
      snprintf(nr_str, sizeof(nr_str), "%u", s->nr);
      esp_rmaker_device_add_attribute(dev, "sensor_nr", nr_str);

      // Add sensor type as attribute
      char type_str[8];
      snprintf(type_str, sizeof(type_str), "%u", s->type);
      esp_rmaker_device_add_attribute(dev, "sensor_type", type_str);

      esp_rmaker_node_add_device(node, dev);
      D->sensor_devices[count] = dev;
      count++;
    }
  }

  D->sensor_count = count;
  ESP_LOGI(TAG, "Created %d sensor devices (dynamic, PSRAM)", count);
}

// ─── Periodic sensor value updates ──────────────────────────────────────────

static void report_sensor_values() {
  if (!D) return;

  // Update controller params
  if (D->controller_device) {
    if (D->param_rain_sensor) {
      bool rain_active = (os.iopts[IOPT_SENSOR1_TYPE] == SENSOR_TYPE_RAIN)
                         ? (os.status.sensor1_active ? true : false) : false;
      esp_rmaker_param_update_and_report(D->param_rain_sensor, esp_rmaker_bool(rain_active));
    }
    if (D->param_water_level) {
      esp_rmaker_param_update_and_report(D->param_water_level,
          esp_rmaker_int(os.iopts[IOPT_WATER_PERCENTAGE]));
    }
    if (D->param_rain_delay) {
      int rd_hours = 0;
      if (os.nvdata.rd_stop_time > os.now_tz()) {
        rd_hours = (int)((os.nvdata.rd_stop_time - os.now_tz()) / 3600UL);
        if (rd_hours < 1) rd_hours = 1; // still active
      }
      esp_rmaker_param_update_and_report(D->param_rain_delay, esp_rmaker_int(rd_hours));
    }
  }

  // Update sensor device values
  uint8_t idx = 0;
  SensorIterator it = sensors_iterate_begin();

  while (idx < D->sensor_count) {
    SensorBase *s = sensors_iterate_next(it);
    if (!s) break;
    if (!s->flags.enable || s->nr == 0) continue;

    esp_rmaker_device_t *dev = D->sensor_devices[idx];
    if (!dev) { idx++; continue; }

    // Get the primary parameter and update it
    // For temp sensors, the primary param is "Temperature"
    // For others, it's the first custom param added
    uint8_t uid = getSensorUnitId(s);
    const char *param_name;
    switch (uid) {
      case UNIT_DEGREE:
      case UNIT_FAHRENHEIT:
        param_name = ESP_RMAKER_DEF_TEMPERATURE_NAME;
        break;
      case UNIT_PERCENT:
        param_name = "Moisture";
        break;
      case UNIT_HUM_PERCENT:
        param_name = "Humidity";
        break;
      case UNIT_LX:
      case UNIT_LM:
        param_name = "Illuminance";
        break;
      default:
        param_name = "Value";
        break;
    }

    esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_name(dev, param_name);
    if (p && s->flags.data_ok) {
      esp_rmaker_param_update_and_report(p, esp_rmaker_float((float)s->last_data));
    }
    idx++;
  }
}

// ─── On Network provisioning helpers ─────────────────────────────────────────

/** Generate PoP (Proof of Possession) from eFuse unique ID.
 *  Falls back to MAC address if eFuse is not programmed. */
static void generate_pop_from_efuse(char *buf, size_t len) {
  uint8_t uid[16] = {};
  esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128);
  if (err == ESP_OK && (uid[0] | uid[1] | uid[2] | uid[3]) != 0) {
    snprintf(buf, len, "%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3]);
    ESP_LOGI(TAG, "PoP derived from eFuse OPTIONAL_UNIQUE_ID: %s", buf);
  } else {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(buf, len, "%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGW(TAG, "eFuse OPTIONAL_UNIQUE_ID not set (err=%s), PoP from MAC: %s",
             esp_err_to_name(err), buf);
  }
}

/** Generate service name from MAC address. */
static void generate_service_name(char *buf, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BASE);
  snprintf(buf, len, "PROV_%02x%02x%02x", mac[3], mac[4], mac[5]);
  ESP_LOGI(TAG, "Service name: %s", buf);
}

/** Manually start esp_local_ctrl with HTTPD transport.
 *  The prebuilt RainMaker library has CONFIG_ESP_RMAKER_LOCAL_CTRL_ENABLE
 *  NOT set, so we start the service ourselves to enable "On Network"
 *  provisioning in the ESP RainMaker phone app. */
static esp_err_t start_local_ctrl_manually() {
  if (D->local_ctrl_started) return ESP_OK;

  // Minimal get/set handlers stored in PSRAM data block
  D->lc_handlers = {
    .get_prop_values = [](size_t, const esp_local_ctrl_prop_t[],
                          esp_local_ctrl_prop_val_t[], void*) -> esp_err_t {
      return ESP_OK;
    },
    .set_prop_values = [](size_t, const esp_local_ctrl_prop_t[],
                          const esp_local_ctrl_prop_val_t[], void*) -> esp_err_t {
      return ESP_OK;
    },
    .usr_ctx = nullptr,
    .usr_ctx_free_fn = nullptr,
  };

  // HTTPD transport on the challenge-response port (plain HTTP, no TLS)
  D->lc_httpd_cfg = HTTPD_SSL_CONFIG_DEFAULT();
  D->lc_httpd_cfg.port_insecure = CHAL_RESP_LOCAL_CTRL_PORT;
  D->lc_httpd_cfg.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
  D->lc_httpd_cfg.httpd.ctrl_port   = D->lc_httpd_cfg.httpd.ctrl_port + 10;  // avoid clash with OTF
  D->lc_httpd_cfg.httpd.max_uri_handlers = 12;

  // Build proper SEC1 security params (PoP struct, not raw string)
  D->lc_sec1 = {};
  const char *pop_str = OSRainMaker::instance().get_pop();
  D->lc_sec1.data = (const uint8_t *)pop_str;
  D->lc_sec1.len  = strlen(pop_str);

  esp_local_ctrl_config_t config = {};
  config.transport = ESP_LOCAL_CTRL_TRANSPORT_HTTPD;
  config.transport_config.httpd = &D->lc_httpd_cfg;
  config.proto_sec.version = PROTOCOM_SEC1;
  config.proto_sec.sec_params = &D->lc_sec1;
  config.handlers = D->lc_handlers;
  config.max_properties = 4;

  esp_err_t err = esp_local_ctrl_start(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_local_ctrl_start failed: %s", esp_err_to_name(err));
    return err;
  }

  D->local_ctrl_started = true;
  ESP_LOGI(TAG, "esp_local_ctrl started manually on port %u (sec1, pop=%s)",
           CHAL_RESP_LOCAL_CTRL_PORT, OSRainMaker::instance().get_pop());

  // Register user-node mapping endpoint ("cloud_user_assoc").
  // The RainMaker app sends CmdSetUserMapping with user_id + secret_key here
  // during the "Confirming Node Association" step of On Network provisioning.
  err = esp_local_ctrl_set_handler("cloud_user_assoc",
                                   esp_rmaker_user_mapping_handler, nullptr);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register cloud_user_assoc handler: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Registered cloud_user_assoc endpoint on local_ctrl");
  }

  // Now register the challenge-response endpoint and mDNS
  enable_local_ctrl_chal_resp();
  return ESP_OK;
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Heap-allocated singleton — only created when IOPT_RAINMAKER_ENABLE == 1.
static OSRainMaker *g_rmaker_inst = nullptr;

OSRainMaker* OSRainMaker::get() {
  return g_rmaker_inst;
}

OSRainMaker& OSRainMaker::instance() {
  if (!g_rmaker_inst) {
    g_rmaker_inst = new OSRainMaker();
  }
  return *g_rmaker_inst;
}

bool OSRainMaker::ensure_data() {
  if (d_) return true;
  void *mem = rmaker_calloc_psram(1, sizeof(OSRainMakerData));
  if (!mem) {
    ESP_LOGE(TAG, "Failed to allocate RainMaker data (%u bytes)", (unsigned)sizeof(OSRainMakerData));
    return false;
  }
  d_ = new (mem) OSRainMakerData();
  D = d_;  // file-scope pointer for callbacks
  ESP_LOGI(TAG, "RainMaker data allocated in %s (%u bytes)",
#if defined(BOARD_HAS_PSRAM)
           "PSRAM",
#else
           "heap",
#endif
           (unsigned)sizeof(OSRainMakerData));
  return true;
}

// ─── Accessor implementations ───────────────────────────────────────────────

bool OSRainMaker::is_initialized() const { return d_ && d_->initialized; }
bool OSRainMaker::is_unlinking() const { return d_ && d_->unlinking; }
bool OSRainMaker::is_ethernet() const { return d_ && d_->use_ethernet; }
bool OSRainMaker::is_local_ctrl_started() const { return d_ && d_->local_ctrl_active; }
bool OSRainMaker::is_mqtt_connected() const { return d_ && d_->mqtt_connected; }
int  OSRainMaker::get_user_mapping_state() const { return d_ ? d_->user_mapping_state : 0; }

const char* OSRainMaker::get_pop() const {
  return d_ ? d_->prov_pop : "";
}
const char* OSRainMaker::get_prov_service_name() const {
  return d_ ? d_->prov_service_name : "";
}
const char* OSRainMaker::get_local_ctrl_pop() const {
  return d_ ? d_->prov_pop : "";
}

// ─── Core lifecycle ─────────────────────────────────────────────────────────

void OSRainMaker::refresh_runtime_state() {
  if (!d_) return;
  d_->local_ctrl_active = local_ctrl_started_runtime();
  d_->mqtt_connected = esp_rmaker_is_mqtt_connected();
  d_->user_mapping_state = (int)esp_rmaker_user_node_mapping_get_state();
}

void OSRainMaker::init() {
  if (!ensure_data()) return;
  if (d_->initialized) return;

  ESP_LOGI(TAG, "=== RainMaker init starting (Arduino RainMaker API) ===");

  // Check Ethernet status
  d_->use_ethernet = useEth;
  ESP_LOGI(TAG, "Connection mode: %s", d_->use_ethernet ? "ETHERNET" : "WiFi");

  // Always generate PoP and service name (needed for display in UI)
  generate_pop_from_efuse(d_->prov_pop, sizeof(d_->prov_pop));
  generate_service_name(d_->prov_service_name, sizeof(d_->prov_service_name));
  log_runtime_snapshot("init-begin", false);

  if (!D->rmaker_handler_registered) {
    esp_event_handler_register(RMAKER_EVENT, ESP_EVENT_ANY_ID,
                               rmaker_event_handler, nullptr);
    D->rmaker_handler_registered = true;
  }

  // ── 1. Initialize RainMaker node using Arduino API ────────────────────────
  // For Ethernet devices: WiFi must be in STA mode for local control service
  if (d_->use_ethernet) {
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&current_mode);
    if (current_mode == WIFI_MODE_NULL) {
      ESP_LOGI(TAG, "Ethernet mode: enabling WiFi STA for local control");
      WiFi.mode(WIFI_MODE_STA);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  
  // Initialize RainMaker node
  RMaker.setTimeSync(true);
  Node node = RMaker.initNode("OpenSprinkler", "Irrigation Controller");
  esp_rmaker_node_t *raw_node = node.getNodeHandle();
  if (!raw_node) {
    ESP_LOGE(TAG, "RMaker.initNode() FAILED — aborting");
    return;
  }
  ESP_LOGI(TAG, "RainMaker node created successfully (Arduino wrapper)");

  // ── 2. Create devices (raw ESP-IDF APIs for fine-grained control) ────────
  create_zone_devices(raw_node);
  create_controller_device(raw_node);
  create_sensor_devices(raw_node);

  // ── 3. Enable RainMaker services ──────────────────────────────────────────
  RMaker.enableOTA(OTA_USING_TOPICS);
  RMaker.enableTZService();
  RMaker.enableSchedule();
  RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 2, 2, 2);
  ESP_LOGI(TAG, "RainMaker services enabled (OTA, TZ, Schedule, System)");

  // ── 4. Start RainMaker agent ──────────────────────────────────────────────
  esp_err_t err = RMaker.start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RMaker.start() FAILED: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "RainMaker agent started");
  
  char *node_id = esp_rmaker_get_node_id();
  ESP_LOGI(TAG, "Node ID: %s | Service: %s | PoP: %s",
           node_id ? node_id : "?", d_->prov_service_name, d_->prov_pop);

  // ── 5. Start esp_local_ctrl manually for "On Network" provisioning ────────
  // The prebuilt RainMaker library has CONFIG_ESP_RMAKER_LOCAL_CTRL_ENABLE
  // NOT set, so we start it ourselves. This enables the ESP RainMaker phone
  // app to discover the device on the local network and perform assisted
  // claiming + user-node mapping.
  err = start_local_ctrl_manually();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Local control start failed — On Network provisioning will not work");
  }

  refresh_runtime_state();
  ESP_LOGI(TAG, "User mapping state: %d | Local control: %s | MQTT: %s",
           d_->user_mapping_state,
           d_->local_ctrl_active ? "active" : "pending",
           d_->mqtt_connected ? "connected" : "connecting");

  d_->last_sensor_update_ms = millis();
  refresh_runtime_state();
  d_->initialized = true;
  log_runtime_snapshot("init-complete", false);

  ESP_LOGI(TAG, "=== RainMaker init COMPLETE: %d zones, 1 controller, %d sensors ===",
           d_->zone_count, d_->sensor_count);
}

void OSRainMaker::loop() {
  if (!d_ || !d_->initialized || d_->unlinking) return;

  bool prev_local_ctrl = d_->local_ctrl_active;
  bool prev_mqtt = d_->mqtt_connected;
  int prev_mapping_state = d_->user_mapping_state;
  refresh_runtime_state();

  if (prev_local_ctrl != d_->local_ctrl_active ||
      prev_mqtt != d_->mqtt_connected ||
      prev_mapping_state != d_->user_mapping_state) {
    log_runtime_snapshot("runtime-change", false);
  }

  if (!d_->mqtt_connected) return;

  unsigned long now = millis();
  if ((long)(now - d_->last_sensor_update_ms) >= (long)SENSOR_UPDATE_INTERVAL_MS) {
    d_->last_sensor_update_ms = now;
    report_sensor_values();
  }
}

void OSRainMaker::update_station(uint8_t sid, bool is_on) {
  if (!d_ || !d_->initialized) return;
  if (!d_->mqtt_connected) return;

  for (uint8_t i = 0; i < d_->zone_count; i++) {
    if (d_->zone_sid_map[i] == sid && d_->zone_devices[i]) {
      esp_rmaker_param_t *p = esp_rmaker_device_get_param_by_type(
          d_->zone_devices[i], ESP_RMAKER_PARAM_POWER);
      if (p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_bool(is_on));
      }
      return;
    }
  }
}

void OSRainMaker::update_sensors() {
  if (!d_ || !d_->initialized) return;
  report_sensor_values();
}

void OSRainMaker::update_rain_sensor(bool rain_detected) {
  if (!d_ || !d_->initialized || !d_->param_rain_sensor) return;
  if (!d_->mqtt_connected) return;
  esp_rmaker_param_update_and_report(d_->param_rain_sensor, esp_rmaker_bool(rain_detected));
}

void OSRainMaker::update_rain_delay(bool delayed) {
  if (!d_ || !d_->initialized || !d_->param_rain_delay) return;
  if (!d_->mqtt_connected) return;
  int hours = delayed ? (int)((os.nvdata.rd_stop_time - os.now_tz()) / 3600UL) : 0;
  if (hours < 0) hours = 0;
  esp_rmaker_param_update_and_report(d_->param_rain_delay, esp_rmaker_int(hours));
}

void OSRainMaker::update_controller_enabled(bool enabled) {
  if (!d_ || !d_->initialized || !d_->param_enabled) return;
  if (!d_->mqtt_connected) return;
  esp_rmaker_param_update_and_report(d_->param_enabled, esp_rmaker_bool(enabled));
}

bool OSRainMaker::unlink() {
  if (!d_ || !d_->initialized) return false;

  ESP_LOGI(TAG, "[UNLINK] Initiating RainMaker unlink");

  // Mark as unlinking FIRST so status queries reflect immediately
  d_->unlinking = true;

  // Step 1: Mark that the factory reset must only happen AFTER the cloud PUBACK.
  // rmaker_event_handler() monitors RMAKER_EVENT_USER_NODE_MAPPING_RESET and
  // calls esp_rmaker_factory_reset(0, 2) immediately when it fires.
  d_->unlink_factory_reset_pending = true;

  // Step 2: Tell the cloud to remove the user-node mapping.
  // Publishes (QoS1): {"node_id":"...","user_id":"esp-rmaker","secret_key":"failed","reset":true}
  // MQTT must be connected — do NOT call esp_rmaker_stop() before this!
  esp_err_t err = esp_rmaker_reset_user_node_mapping();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "[UNLINK] esp_rmaker_reset_user_node_mapping: %s (cloud mapping may persist)",
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "[UNLINK] Cloud mapping reset requested via MQTT (QoS1).");
  }

  // Step 3: Fallback factory reset (20 s timeout).
  // If the MQTT PUBACK arrives within 20 s, rmaker_event_handler() will call
  // esp_rmaker_factory_reset(0, 2) earlier and the device reboots before this
  // fallback fires.  If MQTT is offline or the cloud is slow, this timer fires
  // at 20 s and forces NVS erase + reboot regardless.
  // NOTE: 20 s >> typical MQTT round-trip, giving ample time for cloud processing.
  err = esp_rmaker_factory_reset(20, 2);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "[UNLINK] esp_rmaker_factory_reset (fallback) failed: %s", esp_err_to_name(err));
    d_->unlink_factory_reset_pending = false;
    d_->unlinking = false;
    return false;
  }

  d_->mqtt_connected = false;
  d_->user_mapping_state = 0;

  ESP_LOGI(TAG, "[UNLINK] Waiting for cloud PUBACK (event-driven); fallback NVS erase in ~20 s, reboot ~22 s.");
  log_runtime_snapshot("unlink", false);
  return true;
}

#endif // ESP32 && ENABLE_RAINMAKER
