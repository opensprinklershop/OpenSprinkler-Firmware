#if defined(ESP32)

#include "acme_client.h"
#include "custom_cert.h"
#include "defines.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/error.h>
#include "ArduinoJson.hpp"
#include "psram_utils.h"

#define ACME_TAG "[ACME]"

// ── Static state ─────────────────────────────────────────────────────────────
static AcmeConfig s_config = {};
static AcmeStatus s_status = ACME_STATUS_IDLE;
static char s_last_error[128] = "";
static TimerHandle_t s_renewal_timer = nullptr;
static bool s_requesting = false;

// HTTP-01 challenge state (set during cert request, cleared after)
static char s_challenge_token[128] = "";
static char s_challenge_key_auth[512] = "";

// ACME directory URLs (populated from directory endpoint)
static char s_dir_new_nonce[256] = "";
static char s_dir_new_account[256] = "";
static char s_dir_new_order[256] = "";

// Account URL (from account creation response Location header)
static char s_account_url[256] = "";

// Temporary HTTP server on port 80 for ACME HTTP-01 challenge
static WebServer* s_challenge_server = nullptr;

static void challenge_server_handler() {
  if (!s_challenge_server) return;
  String uri = s_challenge_server->uri();
  const char* prefix = "/.well-known/acme-challenge/";
  if (uri.startsWith(prefix) && s_challenge_token[0] != '\0') {
    String req_token = uri.substring(strlen(prefix));
    if (req_token == s_challenge_token) {
      s_challenge_server->send(200, "text/plain", s_challenge_key_auth);
      DEBUG_PRINTF("%s Served HTTP-01 challenge on port 80\n", ACME_TAG);
      return;
    }
  }
  s_challenge_server->send(404, "text/plain", "Not Found");
}

static void acme_start_challenge_server() {
  if (s_challenge_server) return;
  s_challenge_server = new WebServer(80);
  if (!s_challenge_server) return;
  s_challenge_server->onNotFound(challenge_server_handler);
  s_challenge_server->begin();
  DEBUG_PRINTF("%s Challenge HTTP server started on port 80\n", ACME_TAG);
}

static void acme_stop_challenge_server() {
  if (s_challenge_server) {
    s_challenge_server->stop();
    delete s_challenge_server;
    s_challenge_server = nullptr;
    DEBUG_PRINTF("%s Challenge HTTP server stopped\n", ACME_TAG);
  }
}

// ── Helper: Base64url encode (no padding) ────────────────────────────────────
static size_t base64url_encode(const uint8_t* in, size_t in_len, char* out, size_t out_len) {
  size_t olen = 0;
  int ret = mbedtls_base64_encode((unsigned char*)out, out_len, &olen, in, in_len);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return 0;

  // Convert base64 to base64url: + → -, / → _, remove =
  for (size_t i = 0; i < olen; i++) {
    if (out[i] == '+') out[i] = '-';
    else if (out[i] == '/') out[i] = '_';
  }
  // Remove trailing '='
  while (olen > 0 && out[olen-1] == '=') olen--;
  out[olen] = '\0';
  return olen;
}

// ── Helper: SHA-256 ──────────────────────────────────────────────────────────
static bool sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  return mbedtls_sha256(data, len, out, 0) == 0;
}

// ── Helper: Load or generate EC P-256 key ────────────────────────────────────
static bool load_or_generate_key(const char* path, mbedtls_pk_context* pk) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const uint8_t*)"acme", 4);
  if (ret != 0) {
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return false;
  }

  // Try loading existing key
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    if (f) {
      size_t sz = f.size();
      if (sz > 0 && sz < 4096) {
        uint8_t* buf = (uint8_t*)malloc(sz);
        if (buf) {
          f.read(buf, sz);
          f.close();
          ret = mbedtls_pk_parse_key(pk, buf, sz, NULL, 0,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
          free(buf);
          if (ret == 0) {
            DEBUG_PRINTF("%s Loaded key from %s\n", ACME_TAG, path);
            mbedtls_entropy_free(&entropy);
            mbedtls_ctr_drbg_free(&ctr_drbg);
            return true;
          }
        } else {
          f.close();
        }
      } else {
        f.close();
      }
    }
  }

  // Generate new EC P-256 key
  DEBUG_PRINTF("%s Generating new EC P-256 key for %s\n", ACME_TAG, path);
  ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) goto fail;

  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk),
                             mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) goto fail;

  // Save to LittleFS
  {
    uint8_t der_buf[256];
    ret = mbedtls_pk_write_key_der(pk, der_buf, sizeof(der_buf));
    if (ret > 0) {
      File f = LittleFS.open(path, "w");
      if (f) {
        // mbedtls_pk_write_key_der writes from end of buffer
        f.write(der_buf + sizeof(der_buf) - ret, ret);
        f.close();
        DEBUG_PRINTF("%s Saved new key to %s (%d bytes)\n", ACME_TAG, path, ret);
      }
    }
  }

  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  return true;

