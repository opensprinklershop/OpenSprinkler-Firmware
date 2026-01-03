# OpenSprinkler ESP32-C5 Firmware - Features & Build Guide

## Hardware Support

### ESP32-C5 with External Flash
- **MCU**: ESP32-C5 (RISC-V, 160MHz)
- **Internal Flash**: 4MB
- **External Flash**: 16MB (W25Q128 via SPI)
- **RAM**: 400KB SRAM
- **Connectivity**: WiFi 2.4GHz, BLE 5.0, Ethernet (via ENC28J60)

### Pin Configuration (ESP32-C5)
See `pins_arduino.h` for complete pin mapping:
```cpp
TX = 11, RX = 12
SDA = 27, SCL = 26
MOSI = 5, MISO = 4, SCK = 3, SS = 6
External Flash CS = 6
```

## Features

### Core Functionality
- ✅ **Multi-Station Control**: Up to 48 stations (expandable)
- ✅ **Program Scheduler**: Multiple programs with complex schedules
- ✅ **Weather Integration**: Weather-based watering adjustments
- ✅ **Sensor Support**: 17+ sensor types (see ANALOG_SENSOR_API.md)
- ✅ **Flow Monitoring**: Real-time flow rate tracking
- ✅ **Master Valve/Pump**: Support for 2 master stations
- ✅ **Rain Delay**: Manual and sensor-based rain delay
- ✅ **Notifications**: Email, SMS, IFTTT, Pushover integration
- ✅ **Logging**: Station runtime logs with InfluxDB support

### Network Features
- ✅ **WiFi**: Client and AP modes with WPS
- ✅ **Ethernet**: ENC28J60 support for wired connection
- ✅ **Web Interface**: HTML5 responsive UI
- ✅ **MQTT**: Device control and status reporting
- ✅ **OTA Updates**: Web-based and ArduinoOTA support
- ✅ **OpenThings Framework**: Cloud integration (optional)

### Sensor Integration
- ✅ **RS485/Modbus**: Industrial sensors (see ANALOG_SENSOR_API.md)
- ✅ **Analog Sensor Board (ASB)**: 10 sensor types
- ✅ **I2C Sensors**: ADS1115 ADC support
- ✅ **BLE Sensors**: Bluetooth Low Energy sensors
- ✅ **Zigbee Sensors**: Zigbee coordinator mode (ZCZR)
- ✅ **MQTT Sensors**: Remote sensor values
- ✅ **Weather APIs**: OpenWeatherMap, Weather Underground, etc.

### Advanced Features
- ✅ **Sensor Groups**: AND/OR/XOR/NOT logic for multiple sensors
- ✅ **Monitor Types**: Continuous, on-demand, timed monitoring
- ✅ **Remote Extensions**: Control external relay boards
- ✅ **Flow-based Control**: GPM/Liter-based station control
- ✅ **Current Sensing**: Overcurrent/undercurrent detection
- ⏳ **Matter Integration**: Smart home standard (in development)

## Build Environments

### Standard Build (espc5-12)
Full-featured firmware with Zigbee support:
```bash
pio run -e espc5-12
pio run -e espc5-12 -t upload
```

**Features**:
- WiFi + BLE + Zigbee (ZCZR mode)
- All sensor types
- Web OTA + ArduinoOTA
- OpenThings Framework
- Partition: os4.csv (3.4MB app, 444KB spiffs)

**Build Flags**:
```ini
-D ESP32C5
-D ZIGBEE_MODE_ZCZR
-D ENABLE_DEBUG
```

### Matter Build (espc5-12-matter) 🔄 IN DEVELOPMENT
Matter-enabled firmware (WiFi-based):
```bash
pio run -e espc5-12-matter
```

**Features**:
- Matter WiFi (no Thread to avoid RF conflicts)
- **NO Zigbee** (RF coexistence issues)
- All other features identical to standard build
- Partition: os4_matter.csv (2.5MB app, 1.2MB spiffs, 128KB Matter NVS)

**Build Flags**:
```ini
-D ESP32C5
-D ENABLE_MATTER              # Enables Matter code
-D CHIP_CONFIG_MAX_FABRICS=6  # Memory optimization
-D CONFIG_ENABLE_CHIP_SHELL=0 # Disable shell to save RAM
```

**Status**: API integration complete, waiting for ESP-Matter SDK  
See [MATTER_INTEGRATION.md](MATTER_INTEGRATION.md) for details.

### ESP8266 Build (os3x_esp8266)
Legacy ESP8266 support:
```bash
pio run -e os3x_esp8266
```

### Native Linux Build (linux)
For development/testing on Linux/WSL:
```bash
./build.sh  # Installs dependencies and builds
```

### Demo Build (demo)
Minimal compile test (Windows-friendly):
```bash
pio run -e demo
```

## Flash Partitions

