# OpenSprinkler Firmware and Documentation

This Github repository contains the firmware source code and documentation for OpenSprinkler.

* For product details, visit [https://opensprinkler.com](https://opensprinkler.com).
* For firmware release notes, visit the [OpenSprinkler Firmware Releases](https://github.com/OpenSprinkler/OpenSprinkler-Firmware/releases) page on GitHub.

<hr class="double">

## Firmware Update

### OpenSprinkler v3 {: .hltitle}

Supports **OTA (over-the-air)** updates - you can upload a firmware directly via the web UI.

!!! warning "Back Up Before Updating"
    Before update, please back up your configurations (Sidebar → **Export Configuration**) so you can quickly restore programs and settings if needed.

    * A **Dotted Version Update**, e.g. 2.2.0→2.2.1, triggers a factory reset and **erases all settings**.
    * A **Build Number Update**, e.g. 2.2.1(2)→2.2.1(4), **preserves all settings**.

1. [**Download** a firmware file](https://raysfiles.com/os_compiled_firmware/v3.x/) (`.bin` format). The latest is `os_221_rev4.bin`.
2. **Open a browser** and enter `http://os-ip/update` where `os-ip` is your device's IP.
    * To find the IP: click button **B1** on the controller.
    * If your device is in **WiFi AP** (Access Point) mode, connect your computer/phone to the AP SSID shown on the LCD, and the update page is at `http://192.168.4.1/update`.
3.  At the update page, select the `.bin` file, enter your device password, and **Submit**.
4.  Wait for completion. The controller will reboot automatically once it finishes.
    * If the upload fails, retry the process.
    * If the device hangs, unplug and replug the power, then try again.

**Troubleshooting**

* **Blank Homepage:** If the device homepage is blank or showing an error, and you need to export configuration before updating, see [Blank Page Troubleshooting](troubleshooting.md#ui-app-time-and-lcd).
* **Update via Wired Ethernet:** If your controller is connected via wired Ethernet:
    * If it runs firmware `2.2.0` or newer, follow the same stesps as WiFi.
    * If it runs `2.1.9` or earlier, update must be done in WiFi mode:
        1. Power off the controller.
        2. Remove the Ethernet module.
        3. Power on - it will boot into WiFi AP mode.
        4. Follow the AP-mode update instructions above.
* **AP Mode Password Issues:** When updating in AP mode, if your password fails, try its **MD5 hash** instead (e.g. the MD5 hash of `opendoor` is `a6d82bced638de3def1e9bbb4983225c`).
* **Firmware Corruption / Device Not Booting**: If device fails to boot after an update, you'll need to re-flash the firmware using a [USB-Serial Programmer](https://opensprinkler.com/product/usb-programmer/).


---

### OpenSprinkler v2.3 {: .hltitle}

Requires a USB cable for firmware update. Follow the legacy [v2.3 Firmware Update Instructions](https://openthings.freshdesk.com/a/solutions/articles/5000832311).

---

### OpenSprinkler Pi (OSPi) {: .hltitle}

Update is done directly on the RPi. Follow [OSPi Firmware Update Instructions](https://openthings.freshdesk.com/a/solutions/articles/5000631599).

<hr class="double">

## Firmware Compilation

The firmware compilation instructions below are for OpenSprinkler **v3.x and v2.3** only.
<br>For RPi and Linux-based OpenSprinkler (OSPi), follow the [instructions are here](https://openthings.freshdesk.com/support/solutions/articles/5000631599-installing-and-updating-the-unified-firmware-on-ospi).

### Environment Setup
1.  Download and unzip (or git clone) this (`OpenSprinkler-Firmware`) repository.
2.  Install the latest LTS version of Node.js from [https://nodejs.org/](https://nodejs.org/) if you don't already have it.
3.  In your terminal, navigate to the `OpenSprinkler-Firmware` folder (the source code folder) and run `npm install html-minifier-terser`.
4.  Install Visual Studio Code (VS Code) from [https://code.visualstudio.com/](https://code.visualstudio.com/), if you don't already have it.
5.  Launch VS Code and install the **PlatformIO** extension.

### Building the Firmware
1.  In VS Code, click `File -> Open Folder` and select the `OpenSprinkler-Firmware` folder.
2.  PlatformIO will recognize the `platformio.ini` file in that folder, which contains all the libraries and settings needed to compile the firmware.
3.  Click the **PlatformIO: Build** button (with the checkmark icon ✓) in the blue status bar at the bottom of the screen to build the firmware.
4.  The default build environment is for OpenSprinkler v3.x (`env:os3x_esp8266`). To build for OpenSprinkler v2.3, switch to `env:os23_atmega1284p`.

<hr class="double">
