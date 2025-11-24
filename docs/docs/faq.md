## OpenSprinkler Frequently Asked Questions

!!! note
    This page covers the most common questions. For technical support, check the [User Manual](2.2.1/221_4_manual.md), the [Support Portal](https://support.openthings.io) or the [Community Forums](https://opensprinkler.com/forums/).

### Pre-Sales

**Q: What is OpenSprinkler?**
<br>
OpenSprinkler is an open-source, web-based sprinkler and irrigation controller. It's a drop-in replacement for conventional sprinkler controllers, offering several advantages:

* **Intuitive User Interface (UI):** No more messing with buttons and knobs. Use web browsers and our free mobile apps to access OpenSprinkler.
* **Remote Access:** Access your OpenSprinkler from anywhere, whether you are at home, at the office, or traveling.
* **Smart Control:** OpenSprinkler can connect to the Internet and use real-time weather data (including historic and forecast data) to automatically make watering time adjustments. It not only stops watering when it rains but also scales watering time up or down based on your local temperature and humidity.
* **Connectivity Options:** OpenSprinkler supports both WiFi and wired Ethernet. You can connect to it locally on your network only (without Internet access) or enable cloud access for remote control. The built-in WiFi can also function as an Access Point (AP), which is ideal for situations where you don't have any external router.
* **Expandable:** The main controller handles 8 zones and is expandable up to 72 zones with expanders. Our app can also manage multiple OpenSprinkler systems for limitless expansion.
* **Advanced Features:** OpenSprinkler supports features rarely seen on competing products, such as MQTT, remote zones, HTTP(S) zones, Email notifications, radio frequency (RF) stations, multiple sensors including flow sensor, two independent master stations, repeating program start times, and multi-language support.

---

**Q: Who is OpenSprinkler for?**
<br>
Homeowners and business owners, including growers, farms, business parks, ranches, golf courses, and sprinkler service professionals. It's also for electronics enthusiasts who like to tinker and use OpenSprinkler as a platform for their own projects. The system scales from a few zones to large multi-zone and multi-controller sites.

---
<a id="hardware-versions"></a>
**Q: What hardware versions are available?**
<br>
We currently offer **OpenSprinkler 3.x** (WiFi; optional wired Ethernet module) and **OpenSprinkler Pi** (OSPi, driven by Raspberry Pi). Furthermore, OpenSprinkler 3.x provides three variants:

* **AC-powered:** Operates standard 24VAC sprinkler valves and is powered by a 24VAC adapter.
* **DC-powered:** Also operates 24VAC sprinkler valves, but is powered by a DC adapter. This is convenient for international customers or for off-grid / solar-powered setups.
* **Latch:** Powered by a DC adapter and operates <u>latching solenoids</u> only.

---
<a id="choose-models"></a>
**Q: How do I choose between the AC/DC/Latch models?**


* **Using a pump start relay?** Choose the AC model: it supports all 24VAC pump relays.
    * If you prefer the DC model but need to control a pump, the best option is to use a solid state relay (SSR) or DC-input relay.
* **Using latching solenoids?** Choose the Latch model: it's the only model that works with latching valves (often labeled 'Latch/Latching' on the valve body).
* **Otherwise**: Choose either the AC or DC model.
    * Both work with standard 24VAC valves.
    * DC is more flexible and supports off-grid/solar-powered setups. In addition, DC adapters are easier and cheaper to source outside North America.
    * AC is the simplest drop-in if you already have a 24VAC transformer or accessories (e.g. pump relays, wireless sensors) that require 24VAC.

---

**Q: How many zones does it support?**
<br>
The main controller supports `8` zones; each expander adds `16`. Current firmware supports a maximum of `72` zones on OS 3.x and `200` on OSPi.

---

**Q: I have 12 zones. What do I need to buy?**
<br>
One main controller (`8` zones) and one expander (`16` additional) give you a total of `24` zones that cover all your 12 zones with room for future expansion. Please note that OpenSprinkler uses software-defined Master zone, so each Master counts as a separate zone (e.g. `12` zones plus a master count as `13` zones). 

---

**Q: How many programs and start times are supported?**
<br>
Up to `40` programs. Each program supports per-station durations (`0–18` h, **one-second precision**); weekly, monthly, single-run, or interval schedule dates (e.g. every `n` days); and either up to `4` arbitrary fixed start times, or repeating start times with any number of repeats (e.g., start `8:30 AM`, repeat every `8` min for `55` times).

---

**Q: Does the weather feature work outside North America?**
<br>
Yes. You can choose among a variety of weather data providers (Apple, OpenMeteo, AccuWeather, Weather Underground etc.), most of which have availability worldwide.

---

**Q: Does it require a cloud connection?**
<br>
No. By default it connects only to your local WiFi network and does NOT rely on the Internet. Cloud connection is optional and only needed for remote access.

---

**Q: Is there a subscription fee for cloud access, app, or weather data?**
<br>
No. OpenSprinkler is a one-time purchase. All features are **completely free** without a recurring fee.

---

**Q: What WiFi network is it compatible with?**
<br>
OS v3.x is compatible with 2.4GHz WiFi. It does **NOT** support 5GHz currently. Ensure your router is broadcasting a 2.4GHz WiFi (most 5GHz routers support 'dual-band' and can broadcast both).

---

**Q: How is OpenSprinkler different from competitors?**

* It has built-in web interface for local control, and runs programs on its own, without relying on proprietary or cloud-only software.
* Advanced features, such as second-precision watering times, parallel / simultaneous zone runs, multiple sensors and master zones, pause, flow sensor support, Email notifications, and special station types (Remote, RF, HTTP(S)).
* Supports both WiFi and wired Ethernet options.
* Easy expansion to `72` zones (or `200` with OSPi) at a much lower cost than competitors.
* Cloud access is optional: your controller works locally even if your internet is down.
* Built on **open-source hardware and software**: its design files are public, allowing for customization and extension.

---

**Q: Where are OpenSprinkler's documents and source code?**

* [User Manual](https://opensprinkler.github.io/OpenSprinkler-Firmware/2.2.1/221_4_manual/)
* [API Reference](https://opensprinkler.github.io/OpenSprinkler-Firmware/2.2.1/221_4_api/)
* [Video Tutorials](https://openthings.freshdesk.com/support/solutions/articles/5000860920-videos-introduction-to-opensprinkler-v3)
* [GitHub Repository](https://github.com/opensprinkler)

<hr class="double">

### Shipping

**Q: Where can I buy it?**
<br>
You can purchase it directly from the [OpenSprinkler product page](https://opensprinkler.com/product/opensprinkler/)

---

**Q: What's included in the package?**

* One OpenSprinkler controller (8 zones).
* **AC-powered** model: 24VAC adapter **not** included by default. It can be purchased as an optional add-on (for orders shipped to North America), or you can reuse an existing sprinkler transformer (22-30VAC).
* **DC-powered** & **Latch** models: include a universal DC power adapter for orders shipped to North America.

---

**Q: How fast do you ship?**
<br>
Typically within 1 business day (unless noted otherwise or back-ordered). Same-day shipping also available. Tracking number is emailed to you once shipped.

---

**Q: What shipping options are available? What’s the warranty/return policy?**
<br>
Please check our [Terms and Policies](https://opensprinkler.com/terms-and-conditions/)

<hr class="double">

### Installation and Usage

**Q: I'm new to sprinkler systems. How do I install and wire it?**
<br>
OpenSprinkler is a drop-in replacement for an existing controller. DIY installation is easy.  

*  Carefully label and remove wires from your existing controller.
*  Mount OpenSprinkler and re-insert wires.
*  Connect to your router (WiFi or wired).
*  See tutorial videos on our [Support page](https://opensprinkler.com/support/).

If you are installing a new system which has no labeled wires to begin with, note that each valve has two wires: one to **COM** (common), the other to a zone terminal.

---

**Q: Is the controller waterproof?**
<br>
No. For outdoor installs, use a waterproof enclosure such as [this one](https://www.amazon.com/Orbit-57095-Weather-Resistant-Outdoor-Mounted-Controller/dp/B000VYGMF2).

---
<a id="supported-valves"></a>
**Q: What valves are supported?**
<br>

* **AC-powered (including OSPi):** Standard 24V AC sprinkler valves, motorized ball valves (rated for 24VAC), pump start relays, and wireless sensors that use 24VAC.
* **DC-powered:** Standard 24VAC sprinkler valves, DC **non-latching** valves, motorized ball valves (rated for DC), and DC solid state relays.
* **Latch OpenSprinkler:** Latching solenoid valves only, which typically have two wires with distinct colors (e.g. black and red). 

---

**Q: Are motorized ball valves supported?**
<br>
Yes. The '2-wire auto return' and '2-wire 9-24V AC/DC' types are directly compatible. The '3-wire 9-24V AC/DC' type is also compatible, but each valve will take 2 zones (for open and close).

---

**Q: Does it support master / pump stations?**
<br>
Yes: **two** independent, software-defined master/pump zones. Any station can be a master.

---

**Q: Can I connect sprinkler valves with garden hoses?**
<br>
Yes. Use **NPT** (National Pipe Thread) to **GHT** (Garden Hose Thread) adapter. 

---

**Q: Can I put two wires in one zone port?**
<br>
Yes, but keep in mind that those two zones will always operate together.

---

**Q: What sensors are supported?**
<br>
OpenSprinkler has two sensor ports. Each can be independently configured as a:

* **Rain** sensor (normally open or normally closed)
* **Flow** sensor (two-wire dry-contact; and some three-wire types)
* **Digital soil moisture** sensor (outputs binary signal)
* **Program start button**

---

**Q: Do you support analog soil sensors?** 
<br>
Not yet. OpenSprinkler currently only accepts sensors that output a digital (high or low) signal. Development is underway to enable analog sensors. Meanwhile, you can use our [Analog-to-Digital (A2D) adapter](https://opensprinkler.com/product/a2dadapter/) to digitize an analog sensor to on/off signal.

---

**Q: What is the maximum distance between the controller and valves?**
Depends on the wire gauge (AWG) you use.

* **20 AWG:** Up to 200 m (700 ft)
* **18 AWG:** Up to 300 m (1000 ft)
* **16 AWG:** Up to 450 m (1500 ft)

---

**Q: What is the maximum distance between the main controller and extension boards?**
<br>
Depends on the wire gauge of the extension cable. Default expander cable is 24 AWG at 15 in; longer runs possible with custom cabling (e.g. by using an Ethernet cable).

---

**Q: What are the LCD and buttons for?**
<br>
LCD shows time as well as zone/controller/sensor status. Buttons can show IP, perform factory reset, and start programs manually.

---

**Q: How do I check its IP address?**
<br>
Press button `B1` to display the IP. 

---

**Q: How do I start a program using buttons?**
<br>
Press and hold button `B3` until “Start a Program” appears on the LCD, then follow prompts.

---

**Q: What happens if I lose power?**
<br>
Programs and settings are stored in non-volatile (flash) memory and are preserved during outages. 

---

**Q: Can I run multiple zones simultaneously (concurrently)?**
<br>
Yes. You can assign zones to the **Parallel (P)** group, which allow them to run at the same time as other zones. The maximum number of simultaneous zones is not limited by software, but rather depends on the power capacity of your adapter and the current draw of your valves. Up to `8` have been tested internally. We generally recommend no more than `4`.

---

**Q: Can OpenSprinkler switch other devices?**
<br>
Yes. The firmware supports GPIO/HTTP(S)/RF stations. This allows it to toggle a GPIO pin, send an HTTP(S) command, or talk to RF remote power sockets, which can switch devices like lights, heaters, pumps, and fans. You can also use 24VAC relays to allow switching other devices.

---

**Q: Does it have overcurrent / undercurrent detections?**
<br>
Yes. Current firmware supports an adjustable overcurrent limit, which stops zones from running if the current draw exceeds the limit. It also supports undercurrent alert. Both help detect faulty valves and wiring.

<hr class="double">

### Web Connections and Integration

**Q: Does it have built-in wireless? What about wired Ethernet?**
<br>
Yes, OpenSprinkler v3.x includes WiFi (ESP8266). A wired Ethernet module is an optional add-on. OpenSprinkler Pi's network options depend on your Raspberry Pi.

---

**Q: Do I need a WiFi router?**
<br>
Typically you connect the controller to a router (WiFi or Ethernet). However, without a router, OS v3.x can run in **AP mode** (acts as its own hotspot). Internet-dependent features (like weather) won’t work in pure AP mode.

---

**Q: How do I access OpenSprinkler?**
<br>
OpenSprinkler has a built-in web interface that works with any modern desktop or mobile browser. Simply type the controller's IP address into your browser. We also provide a free OpenSprinkler mobile app (for iOS, Android, and macOS).

For remote access, simply enable the **OTC (OpenThings Cloud)** token in the settings.

---

**Q: Can OpenSprinkler run offline without Internet?**
<br>
Yes. Once programmed, OpenSprinkler runs all schedules offline. The controller has a built-in real-time clock and battery for time-keeping. You can also use the on-board buttons to run programs manually.

---

**Q: Do you support push notifications?**
<br>
Yes. Current firmware supports notifications via Email, MQTT and IFTTT (see [support docs](https://openthings.freshdesk.com/solution/folders/5000099525)).

---

**Q: Can multiple OpenSprinklers talk to each other?**
<br>
Yes. The firmware supports "Remote Stations", a feature that allows one OpenSprinkler to act as a master controller, sending commands to other controllers to open or close their valves.

<hr class="double">

### Technical

**Q: What are the differences between the product versions?**
<br>
OpenSprinkler v3.x is fully assembled and works out of the box. OpenSprinkler Pi (OSPi) is based on Raspberry Pi (RPi) and requires exprience with RPi and basic Linux commands. See detailed comparisons below:

|     | OS v3.x AC/DC/Latch | OSPi | OSBee |
|:----|:-------------------|:------------------------|:--------------------------|
|*Valve Compatibility*|**AC/DC** models both operate 24VAC valves. **DC** additionally support DC non-latching. **Latch** only supports DC latching valves.|24VAC valves only|DC Latching valves only|
|*Power Source*|**AC** model works with 24VAC adapter only; **DC/Latch** work with DC (6-24V) adapter.|24VAC only|USB|
|*Number of Stations*|`8` on main controller, expandable to `72` by linking expanders|`8` on main controller, expandable to `200`.|`3`, not expandable|
|*Processor*|ESP8266|RPi (user-supplied)|ESP8266|
|*Connectivity*|WiFi 2.4GHz (and optional wired Ethernet)|RPi's connectivity|WiFi 2.4GHz only|
|*Assembly*|Fully assembled commercial product in injection-molded enclosure|User provides RPi, SD card, and 3D printed enclosure|Fully assembled in 3D printed enclosure|
|*Weather*|Yes|Yes|No|
|*Physical Interface*|128x64 LCD, 3 buttons|128x64 LCD, 3 buttons|128x64 LCD, 1 button|
|*Target*|Everyone|RPi enthusiasts, tinkerers|Small garden projects|
|*Price*|~$150 USD|~$70 USD (plus the cost of RPi and accessories)|$62 USD|
|*Power Draw*|0.5-0.9 W|0.5 W + RPi's power|0.5 W|
|*Dimensions*|**v3.0-3.3**: 140×56×33 mm<br>**v3.4**: 125×79×25 mm|135×105×38 mm|65×65×20 mm|

---

**Q: How do I upgrade the firmware?**

* **OS v3.x:** Over-the-Air (OTA) update (via either WiFi or wired Ethernet).
* **OSPi:** Network or script-based firmware updates.

---

**Q: Will firmware update erase my settings?**

* **Dotted Version Updates**, e.g. `2.2.0` → `2.2.1`, automatically trigger a factory reset and erase all settings.
* **Build Number Updates**, e.g. `2.2.1(2)` → `2.2.1(4)` preserve all settings.

!!! info "Back Up Before Updating"
    Before any update, back up the current configurations so you can restore quickly if needed.

---

**Q: How can I set a static IP?**
<br>
The recommended way is to use your router's **DHCP Reservation** feature (sometimes called "Bind IP to MAC"). Alternatively, you can turn off the DHCP option on OpenSprinkler and manually set a static IP and gateway (router) IP.

---

**Q: How do I factory reset?**

* **OS v3.x:** Power cycle; when the OpenSprinkler logo appears, hold button `B1` until **Reset All?** shows; confirm Yes, then hold `B3` until the controller restarts.
* **OSPi:** Stop the OpenSprinkler process; go to the folder the firmware is installed, delete the file named `done.dat`; then restart the process.

---

**Q: How can I help with translations / multi-language support?**
<br>
OpenSprinkler's language localization is crowd-sourced; open the **About** page in the web UI to find the localization link.

---

**Q: Can I build my own app / home assistant integration?**
<br>
Absolutely. Please refer to the [OpenSprinkler Firmware API doc](https://opensprinkler.github.io/OpenSprinkler-Firmware/2.2.1/221_4_api/). Note that [Home Assistant integration](https://opensprinkler.com/forums/topic/home-assistant-integration/) for OpenSprinkler already exists, created by third-party developers.

---
