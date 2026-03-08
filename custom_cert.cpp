#if defined(ESP32)

#include "custom_cert.h"
#include "defines.h"
#include <LittleFS.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/pem.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

// Include built-in cert for fallback
#include "cert.h"

// Buffers for custom cert/key DER data (allocated from PSRAM/heap)
static unsigned char* s_custom_cert_der = nullptr;
static uint16_t s_custom_cert_der_len = 0;
static unsigned char* s_custom_key_der = nullptr;
static uint16_t s_custom_key_der_len = 0;
static bool s_custom_loaded = false;

// Global pointers that Esp32LocalServer reads (defined here, declared extern in Esp32LocalServer.h)
const unsigned char* otf_custom_cert_data = nullptr;
uint16_t otf_custom_cert_len = 0;
const unsigned char* otf_custom_key_data = nullptr;
uint16_t otf_custom_key_len = 0;

bool custom_cert_exists() {
	return LittleFS.exists(CUSTOM_CERT_FILENAME) && LittleFS.exists(CUSTOM_KEY_FILENAME);
}

// Read a file from LittleFS into a malloc'd buffer. Returns size, 0 on failure.
static size_t read_file_to_buffer(const char* path, unsigned char** out_buf) {
	File f = LittleFS.open(path, "r");
	if (!f) return 0;

	size_t sz = f.size();
	if (sz == 0 || sz > 8192) {  // Sanity limit: 8KB max for a cert/key
		f.close();
		return 0;
	}

	unsigned char* buf = (unsigned char*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!buf) buf = (unsigned char*)malloc(sz);
	if (!buf) { f.close(); return 0; }

	size_t read = f.read(buf, sz);
	f.close();

	if (read != sz) {
		free(buf);
		return 0;
	}

	*out_buf = buf;
	return sz;
}

void custom_cert_init() {
	if (!custom_cert_exists()) {
		DEBUG_PRINTLN(F("[CERT] No custom cert files found, using internal cert"));
		return;
	}

	DEBUG_PRINTLN(F("[CERT] Loading custom certificate from LittleFS..."));

	unsigned char* cert_buf = nullptr;
	size_t cert_len = read_file_to_buffer(CUSTOM_CERT_FILENAME, &cert_buf);
	if (!cert_len) {
		DEBUG_PRINTLN(F("[CERT] Failed to read custom cert file"));
		return;
	}

	unsigned char* key_buf = nullptr;
	size_t key_len = read_file_to_buffer(CUSTOM_KEY_FILENAME, &key_buf);
	if (!key_len) {
		DEBUG_PRINTLN(F("[CERT] Failed to read custom key file"));
		free(cert_buf);
		return;
	}

	// Validate cert and key with mbedTLS before using
	mbedtls_x509_crt test_cert;
	mbedtls_pk_context test_key;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;

	mbedtls_x509_crt_init(&test_cert);
	mbedtls_pk_init(&test_key);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);

	int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
	if (ret != 0) {
		DEBUG_PRINTLN(F("[CERT] Failed to seed DRBG"));
		goto cleanup_fail;
	}

	ret = mbedtls_x509_crt_parse_der(&test_cert, cert_buf, cert_len);
	if (ret != 0) {
		DEBUG_PRINTF("[CERT] Custom cert parse failed: -0x%x\n", -ret);
		goto cleanup_fail;
	}

	ret = mbedtls_pk_parse_key(&test_key, key_buf, key_len, NULL, 0,
	                           mbedtls_ctr_drbg_random, &ctr_drbg);
	if (ret != 0) {
		DEBUG_PRINTF("[CERT] Custom key parse failed: -0x%x\n", -ret);
		goto cleanup_fail;
	}

	// All good — store buffers
	s_custom_cert_der = cert_buf;
	s_custom_cert_der_len = (uint16_t)cert_len;
	s_custom_key_der = key_buf;
	s_custom_key_der_len = (uint16_t)key_len;
	s_custom_loaded = true;

	// Set global pointers for OTF library
	otf_custom_cert_data = s_custom_cert_der;
	otf_custom_cert_len = s_custom_cert_der_len;
	otf_custom_key_data = s_custom_key_der;
	otf_custom_key_len = s_custom_key_der_len;

	DEBUG_PRINTF("[CERT] Custom cert loaded: %d bytes cert, %d bytes key\n",
	             s_custom_cert_der_len, s_custom_key_der_len);

	mbedtls_x509_crt_free(&test_cert);
	mbedtls_pk_free(&test_key);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	return;