fail:
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  return false;
}

// ── Helper: Build JWK thumbprint from EC key ─────────────────────────────────
// JWK thumbprint = SHA-256 of {"crv":"P-256","kty":"EC","x":"...","y":"..."}
static bool jwk_thumbprint(mbedtls_pk_context* pk, char* out, size_t out_len) {
  mbedtls_ecp_keypair* ec = mbedtls_pk_ec(*pk);
  if (!ec) return false;

  uint8_t x[32], y[32];
  int ret = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), x, 32);
  if (ret != 0) return false;
  ret = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), y, 32);
  if (ret != 0) return false;

  char x_b64[64], y_b64[64];
  base64url_encode(x, 32, x_b64, sizeof(x_b64));
  base64url_encode(y, 32, y_b64, sizeof(y_b64));

  // Canonical JWK (sorted keys)
  char jwk[256];
  snprintf(jwk, sizeof(jwk),
    "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x_b64, y_b64);

  uint8_t hash[32];
  if (!sha256((const uint8_t*)jwk, strlen(jwk), hash)) return false;

  base64url_encode(hash, 32, out, out_len);
  return true;
}

// ── Helper: Build JWK JSON from EC key ───────────────────────────────────────
static bool build_jwk(mbedtls_pk_context* pk, char* out, size_t out_len) {
  mbedtls_ecp_keypair* ec = mbedtls_pk_ec(*pk);
  if (!ec) return false;

  uint8_t x[32], y[32];
  int ret = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), x, 32);
  if (ret != 0) return false;
  ret = mbedtls_mpi_write_binary(&ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), y, 32);
  if (ret != 0) return false;

  char x_b64[64], y_b64[64];
  base64url_encode(x, 32, x_b64, sizeof(x_b64));
  base64url_encode(y, 32, y_b64, sizeof(y_b64));

  snprintf(out, out_len,
    "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x_b64, y_b64);
  return true;
}

// ── Helper: Sign payload with ACME JWS (Flattened JSON) ──────────────────────
// Returns heap-allocated JWS JSON string. Caller must free().
static char* acme_jws_sign(mbedtls_pk_context* pk, const char* url,
                            const char* nonce, const char* payload,
                            bool use_jwk) {
  // Build protected header
  char header[512];
  if (use_jwk) {
    char jwk[256];
    if (!build_jwk(pk, jwk, sizeof(jwk))) return nullptr;
    snprintf(header, sizeof(header),
      "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
      jwk, nonce, url);
  } else {
    snprintf(header, sizeof(header),
      "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
      s_account_url, nonce, url);
  }

  // Base64url encode header and payload
  char* header_b64 = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  char* payload_b64 = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!header_b64 || !payload_b64) {
    free(header_b64); free(payload_b64);
    return nullptr;
  }

  base64url_encode((const uint8_t*)header, strlen(header), header_b64, 1024);
  if (payload && payload[0]) {
    base64url_encode((const uint8_t*)payload, strlen(payload), payload_b64, 2048);
  } else {
    payload_b64[0] = '\0';  // Empty payload for POST-as-GET
  }

  // Signing input: header_b64.payload_b64
  size_t input_len = strlen(header_b64) + 1 + strlen(payload_b64);
  char* signing_input = (char*)heap_caps_malloc(input_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!signing_input) {
    free(header_b64); free(payload_b64);
    return nullptr;
  }
  snprintf(signing_input, input_len + 1, "%s.%s", header_b64, payload_b64);

  // Hash the signing input
  uint8_t hash[32];
  sha256((const uint8_t*)signing_input, strlen(signing_input), hash);

  // Sign with EC P-256
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

  uint8_t sig_der[128];
  size_t sig_len = 0;
  int ret = mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, 32,
                             sig_der, sizeof(sig_der), &sig_len,
                             mbedtls_ctr_drbg_random, &ctr_drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctr_drbg);

  if (ret != 0) {
    free(header_b64); free(payload_b64); free(signing_input);
    return nullptr;
  }

  // Convert DER signature to raw R||S (64 bytes for P-256)
  // DER format: 0x30 <len> 0x02 <rlen> <R> 0x02 <slen> <S>
  uint8_t rs[64];
  memset(rs, 0, 64);
  const uint8_t* p = sig_der;
  if (*p++ != 0x30) { free(header_b64); free(payload_b64); free(signing_input); return nullptr; }
  p++; // skip total length
  if (*p++ != 0x02) { free(header_b64); free(payload_b64); free(signing_input); return nullptr; }
  uint8_t rlen = *p++;
  // R value (may have leading zero if high bit set)
  if (rlen == 33 && *p == 0x00) { p++; rlen = 32; }
  if (rlen <= 32) {
    memcpy(rs + 32 - rlen, p, rlen);
  }
  p += rlen;
  if (*p++ != 0x02) { free(header_b64); free(payload_b64); free(signing_input); return nullptr; }
  uint8_t slen = *p++;
  if (slen == 33 && *p == 0x00) { p++; slen = 32; }
  if (slen <= 32) {
    memcpy(rs + 64 - slen, p, slen);
  }

  char sig_b64[128];
  base64url_encode(rs, 64, sig_b64, sizeof(sig_b64));

  // Build final JWS object
  size_t jws_len = strlen(header_b64) + strlen(payload_b64) + strlen(sig_b64) + 128;
  char* jws = (char*)heap_caps_malloc(jws_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (jws) {
    snprintf(jws, jws_len,
      "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
      header_b64, payload_b64, sig_b64);
  }

  free(header_b64);
  free(payload_b64);
  free(signing_input);
  return jws;
}

