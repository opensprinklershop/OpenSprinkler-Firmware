#!/usr/bin/env python3
"""configure_mbedtls.py

Configure mbedTLS for ESP32-C5.

This script updates the PlatformIO build directory sdkconfig before build.
Important: Kconfig represents disabled booleans as "# CONFIG_FOO is not set".
We therefore explicitly track both enabled values and disabled keys.
"""

import os
Import("env")

# Path to the build directory's sdkconfig
build_dir = env.subst("$BUILD_DIR")
framework_dir = env.PioPlatform().get_package_dir("framework-esp-idf")
sdkconfig_path = os.path.join(build_dir, "sdkconfig")

print("=" * 60)
print("Configuring mbedTLS for ESP32-C5...")
print(f"Build dir: {build_dir}")
print(f"SDK config: {sdkconfig_path}")
print("=" * 60)

# mbedTLS configuration for ESP32-C5 (400KB SRAM, no PSRAM)
# Focus: keep TLS 1.3 only + lower RAM footprint (HTTPS server)
mbedtls_config = {
    # Memory optimization
    "CONFIG_MBEDTLS_DYNAMIC_BUFFER": "y",
    "CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT": "y",
    "CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA": "y",
    # Keep input higher for incoming HTTP requests; shrink output buffer to save RAM.
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN": "4096",
    "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN": "2048",

    # Trim features we don't use on the embedded HTTPS server.
    # (Note: disabled booleans are written as "# CONFIG_FOO is not set")
    "CONFIG_MBEDTLS_SSL_ALPN": None,
    "CONFIG_MBEDTLS_SSL_RENEGOTIATION": None,
    "CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS": None,
    "CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS": None,
    
    # Key exchange methods (essential for client connections)
    "CONFIG_MBEDTLS_KEY_EXCHANGE_RSA": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_DHE_RSA": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_ELLIPTIC_CURVE": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA": "y",
    "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDH_RSA": "y",
    
    # Ciphers
    "CONFIG_MBEDTLS_AES_C": "y",
    "CONFIG_MBEDTLS_GCM_C": "y",
    "CONFIG_MBEDTLS_CIPHER_MODE_CBC": "y",
    
    # TLS versions: TLS 1.3 only
    "CONFIG_MBEDTLS_SSL_PROTO_TLS1_3": "y",
    "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": None,

    # Prefer hardware acceleration for symmetric crypto
    "CONFIG_MBEDTLS_HARDWARE_AES": "y",
}

def configure_sdkconfig():
    """Update sdkconfig with mbedTLS settings."""
    if not os.path.exists(sdkconfig_path):
        print(f"Warning: sdkconfig not found at {sdkconfig_path}")
        print("This PlatformIO environment appears to be using precompiled ESP32 Arduino libraries.")
        print("In that case, mbedTLS feature flags (CONFIG_MBEDTLS_*) are baked into the precompiled libmbedtls_*.a and cannot be changed here.")
        print("If you need sdkconfig/mbedTLS Kconfig trimming to take effect, use a source-build environment (e.g. env:espc5-12-src).")
        return
    
    # Read existing config
    with open(sdkconfig_path, "r") as f:
        lines = f.readlines()

    # Parse existing config. Keep enabled values and disabled keys.
    config_dict = {}
    disabled_keys = set()
    for raw in lines:
        line = raw.strip()
        if not line:
            continue

        if line.startswith("#"):
            # Example: "# CONFIG_FOO is not set"
            if line.startswith("# CONFIG_") and line.endswith("is not set"):
                key = line[len("# "):].split(" ", 1)[0]
                disabled_keys.add(key)
            continue

        if "=" in line:
            key, value = line.split("=", 1)
            config_dict[key] = value
            if key in disabled_keys:
                disabled_keys.remove(key)
    
    # Apply our mbedTLS config
    for key, value in mbedtls_config.items():
        if value is None:
            config_dict.pop(key, None)
            disabled_keys.add(key)
            print(f"  # {key} is not set")
        else:
            config_dict[key] = value
            if key in disabled_keys:
                disabled_keys.remove(key)
            print(f"  {key}={value}")
    
    # Write back (sorted) in Kconfig-compatible format
    with open(sdkconfig_path, "w") as f:
        for key in sorted(disabled_keys):
            f.write(f"# {key} is not set\n")
        for key, value in sorted(config_dict.items()):
            f.write(f"{key}={value}\n")
    
    print("mbedTLS configuration updated successfully!")
    print("=" * 60)

# Run configuration before build
configure_sdkconfig()