cleanup_fail:
	free(cert_buf);
	free(key_buf);
	mbedtls_x509_crt_free(&test_cert);
	mbedtls_pk_free(&test_key);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	DEBUG_PRINTLN(F("[CERT] Falling back to internal cert"));
}

CertInfo custom_cert_get_info() {
	CertInfo info;
	memset(&info, 0, sizeof(info));

	const unsigned char* cert_data;
	uint16_t cert_len;

	if (s_custom_loaded) {
		info.is_custom = true;
		cert_data = s_custom_cert_der;
		cert_len = s_custom_cert_der_len;
	} else {
		info.is_custom = false;
		cert_data = opensprinkler_crt_DER;
		cert_len = opensprinkler_crt_DER_len;
	}

	mbedtls_x509_crt cert;
	mbedtls_x509_crt_init(&cert);

	int ret = mbedtls_x509_crt_parse_der(&cert, cert_data, cert_len);
	if (ret != 0) {
		mbedtls_x509_crt_free(&cert);
		return info;
	}

	// Extract subject CN
	char buf[256];
	ret = mbedtls_x509_dn_gets(buf, sizeof(buf), &cert.subject);
	if (ret > 0) {
		strncpy(info.subject, buf, sizeof(info.subject) - 1);
	}

	// Extract issuer
	ret = mbedtls_x509_dn_gets(buf, sizeof(buf), &cert.issuer);
	if (ret > 0) {
		strncpy(info.issuer, buf, sizeof(info.issuer) - 1);
	}

	// Extract validity dates
	snprintf(info.not_before, sizeof(info.not_before), "%04d-%02d-%02d",
	         cert.valid_from.year, cert.valid_from.mon, cert.valid_from.day);
	snprintf(info.not_after, sizeof(info.not_after), "%04d-%02d-%02d",
	         cert.valid_to.year, cert.valid_to.mon, cert.valid_to.day);

	info.valid = true;
	mbedtls_x509_crt_free(&cert);
	return info;
}

// Decode a PEM section to DER. Returns allocated buffer and sets out_len.
// Caller must free the returned buffer.
static unsigned char* pem_to_der(const char* pem, const char* header, const char* footer,
                                  size_t* out_len) {
	const char* start = strstr(pem, header);
	if (!start) return nullptr;
	start += strlen(header);
	// Skip past the newline after header
	while (*start == '\r' || *start == '\n') start++;

	const char* end = strstr(start, footer);
	if (!end) return nullptr;

	// Base64 decode
	size_t b64_len = end - start;
	// Allocate worst-case output buffer (base64 decodes to 3/4 of input)
	size_t max_out = (b64_len * 3) / 4 + 4;
	unsigned char* der = (unsigned char*)malloc(max_out);
	if (!der) return nullptr;

	size_t decoded_len = 0;
	int ret = mbedtls_base64_decode(der, max_out, &decoded_len,
	                                 (const unsigned char*)start, b64_len);
	if (ret != 0) {
		free(der);
		return nullptr;
	}

	*out_len = decoded_len;
	return der;
}

