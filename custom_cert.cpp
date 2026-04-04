#if defined(ESP32)

#include "custom_cert.h"
#include "defines.h"
#include "OpenSprinkler.h"
#include <LittleFS.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/pem.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/ecp.h>

// Built-in cert removed — auto-generated at runtime

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

// ── Auto-cert helpers (needed before custom_cert_init) ──

bool auto_cert_is_active() {
	return LittleFS.exists(AUTO_CERT_MARKER_FILE);
}

// Write the auto-cert marker file (indicates cert was auto-generated)
static void auto_cert_write_marker() {
	File f = LittleFS.open(AUTO_CERT_MARKER_FILE, "w");
	if (f) {
		f.println("auto");
		f.close();
	}
}

// Forward declaration
bool auto_cert_generate();

// Check if the existing auto-cert has expired (or is unreadable).
// Returns false if system time is not yet set (no NTP).
static bool auto_cert_is_expired() {
	time_t now = time(nullptr);
	// If system time is not set (before NTP sync), can't check expiry
	if (now < 1704067200) return false;  // 2024-01-01

	unsigned char* cert_buf = nullptr;
	size_t cert_len = read_file_to_buffer(CUSTOM_CERT_FILENAME, &cert_buf);
	if (!cert_len) return true;  // Can't read → treat as expired

	mbedtls_x509_crt cert;
	mbedtls_x509_crt_init(&cert);
	int ret = mbedtls_x509_crt_parse_der(&cert, cert_buf, cert_len);
	free(cert_buf);
	if (ret != 0) {
		mbedtls_x509_crt_free(&cert);
		return true;  // Parse failed → treat as expired
	}

	struct tm tm_now;
	gmtime_r(&now, &tm_now);
	int y = tm_now.tm_year + 1900;
	int m = tm_now.tm_mon + 1;
	int d = tm_now.tm_mday;

	bool expired = (cert.valid_to.year < y) ||
	               (cert.valid_to.year == y && cert.valid_to.mon < m) ||
	               (cert.valid_to.year == y && cert.valid_to.mon == m && cert.valid_to.day < d);

	if (expired) {
		DEBUG_PRINTF("[CERT] Certificate expired (%04d-%02d-%02d)\n",
		             cert.valid_to.year, cert.valid_to.mon, cert.valid_to.day);
	} else {
		DEBUG_PRINTF("[CERT] Certificate valid until %04d-%02d-%02d\n",
		             cert.valid_to.year, cert.valid_to.mon, cert.valid_to.day);
	}
	mbedtls_x509_crt_free(&cert);
	return expired;
}

void custom_cert_init() {
	if (!custom_cert_exists()) {
		// No custom cert — auto-generate one (DNS SANs only, 10-year validity)
		DEBUG_PRINTLN(F("[CERT] No cert found, auto-generating..."));
		if (!auto_cert_generate()) {
			DEBUG_PRINTLN(F("[CERT] Auto-generation failed, HTTPS will be unavailable"));
			return;
		}
	} else if (auto_cert_is_active() && auto_cert_is_expired()) {
		// Auto-generated cert exists but expired — regenerate
		DEBUG_PRINTLN(F("[CERT] Auto-cert expired, regenerating..."));
		if (!auto_cert_generate()) {
			DEBUG_PRINTLN(F("[CERT] Regeneration failed, keeping expired cert"));
		}
	}
	// else: valid auto-cert, user-uploaded, or ACME cert — don't touch it

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
	DEBUG_PRINTLN(F("[CERT] Cert validation failed, HTTPS may be unavailable"));
}