// ── Helper: Get a fresh nonce ────────────────────────────────────────────────
static bool acme_get_nonce(char* nonce, size_t nonce_len) {
  if (s_dir_new_nonce[0] == '\0') return false;

  HTTPClient http;
  http.begin(s_dir_new_nonce);
  int code = http.sendRequest("HEAD");
  if (code == 200 || code == 204) {
    String n = http.header("Replay-Nonce");
    if (n.length() > 0) {
      strncpy(nonce, n.c_str(), nonce_len - 1);
      nonce[nonce_len - 1] = '\0';
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

// ── Helper: ACME POST request ────────────────────────────────────────────────
// Returns HTTP status code. Response body is written to resp_buf.
// Location header (if present) is written to location_buf.
static int acme_post(const char* url, const char* jws_body,
                     char* resp_buf, size_t resp_len,
                     char* location_buf, size_t location_len,
                     char* new_nonce, size_t nonce_len) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/jose+json");
  http.collectHeaders(new const char*[2]{"Replay-Nonce", "Location"}, 2);

  int code = http.POST(jws_body);
  if (code > 0) {
    String body = http.getString();
    if (resp_buf && resp_len > 0) {
      strncpy(resp_buf, body.c_str(), resp_len - 1);
      resp_buf[resp_len - 1] = '\0';
    }
    if (location_buf && location_len > 0) {
      String loc = http.header("Location");
      strncpy(location_buf, loc.c_str(), location_len - 1);
      location_buf[location_len - 1] = '\0';
    }
    if (new_nonce && nonce_len > 0) {
      String n = http.header("Replay-Nonce");
      if (n.length() > 0) {
        strncpy(new_nonce, n.c_str(), nonce_len - 1);
        new_nonce[nonce_len - 1] = '\0';
      }
    }
  }
  http.end();
  return code;
}

// ── Load/save config ─────────────────────────────────────────────────────────
static bool load_config() {
  if (!LittleFS.exists(ACME_CONFIG_FILE)) return false;

  File f = LittleFS.open(ACME_CONFIG_FILE, "r");
  if (!f) return false;

  ArduinoJson::JsonDocument doc;
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, f);
  f.close();

  if (err) return false;

  strlcpy(s_config.domain, doc["domain"] | "", sizeof(s_config.domain));
  strlcpy(s_config.email, doc["email"] | "", sizeof(s_config.email));
  strlcpy(s_config.acme_server, doc["server"] | ACME_LETSENCRYPT_PROD, sizeof(s_config.acme_server));
  s_config.enabled = doc["enabled"] | false;

  return s_config.domain[0] != '\0';
}

static bool save_config() {
  ArduinoJson::JsonDocument doc;
  doc["domain"] = s_config.domain;
  doc["email"] = s_config.email;
  doc["server"] = s_config.acme_server;
  doc["enabled"] = s_config.enabled;

  File f = LittleFS.open(ACME_CONFIG_FILE, "w");
  if (!f) return false;
  ArduinoJson::serializeJson(doc, f);
  f.close();
  return true;
}

// ── Fetch ACME directory ─────────────────────────────────────────────────────
static bool acme_fetch_directory() {
  HTTPClient http;
  http.begin(s_config.acme_server);
  int code = http.GET();
  if (code != 200) {
    DEBUG_PRINTF("%s Directory fetch failed: HTTP %d\n", ACME_TAG, code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  ArduinoJson::JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, body)) return false;

  strlcpy(s_dir_new_nonce, doc["newNonce"] | "", sizeof(s_dir_new_nonce));
  strlcpy(s_dir_new_account, doc["newAccount"] | "", sizeof(s_dir_new_account));
  strlcpy(s_dir_new_order, doc["newOrder"] | "", sizeof(s_dir_new_order));

  DEBUG_PRINTF("%s Directory fetched OK\n", ACME_TAG);
  return s_dir_new_nonce[0] && s_dir_new_account[0] && s_dir_new_order[0];
}

// ── Create or find ACME account ──────────────────────────────────────────────
static bool acme_create_account(mbedtls_pk_context* account_key) {
  char nonce[128];
  if (!acme_get_nonce(nonce, sizeof(nonce))) {
    snprintf(s_last_error, sizeof(s_last_error), "Failed to get nonce");
    return false;
  }

  // Payload for newAccount
  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"termsOfServiceAgreed\":true,\"contact\":[\"mailto:%s\"]}",
    s_config.email);

  char* jws = acme_jws_sign(account_key, s_dir_new_account, nonce, payload, true);
  if (!jws) {
    snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (account)");
    return false;
  }

  char resp[1024];
  char location[256] = "";
  int code = acme_post(s_dir_new_account, jws, resp, sizeof(resp),
                       location, sizeof(location), nonce, sizeof(nonce));
  free(jws);

  // 201 = created, 200 = existing account
  if (code != 200 && code != 201) {
    snprintf(s_last_error, sizeof(s_last_error), "Account creation failed: HTTP %d", code);
    DEBUG_PRINTF("%s Account failed: HTTP %d body=%s\n", ACME_TAG, code, resp);
    return false;
  }

  strlcpy(s_account_url, location, sizeof(s_account_url));
  DEBUG_PRINTF("%s Account URL: %s\n", ACME_TAG, s_account_url);
  return s_account_url[0] != '\0';
}