bool custom_cert_upload(const char* cert_pem, const char* key_pem,
                        char* error_buf, size_t error_buf_len) {
	// Convert PEM to DER
	size_t cert_der_len = 0;
	unsigned char* cert_der = pem_to_der(cert_pem,
		"-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----",
		&cert_der_len);
	if (!cert_der || cert_der_len == 0) {
		snprintf(error_buf, error_buf_len, "Invalid certificate PEM format");
		return false;
	}

	size_t key_der_len = 0;
	unsigned char* key_der = nullptr;

	// Try EC private key first, then generic PRIVATE KEY, then RSA
	key_der = pem_to_der(key_pem,
		"-----BEGIN EC PRIVATE KEY-----", "-----END EC PRIVATE KEY-----",
		&key_der_len);
	if (!key_der) {
		key_der = pem_to_der(key_pem,
			"-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----",
			&key_der_len);
	}
	if (!key_der) {
		key_der = pem_to_der(key_pem,
			"-----BEGIN RSA PRIVATE KEY-----", "-----END RSA PRIVATE KEY-----",
			&key_der_len);
	}
	if (!key_der || key_der_len == 0) {
		free(cert_der);
		snprintf(error_buf, error_buf_len, "Invalid private key PEM format");
		return false;
	}

	// Validate with mbedTLS
	mbedtls_x509_crt test_cert;
	mbedtls_pk_context test_key;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;

	mbedtls_x509_crt_init(&test_cert);
	mbedtls_pk_init(&test_key);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);

	bool success = false;

	int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
	if (ret != 0) {
		snprintf(error_buf, error_buf_len, "DRBG seed failed");
		goto cleanup;
	}

	// Parse certificate
	ret = mbedtls_x509_crt_parse_der(&test_cert, cert_der, cert_der_len);
	if (ret != 0) {
		snprintf(error_buf, error_buf_len, "Certificate parse failed: -0x%x", -ret);
		goto cleanup;
	}

	// Parse private key
	ret = mbedtls_pk_parse_key(&test_key, key_der, key_der_len, NULL, 0,
	                           mbedtls_ctr_drbg_random, &ctr_drbg);
	if (ret != 0) {
		snprintf(error_buf, error_buf_len, "Private key parse failed: -0x%x", -ret);
		goto cleanup;
	}

	// Verify key matches certificate (check public key match)
	{
		unsigned char cert_pk_buf[256], key_pk_buf[256];
		int cert_pk_len = mbedtls_pk_write_pubkey_der(&test_cert.pk, cert_pk_buf, sizeof(cert_pk_buf));
		int key_pk_len = mbedtls_pk_write_pubkey_der(&test_key, key_pk_buf, sizeof(key_pk_buf));
		if (cert_pk_len <= 0 || key_pk_len <= 0 || cert_pk_len != key_pk_len ||
		    memcmp(cert_pk_buf + sizeof(cert_pk_buf) - cert_pk_len,
		           key_pk_buf + sizeof(key_pk_buf) - key_pk_len, cert_pk_len) != 0) {
			snprintf(error_buf, error_buf_len, "Certificate and key do not match");
			goto cleanup;
		}
	}

	// Save to LittleFS
	{
		File f = LittleFS.open(CUSTOM_CERT_FILENAME, "w");
		if (!f) {
			snprintf(error_buf, error_buf_len, "Failed to write cert file");
			goto cleanup;
		}
		f.write(cert_der, cert_der_len);
		f.close();

		f = LittleFS.open(CUSTOM_KEY_FILENAME, "w");
		if (!f) {
			LittleFS.remove(CUSTOM_CERT_FILENAME);
			snprintf(error_buf, error_buf_len, "Failed to write key file");
			goto cleanup;
		}
		f.write(key_der, key_der_len);
		f.close();
	}

	success = true;
	DEBUG_PRINTF("[CERT] Custom cert saved: %d bytes cert, %d bytes key\n",
	             (int)cert_der_len, (int)key_der_len);

cleanup:
	mbedtls_x509_crt_free(&test_cert);
	mbedtls_pk_free(&test_key);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	free(cert_der);
	free(key_der);
	return success;
}

bool custom_cert_delete() {
	bool removed = false;
	if (LittleFS.exists(CUSTOM_CERT_FILENAME)) {
		LittleFS.remove(CUSTOM_CERT_FILENAME);
		removed = true;
	}
	if (LittleFS.exists(CUSTOM_KEY_FILENAME)) {
		LittleFS.remove(CUSTOM_KEY_FILENAME);
		removed = true;
	}

	// Clear runtime state
	if (s_custom_cert_der) { free(s_custom_cert_der); s_custom_cert_der = nullptr; }
	if (s_custom_key_der) { free(s_custom_key_der); s_custom_key_der = nullptr; }
	s_custom_cert_der_len = 0;
	s_custom_key_der_len = 0;
	s_custom_loaded = false;

	// Reset global pointers
	otf_custom_cert_data = nullptr;
	otf_custom_cert_len = 0;
	otf_custom_key_data = nullptr;
	otf_custom_key_len = 0;

	DEBUG_PRINTLN(F("[CERT] Custom cert deleted"));
	return removed;
}

const unsigned char* custom_cert_get_cert_data() {
	return s_custom_loaded ? s_custom_cert_der : opensprinkler_crt_DER;
}

uint16_t custom_cert_get_cert_len() {
	return s_custom_loaded ? s_custom_cert_der_len : opensprinkler_crt_DER_len;
}

const unsigned char* custom_cert_get_key_data() {
	return s_custom_loaded ? s_custom_key_der : opensprinkler_key_DER;
}

uint16_t custom_cert_get_key_len() {
	return s_custom_loaded ? s_custom_key_der_len : opensprinkler_key_DER_len;
}

#endif // ESP32
