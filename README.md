# Split-Flap

[![Build ESP Master Sketch](https://github.com/JonnyBooker/split-flap/actions/workflows/build-esp-master.yml/badge.svg)](https://github.com/JonnyBooker/split-flap/actions/workflows/build-esp-master.yml) [![Build Arduino Unit Sketch](https://github.com/JonnyBooker/split-flap/actions/workflows/build-unit.yml/badge.svg)](https://github.com/JonnyBooker/split-flap/actions/workflows/build-unit.yml)

![Split Flap Display](./Images/Split-Flap.jpg)

This project has been forked from the brilliant [Split Flap Project](https://github.com/Dave19171/split-flap) by [David Königsmann](https://github.com/Dave19171). None of this would have been possible without the great foundations that have been put in place.

This project has built on the original project to add extra features such as:

- Message Splittng
  - If a message is longer then the number of units there are, the message will be split up and displayed in sequence with a delay between each message
  - Also messages can be split up by adding a `\n`
- Countdown Mode
  - Countdown in days to a specified date
- Added ability to setup WiFi connection on device
  - The device will set itself up as a Access Point (AP) on first start. You will be able to connect to this network and a web portal will be provided where you can setup the WiFi network you want to connect to
  - If the device was to lose connection, it should retry and if all else fails, it will open up the web portal again to change the WiFi settings if necessary
  - Option to be able use direct mode is also possible as previously implemented
- Reworked UI
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
  - Options for when a scheduled message being shown
    - If the clock was in another mode, such as `Clock` mode, it will show the message for a duration (changable via updating `scheduledMessageDisplayTimeMillis` in `ESPMaster.ino`), then return to that mode afterwards
    - A checkbox on the UI is presented ("Show Indefinitely") that when checked, will show a message and leave it on the display
- Arduino OTA
  - Over the Air updates to the display
- Updated `README.md` to add scenarios of problems encountered

Also the code has been refactored to try facilitate easier development:

- Changed serial prints to one central location so don't have to declare serial enable checks when a new one is required
- Renamed files and functions
- Ping endpoint
- Updated `data` so can test out project locally without having to call a webserver via a `localDevelopment` flag

3D-files here on [Printables](https://www.prusaprinters.org/prints/69464-split-flap-display)!

## General

The display's electronics use 1 x ESP01 (ESP8266) as the main hub and up to 16 Arduinos as receivers. The ESP handles the web interface and communicates to the units via I2C. Each unit is resposible for setting the zero position of the drum on startup and displaying any letter the main hub send its way.

Assemble everything according to the instruction manual which you can find on [GitHub](./Instructions/SplitFlapInstructions.pdf).

### PCB

Gerber files are in the `PCB` folder. These are the scehematics for the PCB boards and say, what is needed and where. You need one per unit. Populate components according to the [instruction manual](./Instructions/SplitFlapInstructions.pdf).

Options to potentially get boards created for you:

- [PCB Way](https://www.pcbway.com/)
- [JLC PCB](https://jlcpcb.com/)

> Note: Services are offered by these companies to assembly the boards for you. There are surface mounted components to these devices that you might not be able to do yourself like small resistors for instance, which must be flow soldered. It could be worth having the company do this aspect for you.

### Unit

Each split-flap unit consists of an Arduino Nano mounted on a custom PCB. It controls a 28BYJ-48 stepper motor via a ULN2003 driver chip. The drum with the flaps is homed with a KY003 hall sensor and a magnet mounted to the drum.

Upload the Arduino sketch `Unit.ino` in the unit folder to each unit's arduino nano. The zero position offset (see [Set Zero Position Offset](#set-zero-position-offset) below) is now set afterwards from the ESPMaster web UI, so there is no separate offset sketch to upload first.

Inside `Unit.ino`, there is a setting for testing the units so that a few letters are cycled through to ensure what is shown is what you expect. At the top of the file once you have opened the project, you will find a line that is commented out:

```c++
#define SERIAL_ENABLE   // uncomment for serial debug communication
#define TEST_ENABLE    	// uncomment for test mode where the unit will cycle a series of test letters.
```

> Note: If you experience any problems uploading the unit sketch, you may have to change your `Processor` to use the old bootloader, called `ATmega328p (Old Bootloader)`.

Remove the comment characters to help with your testing for the next step of Setting the Zero Position Offset.

#### Set Zero Position Offset

The zero position (or blank flaps position in this case) is attained by driving the stepper to the hall sensor and step a few steps forward. This offset is individual to every unit and needs to be saved to the arduino nano's EEPROM.

This used to require a separate `EEPROM_Write_Offset.ino` sketch uploaded over USB per unit. That sketch has been removed - instead, once `Unit.ino` and `ESPMaster.ino` are uploaded and wired up over I2C, open the ESPMaster web UI and go to the **⋮ menu > Units**. This page reads the live offset from every connected unit and lets you set a new one per unit directly from the browser.

A good starting value is usually around **100**, but yours may vary. Enter a value and hit "Save & Recalibrate" - the unit will immediately do a full rotation to re-home using the new offset, and the page will show you the confirmed value once it's done. Repeat until the blank flap is showing exactly right every time the unit homes.

> ⚠️ **Breaking change / migration note:** Reading the offset back over I2C required extending the protocol between `Unit.ino` and `ESPMaster.ino`. Because of this, **both** sketches must be upgraded together - a unit still running an older `Unit.ino` will not work correctly with a newer `ESPMaster.ino` (and vice versa). If you already have units built and calibrated with the old `EEPROM_Write_Offset.ino` sketch, don't worry: a normal sketch upload does not erase the Arduino Nano's EEPROM, so your existing offsets are preserved and should show up correctly on the Units page as soon as `Unit.ino` is reflashed.

#### Set Unit Address

Every unit's address is set by a DIP switch. They need to be set ascending from zero in binary.

**DIP Switch Configuration:**
- The DIP switch has 4 switches (SW1, SW2, SW3, SW4) from left to right
- **SW1** (leftmost) = bit 3 (value 8)
- **SW2** = bit 2 (value 4)
- **SW3** = bit 1 (value 2)
- **SW4** (rightmost) = bit 0 (value 1)
- **Switch ON (up position)** = 1 in binary
- **Switch OFF (down position)** = 0 in binary
- Address = SW4×1 + SW3×2 + SW2×4 + SW1×8

**Visual Guide for 3 Units:**
```
Unit 1 (Address 0):  [DOWN] [DOWN] [DOWN] [DOWN]  = 0000
Unit 2 (Address 1):  [DOWN] [DOWN] [DOWN] [UP]    = 0001
Unit 3 (Address 2):  [DOWN] [DOWN] [UP] [DOWN]     = 0010
```

**Complete Table for up to 10 Units:**
| Unit | I2C Address | SW1 (left) | SW2 | SW3 | SW4 (right) | Binary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Unit 1 | 0 | OFF (down) | OFF (down) | OFF (down) | OFF (down) | 0000 | All switches down |
| Unit 2 | 1 | OFF (down) | OFF (down) | OFF (down) | **ON (up)** | 0001 | Rightmost switch up |
| Unit 3 | 2 | OFF (down) | OFF (down) | **ON (up)** | OFF (down) | 0010 | Second from right up |
| Unit 4 | 3 | OFF (down) | OFF (down) | **ON (up)** | **ON (up)** | 0011 | Last two switches up |
| Unit 5 | 4 | OFF (down) | **ON (up)** | OFF (down) | OFF (down) | 0100 | Second from left up |
| Unit 6 | 5 | OFF (down) | **ON (up)** | OFF (down) | **ON (up)** | 0101 | |
| Unit 7 | 6 | OFF (down) | **ON (up)** | **ON (up)** | OFF (down) | 0110 | |
| Unit 8 | 7 | OFF (down) | **ON (up)** | **ON (up)** | **ON (up)** | 0111 | |
| Unit 9 | 8 | **ON (up)** | OFF (down) | OFF (down) | OFF (down) | 1000 | Leftmost switch up |
| Unit 10 | 9 | **ON (up)** | OFF (down) | OFF (down) | **ON (up)** | 1001 | |

**Important Notes:**
- Each unit must have a **unique address** (0, 1, 2, 3, etc.)
- Addresses must be **sequential starting from 0**
- If you have 3 units, use addresses 0, 1, and 2
- ESPMaster auto-detects how many units are connected at startup - see [Automatic Unit Detection](#automatic-unit-detection) below
- After setting switches, power cycle the unit for the new address to take effect
- You can verify addresses using the I2C scan feature (see ESPMaster README for diagnostic tools)

#### Automatic Unit Detection

ESPMaster no longer needs to be told how many units are connected. On every startup it scans I2C
addresses 0-15 and counts how many respond **sequentially starting from address 0**, stopping at
the first gap - matching the DIP switch requirement above. That count becomes the number of units
the rest of the system (display width, message splitting/alignment, the Units page's unit list,
etc.) uses.

If you connect or fix a unit after ESPMaster has already booted, you don't need to reboot it: hit
"Rescan Units" on the ESPMaster web UI's **⋮ menu > Units** page to re-run the scan immediately.

If zero units are detected (wiring issue, or address 0 isn't assigned to anything), the display
functionality is simply unavailable until that's fixed - the web UI, WiFi and Units page keep
working normally so you can diagnose and rescan without needing physical access to the ESP.

#### Update Unit Firmware over I2C (optional)

Once a unit is set up, future `Unit.ino` changes can be pushed to it over I2C from the ESPMaster
web UI instead of plugging it into USB every time. This requires a **one-time, physical**
setup step per unit: flashing a bootloader onto it via an ISP programmer. We use a lightly
patched fork of [twiboot](https://github.com/orempel/twiboot) (vendored in
[`Unit/bootloader/twiboot`](./Unit/bootloader/twiboot)) that reads the unit's own DIP switches at
boot, so the same address jumpers that set a unit's normal I2C address also work for firmware
updates - one compiled bootloader image for every unit, and moving jumpers later doesn't require
reflashing the bootloader. Full instructions, including the exact build/flash commands and a
mandatory bench-test procedure, are in [`Unit/BOOTLOADER_SETUP.md`](./Unit/BOOTLOADER_SETUP.md).

> ⚠️ This touches already-built hardware and needs an ISP programmer (USBasp, AVRISP mkII, or a
> second Arduino as ISP). It's entirely optional - skip it and keep flashing `Unit.ino` over USB
> as before if you'd rather not set it up. `UNIT_BOOTLOADER_ENABLE` in `ESPMaster.ino` gates this
> feature; only enable it once at least one unit has been through the bench-test procedure in
> `BOOTLOADER_SETUP.md`.

Once a unit has twiboot installed, updating it going forward is done entirely from the browser:

1. In the Arduino IDE, `Sketch > Export compiled Binary` for `Unit.ino` to produce a `.hex` file.
2. Convert it to a raw binary (twiboot writes raw flash images, not Intel HEX):
   ```
   avr-objcopy -I ihex -O binary Unit.ino.hex Unit.ino.bin
   ```
   (`avr-objcopy` ships with the Arduino IDE's AVR toolchain.)
3. On the ESPMaster web UI's **⋮ menu > Firmware Update > Units** tab, upload `Unit.ino.bin`
   and pick the target unit, then hit "Update starten".

The unit reboots into its bootloader, the new firmware is written and verified page-by-page, and
only once everything checks out does the unit switch over to running it. If anything goes wrong
mid-update, the old firmware is left untouched (see `BOOTLOADER_SETUP.md` for why) and you can
just try again.

### ESP01/ESP8266

#### Pre-requisites

To upload the sketch to the ESP you need to install a few things to your arduino IDE.

- Install the ESP8266 board to your Arduino IDE. You can follow [this tutorial](https://randomnerdtutorials.com/how-to-install-esp8266-board-arduino-ide/)
- Install the arduino ESP8266 littleFS plugin to use the file system of the ESP, you can follow [this tutorial](https://randomnerdtutorials.com/install-esp8266-nodemcu-littlefs-arduino/)
- Install the following libraries via Library Manager:
  - [ArduinoJSON](https://github.com/bblanchon/ArduinoJson) - Version: 7.0.4
  - [ESPAsyncWebSrv](https://github.com/dvarrel/ESPAsyncWebSrv) - Version: 1.2.7
    - Dependencies which should be installed automatically:
      - [ESPAsyncTCP](https://github.com/dvarrel/ESPAsyncTCP)
      - [AsyncTCP](https://github.com/dvarrel/AsyncTCP)
  - [NTPClient](https://github.com/arduino-libraries/NTPClient) - Version: 3.2.1
  - [ezTime](https://github.com/ropg/ezTime) - Version: 0.8.3
  - [LinkedList](https://github.com/ivanseidel/LinkedList) - Version: 1.3.3
  - [WiFiManager](https://github.com/tzapu/WiFiManager) - Version: 2.0.17

To upload sketches to the ESP8266 you can either use an [Arduino Uno](https://create.arduino.cc/projecthub/pratikdesai/how-to-program-esp8266-esp-01-module-with-arduino-uno-598166) or you can buy a dedicated programmer. It is highly recommend getting a programmer as it makes uploading programs onto the ESP8266 much faster.

> Note: Be wary of ESP8266 programmers that are available which allow USB connection to your PC which may not have programming abilities. Typically extra switches are available so that the ESP8266 can be put in programming mode, although you can modify the programmer through a simple solder job to allow it to enter programming mode. Examples can be found in the customer reviews of [Amazon](https://www.amazon.co.uk/gp/product/B078J7LDLY/ref=ppx_yo_dt_b_search_asin_title?ie=UTF8&th=1).

> Alternatively, you can get a dedicated programmer from Amazon such as [this one](https://www.amazon.co.uk/dp/B083QHJW21). This is also available on [AliExpress](https://www.aliexpress.com/item/1005001793822720.html?spm=a2g0o.detail.0.0.48622aefV0Zv89&mp=1) if you are willing to wait a while for it.

#### Uploading the Static Assets via LittleFS

There are static files located [here](./ESPMaster/data/) in the `data` folder of ESPMaster which will need to be uploaded. These make up the website that will be accessible on your WiFi so you can update the Split-Flap display.

Open the sketch `ESPMaster.ino` in the `ESPMaster` folder, change your board to "Generic ESP8266 Module", choose the correct COM-port and click:

> Tools -> ESP8266 LittleFS Data Upload

This uploads the website onto the ESP8266's file system.

**NOTE:** No sketch has been uploaded yet! Only the static files. At the time of writing, this will also only work on an older version of Arduino IDE < version 2. The latest Arduino IDE broke support for Plugins such as the LittleFS plugin.

#### Updating Settings of the Sketch

There are several options in the Sketch you can modify to customise or change the behaviour of the display. These are marked in the code as "Configurable".

By default, the system will run in an "Access Point" mode where you will be able to connect to the display and put in WiFi credentials directly. This means if you WiFi changes, you don't have to re-upload a new sketch. Screenshot of the WiFi setup portal:

![Screenshot WiFi Portal](./Images/Access-Point-Screenshot.jpg)

Alternatively, you can specify credentials directly. You can go ahead and change the credentials in these variables:

```c++
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";
```

You will also need to change the WiFi Mode in the code via changing this variable to `true`:

```c++
//Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Default: false (puts device in AP mode)
#define WIFI_USE_DIRECT false
```

You will also want to change the `timezoneString` to your time zone. You can find the TZ database names here: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

You can also modify the date and clock format easily by using this table: https://github.com/ropg/ezTime#datetime

There are several helper `define` variables to help during debugging/running:

- **SERIAL_ENABLE**
  - Use this to enable Serial output lines for tracking executing code
- **WEB_FIRMWARE_UPDATE_ENABLE**
  - Enabled by default. Adds a browser-based firmware upload to the web UI's **⋮ menu > Firmware Update** page - no Arduino IDE or USB cable required
  - To use it: in the Arduino IDE, `Sketch > Export compiled Binary` to produce a `.bin` file for `ESPMaster.ino`, then upload that file via the ESPMaster tab on the Firmware Update page
  - If `otaPassword` is set, the same password is required for the browser upload
  - Only updates the sketch itself, not the static web files - use the normal `ESP8266 LittleFS Data Upload` step (over USB) whenever `ESPMaster/data/` changes
  - If the upload fails partway through, the currently running firmware stays active - the device only reboots into the new firmware once the upload has fully succeeded
- **UNIT_CALLS_DISABLE**
  - Use this to disable the communication with the Arduino Nano Units. This will mean you can check code over function for the ESP module.

#### Experiments

In the main Sketch under "Configurable Defines", an "EXPERIMENTAL" section has been included. This section has been created for features that are things that can be changed and trialled however aren't going to be necessary to be changed for general use.

##### Static IP Address

A feature request of being able to set a Static IP Address was created by [beroliv](https://github.com/beroliv) (thank you for the suggestion). This was to get around issues whereby in some routers, there was issues in being able to do this.

Code has been added to be able to set a Static IP Address on device. To do this:

1. Set the `WIFI_STATIC_IP` variable to `true` (defaulted to `false`)
2. Update the following settings as necessary for your needs:

   ```c++
   IPAddress wifiDeviceStaticIp(192, 168, 1, 100);

   IPAddress wifiRouterGateway(192, 168, 1, 1);
   IPAddress wifiSubnet(255, 255, 0, 0);

   IPAddress wifiPrimaryDns(8, 8, 8, 8);
   ```

**Suggestion:** Set your device up with a Static IP via your router if possible and to avoid conflicts on your network, however feel free to run this code if you are not able to. Testing this functionality showed it does work in both AP/Direct WiFi modes.

#### Sketch Upload

So far we've only uploaded static files to the ESP8266. You now need to `Upload` the sketch to the ESP8266. Click on Upload and the ESP8266 will be upadted with the sketch and you are done. Stick the ESP8266 onto the first unit's PCB and navigate to the IP-address the ESP8266 is getting assigned from your router.

### Common Problems

- If the ESP is not talking to the units correctly, check that unit addresses are sequential starting from 0 with no gaps (see [Automatic Unit Detection](#automatic-unit-detection) - a gap stops the count early) and use "Rescan Units" on the **⋮ menu > Units** page after fixing anything.
- Ensure you upload the sketch and the LittleFS sketch upload to the ESP8266.
- When the system is powered, your Hall Sensor should only light up when a magnet is nearby.
- Ensure you are running an older version of Arduino IDE to be able to upload static files to the device. You will need a version prior to version 2.x.
- User [@beroliv](https://github.com/beroliv) has reported having issues with WiFi connections. One solution they have proposed is soldering a wire to the antenna to be able to extend its range by creating an antenna. Here is the [link](https://www.stall.biz/project/verbesserte-wlan-konnektivitaet-mit-externen-antennen-fuer-wiffi-weatherman-und-andere-module-mit-esp8266/) (in German but Google Translate does a good job for other languages) they provided to detail the solution. Please take care when carrying out this solution. Thank you for the information [@beroliv](https://github.com/beroliv)!
