# Split-Flap

[![Build](https://github.com/Tortoc/split-flap/actions/workflows/build.yml/badge.svg)](https://github.com/Tortoc/split-flap/actions/workflows/build.yml)

![Split Flap Display](./Images/Split-Flap.jpg)

This repository is a fork of [scientress/split-flap](https://github.com/scientress/split-flap),
which builds on [JonnyBooker/split-flap](https://github.com/JonnyBooker/split-flap), itself a
fork of the brilliant [Split Flap Project](https://github.com/Dave19171/split-flap) by
[David Königsmann](https://github.com/Dave19171). None of this would have been possible without
the great foundations they have put in place.

### What this fork changes

- Runs on the fae-pcb (ATmega328P directly on the board, ESP8266 as ESP-12F module, 3.3 V); the original Arduino Nano/ESP01 build is no longer supported
- Unit firmware updates over I2C via a patched [twiboot](https://github.com/orempel/twiboot) bootloader
- Unit calibration and automatic unit detection in the web UI
- Login with per-category access control (salted PBKDF2 password hash)
- Multiple displays working as one (extend or mirror)
- Many stability fixes for the ESP8266's tight heap and network stack

This project has built on the original project to add extra features such as:

- Message Splitting
  - If a message is longer than the number of units there are, the message will be split up and displayed in sequence with a delay between each message
  - Also messages can be split up by adding a `\n`
- Countdown Mode
  - Countdown in days to a specified date
- Added ability to setup WiFi connection on device
  - The device will set itself up as an Access Point (AP) on first start. You will be able to connect to this network and a web portal will be provided where you can setup the WiFi network you want to connect to. The network (`Split-Flap-AP`) is protected by a random 8-character password that the display shows while the setup portal is open (a display with fewer than 8 units shows it in parts, over and over, each round starting after a row of dashes)
  - Alternatively a fixed hotspot password (8-63 characters) can be set in the web UI (**☰ Menu > System > Network**); the display then stays blank while the portal is open. If it is forgotten, the serial log prints it when the hotspot opens
  - If the device was to lose connection, it should retry and if all else fails, it will open up the web portal again to change the WiFi settings if necessary
  - Alternatively the WiFi credentials can be compiled in (direct mode, see [Updating Settings of the Sketch](#updating-settings-of-the-sketch))
- Reworked UI, available in English and German (pick one when flashing or uploading the web UI)
  - Can see messages scheduled to be displayed and option to remove them
  - Loading indicators
  - Show hide information to fill out for the mode you select
  - See extra information on the UI such as:
    - Last Message Received
    - Number of Flaps registered
    - Version Number running
    - How many characters/lines are in the textbox for text
    - Add newline button (as typing `\n` is a pain on a mobile keyboard)
- Message Scheduling
  - Ability to send a message and display it at a later date.
  - Options for when a scheduled message is shown
    - If the clock was in another mode, such as `Clock` mode, it will show the message for a duration (changeable via updating `scheduledMessageDisplayTimeMillis` in `ESPMaster.ino`), then return to that mode afterwards
    - A checkbox on the UI is presented ("Show indefinitely") that when checked, will show a message and leave it on the display
- Over the Air updates
  - ESPMaster firmware upload from the browser (**☰ Menu > Firmware update**), and `Unit.ino` updates pushed to each unit over I2C (see [Update Unit Firmware over I2C](#update-unit-firmware-over-i2c-optional))
- Further modes and features added since
  - Clock, Date and Count-up modes, plus a Rotation mode cycling through texts and the other modes
  - Quiet hours, crash log, time zone and network settings in the web UI
  - Multiple displays working as one (extend or mirror), see [Multiple Displays](#multiple-displays)
  - Login and per-category access control, see [Security](#security)
  - Optional MQTT / Home Assistant integration (`MQTT_ENABLE`, off by default)
  - Every HTTP endpoint is documented in [`ESPMaster/API.md`](./ESPMaster/API.md)

Also the code has been refactored to facilitate easier development:

- Changed serial prints to one central location so you don't have to declare serial enable checks when a new one is required
- Renamed files and functions
- Updated `data` so you can test the project locally without having to call a webserver via a `localDevelopment` flag

Hardware:

- PCB: [fae-pcb](https://github.com/codingcatgirl/split-flap-fae-pcb)
- 3D-printed parts: the original [Split-Flap Display](https://www.printables.com/model/69464-split-flap-display) models by David Königsmann on Printables, plus the [Extension parts and alignment notch mod](https://www.printables.com/model/847231-extension-parts-and-alignment-notch-mod-for-split)
- Mechanical assembly: the original [instruction manual](https://github.com/Dave19171/split-flap/blob/master/Instructions/SplitFlapInstructions.pdf) (its PCB and Arduino Nano/ESP01 parts don't apply to the fae-pcb)

## General

Every unit is a fae-pcb with an ATmega328P running `Unit.ino`. One of the boards additionally carries an ESP-12F (ESP8266) module running `ESPMaster.ino`: it is the main hub, serves the web interface and talks to up to 64 units (`UNITS_AMOUNT_MAX`) via I2C. Each unit is responsible for setting the zero position of its drum on startup and displaying any letter the main hub sends its way. The whole assembly runs on 3.3 V logic.

### PCB

Gerber, BOM and pick-and-place files, the KiCad project and the assembly notes are in the [fae-pcb repository](https://github.com/codingcatgirl/split-flap-fae-pcb). The boards are made for JLCPCB's SMT assembly service. You need one board per unit; only one of them gets the ESP-12F module.

### Unit

Each unit's ATmega328P drives a 28BYJ-48 stepper motor. The drum with the flaps is homed with a hall sensor and a magnet mounted to the drum.

The ATmega328P sits directly on the board (no Arduino Nano, no USB port), so `Unit.ino` is flashed through the board's ISP header with an ISP programmer. Build it with `arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old Unit` (the profile matches the board's 16 MHz crystal) or take `Unit.hex` from the latest `unit-v…` [release](https://github.com/Tortoc/split-flap/releases), see [`firmware/README.md`](./firmware/README.md). Once the twiboot bootloader is installed (see [Update Unit Firmware over I2C](#update-unit-firmware-over-i2c-optional)), later updates go through the web UI. The zero position offset (see [Set Zero Position Offset](#set-zero-position-offset) below) is set afterwards from the ESPMaster web UI.

Inside `Unit.ino`, there is a setting for testing the units so that a few letters are cycled through to ensure what is shown is what you expect. At the top of the file once you have opened the project, you will find two lines that are commented out:

```c++
//#define SERIAL_ENABLE // uncomment for serial debug communication
//#define TEST_ENABLE   // uncomment for Test mode. Rotates through a few character to make sure unit is working. ...
```

Remove the comment characters to help with your testing for the next step of Setting the Zero Position Offset.

#### Set Zero Position Offset

The zero position (or blank flaps position in this case) is attained by driving the stepper to the hall sensor and step a few steps forward. This offset is individual to every unit and is saved to the ATmega328P's EEPROM.

Once `Unit.ino` and `ESPMaster.ino` are running and wired up over I2C, open the ESPMaster web UI and go to the **☰ Menu > Units**. This page reads the live offset from every connected unit and lets you set a new one per unit directly from the browser.

A good starting value is usually around **100**, but yours may vary. Enter a value and hit "Save & recalibrate" - the unit will immediately do a full rotation to re-home using the new offset, and the page will show you the confirmed value once it's done. Repeat until the blank flap is showing exactly right every time the unit homes.

#### Set Unit Address

Every unit's I2C address is set with the address jumpers on J4. They need to be set ascending from zero in binary.

**Jumper Configuration:**
- J4 has 8 pin pairs. From the bottom up: A0 = bit 0 (value 1), A1 = bit 1 (value 2), A2 = bit 2 (value 4), A3 = bit 3 (value 8), D3 = bit 4 (value 16), D4 = bit 5 (value 32)
- The top two pairs (D5, D6, nearest the ISP header) are unused
- **Jumper set** = 1 in binary, **no jumper** = 0
- Address = sum of the values of all set jumpers (0-63)

**Table for up to 10 Units:**
| Unit | I2C Address | Jumpers set | Binary |
| --- | --- | --- | --- |
| Unit 1 | 0 | none | 000000 |
| Unit 2 | 1 | A0 | 000001 |
| Unit 3 | 2 | A1 | 000010 |
| Unit 4 | 3 | A0, A1 | 000011 |
| Unit 5 | 4 | A2 | 000100 |
| Unit 6 | 5 | A0, A2 | 000101 |
| Unit 7 | 6 | A1, A2 | 000110 |
| Unit 8 | 7 | A0, A1, A2 | 000111 |
| Unit 9 | 8 | A3 | 001000 |
| Unit 10 | 9 | A0, A3 | 001001 |

See the comment above `addressPins` in `Unit.ino` for the pin mapping.

**Important Notes:**
- Each unit must have a **unique address** (0, 1, 2, 3, etc.)
- Addresses must be **sequential starting from 0**
- If you have 3 units, use addresses 0, 1, and 2
- ESPMaster auto-detects how many units are connected at startup - see [Automatic Unit Detection](#automatic-unit-detection) below
- After setting jumpers, power cycle the unit for the new address to take effect
- You can check which units were found on the **☰ Menu > Units** page ("Rescan units" runs the scan again)

#### Automatic Unit Detection

ESPMaster no longer needs to be told how many units are connected. On every startup it scans I2C
addresses 0-63 and counts how many respond **sequentially starting from address 0**, stopping at
the first gap - matching the address requirement above. That count becomes the number of units
the rest of the system (display width, message splitting/alignment, the Units page's unit list,
etc.) uses.

If you connect or fix a unit after ESPMaster has already booted, you don't need to reboot it: hit
"Rescan units" on the ESPMaster web UI's **☰ Menu > Units** page to re-run the scan immediately.

If zero units are detected (wiring issue, or address 0 isn't assigned to anything), the display
functionality is simply unavailable until that's fixed - the web UI, WiFi and Units page keep
working normally so you can diagnose and rescan without needing physical access to the ESP.

#### Update Unit Firmware over I2C (optional)

Once a unit is set up, future `Unit.ino` changes can be pushed to it over I2C from the ESPMaster
web UI instead of connecting an ISP programmer every time. This requires a **one-time, physical**
setup step per unit: flashing a bootloader onto it via an ISP programmer. We use a lightly
patched fork of [twiboot](https://github.com/orempel/twiboot) (vendored in
[`Unit/bootloader/twiboot`](./Unit/bootloader/twiboot)) that reads the unit's address jumpers at
boot, so the same jumpers that set a unit's normal I2C address also work for firmware
updates - one compiled bootloader image for every unit, and moving jumpers later doesn't require
reflashing the bootloader. Full instructions, including the exact build/flash commands and a
mandatory bench-test procedure, are in [`Unit/BOOTLOADER_SETUP.md`](./Unit/BOOTLOADER_SETUP.md).

> ⚠️ This touches already-built hardware and needs an ISP programmer (USBasp, AVRISP mkII, or a
> second Arduino as ISP). It's entirely optional - skip it and keep flashing `Unit.ino` over ISP
> if you'd rather not set it up. `UNIT_BOOTLOADER_ENABLE` in `ESPMaster.ino` gates this
> feature and is `true` by default, because ESPMaster also uses twiboot to reboot a unit that
> stopped responding. Set it to `false` if your units don't run twiboot.

Once a unit has twiboot installed, updating it going forward is done entirely from the browser:

1. Take `Unit.bin` from the latest `unit-v…` [release](https://github.com/Tortoc/split-flap/releases), or build `Unit.ino` yourself and
   convert the `.hex` to a raw binary (twiboot writes raw flash images, not Intel HEX):
   ```
   arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old Unit --output-dir build
   avr-objcopy -I ihex -O binary build/Unit.ino.hex build/Unit.ino.bin
   ```
   (`avr-objcopy` ships with the Arduino AVR toolchain.)
2. On the ESPMaster web UI's **☰ Menu > Firmware update > Units** tab, upload the `.bin`
   and pick the target unit, then hit "Start update".

The unit reboots into its bootloader, the new firmware is written and verified page-by-page, and
only once everything checks out does the unit switch over to running it. There is no rollback: the
pages are overwritten in place (the ATmega328P has no room for a second image). If an update is
aborted once writing has started, an EEPROM marker keeps the unit in its bootloader instead of
starting a half-written firmware. Such a unit is not detected by the unit scan (nor are the units
after it), so choose "Other unit number..." on the Units tab, enter its number and run the update
again. If the unit doesn't answer in bootloader mode at all, nothing is written and the old
firmware keeps running.

### ESP8266

#### Pre-requisites

The recommended toolchain is [arduino-cli](https://arduino.github.io/arduino-cli/). Install the cores and the libraries in the versions the [CI build](./.github/workflows/build.yml) uses (`arduino:avr` and `Stepper` are for `Unit.ino`; `--no-deps` keeps the dependencies at these versions too):

```bash
arduino-cli core install esp8266:esp8266@3.1.2 arduino:avr@1.8.8 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install --no-deps "ArduinoJson@7.0.4" "ESPAsyncWebSrv@1.2.7" "ESPAsyncTCP@1.2.4" "NTPClient@3.2.1" \
  "ezTime@0.8.3" "LinkedList@1.3.3" "WiFiManager@2.0.17" "PubSubClient@2.8" "Stepper@1.1.3"
```

- [ArduinoJSON](https://github.com/bblanchon/ArduinoJson) - Version: 7.0.4
- [ESPAsyncWebSrv](https://github.com/dvarrel/ESPAsyncWebSrv) - Version: 1.2.7, with its dependency [ESPAsyncTCP](https://github.com/dvarrel/ESPAsyncTCP) - Version: 1.2.4
- [NTPClient](https://github.com/arduino-libraries/NTPClient) - Version: 3.2.1
- [ezTime](https://github.com/ropg/ezTime) - Version: 0.8.3
- [LinkedList](https://github.com/ivanseidel/LinkedList) - Version: 1.3.3
- [WiFiManager](https://github.com/tzapu/WiFiManager) - Version: 2.0.17
- [PubSubClient](https://github.com/knolleary/pubsubclient) - Version: 2.8.0 (MQTT support -
  only linked in if `MQTT_ENABLE` in `ESPMaster.ino` is `true`; off by default)
- [Stepper](https://github.com/arduino-libraries/Stepper) - Version: 1.1.3 (`Unit.ino` only)

The Arduino IDE works as well: add the board manager URL above, install the ESP8266 core and the same libraries through the Library Manager.

The ESP-12F module is flashed through the board's serial header (J8) with a 3.3 V USB-serial adapter (e.g. FTDI FT232R). If an upload fails partway through, `esptool.py --no-stub` is more robust than the default stub-based upload.

#### Building the ESPMaster Sketch

The sketch needs the flash layout `eesz=4M1M` (4 MB, 1 MB LittleFS), `mmu=3232` and the linker
flags `-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept`. Build it with
[`firmware/compile-espmaster.sh`](./firmware/compile-espmaster.sh), which sets all of them
(`firmware/compile-espmaster.sh --output-dir build` also writes the `.bin`), or flash the prebuilt
binaries from the latest `esp-v…` [release](https://github.com/Tortoc/split-flap/releases) - [`firmware/README.md`](./firmware/README.md) explains the
options and the flash addresses (sketch at `0x0`, `littlefs-<lang>.bin` at `0x300000`). A build without
`eesz=4M1M` compiles fine but can't find its settings and breaks OTA updates.

**Arduino IDE (alternative):** choose the board "Generic ESP8266 Module", **Flash Size** "4MB
(FS:1MB OTA:~1019KB)" and **MMU** "32KB cache + 32KB IRAM (balanced)". The IDE has no setting
for the linker flags: put them into a `platform.local.txt` next to the core's `platform.txt`
(e.g. `~/.arduino15/packages/esp8266/hardware/esp8266/3.1.2/`):

```
compiler.c.elf.extra_flags=-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept
```

This applies to every ESP8266 sketch built with that core installation. Without the flags the
build fails with "undefined reference to `__real__Znwj`".

#### Uploading the Static Assets via LittleFS

There are static files located [here](./ESPMaster/data/) in the `data` folder of ESPMaster which will need to be uploaded. These make up the website that will be accessible on your WiFi so you can update the Split-Flap display.

The web UI comes in **English** and **German** - pick one by choosing the matching image. Take `littlefs-en.bin` or `littlefs-de.bin` from the latest `esp-v…` [release](https://github.com/Tortoc/split-flap/releases), or build both with `firmware/build-webui.py` (see [`firmware/README.md`](./firmware/README.md#web-ui-littlefsbin)), and flash it at address `0x300000`:

```bash
esptool.py --port /dev/ttyUSB0 write_flash 0x300000 littlefs-en.bin
```

To switch the language later without a cable, upload the other page with `POST /api/web-file` (see [`ESPMaster/API.md`](./ESPMaster/API.md#post-apiweb-file)); the settings stay as they are.

**Arduino IDE (alternative):** the LittleFS upload plugin ([arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) for IDE 2.x, [arduino-esp8266littlefs-plugin](https://github.com/earlephilhower/arduino-esp8266littlefs-plugin) for IDE 1.x) uploads the `data` folder directly, which gives the English UI. Choose the flash size "4MB (FS:1MB OTA:~1019KB)" first, otherwise the files land where the firmware doesn't look.

**NOTE:** No sketch has been uploaded yet! Only the static files.

#### Updating Settings of the Sketch

There are several options in the Sketch you can modify to customise or change the behaviour of the display: the defines and settings at the top of `ESPMaster.ino` (see the feature flag table in [`ESPMaster/README.md`](./ESPMaster/README.md#feature-flags)).

By default, the system will run in an "Access Point" mode where you will be able to connect to the display and put in WiFi credentials directly. This means if your WiFi changes, you don't have to re-upload a new sketch. Screenshot of the WiFi setup portal:

![Screenshot WiFi Portal](./Images/Access-Point-Screenshot.jpg)

Alternatively, you can specify credentials directly. You can go ahead and change the credentials in these variables:

```c++
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";
```

You will also need to change the WiFi Mode in the code via changing this variable to `true`:

```c++
#define WIFI_USE_DIRECT     false  //false = AP config portal (WiFiManager), true = direct connect
```

The time zone is set in the web UI (**☰ Menu > System > Time zone**, default `Europe/Berlin`), using the TZ database names: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

You can also modify the date and clock format easily by using this table: https://github.com/ropg/ezTime#datetime

There are several helper `define` variables to help during debugging/running:

- **SERIAL_ENABLE**
  - Use this to enable Serial output lines for tracking executing code
- **WEB_FIRMWARE_UPDATE_ENABLE**
  - Enabled by default. Adds a browser-based firmware upload to the web UI's **☰ Menu > Firmware update** page - no Arduino IDE or USB cable required
  - To use it: build the `.bin` (see [Building the ESPMaster Sketch](#building-the-espmaster-sketch)) or take `ESPMaster.bin` from the latest `esp-v…` release, then upload that file via the ESPMaster tab on the Firmware update page
  - Needs the admin password (see [Security](#security))
  - Only updates the sketch itself, not the static web files - the web UI is replaced with `POST /api/web-file` (`index-en.html.gz` or `index-de.html.gz`, see [`ESPMaster/API.md`](./ESPMaster/API.md)) or over USB with `littlefs-en.bin`/`littlefs-de.bin`
  - If the upload fails partway through, the currently running firmware stays active - the device only reboots into the new firmware once the upload has fully succeeded
- **UNIT_CALLS_DISABLE**
  - Use this to disable the communication with the units. This will mean you can check code over function for the ESP module.

#### Static IP Address

A feature request of being able to set a Static IP Address was created by [beroliv](https://github.com/beroliv) (thank you for the suggestion). This was to get around issues whereby in some routers, there were issues in being able to do this.

Code has been added to be able to set a Static IP Address on device. To do this:

1. Set the `WIFI_STATIC_IP` variable to `true` (defaulted to `false`)
2. Update the following settings as necessary for your needs:

   ```c++
   IPAddress wifiDeviceStaticIp(192, 168, 1, 50);

   IPAddress wifiRouterGateway(192, 168, 1, 1);
   IPAddress wifiSubnet(255, 255, 0, 0);

   IPAddress wifiPrimaryDns(8, 8, 8, 8);
   ```

**Suggestion:** Set your device up with a Static IP via your router if possible and to avoid conflicts on your network, however feel free to run this code if you are not able to. Testing this functionality showed it does work in both AP/Direct WiFi modes.

#### Sketch Upload

So far we've only uploaded static files to the ESP8266. You now need to upload the sketch: build it as described in [Building the ESPMaster Sketch](#building-the-espmaster-sketch) and flash it with `arduino-cli upload`, `esptool.py` or the IDE's Upload button (or flash `ESPMaster.bin` from the latest `esp-v…` release at `0x0`), and you are done. Later updates can go through the browser instead. Then navigate to the IP-address the ESP8266 is getting assigned from your router.

### Security

- The web UI asks for the admin password; scripts send it as the `X-OTA-Password` header. The factory default is `splitflap` - change it in **☰ Menu > System > Access control** (8 characters minimum). Individual categories can be opened up without a password there, and an empty password switches protection off entirely.
- Since ESPMaster 5.0.0 the password is stored only as a salted PBKDF2-SHA256 hash. Flashing a firmware older than 5.0.0 afterwards falls back to `splitflap`.
- The ESP8266 can't do TLS: the password and session cookie travel unencrypted over your network. Keep the display in a network you trust.
- The device only answers requests addressed to its IP address, its mDNS name (with or without `.local`) or the router DNS name entered in **☰ Menu > System > Network**.

### Multiple Displays

Several displays in the same network can work as one (**☰ Menu > System > Multi-device group**): one is the **primary**, the others **extensions**. The primary either spreads its text across all of them (Extend) or has each show the same characters (Mirror).

1. On each extension, set the role to "Extension device".
2. On the primary, set the role to "Primary device", enter the extensions' IP addresses or host names and save.
3. Pair each extension once from the primary, with that extension's admin password. From then on the primary signs every update instead of sending a password; the devices' passwords may differ. Since 5.0.0 a primary older than 5.0.0 can't drive a password-protected extension any more.

Give every display its own mDNS name (**☰ Menu > System > Network**) so the `.local` names stay unique.

### Common Problems

- If the ESP is not talking to the units correctly, check that unit addresses are sequential starting from 0 with no gaps (see [Automatic Unit Detection](#automatic-unit-detection) - a gap stops the count early) and use "Rescan units" on the **☰ Menu > Units** page after fixing anything.
- Ensure you upload the sketch and the LittleFS sketch upload to the ESP8266.
- When the system is powered, your Hall Sensor should only light up when a magnet is nearby.

## License

This project is licensed under the GNU General Public License v3.0, see [`LICENSE`](./LICENSE).

The patched twiboot bootloader in [`Unit/bootloader/twiboot`](./Unit/bootloader/twiboot) is a
separate program under the GNU General Public License v2, see its own
[`LICENSE`](./Unit/bootloader/twiboot/LICENSE). The PCB design is not part of this repository;
see the [fae-pcb repository](https://github.com/codingcatgirl/split-flap-fae-pcb) for its license.
