/* OpenSprinkler on-device web UI: package signing public key.
 *
 * UI packages (ui.tar on the upgrade server, see tools/mkuipack.py) are signed
 * with the same ECDSA P-256 key that signs the UI snapshot file lists for the
 * mobile apps (App-Keys/ui-update-signing.pem, scripts/gen-ui-filelist.js in
 * the app repo). The signature is ECDSA over SHA-256(package bytes), encoded as
 * the raw 64-byte r||s pair in base64, published next to the package as
 * <package>.sig and in manifest.json as "ui_sig".
 *
 * ui_local.cpp installs nothing unless the signature verifies against this key:
 * the firmware fetches the package over TLS without certificate validation
 * (WiFiClientSecure::setInsecure, like the firmware OTA), so the signature is
 * what guarantees origin and integrity, also for packages uploaded via /uf.
 *
 * Uncompressed SEC1 point (0x04 || X || Y), 65 bytes.
 */
#ifndef UI_LOCAL_PUBKEY_H
#define UI_LOCAL_PUBKEY_H
#include <stdint.h>

static const uint8_t UI_LOCAL_PUBKEY[65] = {
	0x04,
	0x5f,0x7e,0x21,0xe6,0x88,0xfe,0xd0,0x3d,0xb0,0xe8,0xa4,0x54,0x0d,0x0d,0xb4,0x4f,
	0xa0,0x72,0x9f,0xe0,0x5f,0x4f,0x55,0x70,0x12,0xcf,0x89,0x55,0xed,0x82,0x4e,0x8b,
	0x1e,0x78,0x0c,0x30,0x49,0x0c,0xb1,0x4f,0x5d,0x25,0xe3,0x58,0xb8,0xdc,0x22,0x1b,
	0x98,0x00,0xef,0xe1,0x1f,0x4e,0x5f,0xa7,0x2a,0xca,0xa7,0xed,0xdc,0x2e,0x45,0x06
};

#endif // UI_LOCAL_PUBKEY_H
