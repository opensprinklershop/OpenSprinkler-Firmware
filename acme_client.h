#ifndef ACME_CLIENT_H
#define ACME_CLIENT_H

#if defined(ESP32)

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

// LittleFS file paths for ACME data
#define ACME_ACCOUNT_KEY_FILE  "/acme_account.der"
#define ACME_DOMAIN_KEY_FILE   "/acme_domain.der"
#define ACME_CONFIG_FILE       "/acme_config.json"

// ACME server URLs
#define ACME_LETSENCRYPT_PROD  "https://acme-v02.api.letsencrypt.org/directory"
#define ACME_LETSENCRYPT_STAGE "https://acme-staging-v02.api.letsencrypt.org/directory"

// Renewal check interval: 24 hours
#define ACME_RENEWAL_CHECK_MS  (24UL * 3600UL * 1000UL)
// Renew when certificate has less than this many days remaining
#define ACME_RENEW_BEFORE_DAYS 30

// Status codes for the UI
enum AcmeStatus {
  ACME_STATUS_IDLE = 0,       // No ACME configured
  ACME_STATUS_CONFIGURED,     // Configured but no cert yet
  ACME_STATUS_REQUESTING,     // Currently requesting cert
  ACME_STATUS_ACTIVE,         // Cert active and valid
  ACME_STATUS_RENEWAL_DUE,    // Cert needs renewal soon
  ACME_STATUS_ERROR,          // Last operation failed
};

struct AcmeConfig {
  char domain[128];
  char email[128];
  char acme_server[256];
  bool enabled;
};

// Initialize ACME subsystem (call once at startup after LittleFS is ready)
void acme_init();

// Get current ACME status
AcmeStatus acme_get_status();

// Get last error message
const char* acme_get_last_error();

// Get current config
AcmeConfig acme_get_config();

// Save config and optionally start certificate request
bool acme_set_config(const char* domain, const char* email,
                     const char* acme_server, bool enabled);

// Request a new certificate (async — runs in background task)
bool acme_request_certificate();

// Check and renew certificate if needed
bool acme_check_renewal();

// Get days until current ACME cert expires (-1 if no cert)
int acme_days_until_expiry();

// Start the renewal timer (daily check)
void acme_start_renewal_timer();

// Stop renewal timer
void acme_stop_renewal_timer();

// Check if a URI matches the ACME challenge path and fill the response
// Returns true if the request was handled (response written to buffer)
bool acme_handle_challenge(const char* uri, char* response, size_t max_len);

// Delete all ACME data and revert to internal cert
bool acme_delete();

#endif // ESP32
#endif // ACME_CLIENT_H