// ── Generate CSR ─────────────────────────────────────────────────────────────
static uint8_t* acme_generate_csr(mbedtls_pk_context* domain_key,
                                   const char* domain, size_t* out_len) {
  mbedtls_x509write_csr csr;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;

  mbedtls_x509write_csr_init(&csr);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

  char subject[256];
  snprintf(subject, sizeof(subject), "CN=%s", domain);
  mbedtls_x509write_csr_set_subject_name(&csr, subject);
  mbedtls_x509write_csr_set_key(&csr, domain_key);
  mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);

  uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    mbedtls_x509write_csr_free(&csr);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return nullptr;
  }

  int ret = mbedtls_x509write_csr_der(&csr, buf, 4096,
                                       mbedtls_ctr_drbg_random, &ctr_drbg);
  mbedtls_x509write_csr_free(&csr);
  mbedtls_entropy_free(&entropy);
  mbedtls_ctr_drbg_free(&ctr_drbg);

  if (ret < 0) {
    free(buf);
    return nullptr;
  }

  // mbedtls writes from end of buffer
  *out_len = (size_t)ret;
  uint8_t* der = (uint8_t*)malloc(ret);
  if (der) {
    memcpy(der, buf + 4096 - ret, ret);
  }
  free(buf);
  return der;
}

