#!/bin/bash

	echo "Compiling OSPi firmware..."

	USEGPIO=""
        GPIOLIB=""

	source /etc/os-release
        VERSION_MAJOR=${VERSION_ID%%.*}
        # Trixie (Debian 13+) uses lgpio, Bookworm (Debian 12) uses libgpiod, older uses sysfs
        if [[ ${VERSION_MAJOR} -ge 13 ]]; then
                echo "Detected Debian ${VERSION_ID} (Trixie or newer) - using lgpio"
                USEGPIO="-DLIBLGPIO"
                GPIOLIB="-llgpio"
        elif [[ ${VERSION_MAJOR} -ge 10 ]]; then
                echo "Detected Debian ${VERSION_ID} (Bookworm or similar) - using libgpiod"
                USEGPIO="-DLIBGPIOD"
                GPIOLIB="-lgpiod"
        fi


        ADS1115=""
        ADS1115FILES=""

        I2C=$(i2cdetect -y 1)
        if [[ "${I2C,,}" == *"48 --"* ]] ;then
                echo "found PCF8591"
                PCF8591="-DPCF8591"
                PCF8591FILES="./ospi-analog/driver_pcf8591*.c ./ospi-analog/iic.c"
        fi

        if [[ "${I2C,,}" == *"48 49"* ]] ;then
                echo "found ADS1115"
                ADS1115="-DADS1115"
                ADS1115FILES="./ospi-analog/driver_ads1115*.c ./ospi-analog/iic.c"
        fi


        echo "Compiling firmware..."
        # Use workspace libraries if external submodules don't exist
        if [ -d "external/TinyWebsockets/tiny_websockets_lib/src" ]; then
            ws=$(ls external/TinyWebsockets/tiny_websockets_lib/src/*.cpp)
            ws_include="-Iexternal/TinyWebsockets/tiny_websockets_lib/include"
        elif [ -d "../arduinoWebSockets/src" ]; then
            ws=$(ls ../arduinoWebSockets/src/*.cpp)
            ws_include="-I../arduinoWebSockets/src"
        else
            ws=""
            ws_include=""
        fi
        
        if [ -d "external/OpenThings-Framework-Firmware-Library" ]; then
            otf=$(ls external/OpenThings-Framework-Firmware-Library/*.cpp)
            otf_include="-Iexternal/OpenThings-Framework-Firmware-Library/"
        elif [ -d "../OpenThings-Framework-Firmware-Library" ]; then
            otf=$(ls ../OpenThings-Framework-Firmware-Library/*.cpp)
            otf_include="-I../OpenThings-Framework-Firmware-Library/"
        else
            otf=""
            otf_include=""
        fi
        
        if [ -d "external/influxdb-cpp" ]; then
            ifx=$(ls external/influxdb-cpp/*.hpp 2>/dev/null || echo "")
            ifx_include="-Iexternal/influxdb-cpp/"
        else
            ifx=""
            ifx_include=""
        fi
        
        g++ -o OpenSprinkler -DOSPI $USEGPIO $ADS1115 $PCF8591 -DSMTP_OPENSSL -DHAVE_TINY_WEBSOCKETS $DEBUG -std=c++17 -include string.h main.cpp \
                OpenSprinkler.cpp program.cpp opensprinkler_server.cpp utils.cpp weather.cpp gpio.cpp mqtt.cpp sunrise.cpp \
                smtp.c RCSwitch.cpp psram_utils.cpp TimeLib.cpp sensor*.cpp notifier.cpp naett.c \
                $ADS1115FILES $PCF8591FILES \
                $ws_include \
                $ws \
                $otf_include \
                $otf \
                $ifx osinfluxdb.cpp $ifx_include \
                -lpthread -lmosquitto -lssl -lcrypto -lcurl -li2c -lmodbus -lbluetooth $GPIOLIB


