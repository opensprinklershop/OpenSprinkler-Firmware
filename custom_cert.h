#ifndef CUSTOM_CERT_H
#define CUSTOM_CERT_H

#if defined(ESP32)

#include <Arduino.h>

// LittleFS file paths for custom certificate and key (DER format)
#define CUSTOM_CERT_FILENAME "/custom_cert.der"
#define CUSTOM_KEY_FILENAME  "/custom_key.der"

// Certificate info structure
struct CertInfo {
	bool is_custom;           // true if custom cert is loaded
	char subject[128];        // Subject CN
	char issuer[128];         // Issuer CN
	char not_before[32];      // Validity start (ISO date)
	char not_after[32];       // Validity end (ISO date)
	bool valid;               // true if parsing succeeded
};

// Initialize custom certificate system.
// Must be called BEFORE OTF/server creation.
// If custom cert files exist on LittleFS, loads them and sets the
// global cert pointers that Esp32LocalServer will use.
void custom_cert_init();

// Get certificate info (current active cert — custom or internal)
CertInfo custom_cert_get_info();

// Upload and validate PEM certificate and key.
// Parses PEM, extracts DER, validates cert+key pair with mbedTLS.
// On success, saves DER files to LittleFS and returns true.
// On failure, sets error message and returns false.
bool custom_cert_upload(const char* cert_pem, const char* key_pem, char* error_buf, size_t error_buf_len);

// Delete custom certificate files, reverting to internal cert.
// Returns true if files were deleted (or didn't exist).
bool custom_cert_delete();

// Check if custom cert files exist on LittleFS
bool custom_cert_exists();

// Get the active cert/key DER data (custom if loaded, otherwise internal).
// These pointers are valid as long as the firmware is running.
const unsigned char* custom_cert_get_cert_data();
uint16_t custom_cert_get_cert_len();
const unsigned char* custom_cert_get_key_data();
uint16_t custom_cert_get_key_len();

#endif // ESP32
#endif // CUSTOM_CERT_H
