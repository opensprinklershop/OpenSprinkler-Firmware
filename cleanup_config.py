#!/usr/bin/env python3
"""
Cleanup sdkconfig: Entferne doppelte Parameter und aktualisiere bestehende
"""

# Lese sdkconfig
with open('sdkconfig.esp32-c5', 'r', encoding='utf-8') as f:
    content = f.read()

# Entferne die hinzugefügten AGGRESSIVE MEMORY Zeilen am Ende
lines = content.split('\n')
cutoff_idx = len(lines)
for i, line in enumerate(lines):
    if '# AGGRESSIVE MEMORY OPTIMIZATION' in line:
        cutoff_idx = i
        break

# Behalte nur die ursprünglichen Zeilen
original_lines = lines[:cutoff_idx]

# Updates für bestehende Parameter
updates = {
    'CONFIG_ENABLE_CHIP_SHELL': ('n', False),  # (value, is_y)
    'CONFIG_NIMBLE_MAX_CONNECTIONS': ('1', True),
    'CONFIG_BTDM_CTRL_BLE_MAX_CONN': ('1', True),
    'CONFIG_BT_NIMBLE_MAX_CONNECTIONS': ('1', True),
    'CONFIG_BT_NIMBLE_ROLE_OBSERVER': ('n', False),
    'CONFIG_BT_NIMBLE_SECURITY_ENABLE': ('n', False),
    'CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT': ('n', False),
    'CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT': ('n', False),
    'CONFIG_SPI_MASTER_ISR_IN_IRAM': ('n', False),
    'CONFIG_SPI_SLAVE_ISR_IN_IRAM': ('n', False),
    'CONFIG_NEWLIB_NANO_FORMAT': ('y', True),
    'CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT': ('2', True),
    'CONFIG_ESP_MATTER_MAX_DEVICE_TYPE_COUNT': ('2', True),
    'CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY': ('y', True),
    'CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH': ('y', True),
    'CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH': ('y', True),
    'CONFIG_SPI_FLASH_ROM_IMPL': ('y', True),
    'CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH': ('y', True),
}

output = []
replaced_count = 0

for line in original_lines:
    replaced = False
    
    for key, (value, is_y) in updates.items():
        if line.startswith(key + '='):
            # Ersetze den Wert
            if is_y and value == 'y':
                output.append(f'{key}=y')
            elif is_y and value != 'y':
                output.append(f'# CONFIG_{key.replace("CONFIG_", "")} is not set')
            elif not is_y and value == 'n':
                output.append(f'# CONFIG_{key.replace("CONFIG_", "")} is not set')
            else:
                output.append(f'{key}={value}')
            replaced = True
            replaced_count += 1
            break
        elif line.startswith('# ' + key + ' is not set'):
            # Ersetze "not set" Zeilen
            if is_y and value == 'y':
                output.append(f'{key}=y')
            elif not is_y and value == 'n':
                output.append(f'# CONFIG_{key.replace("CONFIG_", "")} is not set')
            else:
                output.append(f'{key}={value}')
            replaced = True
            replaced_count += 1
            break
    
    if not replaced:
        output.append(line)

# Schreibe zurück
with open('sdkconfig.esp32-c5', 'w', encoding='utf-8') as f:
    f.write('\n'.join(output))

print(f"✓ {replaced_count} Konfigurationen aktualisiert")
print(f"✓ Datei bereinigt und optimiert")