// ── Main certificate request flow ────────────────────────────────────────────
static void acme_request_task(void* param) {
  DEBUG_PRINTF("%s Starting certificate request for %s\n", ACME_TAG, s_config.domain);
  s_status = ACME_STATUS_REQUESTING;
  s_last_error[0] = '\0';

  // Step 1: Load/generate account key
  mbedtls_pk_context account_key;
  mbedtls_pk_init(&account_key);
  if (!load_or_generate_key(ACME_ACCOUNT_KEY_FILE, &account_key)) {
    snprintf(s_last_error, sizeof(s_last_error), "Failed to load/generate account key");
    s_status = ACME_STATUS_ERROR;
    mbedtls_pk_free(&account_key);
    s_requesting = false;
    PSRAM_TASK_SELF_DELETE();
    return;
  }

  // Step 2: Load/generate domain key
  mbedtls_pk_context domain_key;
  mbedtls_pk_init(&domain_key);
  if (!load_or_generate_key(ACME_DOMAIN_KEY_FILE, &domain_key)) {
    snprintf(s_last_error, sizeof(s_last_error), "Failed to load/generate domain key");
    s_status = ACME_STATUS_ERROR;
    mbedtls_pk_free(&account_key);
    mbedtls_pk_free(&domain_key);
    s_requesting = false;
    PSRAM_TASK_SELF_DELETE();
    return;
  }

  // Step 3: Fetch ACME directory
  if (!acme_fetch_directory()) {
    snprintf(s_last_error, sizeof(s_last_error), "Failed to fetch ACME directory");
    s_status = ACME_STATUS_ERROR;
    goto cleanup;
  }

  // Step 4: Create/find account
  if (!acme_create_account(&account_key)) {
    s_status = ACME_STATUS_ERROR;
    goto cleanup;
  }

  // Step 5: Create order
  {
    char nonce[128];
    if (!acme_get_nonce(nonce, sizeof(nonce))) {
      snprintf(s_last_error, sizeof(s_last_error), "Nonce fetch failed");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
      "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"%s\"}]}", s_config.domain);

    char* jws = acme_jws_sign(&account_key, s_dir_new_order, nonce, payload, false);
    if (!jws) {
      snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (order)");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    char resp[2048];
    char order_location[256] = "";
    int code = acme_post(s_dir_new_order, jws, resp, sizeof(resp),
                         order_location, sizeof(order_location), nonce, sizeof(nonce));
    free(jws);

    if (code != 201) {
      snprintf(s_last_error, sizeof(s_last_error), "Order creation failed: HTTP %d", code);
      DEBUG_PRINTF("%s Order failed: %s\n", ACME_TAG, resp);
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    // Parse order response to get authorization URL and finalize URL
    ArduinoJson::JsonDocument order_doc;
    if (ArduinoJson::deserializeJson(order_doc, resp)) {
      snprintf(s_last_error, sizeof(s_last_error), "Order parse failed");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    const char* authz_url = order_doc["authorizations"][0];
    const char* finalize_url = order_doc["finalize"];
    if (!authz_url || !finalize_url) {
      snprintf(s_last_error, sizeof(s_last_error), "Order missing authz/finalize URL");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    // Step 6: Get authorization details (find HTTP-01 challenge)
    jws = acme_jws_sign(&account_key, authz_url, nonce, "", false);
    if (!jws) {
      snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (authz)");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    code = acme_post(authz_url, jws, resp, sizeof(resp), NULL, 0, nonce, sizeof(nonce));
    free(jws);

    if (code != 200) {
      snprintf(s_last_error, sizeof(s_last_error), "Authorization fetch failed: HTTP %d", code);
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    ArduinoJson::JsonDocument authz_doc;
    if (ArduinoJson::deserializeJson(authz_doc, resp)) {
      snprintf(s_last_error, sizeof(s_last_error), "Authorization parse failed");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    // Find HTTP-01 challenge
    const char* challenge_url = nullptr;
    const char* token = nullptr;
    for (ArduinoJson::JsonObject ch : authz_doc["challenges"].as<ArduinoJson::JsonArray>()) {
      if (strcmp(ch["type"] | "", "http-01") == 0) {
        challenge_url = ch["url"];
        token = ch["token"];
        break;
      }
    }

    if (!challenge_url || !token) {
      snprintf(s_last_error, sizeof(s_last_error), "No HTTP-01 challenge in authorization");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    // Step 7: Set up challenge response
    // Key authorization = token + "." + JWK thumbprint
    char thumbprint[64];
    if (!jwk_thumbprint(&account_key, thumbprint, sizeof(thumbprint))) {
      snprintf(s_last_error, sizeof(s_last_error), "JWK thumbprint failed");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    strlcpy(s_challenge_token, token, sizeof(s_challenge_token));
    snprintf(s_challenge_key_auth, sizeof(s_challenge_key_auth), "%s.%s", token, thumbprint);
    DEBUG_PRINTF("%s Challenge token: %s\n", ACME_TAG, s_challenge_token);

    // Start temporary HTTP server on port 80 for challenge validation
    acme_start_challenge_server();

    // Step 8: Notify ACME server we're ready
    jws = acme_jws_sign(&account_key, challenge_url, nonce, "{}", false);
    if (!jws) {
      snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (challenge)");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    code = acme_post(challenge_url, jws, resp, sizeof(resp), NULL, 0, nonce, sizeof(nonce));
    free(jws);

    if (code != 200) {
      snprintf(s_last_error, sizeof(s_last_error), "Challenge response failed: HTTP %d", code);
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    // Step 9: Poll order status until ready (max 60 seconds)
    // Handle HTTP requests on port 80 for ACME challenge during polling
    DEBUG_PRINTF("%s Waiting for challenge validation...\n", ACME_TAG);
    bool order_ready = false;
    for (int attempt = 0; attempt < 20; attempt++) {
      // Service the challenge HTTP server while waiting
      for (int t = 0; t < 30; t++) {
        if (s_challenge_server) s_challenge_server->handleClient();
        vTaskDelay(pdMS_TO_TICKS(100));
      }

      jws = acme_jws_sign(&account_key, order_location, nonce, "", false);
      if (!jws) continue;
      code = acme_post(order_location, jws, resp, sizeof(resp), NULL, 0, nonce, sizeof(nonce));
      free(jws);

      if (code == 200) {
        ArduinoJson::JsonDocument status_doc;
        if (!ArduinoJson::deserializeJson(status_doc, resp)) {
          const char* status = status_doc["status"];
          if (status && strcmp(status, "ready") == 0) {
            order_ready = true;
            break;
          } else if (status && strcmp(status, "valid") == 0) {
            // Already finalized — certificate is ready
            const char* cert_url = status_doc["certificate"];
            if (cert_url) {
              // Jump to certificate download
              jws = acme_jws_sign(&account_key, cert_url, nonce, "", false);
              if (jws) {
                char* cert_resp = (char*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (cert_resp) {
                  code = acme_post(cert_url, jws, cert_resp, 8192, NULL, 0, nonce, sizeof(nonce));
                  free(jws);
                  if (code == 200) {
                    // Save as custom cert
                    char err_buf[128];
                    // cert_resp contains the PEM certificate chain
                    // We need to extract something for the key — use domain key PEM
                    uint8_t key_pem_buf[512];
                    int key_len = mbedtls_pk_write_key_pem(&domain_key, key_pem_buf, sizeof(key_pem_buf));
                    if (key_len == 0) {
                      if (custom_cert_upload(cert_resp, (const char*)key_pem_buf, err_buf, sizeof(err_buf))) {
                        s_status = ACME_STATUS_ACTIVE;
                        s_challenge_token[0] = '\0';
                        s_challenge_key_auth[0] = '\0';
                        DEBUG_PRINTF("%s Certificate obtained and installed!\n", ACME_TAG);
                        free(cert_resp);
                        goto cleanup;
                      }
                    }
                  }
                  free(cert_resp);
                } else {
                  free(jws);
                }
              }
            }
            order_ready = true;
            break;
          } else if (status && strcmp(status, "invalid") == 0) {
            snprintf(s_last_error, sizeof(s_last_error), "Order became invalid (challenge failed)");
            s_status = ACME_STATUS_ERROR;
            s_challenge_token[0] = '\0';
            s_challenge_key_auth[0] = '\0';
            goto cleanup;
          }
          DEBUG_PRINTF("%s Order status: %s (attempt %d)\n", ACME_TAG, status, attempt);
        }
      }
    }

    if (!order_ready) {
      snprintf(s_last_error, sizeof(s_last_error), "Timeout waiting for order to become ready");
      s_status = ACME_STATUS_ERROR;
      s_challenge_token[0] = '\0';
      goto cleanup;
    }

    // Clear challenge token — validation done
    s_challenge_token[0] = '\0';
    s_challenge_key_auth[0] = '\0';

    // Step 10: Finalize order with CSR
    size_t csr_der_len = 0;
    uint8_t* csr_der = acme_generate_csr(&domain_key, s_config.domain, &csr_der_len);
    if (!csr_der) {
      snprintf(s_last_error, sizeof(s_last_error), "CSR generation failed");
      s_status = ACME_STATUS_ERROR;
      goto cleanup;
    }

    {
      char* csr_b64 = (char*)heap_caps_malloc(csr_der_len * 2 + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!csr_b64) {
        free(csr_der);
        snprintf(s_last_error, sizeof(s_last_error), "Memory allocation failed");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }
      base64url_encode(csr_der, csr_der_len, csr_b64, csr_der_len * 2 + 16);
      free(csr_der);

      char* fin_payload = (char*)heap_caps_malloc(strlen(csr_b64) + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!fin_payload) {
        free(csr_b64);
        snprintf(s_last_error, sizeof(s_last_error), "Memory allocation failed");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }
      snprintf(fin_payload, strlen(csr_b64) + 32, "{\"csr\":\"%s\"}", csr_b64);
      free(csr_b64);

      jws = acme_jws_sign(&account_key, finalize_url, nonce, fin_payload, false);
      free(fin_payload);
      if (!jws) {
        snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (finalize)");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      code = acme_post(finalize_url, jws, resp, sizeof(resp), NULL, 0, nonce, sizeof(nonce));
      free(jws);

      if (code != 200) {
        snprintf(s_last_error, sizeof(s_last_error), "Finalize failed: HTTP %d", code);
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }
    }

    // Step 11: Poll for certificate URL
    {
      const char* cert_url = nullptr;
      for (int attempt = 0; attempt < 10; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        jws = acme_jws_sign(&account_key, order_location, nonce, "", false);
        if (!jws) continue;
        code = acme_post(order_location, jws, resp, sizeof(resp), NULL, 0, nonce, sizeof(nonce));
        free(jws);

        if (code == 200) {
          ArduinoJson::JsonDocument final_doc;
          if (!ArduinoJson::deserializeJson(final_doc, resp)) {
            const char* status = final_doc["status"];
            if (status && strcmp(status, "valid") == 0) {
              cert_url = final_doc["certificate"];
              break;
            }
          }
        }
      }

      if (!cert_url) {
        snprintf(s_last_error, sizeof(s_last_error), "Certificate URL not available");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      // Step 12: Download certificate
      jws = acme_jws_sign(&account_key, cert_url, nonce, "", false);
      if (!jws) {
        snprintf(s_last_error, sizeof(s_last_error), "JWS sign failed (cert download)");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      char* cert_pem = (char*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!cert_pem) {
        free(jws);
        snprintf(s_last_error, sizeof(s_last_error), "Memory allocation failed");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      code = acme_post(cert_url, jws, cert_pem, 8192, NULL, 0, nonce, sizeof(nonce));
      free(jws);

      if (code != 200) {
        free(cert_pem);
        snprintf(s_last_error, sizeof(s_last_error), "Certificate download failed: HTTP %d", code);
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      // Step 13: Save certificate and key
      // Generate PEM for domain key
      uint8_t key_pem_buf[512];
      int key_ret = mbedtls_pk_write_key_pem(&domain_key, key_pem_buf, sizeof(key_pem_buf));
      if (key_ret != 0) {
        free(cert_pem);
        snprintf(s_last_error, sizeof(s_last_error), "Domain key PEM export failed");
        s_status = ACME_STATUS_ERROR;
        goto cleanup;
      }

      char err_buf[128];
      if (custom_cert_upload(cert_pem, (const char*)key_pem_buf, err_buf, sizeof(err_buf))) {
        s_status = ACME_STATUS_ACTIVE;
        DEBUG_PRINTF("%s Certificate obtained and installed for %s!\n", ACME_TAG, s_config.domain);
        DEBUG_PRINTF("%s Reboot required to activate the new certificate.\n", ACME_TAG);
      } else {
        snprintf(s_last_error, sizeof(s_last_error), "Cert install failed: %s", err_buf);
        s_status = ACME_STATUS_ERROR;
      }
      free(cert_pem);
    }
  }

cleanup:
  acme_stop_challenge_server();
  s_challenge_token[0] = '\0';
  s_challenge_key_auth[0] = '\0';
  mbedtls_pk_free(&account_key);
  mbedtls_pk_free(&domain_key);
  s_requesting = false;
  if (s_status == ACME_STATUS_ERROR) {
    DEBUG_PRINTF("%s Error: %s\n", ACME_TAG, s_last_error);
  }
  PSRAM_TASK_SELF_DELETE();
}

// ── Renewal timer callback ───────────────────────────────────────────────────
static void acme_renewal_timer_cb(TimerHandle_t xTimer) {
  (void)xTimer;
  if (!s_config.enabled || s_requesting) return;

  int days = acme_days_until_expiry();
  if (days >= 0 && days < ACME_RENEW_BEFORE_DAYS) {
    DEBUG_PRINTF("%s Certificate expires in %d days, starting renewal\n", ACME_TAG, days);
    acme_request_certificate();
  } else if (days < 0 && custom_cert_exists()) {
    // Cert exists but we can't read expiry — might be expired
    DEBUG_PRINTF("%s Certificate expiry check failed, attempting renewal\n", ACME_TAG);
    acme_request_certificate();
  }
}

// ── Public API ───────────────────────────────────────────────────────────────

void acme_init() {
  if (load_config() && s_config.enabled) {
    if (custom_cert_exists()) {
      s_status = ACME_STATUS_ACTIVE;
      int days = acme_days_until_expiry();
      if (days >= 0 && days < ACME_RENEW_BEFORE_DAYS) {
        s_status = ACME_STATUS_RENEWAL_DUE;
      }
    } else {
      s_status = ACME_STATUS_CONFIGURED;
    }
    acme_start_renewal_timer();
  }
}

AcmeStatus acme_get_status() {
  return s_status;
}

const char* acme_get_last_error() {
  return s_last_error;
}

AcmeConfig acme_get_config() {
  return s_config;
}

bool acme_set_config(const char* domain, const char* email,
                     const char* acme_server, bool enabled) {
  strlcpy(s_config.domain, domain ? domain : "", sizeof(s_config.domain));
  strlcpy(s_config.email, email ? email : "", sizeof(s_config.email));
  strlcpy(s_config.acme_server,
          (acme_server && acme_server[0]) ? acme_server : ACME_LETSENCRYPT_PROD,
          sizeof(s_config.acme_server));
  s_config.enabled = enabled;

  if (!save_config()) {
    snprintf(s_last_error, sizeof(s_last_error), "Failed to save config");
    return false;
  }

  if (enabled) {
    s_status = ACME_STATUS_CONFIGURED;
    acme_start_renewal_timer();
  } else {
    acme_stop_renewal_timer();
    s_status = ACME_STATUS_IDLE;
  }
  return true;
}

bool acme_request_certificate() {
  if (s_requesting) return false;
  if (!s_config.enabled || s_config.domain[0] == '\0') {
    snprintf(s_last_error, sizeof(s_last_error), "ACME not configured");
    return false;
  }

  s_requesting = true;
  // Run in a dedicated task — ACME protocol has multiple HTTPS roundtrips
  BaseType_t ret = PSRAM_TASK_CREATE(acme_request_task, "acme", 8192, NULL, 2, NULL);
  if (ret != pdPASS) {
    s_requesting = false;
    snprintf(s_last_error, sizeof(s_last_error), "Failed to create ACME task");
    return false;
  }
  return true;
}

bool acme_check_renewal() {
  if (!s_config.enabled || s_requesting) return false;

  int days = acme_days_until_expiry();
  if (days >= 0 && days < ACME_RENEW_BEFORE_DAYS) {
    return acme_request_certificate();
  }
  return false;
}

int acme_days_until_expiry() {
  if (!custom_cert_exists()) return -1;

  CertInfo info = custom_cert_get_info();
  if (!info.valid || !info.not_after[0]) return -1;

  // Parse not_after date: YYYY-MM-DD
  int year, month, day;
  if (sscanf(info.not_after, "%d-%d-%d", &year, &month, &day) != 3) return -1;

  // Simple calculation using time_t
  struct tm expiry_tm = {};
  expiry_tm.tm_year = year - 1900;
  expiry_tm.tm_mon = month - 1;
  expiry_tm.tm_mday = day;
  time_t expiry = mktime(&expiry_tm);

  time_t now;
  time(&now);

  if (expiry <= now) return 0;
  return (int)((expiry - now) / 86400);
}

void acme_start_renewal_timer() {
  if (s_renewal_timer) {
    xTimerStart(s_renewal_timer, 0);
    return;
  }
  s_renewal_timer = xTimerCreate("acme_renew", pdMS_TO_TICKS(ACME_RENEWAL_CHECK_MS),
                                  pdTRUE, nullptr, acme_renewal_timer_cb);
  if (s_renewal_timer) {
    xTimerStart(s_renewal_timer, 0);
    DEBUG_PRINTF("%s Renewal timer started (24h interval)\n", ACME_TAG);
  }
}

void acme_stop_renewal_timer() {
  if (s_renewal_timer) {
    xTimerStop(s_renewal_timer, 0);
  }
}

bool acme_handle_challenge(const char* uri, char* response, size_t max_len) {
  // Expected: /.well-known/acme-challenge/<TOKEN>
  const char* prefix = "/.well-known/acme-challenge/";
  if (strncmp(uri, prefix, strlen(prefix)) != 0) return false;

  const char* req_token = uri + strlen(prefix);
  if (s_challenge_token[0] == '\0') return false;
  if (strcmp(req_token, s_challenge_token) != 0) return false;

  strlcpy(response, s_challenge_key_auth, max_len);
  DEBUG_PRINTF("%s Served HTTP-01 challenge for token %s\n", ACME_TAG, req_token);
  return true;
}

bool acme_delete() {
  acme_stop_renewal_timer();

  if (LittleFS.exists(ACME_ACCOUNT_KEY_FILE)) LittleFS.remove(ACME_ACCOUNT_KEY_FILE);
  if (LittleFS.exists(ACME_DOMAIN_KEY_FILE)) LittleFS.remove(ACME_DOMAIN_KEY_FILE);
  if (LittleFS.exists(ACME_CONFIG_FILE)) LittleFS.remove(ACME_CONFIG_FILE);

  memset(&s_config, 0, sizeof(s_config));
  s_status = ACME_STATUS_IDLE;
  s_last_error[0] = '\0';
  s_challenge_token[0] = '\0';
  s_challenge_key_auth[0] = '\0';

  // Also delete the certificate/key
  custom_cert_delete();

  DEBUG_PRINTF("%s All ACME data deleted\n", ACME_TAG);
  return true;
}

#endif // ESP32