### Standard Partition (os4.csv)
```csv
nvs,         data, nvs,      0x9000,   0x5000,   # 20KB
otadata,     data, ota,      0xE000,   0x2000,   # 8KB
app0,        app,  factory,  0x10000,  0x350000, # 3392KB
zb_storage,  data, fat,      0x360000, 0x20000,  # 128KB Zigbee
zb_fct,      data, fat,      0x380000, 0x1000,   # 4KB
spiffs,      data, spiffs,   0x381000, 0x6F000,  # 444KB
coredump,    data, coredump, 0x3F0000, 0x10000,  # 64KB
```

### Matter Partition (os4_matter.csv)
```csv
nvs,         data, nvs,      0x9000,   0x6000,   # 24KB
matter_fct,  data, nvs,      0xF000,   0x1000,   # 4KB Matter Factory
app0,        app,  factory,  0x10000,  0x280000, # 2560KB (smaller for Matter)
matter_kvs,  data, nvs,      0x290000, 0x20000,  # 128KB Matter KVS
zb_storage,  data, fat,      0x2B0000, 0x10000,  # 64KB (reduced)
spiffs,      data, spiffs,   0x2C0000, 0x130000, # 1216KB (larger for external files)
coredump,    data, coredump, 0x3F0000, 0x10000,  # 64KB
```

## Development Workflow

### Initial Setup
```bash
# Clone repository
git clone https://github.com/OpenSprinkler/OpenSprinkler-Firmware.git
cd OpenSprinkler-Firmware

# Install PlatformIO
pip install platformio

# Build
pio run -e espc5-12
```

### Upload Methods

**1. Serial Upload (First time)**:
```bash
pio run -e espc5-12 -t upload
```

**2. OTA Upload (subsequent)**:
Edit `platformio.ini`:
```ini
upload_protocol = espota
upload_port = 192.168.x.x  # Your device IP
upload_flags = --auth=yourpassword
```
Then:
```bash
pio run -e espc5-12 -t upload
```

**3. Web OTA**:
Navigate to `http://device-ip/update` and upload firmware.bin

**4. ArduinoOTA** (if enabled):
```bash
pio run -e espc5-12 -t upload --upload-port device-ip
```

### Debugging
```bash
# Monitor serial output
pio device monitor -e espc5-12

# Enable debug output in platformio.ini
build_flags = 
    -D ENABLE_DEBUG
    -D SERIAL_DEBUG  # More verbose
```

## Configuration Files

### platformio.ini
Main build configuration with environments, dependencies, and build flags.

### sdkconfig / sdkconfig.defaults
ESP-IDF configuration for ESP32-C5:
- WiFi/BLE/Zigbee coexistence settings
- Memory optimization (see sdkconfig.defaults)
- Flash/partition configuration

### External Dependencies
Located in `lib_deps`:
- `Ethernet` - ENC28J60 Ethernet
- `Zigbee` - ESP32 Zigbee stack
- `PubSubClient` - MQTT
- `WebSockets` - WebSocket support
- `OpenThings-Framework-Firmware-Library` - Cloud integration
- `ADS1X15` - I2C ADC
- `InfluxDB-Client-for-Arduino` - Time-series logging

## Documentation

### API Documentation
- [ANALOG_SENSOR_API.md](ANALOG_SENSOR_API.md) - Complete sensor API reference
- [MATTER_INTEGRATION.md](MATTER_INTEGRATION.md) - Matter smart home integration

### Code Structure
```
openSprinkler-firmware-esp32/
├── main.cpp                    # Main loop and initialization
├── OpenSprinkler.{h,cpp}       # Core OS class
├── program.{h,cpp}             # Program scheduling
├── opensprinkler_server.cpp    # HTTP API endpoints
├── mqtt.{h,cpp}                # MQTT integration
├── sensors.{h,cpp}             # Sensor framework
├── sensor_*.{h,cpp}            # Individual sensor implementations
├── opensprinkler_matter.{h,cpp}# Matter integration
├── gpio.{h,cpp}                # GPIO abstraction
├── defines.h                   # Global constants and options
└── platformio.ini              # Build configuration
```

## Sensor API

See [ANALOG_SENSOR_API.md](ANALOG_SENSOR_API.md) for complete documentation on:
- 17+ sensor types (RS485, ASB, BLE, Zigbee, MQTT, Weather, etc.)
- Unit IDs (%, °C, V, kPa, etc.)
- Monitor types (AND, OR, XOR, NOT, TIME, REMOTE)
- Sensor groups and thresholds
- Implementation examples

## Matter Integration

Matter (CHIP) smart home integration in development:
- Device Type: **Valve** (0x0042) for irrigation stations
- Protocol: WiFi-based (not Thread)
- Status: API integration complete, ESP-Matter SDK pending

See [MATTER_INTEGRATION.md](MATTER_INTEGRATION.md) for:
- Architecture details
- Implementation status
- Build instructions
- Future roadmap

## Contributing

Pull requests welcome! Please:
1. Follow existing code style
2. Test on real hardware (ESP32-C5 preferred)
3. Document new features
4. Update relevant .md files

## License

GPL v3.0 - See LICENSE.txt

## Support

- GitHub Issues: https://github.com/OpenSprinkler/OpenSprinkler-Firmware/issues
- Forums: https://opensprinkler.com/forums
- Documentation: https://opensprinkler.github.io/OpenSprinkler-Firmware/