CertInfo custom_cert_get_info() {
	CertInfo info;
	memset(&info, 0, sizeof(info));

	const unsigned char* cert_data;
	uint16_t cert_len;

	if (s_custom_loaded) {
		info.is_custom = true;
		info.is_auto = auto_cert_is_active();
		cert_data = s_custom_cert_der;
		cert_len = s_custom_cert_der_len;
	} else {
		// No cert loaded at all
		info.is_custom = false;
		info.is_auto = false;
		info.valid = false;
		return info;
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
	// User-uploaded cert replaces any auto-generated cert
	if (LittleFS.exists(AUTO_CERT_MARKER_FILE)) {
		LittleFS.remove(AUTO_CERT_MARKER_FILE);
	}
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
	if (LittleFS.exists(AUTO_CERT_MARKER_FILE)) {
		LittleFS.remove(AUTO_CERT_MARKER_FILE);
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
	return s_custom_cert_der;
}

uint16_t custom_cert_get_cert_len() {
	return s_custom_cert_der_len;
}

const unsigned char* custom_cert_get_key_data() {
	return s_custom_key_der;
}

uint16_t custom_cert_get_key_len() {
	return s_custom_key_der_len;
}

// ============================================================================
// Auto-generated self-signed certificate
// ============================================================================

bool auto_cert_generate() {
	DEBUG_PRINTLN(F("[CERT] Generating self-signed cert (DNS SANs, 10-year validity)..."));

	mbedtls_pk_context key;
	mbedtls_x509write_cert crt;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	bool success = false;
	unsigned char* cert_buf = nullptr;
	unsigned char* key_buf = nullptr;

	mbedtls_pk_init(&key);
	mbedtls_x509write_crt_init(&crt);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);

	int ret;
	const char* pers = "os_cert_gen";

	// Seed DRBG
	ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
	                             (const unsigned char*)pers, strlen(pers));
	if (ret != 0) {
		DEBUG_PRINTF("[CERT] DRBG seed failed: -0x%x\n", -ret);
		goto cleanup;
	}

	// Generate ECDSA P-256 key pair
	ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
	if (ret != 0) {
		DEBUG_PRINTF("[CERT] pk_setup failed: -0x%x\n", -ret);
		goto cleanup;
	}

	ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
	                          mbedtls_ctr_drbg_random, &ctr_drbg);
	if (ret != 0) {
		DEBUG_PRINTF("[CERT] Key generation failed: -0x%x\n", -ret);
		goto cleanup;
	}
	DEBUG_PRINTLN(F("[CERT] ECDSA P-256 key pair generated"));

	// Configure certificate
	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&crt, &key);
	mbedtls_x509write_crt_set_issuer_key(&crt, &key);  // self-signed

	// Set subject and issuer DN
	{
		char subject_name[128];
		snprintf(subject_name, sizeof(subject_name),
		         "C=DE,ST=NRW,L=Duesseldorf,O=OpenSprinkler,CN=opensprinkler.local");
		ret = mbedtls_x509write_crt_set_subject_name(&crt, subject_name);
		if (ret != 0) {
			DEBUG_PRINTF("[CERT] set_subject_name failed: -0x%x\n", -ret);
			goto cleanup;
		}
		ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject_name);
		if (ret != 0) {
			DEBUG_PRINTF("[CERT] set_issuer_name failed: -0x%x\n", -ret);
			goto cleanup;
		}
	}

	// Set serial number (random 16 bytes)
	{
		unsigned char serial[16];
		ret = mbedtls_ctr_drbg_random(&ctr_drbg, serial, sizeof(serial));
		if (ret != 0) goto cleanup;
		serial[0] &= 0x7F;  // Ensure positive (high bit clear)
		ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
		if (ret != 0) {
			DEBUG_PRINTF("[CERT] set_serial failed: -0x%x\n", -ret);
			goto cleanup;
		}
	}

	// Set validity: now to +10 years (format: "YYYYMMDDHHMMSS")
	{
		time_t now = time(nullptr);
		// If time is not set (before NTP), use a fixed start date
		if (now < 1704067200) now = 1704067200;  // 2024-01-01
		struct tm tm_now;
		gmtime_r(&now, &tm_now);
		char not_before[16], not_after[16];
		snprintf(not_before, sizeof(not_before), "%04d%02d%02d%02d%02d%02d",
		         tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
		         tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
		struct tm tm_exp = tm_now;
		tm_exp.tm_year += 10;
		snprintf(not_after, sizeof(not_after), "%04d%02d%02d%02d%02d%02d",
		         tm_exp.tm_year + 1900, tm_exp.tm_mon + 1, tm_exp.tm_mday,
		         tm_exp.tm_hour, tm_exp.tm_min, tm_exp.tm_sec);
		ret = mbedtls_x509write_crt_set_validity(&crt, not_before, not_after);
		if (ret != 0) {
			DEBUG_PRINTF("[CERT] set_validity failed: -0x%x\n", -ret);
			goto cleanup;
		}
	}

	// Set basic constraints (not a CA)
	ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
	if (ret != 0) goto cleanup;

	// Set key usage: digitalSignature
	ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
	if (ret != 0) goto cleanup;

	// Set extended key usage: serverAuth (OID 1.3.6.1.5.5.7.3.1)
	{
		mbedtls_asn1_sequence seq;
		memset(&seq, 0, sizeof(seq));
		// TLS Web Server Authentication OID
		static const unsigned char server_auth_oid[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
		seq.buf.tag = MBEDTLS_ASN1_OID;
		seq.buf.len = sizeof(server_auth_oid);
		seq.buf.p = (unsigned char*)server_auth_oid;
		seq.next = nullptr;
		ret = mbedtls_x509write_crt_set_ext_key_usage(&crt, &seq);
		if (ret != 0) goto cleanup;
	}

	// Set Subject Key Identifier
	ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
	if (ret != 0) goto cleanup;

	// Set Subject Alternative Names: DNS:opensprinkler.local, DNS:opensprinkler
	{
		mbedtls_x509_san_list san[2];
		memset(san, 0, sizeof(san));

		// DNS: opensprinkler.local
		san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
		san[0].node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
		san[0].node.san.unstructured_name.p = (unsigned char*)"opensprinkler.local";
		san[0].node.san.unstructured_name.len = strlen("opensprinkler.local");
		san[0].next = &san[1];

		// DNS: opensprinkler
		san[1].node.type = MBEDTLS_X509_SAN_DNS_NAME;
		san[1].node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
		san[1].node.san.unstructured_name.p = (unsigned char*)"opensprinkler";
		san[1].node.san.unstructured_name.len = strlen("opensprinkler");
		san[1].next = nullptr;

		ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san);
		if (ret != 0) {
			DEBUG_PRINTF("[CERT] set_subject_alternative_name failed: -0x%x\n", -ret);
			goto cleanup;
		}
	}

	// Write cert to DER
	cert_buf = (unsigned char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!cert_buf) cert_buf = (unsigned char*)malloc(2048);
	if (!cert_buf) {
		DEBUG_PRINTLN(F("[CERT] Failed to allocate cert buffer"));
		goto cleanup;
	}

	{
		// mbedtls_x509write_crt_der writes from the END of the buffer
		int cert_len = mbedtls_x509write_crt_der(&crt, cert_buf, 2048,
		                                          mbedtls_ctr_drbg_random, &ctr_drbg);
		if (cert_len < 0) {
			DEBUG_PRINTF("[CERT] write_crt_der failed: -0x%x\n", -cert_len);
			goto cleanup;
		}

		// Data is at cert_buf + 2048 - cert_len
		unsigned char* cert_start = cert_buf + 2048 - cert_len;

		// Write key to DER
		key_buf = (unsigned char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!key_buf) key_buf = (unsigned char*)malloc(512);
		if (!key_buf) {
			DEBUG_PRINTLN(F("[CERT] Failed to allocate key buffer"));
			goto cleanup;
		}

		int key_len = mbedtls_pk_write_key_der(&key, key_buf, 512);
		if (key_len < 0) {
			DEBUG_PRINTF("[CERT] write_key_der failed: -0x%x\n", -key_len);
			goto cleanup;
		}
		// Key data is at key_buf + 512 - key_len
		unsigned char* key_start = key_buf + 512 - key_len;

		// Save cert DER to LittleFS
		File f = LittleFS.open(CUSTOM_CERT_FILENAME, "w");
		if (!f) {
			DEBUG_PRINTLN(F("[CERT] Failed to write cert file"));
			goto cleanup;
		}
		f.write(cert_start, cert_len);
		f.close();

		// Save key DER to LittleFS
		f = LittleFS.open(CUSTOM_KEY_FILENAME, "w");
		if (!f) {
			LittleFS.remove(CUSTOM_CERT_FILENAME);
			DEBUG_PRINTLN(F("[CERT] Failed to write key file"));
			goto cleanup;
		}
		f.write(key_start, key_len);
		f.close();

		// Write auto-cert marker
		auto_cert_write_marker();

		DEBUG_PRINTF("[CERT] Self-signed cert generated: %d bytes cert, %d bytes key\n",
		             cert_len, key_len);
		success = true;
	}

cleanup:
	if (cert_buf) free(cert_buf);
	if (key_buf) free(key_buf);
	mbedtls_x509write_crt_free(&crt);
	mbedtls_pk_free(&key);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
	return success;
}

