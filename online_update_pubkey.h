/* OpenSprinkler ESP8266 online update: firmware signing public key.
 *
 * Release binaries for the ESP8266 (firmware_esp8266.bin on the upgrade
 * server) carry an RSA-2048 / SHA-256 signature in the format of the ESP8266
 * Arduino core signing tool (tools/signing.py): <binary><signature><u32 length>.
 * fw.sh appends it when the binary is copied to the upgrade directory
 * (ESP8266_SIGNING_KEY in .env, default /data/Workspace/App-Keys/esp8266-fw-signing.pem).
 *
 * The online updater (online_update.cpp, ESP8266 branch) installs this key as
 * BearSSL::SigningVerifier on the Updater, so a download whose signature does
 * not verify is discarded before the new image is activated. The ESP8266 cannot
 * validate the server certificate chain (manifest and binary are fetched over
 * plain HTTP), so this signature is what guarantees image integrity and origin.
 *
 * Manual uploads via /update are not affected: the verifier is installed only
 * for the online update and removed afterwards. Signed binaries can still be
 * flashed by serial or /update; the bootloader ignores the trailing signature.
 *
 * Define OTA_ESP8266_NO_SIGNATURE_CHECK at build time to disable verification
 * (development builds that fetch unsigned images from a private manifest).
 */
#ifndef _ONLINE_UPDATE_PUBKEY_H
#define _ONLINE_UPDATE_PUBKEY_H

#define OTA_ESP8266_SIGNING_PUBKEY_PEM \
"-----BEGIN PUBLIC KEY-----\n" \
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAybOvB98wpwnTXUcYEca1\n" \
"D6YHJ31pnWWLxSxur92xpJbIAAXymGgs5aRjMZ7F+ZcYWCJUY+Nk9r6v3hliQ0LM\n" \
"pSe0b33ebcifr8byKGl9vx3DrUUiqbLSi5vCjlULRXjFbSNvFSxO54/VdNyHWSUj\n" \
"xqNZz7tghXNwDKu9s1N4RHtFIrmuuiAxe3iOtSJ4v8uEg0kcxDQY22vwUnLlXX8S\n" \
"6AeGIU3edQymIgTUwLFjXCjhd5K0DyExIR//JIk7eO7CsMqEsmwdFzdlzEt3boQU\n" \
"FpWu78FHViCoW1c+bwedwfY1nImm5nruzI748KNDg0Fi5mwWa8stMDy2iCsAbvfu\n" \
"NQIDAQAB\n" \
"-----END PUBLIC KEY-----\n"

#endif // _ONLINE_UPDATE_PUBKEY_H
