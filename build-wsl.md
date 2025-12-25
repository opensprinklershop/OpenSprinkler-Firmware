# WSL Build-Anleitung für OpenSprinkler

## Setup (einmalig)

### 1. WSL Terminal öffnen und zum Projekt navigieren:
```bash
cd /mnt/d/Projekte/openSprinkler-firmware-esp32
```

### 2. Abhängigkeiten installieren:
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    git \
    libmosquitto-dev \
    libi2c-dev \
    libssl-dev \
    libgpiod-dev \
    gpiod \
    libmodbus-dev \
    libcurlpp-dev \
    python3 \
    python3-pip
```

### 3. PlatformIO Core in WSL installieren (optional für PlatformIO-Builds):
```bash
pip3 install platformio
export PATH=$PATH:~/.local/bin
```

### 4. Git Submodule initialisieren:
```bash
git submodule update --recursive --init
```

## Build-Optionen

### Option A: Native Linux-Build mit build.sh
```bash
chmod +x build.sh build2.sh
./build.sh
```

### Option B: PlatformIO in WSL (für env:linux)
```bash
pio run -e linux
```

### Option C: Demo-Build
```bash
./build.sh demo
```

## Wichtig

- **Arduino-Builds (ESP32/ESP8266)**: Bleib in **Windows** mit VS Code + PlatformIO
- **Linux-Builds (env:linux, OSPI)**: Nutze **WSL** oder Docker
- Pfade: Windows-Laufwerke sind in WSL unter `/mnt/` gemountet (z.B. `D:` → `/mnt/d`)

## Troubleshooting

Wenn `build.sh` einen Fehler mit `stat` zeigt, ist das normal für WSL1 (funktioniert aber weiter).