bool auto_cert_check_expiry() {
	if (!auto_cert_is_active()) return false;  // Not an auto-cert — don't touch
	if (!auto_cert_is_expired()) return false; // Still valid

	DEBUG_PRINTLN(F("[CERT] Auto-cert expired, regenerating..."));

	if (auto_cert_generate()) {
		// Reload the new cert into memory so server can use it
		// Free old buffers
		if (s_custom_cert_der) { free(s_custom_cert_der); s_custom_cert_der = nullptr; }
		if (s_custom_key_der) { free(s_custom_key_der); s_custom_key_der = nullptr; }
		s_custom_loaded = false;
		otf_custom_cert_data = nullptr;
		otf_custom_cert_len = 0;
		otf_custom_key_data = nullptr;
		otf_custom_key_len = 0;

		// Reload from newly generated files
		unsigned char* cert_buf = nullptr;
		size_t cert_len = read_file_to_buffer(CUSTOM_CERT_FILENAME, &cert_buf);
		unsigned char* key_buf = nullptr;
		size_t key_len = cert_len ? read_file_to_buffer(CUSTOM_KEY_FILENAME, &key_buf) : 0;

		if (cert_len && key_len) {
			s_custom_cert_der = cert_buf;
			s_custom_cert_der_len = (uint16_t)cert_len;
			s_custom_key_der = key_buf;
			s_custom_key_der_len = (uint16_t)key_len;
			s_custom_loaded = true;
			otf_custom_cert_data = s_custom_cert_der;
			otf_custom_cert_len = s_custom_cert_der_len;
			otf_custom_key_data = s_custom_key_der;
			otf_custom_key_len = s_custom_key_der_len;
			DEBUG_PRINTLN(F("[CERT] New cert loaded, will take effect on next HTTPS connection or reboot"));
		} else {
			if (cert_buf) free(cert_buf);
			if (key_buf) free(key_buf);
			DEBUG_PRINTLN(F("[CERT] Failed to reload new cert"));
		}
		return true;
	}
	return false;
}

#endif // ESP32
