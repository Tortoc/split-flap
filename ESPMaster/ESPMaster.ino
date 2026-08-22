/* ####################################################################################################################### */
/* # ____  ____  _     ___ _____   _____ _        _    ____    _____ ____  ____    __  __    _    ____ _____ _____ ____  # */
/* #/ ___||  _ \| |   |_ _|_   _| |  ___| |      / \  |  _ \  | ____/ ___||  _ \  |  \/  |  / \  / ___|_   _| ____|  _ \ # */
/* #\___ \| |_) | |    | |  | |   | |_  | |     / _ \ | |_) | |  _| \___ \| |_) | | |\/| | / _ \ \___ \ | | |  _| | |_) |# */
/* # ___) |  __/| |___ | |  | |   |  _| | |___ / ___ \|  __/  | |___ ___) |  __/  | |  | |/ ___ \ ___) || | | |___|  _ < # */
/* #|____/|_|   |_____|___| |_|   |_|   |_____/_/   \_|_|     |_____|____/|_|     |_|  |_/_/   \_|____/ |_| |_____|_| \_\# */
/* ####################################################################################################################### */
/*
  This project project is done for fun as part of: https://github.com/JonnyBooker/split-flap
  None of this would be possible without the brilliant work of David Königsmann: https://github.com/Dave19171/split-flap

  Licensed under GNU: https://github.com/JonnyBooker/split-flap/blob/master/LICENSE
  
  Modified by Scott - Added LED debug codes and non-blocking operations for better web server responsiveness
*/

/* .--------------------------------------------------------------------------------. */
/* |  ___           __ _                    _    _       ___       __ _             | */
/* | / __|___ _ _  / _(_)__ _ _  _ _ _ __ _| |__| |___  |   \ ___ / _(_)_ _  ___ ___| */
/* || (__/ _ | ' \|  _| / _` | || | '_/ _` | '_ | / -_) | |) / -_|  _| | ' \/ -_(_-<| */
/* | \___\___|_||_|_| |_\__, |\_,_|_| \__,_|_.__|_\___| |___/\___|_| |_|_||_\___/__/| */
/* |                    |___/                                                       | */
/* '--------------------------------------------------------------------------------' */
/*
  These define statements can be changed as you desire for changing the functionality and
  behaviour of your device.
*/
#define SERIAL_ENABLE       true   //Option to enable serial debug messages
#define UNIT_CALLS_DISABLE  false   //Option to disable the call to the units so can just debug the ESP with no connections
#define OTA_ENABLE          true    //Option to enable OTA functionality via the Arduino IDE (same WiFi network + IDE required).
                                     //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): flipped from false to true on user request - going
                                     //forward, prefer flashing over the network (this, or the always-on WEB_FIRMWARE_UPDATE_ENABLE
                                     //endpoint below) and reserve the physical USB/serial programmer mainly for diagnostics
                                     //(serial log capture) rather than routine updates.
#define WEB_FIRMWARE_UPDATE_ENABLE true //Option to enable browser-based firmware upload from the Admin page, no Arduino IDE required
#define UNIT_BOOTLOADER_ENABLE true //Option to enable pushing Unit.ino firmware updates to units over I2C via the twiboot bootloader.
                                     //Requires every unit to first be flashed with twiboot per Unit/BOOTLOADER_SETUP.md - leave false until that's done!
#define UNITS_AMOUNT_MAX    16     //Maximum possible units (matches the 4-bit DIP switch addressing range 0-15).
                                    //The actual amount is auto-detected at startup via I2C scan, see scanI2CBus() - no longer set here.
#define SERIAL_BAUDRATE     115200  //Serial debugging BAUD rate
#define WIFI_USE_DIRECT     false  //Option to either direct connect to a WiFi Network or setup a AP to configure WiFi. Setting to false will setup as a AP.
#define LED_ENABLE          true   //Option to enable LED debug blink codes (set to false if not using LED)
#define DEBUG_ENABLE true  //Enable debug features: startup debug page, error status panel, page load debug log, and serial debug log at bottom of page

/*
  EXPERIMENTAL: Try to use your Router when possible to set a Static IP address for your device to avoid conflicts with other devices
  on your network. This will try and setup your device with a static IP address of your chosing. See below for more details.
*/
#define WIFI_STATIC_IP      false
#define FAE_MOD                   //Option for the modified PCB that includes an ESP-12F module

/*
  ============================================================================
  ESP01 PINOUT DIAGRAM (2 rows x 4 pins)
  ============================================================================
  
  ESP01 has 8 pins arranged in 2 rows of 4 pins. Looking at the module with
  the antenna at the top:
  
         [Antenna]
            |
      ┌─────┴─────┐
      │   ESP01   │
      └─────┬─────┘
            |
  
  Pin Layout (2 rows x 4 pins):
  
  TOP ROW (left to right):
  ┌─────────┬─────────┬─────────┬─────────┐
  │ Pin 1   │ Pin 2   │ Pin 3   │ Pin 4   │
  │  GND    │ GPIO 2  │ GPIO 0  │GPIO 1/TX│
  └─────────┴─────────┴─────────┴─────────┘
  
  BOTTOM ROW (left to right):
  ┌─────────┬─────────┬─────────┬─────────┐
  │ Pin 5   │ Pin 6   │ Pin 7   │ Pin 8   │
  │ CH_PD   │  RST    │  VCC    │GPIO 3/RX│
  └─────────┴─────────┴─────────┴─────────┘
  
  Detailed Pin Function Table:
  ┌─────────┬──────────────┬─────────────────────────────────────────────┐
  │ Pin #   │ Pin Name     │ Function / Notes                            │
  ├─────────┼──────────────┼─────────────────────────────────────────────┤
  │ 1       │ GND         │ Ground reference (0V)                        │
  │ 2       │ GPIO 2      │ Available for external LED (this code)       │
  │ 3       │ GPIO 0      │ Boot mode pin (LOW = flash/programming mode)│
  │ 4       │ GPIO 1 / TX │ Serial TX / Built-in blue LED / I2C SDA      │
  │         │             │   (Used for I2C in this code: Wire.begin)   │
  │ 5       │ CH_PD       │ Chip enable (must be HIGH, connect to 3.3V) │
  │         │             │   Typically via 10kΩ pull-up resistor        │
  │ 6       │ RST         │ Reset (LOW = reset, HIGH = normal operation)│
  │         │             │   Typically via 10kΩ pull-up resistor       │
  │ 7       │ VCC         │ Power input (3.3V ONLY, NOT 5V!)            │
  │         │             │   ⚠️ WARNING: 5V will damage the module!    │
  │ 8       │ GPIO 3 / RX │ Serial RX / I2C SCL                         │
  │         │             │   (Used for I2C in this code: Wire.begin)   │
  └─────────┴──────────────┴─────────────────────────────────────────────┘
  
  Important Notes:
  - VCC must be 3.3V (NOT 5V - will damage the module!)
  - GPIO 0 must be HIGH during normal operation (LOW = programming mode)
  - CH_PD must be HIGH (connect to 3.3V, typically via 10kΩ resistor)
  - RST must be HIGH (connect to 3.3V, typically via 10kΩ resistor)
  - GPIO 1 has built-in blue LED (active LOW) but is used for I2C in this code
  - GPIO 2 is available for external LED connection
  - GPIO 1 and GPIO 3 are used for I2C communication (Wire.begin(1, 3))
  
  ============================================================================
*/

// LED Debug Pin Configuration for ESP01
// Since GPIO 1 is used for I2C (Wire.begin(1, 3)), we use GPIO 2 for LED
// You need to connect an external LED: LED anode -> GPIO 2, LED cathode -> GND
// Use a 220-470 ohm resistor in series with the LED
//
// Alternative: If not using I2C, change to GPIO 1 to use built-in blue LED
#define LED_DEBUG_PIN 2  // GPIO 2 = external LED (GPIO 1 is used for I2C)
#define LED_ON HIGH      // External LED: HIGH = ON (opposite of built-in LED)
#define LED_OFF LOW

/*
  LED BLINK CODE REFERENCE:
  The LED on GPIO 2 will blink to indicate system status during initialization:
  
  1 blink   = WiFi connecting
  2 blinks  = WiFi connected successfully
  3 blinks  = NTP time synchronization starting
  4 blinks  = NTP sync successful
  5 blinks  = NTP sync failed or timed out
  6 blinks  = File system initialization
  7 blinks  = Web server starting
  8 blinks  = Web server ready - system operational
  Continuous = Error detected (WiFi connection failed, etc.)
  
  These codes help identify where the system is during startup and can aid in
  troubleshooting initialization issues.
*/

/* .--------------------------------------------------------. */
/* | ___         _               ___       __ _             | */
/* |/ __|_  _ __|_| ___ _ __   |   \ ___ / _(_)_ _  ___ ___| */
/* |\__ | || (_-|  _/ -_| '  \  | |) / -_|  _| | ' \/ -_(_-<| */
/* ||___/\_, /__/\__\___|_|_|_| |___/\___|_| |_|_||_\___/__/| */
/* |     |__/                                               | */
/* '--------------------------------------------------------' */
/*
  These are important to maintain normal system behaviour. Only change if you know 
  what your doing.
*/
//Status byte + 2 offset bytes + phase byte + 2 progress bytes - must match Unit.ino's requestEvent().
#define ANSWER_SIZE         6
//Status + offset only (first 3 bytes of the same response) - Unit.ino doesn't need to know which
//size is being requested; Wire.requestFrom()'s length parameter alone controls how many bytes
//actually get clocked out (I2C read length is purely master-controlled - the slave's TWI ISR
//stops firing as soon as the master NACKs, regardless of how much more it queued via
//Wire.write()). See readUnitInfo()'s 3-arg overload in ServiceFlapFunctions.ino.
#define ANSWER_SIZE_LIGHT   3
#define FLAP_AMOUNT         45      //Amount of Flaps in each unit
#define MIN_SPEED           1       //Min Speed
#define MAX_SPEED           12      //Max Speed
#define UNIT_STEPS          2038    //Must match STEPS in Unit.ino - used to validate offset values sent from the Admin UI
#define UNIT_BOOTLOADER_CMD 255     //Must match CMD_ENTER_BOOTLOADER in Unit.ino
#define UNIT_BOOTLOADER_BASE_ADDRESS 0x29 //Start of the range scanned for a unit's twiboot bootloader (0x29-0x38).
#define CRASH_LOG_PATH "/crash-log.jsonl" //See ServiceCrashLogFunctions.ino

//I2C pins, named so ServiceI2CRecoveryFunctions.ino's bus-recovery routine can reuse the exact
//same pins Wire.begin() is called with below, instead of duplicating the FAE_MOD #ifdef there.
#ifdef FAE_MOD
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#else
#define I2C_SDA_PIN 1
#define I2C_SCL_PIN 3
#endif
                                          //Must match TWI_ADDRESS_BASE in Unit/bootloader/twiboot/main.c - that
                                          //bootloader reads the unit's own DIP switches and adds them to this base,
                                          //so it doesn't need to match any particular unitIndex - see BOOTLOADER_SETUP.md

/* .-----------------------------------. */
/* | _    _ _                 _        | */
/* || |  (_| |__ _ _ __ _ _ _(_)___ ___| */
/* || |__| | '_ | '_/ _` | '_| / -_(_-<| */
/* ||____|_|_.__|_| \__,_|_| |_\___/__/| */
/* '-----------------------------------' */
/*
  External library dependencies, not much more to say!
*/

//WiFi Setup Library if we use that mode
//Specifically put here in this order to avoid conflict with other libraries
#if WIFI_USE_DIRECT == false
//Needed in order to be compatible with WiFiManager: https://github.com/me-no-dev/ESPAsyncWebServer/issues/418#issuecomment-667976368
#define WEBSERVER_H
#include <WiFiManager.h>
#endif

//OTA Libary if we are into that kind of thing
#if OTA_ENABLE == true
#include <ArduinoOTA.h>
#endif

//Used for the browser-based firmware upload on the Admin page
#if WEB_FIRMWARE_UPDATE_ENABLE == true
#include <Updater.h>
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <ESP8266WiFi.h>
#include <ezTime.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
//BUG FIX, 2026-08-22 (see BUGFIX_LOG.md #86): a twiboot flash-page write needs one unbroken I2C
//transaction of 4 header bytes + a full 128-byte page = 132 bytes - but Wire's internal TX buffer
//(I2C_BUFFER_LENGTH, Wire.h) defaults to 128 bytes, so the last 4 bytes of every page write
//silently never queue (Wire.write() drops anything past the buffer, no error returned) - twiboot's
//own byte-count trigger for actually committing the page write is never reached, so the write does
//*nothing* at all. (An attempted #define I2C_BUFFER_LENGTH override before this include did NOT
//fix it - Wire.cpp is compiled as its own separate translation unit with its own default,
//unaffected by a #define in this one.) This went undetected since the feature's original test
//(BUGFIX_LOG #54) happened to push firmware byte-identical to what was already running, so even a
//fully inert write still "verified" successfully - a live-reproduced, byte-level diff against the
//old firmware (BUGFIX_LOG #86) proved the "new" flash content was actually just the untouched old
//content. Fixed in ServiceUnitFirmwareFunctions.ino by using twi_writeTo() (twi.h) directly for
//flash-page writes - a raw, unbuffered write with no size limit, bypassing Wire's TX buffer
//entirely.
#include <twi.h>
#include <user_interface.h> // struct rst_info, for the on-device crash reporter (ServiceCrashLogFunctions.ino)
#include <lwip/priv/tcp_priv.h> // internal lwIP header - exposes tcp_active_pcbs/etc. for /api/tcp-status
#include "Classes.h"
#include "LittleFS.h"

/* .------------------------------------------------------------------------------------. */
/* |  ___           __ _                    _    _       ___     _   _   _              | */
/* | / __|___ _ _  / _(_)__ _ _  _ _ _ __ _| |__| |___  / __|___| |_| |_(_)_ _  __ _ ___| */
/* || (__/ _ | ' \|  _| / _` | || | '_/ _` | '_ | / -_) \__ / -_|  _|  _| | ' \/ _` (_-<| */
/* | \___\___|_||_|_| |_\__, |\_,_|_| \__,_|_.__|_\___| |___\___|\__|\__|_|_||_\__, /__/| */
/* |                    |___/                                                  |___/    | */
/* '------------------------------------------------------------------------------------' */
/*
  Settings you can feel free to change to customise how your display works.
*/
//Used if connecting via "WIFI_USE_DIRECT" of "true" - Otherwise, leave blank
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";

//Runtime-changeable OTA password, persisted to LittleFS (see /api/ota-password). Default "splitflap".
String otaPassword = "";
const char* otaPasswordPath = "/otapassword.txt";

//Runtime-changeable timezone, persisted to LittleFS (see /api/timezone). Default "Europe/Berlin".
//Use a TZ database name: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
String timezoneSetting = "";
const char* timezonePath = "/timezone.txt";

//If you want to have a different date or clock format change these two
//Complete table with every char: https://github.com/ropg/ezTime#getting-date-and-time
const char* dateFormat = "d.m.Y"; //Examples: d.m.Y -> 11.09.2021, D M y -> SAT SEP 21
const char* clockFormat = "H:i"; //Examples: H:i -> 21:19, h:ia -> 09:19PM

//How long to show a message for when a scheduled message is shown for
const int scheduledMessageDisplayTimeMillis = 7500;

#if WIFI_STATIC_IP == true
//Static IP address for your device. Try take care to not conflict with something else on your network otherwise
//it is likely to not work
IPAddress wifiDeviceStaticIp(192, 168, 0, 111);

//Your router details
IPAddress wifiRouterGateway(192, 168, 0, 1);
IPAddress wifiSubnet(255, 255, 0, 0);

//DNS Entry. Default: Google DNS
IPAddress wifiPrimaryDns(8, 8, 8, 8);
#endif

/* .------------------------------------------------------------. */
/* | ___         _               ___     _   _   _              | */
/* |/ __|_  _ __|_| ___ _ __   / __|___| |_| |_(_)_ _  __ _ ___| */
/* |\__ | || (_-|  _/ -_| '  \  \__ / -_|  _|  _| | ' \/ _` (_-<| */
/* ||___/\_, /__/\__\___|_|_|_| |___\___|\__|\__|_|_||_\__, /__/| */
/* |     |__/                                          |___/    | */
/* '------------------------------------------------------------' */
/*
  Used for normal running of the system so changing things here might make things 
  behave a little strange.
*/
//The current version of code to display on the UI
const char* espVersion = "2.3.0-Scott";

//All the letters on the units that we have to be displayed. You can change these if it so pleases at your own risk
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
int displayState[UNITS_AMOUNT_MAX];
unsigned long previousMillis = 0;
int unitsAmount = 0; //Auto-detected at startup by scanning I2C for units 0,1,2,... sequentially
                      //(stops at the first missing address, same "sequential from 0" requirement
                      //the DIP switches already have) - see scanI2CBus() in setup().

//Calling Wire directly from an AsyncWebServer callback crashes the ESP (nested call stack has too
//little headroom for I2C clock-stretch handling) - these handlers just stash the request here,
//loop() does the actual I2C work and calls send() itself. One in flight at a time; a second
//request while one's pending gets 503 instead of silently replacing it.
AsyncWebServerRequest* pendingRescanRequest = nullptr;
AsyncWebServerRequest* pendingUnitsInfoRequest = nullptr;
AsyncWebServerRequest* pendingLegacyI2CScanRequest = nullptr;

//ESPAsyncWebServer deletes the request object on client disconnect, even mid-flight while loop()
//is doing the deferred I2C work for it - a plain null-the-pointer-on-disconnect only catches the
//still-queued case. Each pendingXxxRequest gets a matching *Disconnected flag so loop() can tell
//its local copy of the pointer is no longer safe to call send() on.
bool pendingRescanRequestDisconnected = false;
bool pendingUnitsInfoRequestDisconnected = false;
bool pendingLegacyI2CScanRequestDisconnected = false;
//DIAGNOSTIC, 2026-08-22 (see BUGFIX_LOG.md #84): captured from the "detail" query param at
//request time (query params aren't available anymore once loop() picks this up later) - see the
//comment where this is used, below.
bool pendingUnitsInfoRequestDetail = false;

//Same reasoning, for the twiboot/bootloader endpoints (only relevant when
//UNIT_BOOTLOADER_ENABLE is true - see ServiceUnitFirmwareFunctions.ino).
AsyncWebServerRequest* pendingFirmwarePushRequest = nullptr;
int pendingFirmwarePushUnitIndex = -1;
bool pendingFirmwarePushRequestDisconnected = false;
AsyncWebServerRequest* pendingBootloaderProbeRequest = nullptr;
int pendingBootloaderProbeUnitIndex = -1;
bool pendingBootloaderProbeRequestDisconnected = false;

//Search for parameter in HTTP POST request
const char* PARAM_ALIGNMENT = "alignment";
const char* PARAM_FLAP_SPEED = "flapSpeed";
const char* PARAM_DEVICEMODE = "deviceMode";
const char* PARAM_INPUT_TEXT = "inputText";
const char* PARAM_SCHEDULE_ENABLED = "scheduleEnabled";
const char* PARAM_SCHEDULE_DATE_TIME = "scheduledDateTimeUnix";
const char* PARAM_SCHEDULE_SHOW_INDEFINITELY = "scheduleShowIndefinitely";
const char* PARAM_COUNTDOWN_DATE = "countdownDateTimeUnix";
const char* PARAM_ID = "id";
const char* PARAM_UNIT_OFFSET = "offset";

//Device Modes
const char* DEVICE_MODE_TEXT = "text";
const char* DEVICE_MODE_CLOCK = "clock";
const char* DEVICE_MODE_DATE = "date";
const char* DEVICE_MODE_COUNTDOWN = "countdown";

//Alignment options
const char* ALIGNMENT_MODE_LEFT = "left";
const char* ALIGNMENT_MODE_CENTER = "center";
const char* ALIGNMENT_MODE_RIGHT = "right";

//File paths to save input values permanently
const char* alignmentPath = "/alignment.txt";
const char* flapSpeedPath = "/flapspeed.txt";
const char* deviceModePath = "/devicemode.txt";
const char* countdownPath = "/countdown.txt";
const char* scheduledMessagesPath = "/scheduled-messages.txt";
const char* debugModePath = "/debugmode.txt";
const char* unitFirmwarePath = "/unit-firmware.bin";

//Variables for storing things for checking and use in normal running
String alignment = "";
String flapSpeed = "";
String inputText = "";
String deviceMode = "";
String countdownToDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
bool alignmentUpdated = false;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
bool isWifiConfigured = false;
//Defaults off - the dynamically-built startup debug page caused blank/corrupted pages on fresh
//boots (see BUGFIX_LOG.md). Toggle via POST /api/debug-mode.
bool showDebugPage = false;
volatile bool webRequestActive = false; // Flag to skip display updates during web requests
LList<ScheduledMessage> scheduledMessages;
Timezone timezone; 

//Create AsyncWebServer object on port 80
AsyncWebServer webServer(80);

//Used for creating a Access Point to allow WiFi setup
#if WIFI_USE_DIRECT == false
WiFiManager wifiManager;
bool isPendingWifiReset = false;
#endif

//Used to denote that the system has gone into OTA mode
#if OTA_ENABLE == true
bool isInOtaMode = false;
#endif

//Used to reject a browser firmware upload part-way through if the password didn't match
#if WEB_FIRMWARE_UPDATE_ENABLE == true
bool firmwareUpdateAuthFailed = false;

//BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #79): Update.begin()/write() used to run directly inside
//the AsyncWebServer upload callback - live-confirmed to crash the ESP with a real stack-overflow
//panic (`Panic core_esp8266_main.cpp:191 __yield`, ctx: sys) on the very first chunk, every time.
//Same root cause this project already hit once with I2C calls in an AsyncWebServer callback (see
//pendingRescanRequest etc. above) - that context's stack is too small for this kind of work.
//Fix: the callback below only copies bytes into this ring buffer; loop() does all the actual
//Update.begin()/write()/end() calls in its own large, safe stack. Sized well above what a single
//upload chunk plus whatever accumulates during one Update.write() call's internal yield()s should
//ever need, with graceful abort (not memory corruption) if it's ever not enough.
//Allocated with new[] only while an upload is actually in progress (see index==0 below) and freed
//right after - this device's free heap is chronically tight (see BUGFIX_LOG.md #41-43), a 4KB
//buffer held statically for a rarely-used feature would eat noticeably into that budget for no
//benefit the other 99% of the time this device is running.
//Live-tested progression: 4096 overflowed early. 8192 got a full transfer through the network at
//~150KB/s but the ring still overflowed partway (flash erase/write apparently can't always keep
//up with that network speed on this device - a genuine throughput limit, not just a buffer-size
//one). 16384 sounded like the fix but simply failed to allocate - this device's free heap is only
//~15KB total (see BUGFIX_LOG.md #41-43), so a 16KB request never had headroom to succeed. 8192 is
//close to the practical ceiling here: big enough to ride out normal short bursts, small enough to
//reliably allocate. On a sufficiently fast connection this can still legitimately run out of
//buffer - that now fails cleanly (a proper error response, old firmware stays active, no crash)
//instead of the original stack-overflow panic, which is the actual bug this fix targets.
#define OTA_RING_SIZE 8192
uint8_t* otaRingBuffer = nullptr;
volatile size_t otaRingHead = 0;   //next write index (callback advances this)
volatile size_t otaRingTail = 0;   //next read index (loop() advances this)
volatile size_t otaRingCount = 0;  //bytes currently queued, valid range 0..OTA_RING_SIZE
volatile bool otaUpdateActive = false;    //true from the first chunk until fully finished
volatile bool otaUpdateBegun = false;     //Update.begin() has been called for this upload
volatile bool otaFinalChunkQueued = false;//callback has queued the last chunk (final==true)
volatile bool otaRingOverflowed = false;  //callback had no room to queue a chunk - abort
volatile bool otaClientDisconnected = false; //client left before the upload finished
volatile bool otaResultReady = false;     //loop() has finished Update.end() (success or not)
volatile bool otaResultSuccess = false;   //valid once otaResultReady is true
//BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): captured from request->contentLength() at the start
//of the upload so loop() can size Update.begin() to what's actually being uploaded instead of the
//whole available OTA slot - see the comment where this is used, below.
volatile size_t otaUploadContentLength = 0;
AsyncWebServerRequest* pendingFirmwareUploadRequest = nullptr;
bool pendingFirmwareUploadRequestDisconnected = false;
#endif
#if UNIT_BOOTLOADER_ENABLE == true
bool unitFirmwareUploadAuthFailed = false;
#endif
bool webFileUploadAuthFailed = false;

// LED Debugging variables
String debugStatus = "";

// Serial log buffer for web interface (circular buffer, stores last N messages).
// DIAGNOSTIC FIX, 2026-08-13: message used to be a String. addToSerialLog() runs on
// *every single* SerialPrint()/SerialPrintln() call (regardless of SERIAL_ENABLE - see
// HelpersSerialHandling.ino), cycling through 100 slots with wildly varying message
// lengths - a textbook heap-fragmentation pattern on the ESP8266's small heap, and the
// leading remaining suspect for this session's "crashes eventually, at a seemingly
// random point" bug (after the /log stack-overflow fix in 1483f66 measurably improved
// but didn't fully eliminate it). Switched to a fixed-size char buffer: no allocation,
// no fragmentation, at the cost of a flat chunk of static RAM instead of scattered heap
// churn. Longer messages are truncated, not an error for a debug log.
//
// BUG FIX, 2026-08-14: the original 100*150=15000 bytes for this buffer, on top of
// everything else already static in this sketch, left only ~8KB of free heap on a
// *freshly booted* device (measured live via the new /heap diagnostic endpoint) - out of
// a total ~80KB RAM budget, over 80% was already claimed by static/global variables before
// a single dynamic allocation happened. That's this session's real root cause for a whole
// separate class of crash (reason 254 "user exception (panic/abort)", live-caught via the
// crash reporter - see BUGFIX_LOG.md #38/#41): with that little heap margin, ordinary
// allocations (a JSON response String, even a mere 1460-byte AsyncResponseStream buffer)
// can fail outright, and this core's `operator new` aborts rather than gracefully
// returning null. Shrunk to 40*120=4800 bytes (a >10KB heap-budget win) - still a useful
// rolling debug window, just no longer eating the majority of available RAM by itself.
#define SERIAL_LOG_SIZE 40
#define SERIAL_LOG_MESSAGE_MAX 120
struct SerialLogEntry {
  char message[SERIAL_LOG_MESSAGE_MAX];
  unsigned long timestamp;
};
SerialLogEntry serialLog[SERIAL_LOG_SIZE];
int serialLogIndex = 0;
int serialLogCount = 0;
String currentSerialLine = ""; // Buffer for building complete lines

// Function to add message to serial log buffer
void addToSerialLog(const String &message) {
  if (message.length() == 0) return; // Don't log empty messages

  // Add timestamp
  unsigned long timestamp = millis();

  // Store in circular buffer (fixed-size copy, see the comment on SerialLogEntry above)
  strncpy(serialLog[serialLogIndex].message, message.c_str(), SERIAL_LOG_MESSAGE_MAX - 1);
  serialLog[serialLogIndex].message[SERIAL_LOG_MESSAGE_MAX - 1] = '\0';
  serialLog[serialLogIndex].timestamp = timestamp;

  // Update indices
  serialLogIndex = (serialLogIndex + 1) % SERIAL_LOG_SIZE;
  if (serialLogCount < SERIAL_LOG_SIZE) {
    serialLogCount++;
  }
}

/* .-----------------------------------------------. */
/* | ___          _          ___     _             | */
/* ||   \ _____ _(_)__ ___  / __|___| |_ _  _ _ __ | */
/* || |) / -_\ V | / _/ -_) \__ / -_|  _| || | '_ \| */
/* ||___/\___|\_/|_\__\___| |___\___|\__|\_,_| .__/| */
/* |                                         |_|   | */
/* '-----------------------------------------------' */

// LED Blink Code Functions
// See header section for complete blink code reference

void ledOn() {
#if LED_ENABLE == true
  digitalWrite(LED_DEBUG_PIN, LED_ON);
#endif
}

void ledOff() {
#if LED_ENABLE == true
  digitalWrite(LED_DEBUG_PIN, LED_OFF);
#endif
}

void blinkLed(int count, int onTime, int offTime) {
#if LED_ENABLE == true
  for (int i = 0; i < count; i++) {
    ledOn();
    delay(onTime);
    ledOff();
    if (i < count - 1) delay(offTime);
  }
#endif
}

void setup() {
  //HARDENING, 2026-08-22 (see BUGFIX_LOG.md #85): the very first thing this firmware does, before
  //anything else (even Serial.begin()) - pull SDA/SCL to a defined idle-high state as early as
  //physically possible. Between a hard ESP reset and this point, these pins sit in an undefined
  //hardware-default state while the Unit's TWI hardware (which does NOT reset when the ESP does)
  //stays continuously armed and listening - any glitch during that gap could be misread as a
  //start condition. This doesn't close the gap entirely (nothing runs before setup() at all), but
  //shrinks it to a minimum and gets the lines into a safe state before anything else in setup()
  //could plausibly toggle them further.
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);

#if SERIAL_ENABLE == true
  //Setup so we can see serial messages
  Serial.begin(SERIAL_BAUDRATE);
#elif !defined(FAE_MOD)
  //For ESP01 only
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  //De-activate I2C if debugging the ESP, otherwise serial does not work
  //Wire.begin(D1, D2); //For NodeMCU testing only SDA=D1 and SCL=D2
#endif

#ifdef FAE_MOD
  //Fae Mod
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#endif

  //I2C Fast Mode (400kHz) instead of the 100kHz default - every I2C read runs inside the Unit's
  //TWI interrupt handler (briefly blocking its main loop, e.g. mid-stepper-timing during
  //calibration), so a faster bus means less time stolen per transaction. Units run at 16MHz and
  //already have 4.7k pull-ups on SDA/SCL, both comfortably support Fast Mode. See BUGFIX_LOG.md.
  Wire.setClock(400000);

#if UNIT_BOOTLOADER_ENABLE == true
  //twiboot (compiled with USE_CLOCKSTRETCH=1, see Unit/BOOTLOADER_SETUP.md) holds SCL low
  //while a flash page write is in progress. Give it comfortably more time than the ATmega328p's
  //worst-case erase+write duration so a page write never gets misread as a bus error.
  Wire.setClockStretchLimit(25000);
#endif

  //HARDENING, 2026-08-22 (see BUGFIX_LOG.md #85): recoverI2CBus() (ServiceI2CRecoveryFunctions.ino)
  //used to only run reactively, after a read already failed (see #45). Called here too, proactively,
  //right after Wire.begin() but before any real transaction - clocks SCL up to 9 times and forces a
  //clean STOP condition (AN10216), cleaning up any bus state left over from whatever happened
  //between the ESP's reset and this point (a Unit's TWI hardware never resets when the ESP does, so
  //it's been listening the whole time). Cheap (a few ms at most) and a no-op if the bus already
  //looks idle - safe to always run.
  recoverI2CBus();

  // Initialize LED for debugging
#if LED_ENABLE == true
  pinMode(LED_DEBUG_PIN, OUTPUT);
  ledOff();
  blinkLed(1, 100, 50); // Quick blink to show startup
#endif

  // DIAGNOSTIC, 2026-08-16 (see BUGFIX_LOG.md #73/#76): the first I2C scan used to happen right
  // here, blocking setup() with a delay(500) ("give the I2C bus time to stabilize") - later
  // pushed to delay(30000) as a live experiment (individual Units have repeatedly been observed
  // spinning endlessly trying to calibrate right after an ESPMaster reboot, see BUGFIX_LOG.md's
  // Offene Punkte), which also pushed WiFi/web server startup back by the same ~29.5s as an
  // unwanted side effect. Nothing else in setup() actually depends on unitsAmount being known
  // yet (the web server and display logic already treat unitsAmount==0 as "no units found so
  // far" gracefully) - so the scan is now deferred to loop() instead, see the
  // firstUnitScanDone block there. Same 30s delay before the first real I2C transaction, but
  // WiFi/the web server start immediately as before #73.
  SerialPrintln("");
  SerialPrintln("#######################################################");
  SerialPrintln("..............Split Flap Display Starting..............");
  SerialPrintln("#######################################################");
  debugStatus = "Starting";
  SerialPrintln("DEBUG: Status = " + debugStatus);

  //Load and read all the things
  debugStatus = "WiFi Init";
  SerialPrintln("DEBUG: Status = " + debugStatus);
  blinkLed(1, 200, 100); // 1 blink = WiFi connecting
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    blinkLed(2, 200, 100); // 2 blinks = WiFi connected
    debugStatus = "WiFi Connected";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    
    //ezTime initialization - NON-BLOCKING with timeout
    debugStatus = "NTP Sync Starting";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    blinkLed(3, 200, 100); // 3 blinks = NTP syncing
    
    // BUG FIX, 2026-08-16 (see BUGFIX_LOG.md #78): was setInterval(60). ezTime's vendored
    // NTP_RETRY constant (ezTime.h) unconditionally reschedules a retry 20s after any failed
    // sync, independent of this interval - resyncing every 60s gave that retry path frequent
    // chances to fire and, per live monitoring, correlated with a recurring ~50-65s
    // unreachable/reachable WiFi cycle. A wall-clock display doesn't need minute-level NTP
    // freshness, so we back off hard to shrink that exposure window. setInterval() takes a
    // uint16_t (max 65535s / ~18.2h), so a true 24h interval isn't representable - 6h is the
    // most conservative round value comfortably in range.
    setInterval(21600); // Sync every 6 hours

    //Was setDebug(INFO) - ezTime's own internal logging was firing 44000+ times in a few
    //minutes around WiFi hiccups, wasting loop()/UART time. Our own SerialPrintln() logging is
    //unaffected. See BUGFIX_LOG.md.
    setDebug(NONE);
    
    // Try to sync with timeout (non-blocking)
    unsigned long ntpStartTime = millis();
    unsigned long ntpTimeout = 30000; // 30 second timeout
    
    SerialPrintln("DEBUG: Starting NTP sync (non-blocking, 30s timeout)");
    
    // Wait for sync with timeout and yield
    while (timeStatus() == timeNotSet && (millis() - ntpStartTime) < ntpTimeout) {
      events(); // Process ezTime events
      yield();  // Allow other tasks to run
      delay(100);
    }
    
    if (timeStatus() == timeSet) {
      blinkLed(4, 200, 100); // 4 blinks = NTP sync success
      debugStatus = "NTP Sync Success";
      SerialPrintln("DEBUG: Status = " + debugStatus);
      SerialPrintln("DEBUG: NTP sync successful!");
    } else {
      blinkLed(5, 200, 100); // 5 blinks = NTP sync failed
      debugStatus = "NTP Sync Failed";
      SerialPrintln("DEBUG: Status = " + debugStatus);
      SerialPrintln("DEBUG: WARNING - NTP sync failed or timed out, continuing anyway");
    }
    
    //Load various variables
    debugStatus = "File System Init";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    blinkLed(6, 200, 100); // 6 blinks = File system init
    initialiseFileSystem();
    loadValuesFromFileSystem();
    checkAndPersistCrashLog(); // see ServiceCrashLogFunctions.ino - must run after LittleFS mounts

    // Moved here (see BUGFIX_LOG.md #78): needs timezoneSetting, which loadValuesFromFileSystem()
    // just populated from LittleFS (or its "Europe/Berlin" default on first boot).
    timezone.setLocation(timezoneSetting);
    SerialPrintln("DEBUG: Timezone set to: " + timezoneSetting);
    
    // Load debug mode setting
#if DEBUG_ENABLE == true
    String debugModeSetting = readFile(LittleFS, debugModePath, "false");
    showDebugPage = (debugModeSetting == "true");
    SerialPrintln("DEBUG: Debug mode: " + String(showDebugPage ? "enabled" : "disabled") + " (controls startup page, error status, page load log, and serial log)");
#endif

#if OTA_ENABLE == true
    SerialPrintln("OTA is enabled! Yay!");
#endif

    //Web Server Endpoint configuration
    debugStatus = "Web Server Setup";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    blinkLed(7, 200, 100); // 7 blinks = Web server starting
    
    //No .setCacheControl() - it drove heap fragmentation up fast on repeated requests (reverted,
    //see BUGFIX_LOG.md #69). Hard-refresh needed after updating a web file.
    webServer.serveStatic("/", LittleFS, "/");

    //All API routes below live under a single "/api/" prefix (was a mix of "/admin/..." and
    //no-prefix paths) - full endpoint reference in API.md, rename rationale in BUGFIX_LOG.md #74.
    //Not yet flash-tested on hardware as of the #74 rename (programmer was broken) - treat as
    //"ready to test" until the next successful flash confirms it.
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
      unsigned long pageRequestStart = millis();
      SerialPrintln("DEBUG: Request Home Page Received");
      
#if DEBUG_ENABLE == true
      // If debug mode is enabled and we're still in debug mode, show debug page
      if (showDebugPage) {
        // Generate debug HTML page with live log
        IPAddress ip = WiFi.localIP();
        String html = "<!DOCTYPE html><html><head>";
        html += "<title>Split Flap - Debug Mode</title>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>";
        html += "body { font-family: Arial, sans-serif; margin: 20px; background: #1e1e1e; color: #d4d4d4; }";
        html += "h1 { color: #4ec9b0; }";
        html += ".log-container { background: #252526; border: 1px solid #3e3e42; padding: 15px; font-family: 'Courier New', monospace; font-size: 0.9em; white-space: pre-wrap; }";
        html += ".log-entry { margin: 2px 0; }";
        html += ".timestamp { color: #808080; margin-right: 10px; }";
        html += ".debug { color: #4ec9b0; }";
        html += ".error { color: #f48771; }";
        html += ".warning { color: #dcdcaa; }";
        html += "button { background: #0e639c; color: white; border: none; padding: 10px 20px; font-size: 16px; cursor: pointer; border-radius: 4px; margin-bottom: 20px; }";
        html += "button:hover { background: #1177bb; }";
        html += ".status { color: #4ec9b0; font-weight: bold; margin: 10px 0; }";
        html += "</style>";
        html += "</head><body>";
        html += "<h1>Split Flap - Debug Mode</h1>";
        html += "<button onclick='continueToNormal()'>Continue to Normal Mode</button>";
        html += "<div class='status'>Current Status: " + debugStatus + "</div>";
        html += "<p>Debug mode is enabled. This page shows the initialization log. When you continue to normal mode, you'll also see error status panel, page load debug log, and serial debug log features.</p>";
        html += "<div class='log-container' id='logContainer'>";
        html += "<div style='color: #888;'>Loading log...</div>";
        html += "</div>";
        html += "<script>";
        html += "function loadLog() {";
        html += "  var xhr = new XMLHttpRequest();";
        html += "  xhr.onreadystatechange = function() {";
        html += "    if (this.readyState == 4) {";
        html += "      var container = document.getElementById('logContainer');";
        html += "      if (this.status == 200) {";
        html += "        try {";
        html += "          var data = JSON.parse(this.responseText);";
        html += "          var html = '';";
        html += "          if (data.logs && data.logs.length > 0) {";
        html += "            for (var i = 0; i < data.logs.length; i++) {";
        html += "              var entry = data.logs[i];";
        html += "              var msg = entry.message || '';";
        html += "              var timestamp = (entry.timestamp / 1000).toFixed(1) + 's';";
        html += "              var colorClass = '';";
        html += "              if (msg.indexOf('DEBUG:') >= 0) colorClass = 'debug';";
        html += "              else if (msg.indexOf('ERROR') >= 0 || msg.indexOf('Error') >= 0) colorClass = 'error';";
        html += "              else if (msg.indexOf('WARNING') >= 0 || msg.indexOf('Warning') >= 0) colorClass = 'warning';";
        html += "              html += '<div class=\"log-entry\"><span class=\"timestamp\">[' + timestamp + ']</span><span class=\"' + colorClass + '\">' + escapeHtml(msg) + '</span></div>';";
        html += "            }";
        html += "          } else {";
        html += "            html = '<div style=\"color: #888;\">No log messages yet. (Count: ' + (data.count || 0) + ')</div>';";
        html += "          }";
        html += "          container.innerHTML = html;";
        html += "        } catch (e) {";
        html += "          container.innerHTML = '<div style=\"color: #f48771;\">Error parsing log: ' + e.message + '<br>Response: ' + escapeHtml(this.responseText.substring(0, 200)) + '</div>';";
        html += "        }";
        html += "      } else {";
        html += "        container.innerHTML = '<div style=\"color: #f48771;\">Error loading log: Status ' + this.status + '</div>';";
        html += "      }";
        html += "    }";
        html += "  };";
        html += "  xhr.onerror = function() {";
        html += "    var container = document.getElementById('logContainer');";
        html += "    container.innerHTML = '<div style=\"color: #f48771;\">Network error loading log</div>';";
        html += "  };";
        html += "  xhr.open('GET', '/log', true);";
        html += "  xhr.send();";
        html += "}";
        html += "function escapeHtml(text) {";
        html += "  var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;', \"'\": '&#039;' };";
        html += "  return text.replace(/[&<>\"']/g, function(m) { return map[m]; });";
        html += "}";
        html += "function continueToNormal() {";
        html += "  var xhr = new XMLHttpRequest();";
        html += "  xhr.onreadystatechange = function() {";
        html += "    if (this.readyState == 4 && this.status == 200) {";
        html += "      window.location.href = '/';";
        html += "    }";
        html += "  };";
        html += "  xhr.open('POST', '/api/debug-mode', true);";
        html += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
        html += "  xhr.send('enabled=false');";
        html += "}";
        html += "loadLog();";
        html += "setInterval(loadLog, 1000);"; // Auto-refresh every second
        html += "</script>";
        html += "</body></html>";
        
        request->send(200, "text/html", html);
        return;
      }
#endif
      // Normal mode - serve regular index.html
      unsigned long beforeSend = millis();
      SerialPrintln("DEBUG: Serving index.html (took " + String(beforeSend - pageRequestStart) + "ms to get here)");
      request->send(LittleFS, "/index.html", "text/html");
      SerialPrintln("DEBUG: index.html sent (total: " + String(millis() - pageRequestStart) + "ms)");
    });

    webServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
      unsigned long settingsStart = millis();
      SerialPrintln("DEBUG: Request for Settings Received at " + String(settingsStart) + "ms");
      
      // Set flag to prevent display updates during web request
      webRequestActive = true;
      
      // Build minimal response immediately (fast path)
      JsonDocument minimalDoc;
      minimalDoc["timezoneOffset"] = timezone.getOffset();
      minimalDoc["unitCount"] = unitsAmount;
      minimalDoc["alignment"] = alignment;
      minimalDoc["flapSpeed"] = flapSpeed;
      minimalDoc["deviceMode"] = deviceMode;
      minimalDoc["version"] = espVersion;
      minimalDoc["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
      minimalDoc["lastWrittenText"] = lastWrittenText;
      minimalDoc["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());
      minimalDoc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
      minimalDoc["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      minimalDoc["wifiIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
      minimalDoc["scheduledMessages"] = JsonArray();
      minimalDoc["wifiSettingsResettable"] = true;
      //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): this was hardcoded to false regardless of the
      //actual OTA_ENABLE compile flag (a stale leftover from the #80 minimalDoc fast-path refactor -
      //getCurrentSettingValues(), the older/fuller settings builder elsewhere in this file, already
      //had the correct #if OTA_ENABLE check). Found while verifying OTA_ENABLE=true actually took
      //effect after flashing - /api/settings kept reporting false either way.
#if OTA_ENABLE == true
      minimalDoc["otaEnabled"] = true;
#else
      minimalDoc["otaEnabled"] = false;
#endif
      //Folded in here (see BUGFIX_LOG.md #80) so the Administration view can populate its OTA
      //password/timezone displays from the one /api/settings fetch it already does, instead of
      //two more separate XHRs - opening that view used to fire 4 near-simultaneous requests
      //against this device's hard 5-connection TCP limit (see #70).
      minimalDoc["otaPassword"] = otaPassword;
      minimalDoc["timezone"] = timezoneSetting;
      
      // Try to get scheduled messages quickly (with timeout)
      unsigned long beforeScheduled = millis();
      int scheduledCount = scheduledMessages.size();
      if (scheduledCount > 0 && (millis() - settingsStart) < 2000) { // Only if we have time
        for(int i = 0; i < scheduledCount && (millis() - beforeScheduled) < 1000; i++) {
          ScheduledMessage msg = scheduledMessages[i];
          minimalDoc["scheduledMessages"][i]["scheduledDateTimeUnix"] = msg.ScheduledDateTimeUnix;
          minimalDoc["scheduledMessages"][i]["message"] = msg.Message;
          minimalDoc["scheduledMessages"][i]["showIndefinitely"] = msg.ShowIndefinitely;
          yield();
        }
      }
      
      //beginResponseStream() avoids building the JSON into one contiguous String first (same fix
      //as /api/log, see BUGFIX_LOG.md #42/#44/#52).
      unsigned long totalTime = millis() - settingsStart;
      SerialPrintln("DEBUG: Settings response ready in " + String(totalTime) + "ms, JSON size: " + String(measureJson(minimalDoc)) + " bytes");

      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(minimalDoc, *response);
      request->send(response);

      SerialPrintln("DEBUG: Settings response sent (total: " + String(millis() - settingsStart) + "ms)");
      
      // Clear flag after request completes
      webRequestActive = false;
    });
    
    webServer.on("/api/health", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request for Health Check Received");
      request->send(200, "text/plain", "Healthy");
    });

    // Heap health check (added 2026-08-14 while chasing a live OOM-abort() crash - see
    // BUGFIX_LOG.md #42/#43, directly led to finding the low-free-heap root cause). Kept
    // permanently, unlike the one-off /debug/trigger-crash endpoint from #38 - this one is
    // read-only, has no allocation/crash risk of its own (plain text, stack-only char buffer),
    // and is cheap, ongoing insight worth having if heap pressure ever creeps back up.
    webServer.on("/api/heap", HTTP_GET, [](AsyncWebServerRequest * request) {
      char buf[220];
      //DIAGNOSTIC, 2026-08-21 (see BUGFIX_LOG.md #82): getFlashChipRealSize() reads the physical
      //chip's own SPI ID at boot (ground truth, independent of what this build was compiled for);
      //getFlashChipSize() is what the current eesz board-menu choice assumes. Added while figuring
      //out whether the flash layout can safely be widened for a bigger OTA slot (web-based
      //firmware upload currently fails outright - this sketch already exceeds eesz=1M128's ~438KB
      //OTA budget) - a mismatch between the two would mean the compiled layout doesn't match the
      //real chip, which needs fixing before touching eesz at all. Read-only, no allocation.
      //getFlashChipId()/getChipId() added on user request for full identification (manufacturer/
      //device ID of the actual flash part, and the ESP8266's own unique chip ID) - useful
      //documentation regardless of the eesz decision, and a second cross-check that FlashMode/
      //FlashFreq (dout/40MHz, unchanged) still matches this real part.
      snprintf(buf, sizeof(buf), "free=%u frag=%u%% maxBlock=%u flashReal=%u flashConfigured=%u flashChipId=0x%08X espChipId=0x%08X sdk=%s core=%s",
        ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize(),
        ESP.getFlashChipRealSize(), ESP.getFlashChipSize(), ESP.getFlashChipId(), ESP.getChipId(),
        ESP.getSdkVersion(), ESP.getCoreVersion().c_str());
      request->send(200, "text/plain", buf);
    });

    // DIAGNOSTIC, 2026-08-15 (see BUGFIX_LOG.md #70): added while investigating the residual,
    // self-recovering "ERR_EMPTY_RESPONSE" pattern noted in #69. This board's lwIP build
    // (verified in lwipopts.h, all 8 board-menu "lwIP variant" choices share this same value)
    // hard-caps MEMP_NUM_TCP_PCB at 5 - only 5 simultaneously *active* TCP connections, with a
    // *separate* 5-slot pool for connections in TIME-WAIT after closing (ESPAsyncWebSrv sets
    // "Connection: close" on every response, so every request opens a fresh connection). Read-
    // only, no allocation, same risk profile as /heap - walks lwIP's own linked lists.
    webServer.on("/api/tcp-status", HTTP_GET, [](AsyncWebServerRequest * request) {
      int activeCount = 0;
      for (struct tcp_pcb *p = tcp_active_pcbs; p != NULL; p = p->next) activeCount++;
      int timeWaitCount = 0;
      for (struct tcp_pcb *p = tcp_tw_pcbs; p != NULL; p = p->next) timeWaitCount++;
      int boundCount = 0;
      for (struct tcp_pcb *p = tcp_bound_pcbs; p != NULL; p = p->next) boundCount++;
      int listenCount = 0;
      for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p != NULL; p = p->next) listenCount++;

      char buf[128];
      snprintf(buf, sizeof(buf), "active=%d/5 timeWait=%d/5 bound=%d listen=%d", activeCount, timeWaitCount, boundCount, listenCount);
      request->send(200, "text/plain", buf);
    });


    // Simple test endpoint that responds immediately
    webServer.on("/api/test", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Test endpoint called");
      request->send(200, "application/json", "{\"status\":\"ok\",\"time\":" + String(millis()) + "}");
    });
    
    webServer.on("/api/log", HTTP_GET, [](AsyncWebServerRequest * request) {
      // DIAGNOSTIC FIX, 2026-08-13: this used to be `StaticJsonDocument<25000> document;` -
      // as a *local* variable, that puts a 25000-byte buffer on the stack of this very
      // AsyncWebServer callback, which (see the pendingRescanRequest comment near unitsAmount)
      // has much less stack headroom than loop() - a near-guaranteed stack overflow on every
      // single /log call, unlike everywhere else in this file which already uses the
      // heap-backed JsonDocument (ArduinoJson v7) exactly to avoid this. This endpoint was
      // called very frequently while debugging tonight's other issues, and is the leading
      // suspect for this session's "crashes eventually, at a seemingly random point" bug.
      JsonDocument document;
      document["count"] = serialLogCount;
      
      int startIndex = serialLogCount < SERIAL_LOG_SIZE ? 0 : serialLogIndex;
      int entriesToReturn = serialLogCount < SERIAL_LOG_SIZE ? serialLogCount : SERIAL_LOG_SIZE;
      
      // Limit to last 50 entries to prevent JSON buffer overflow
      int maxEntries = entriesToReturn > 50 ? 50 : entriesToReturn;
      int actualStart = entriesToReturn > 50 ? (startIndex + entriesToReturn - 50) % SERIAL_LOG_SIZE : startIndex;
      
      for (int i = 0; i < maxEntries; i++) {
        int idx = (actualStart + i) % SERIAL_LOG_SIZE;
        // serialLog[idx].message is already a bounded, null-terminated char buffer
        // (SERIAL_LOG_MESSAGE_MAX) - no further truncation/String copy needed here.
        document["logs"][i]["message"] = serialLog[idx].message;
        document["logs"][i]["timestamp"] = serialLog[idx].timestamp;
      }
      
      // BUG FIX, 2026-08-14: this used to build the whole response into one contiguous
      // `String jsonString`, then hand it to request->send(200, ..., jsonString), which copies
      // it again internally (AsyncBasicResponse::_content = content). With up to 50 entries at
      // up to 150 bytes each, that's two back-to-back multi-KB contiguous allocations - live-
      // reproduced today via the crash reporter (finally working, see BUGFIX_LOG.md #38): a
      // real abort()/panic crash with a stack trace pointing exactly here (operator new ->
      // String::reserve() -> AsyncWebServerRequest::send() -> this line), most likely an
      // allocation failing against a heap fragmented by many hours of uptime - exactly the kind
      // of large contiguous allocation that's hardest to satisfy on a fragmented ESP8266 heap,
      // even when total free heap would otherwise be enough. Switched to beginResponseStream(),
      // which lets ArduinoJson serialize directly into small (~1460 byte) chunks as TCP send
      // buffer space becomes available - no large contiguous buffer ever needed for the JSON
      // body itself.
      //
      // BUG FIX, 2026-08-14 (follow-up, BUGFIX_LOG.md #44): the heap-budget fix (#43) made this
      // crash far rarer, but not impossible - live-caught again, same site, after ~75 minutes of
      // otherwise idle uptime (presumably a browser tab left open with the Serial Log panel
      // auto-polling /log, occasionally landing on a moment when 1460 contiguous bytes genuinely
      // weren't available, e.g. mid WiFi/TCP activity). ESP8266's operator new aborts outright on
      // failure rather than returning null, so there's no way to catch this after the fact -
      // check free heap defensively *before* attempting the allocation instead, and degrade to a
      // clean error response rather than risking a crash.
      if (ESP.getFreeHeap() < 4000) {
        SerialPrintln("WARNING: /log skipped building response - free heap too low (" + String(ESP.getFreeHeap()) + " bytes)");
        request->send(503, "text/plain", "Too little free heap right now to safely build the log response - try again shortly");
        return;
      }
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(document, *response);
      request->send(response);
    });

    // On-device crash log (added 2026-08-13, see ServiceCrashLogFunctions.ino) - survives
    // even if the live serial connection wasn't available/reliable at the moment of a
    // crash. One JSON object per line (JSON Lines), oldest first, newest last.
    webServer.on("/api/crash-log", HTTP_GET, [](AsyncWebServerRequest * request) {
      if (!LittleFS.exists(CRASH_LOG_PATH)) {
        request->send(200, "application/jsonl", "");
        return;
      }
      request->send(LittleFS, CRASH_LOG_PATH, "application/jsonl");
    });

    // Clears the persisted crash log. POST (not GET) per this project's CSRF-hardening
    // convention (see the 2026-08-13 BUGFIX_LOG entry on GET-triggered state changes).
    webServer.on("/api/crash-log", HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (LittleFS.exists(CRASH_LOG_PATH)) {
        LittleFS.remove(CRASH_LOG_PATH);
      }
      request->send(200, "text/plain", "Crash log cleared");
    });

    // I2C bus scanner endpoint for diagnostics
    webServer.on("/api/i2c-scan", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: I2C scan requested");
      //Deferred to loop().
      if (pendingLegacyI2CScanRequest != nullptr) {
        request->send(503, "text/plain", "Another I2C scan is already in progress, try again shortly");
        return;
      }
      pendingLegacyI2CScanRequestDisconnected = false;
      pendingLegacyI2CScanRequest = request;
      request->onDisconnect([]() {
        pendingLegacyI2CScanRequest = nullptr;
        pendingLegacyI2CScanRequestDisconnected = true;
      });
    });

    // Admin: read live status + calibration offset for every unit.
    //Deferred to loop().
    webServer.on("/api/units", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Admin units request received");
      if (pendingUnitsInfoRequest != nullptr) {
        request->send(503, "text/plain", "Another units request is already in progress, try again shortly");
        return;
      }
      //DIAGNOSTIC, 2026-08-22 (see BUGFIX_LOG.md #84): opt-in heavy 6-byte read (adds phase/
      //progress) instead of the default light 3-byte one (#77) - only when explicitly asked for
      //via ?detail=1, so routine Administration-page polling stays on the cheaper path. Added to
      //investigate a Unit spontaneously rotating slowly/never finishing homing with no ESPMaster-
      //issued display job involved (see BUGFIX_LOG's Offene Punkte) - watching whether
      //currentPhaseProgress climbs steadily (mechanical/torque theory) or keeps resetting toward 0
      //(repeated-brownout-reset theory) distinguishes between the two live, without needing a
      //multimeter/scope.
      pendingUnitsInfoRequestDetail = request->hasParam("detail");
      pendingUnitsInfoRequestDisconnected = false;
      pendingUnitsInfoRequest = request;
      request->onDisconnect([]() {
        pendingUnitsInfoRequest = nullptr;
        pendingUnitsInfoRequestDisconnected = true;
      });
    });

    // Admin: re-run the I2C scan to pick up units that were connected/fixed after boot,
    // without needing a full reboot. Updates the global unitsAmount used everywhere else.
    //Deferred to loop().
    webServer.on("/api/units/rescan", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Admin rescan-units request received");
      if (pendingRescanRequest != nullptr) {
        request->send(503, "text/plain", "Another rescan is already in progress, try again shortly");
        return;
      }
      pendingRescanRequestDisconnected = false;
      pendingRescanRequest = request;
      request->onDisconnect([]() {
        pendingRescanRequest = nullptr;
        pendingRescanRequestDisconnected = true;
      });
    });

    // Admin: set a new calibration offset on a single unit, unit recalibrates immediately
    webServer.on("/api/units/offset", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam(PARAM_ID, true) || !request->hasParam(PARAM_UNIT_OFFSET, true)) {
        SerialPrintln("Admin unit-offset request missing parameters");
        request->send(400, "text/plain", "Both 'id' and 'offset' are required");
        return;
      }

      String idValue = request->getParam(PARAM_ID, true)->value();
      String offsetValue = request->getParam(PARAM_UNIT_OFFSET, true)->value();

      if (!isNumber(idValue) || !isNumber(offsetValue)) {
        SerialPrintln("Admin unit-offset request had non-numeric values");
        request->send(400, "text/plain", "'id' and 'offset' must be numbers");
        return;
      }

      int unitId = idValue.toInt();
      long offset = atol(offsetValue.c_str());

      if (unitId < 0 || unitId >= unitsAmount) {
        SerialPrintln("Admin unit-offset request had out of range unit id: " + idValue);
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(unitsAmount - 1));
        return;
      }

      if (offset < 0 || offset >= UNIT_STEPS) {
        SerialPrintln("Admin unit-offset request had out of range offset: " + offsetValue);
        request->send(400, "text/plain", "Offset out of range. Valid range: 0 to " + String(UNIT_STEPS - 1));
        return;
      }

      SerialPrintln("Setting Unit " + String(unitId) + " offset to " + String(offset));
      writeOffsetToUnit(unitId, (uint16_t)offset);

      request->send(202, "text/plain", "Offset sent. Unit is recalibrating now, this can take a few seconds.");
    });

    //Replaces a single static web UI file in place, without a full littlefs.bin reflash (which
    //would wipe runtime settings files). Filename allowlisted, not an arbitrary path.
    webServer.on("/api/web-file", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        if (webFileUploadAuthFailed) {
          webFileUploadAuthFailed = false;
          request->send(401, "text/plain", "Invalid password");
          return;
        }
        request->send(200, "text/plain", "OK - file updated");
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File webUploadFile;
        static bool webUploadFilenameValid = false;

        if (index == 0) {
          webFileUploadAuthFailed = false;
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (providedPassword != String(otaPassword)) {
              SerialPrintln("Web file upload rejected: invalid password");
              webFileUploadAuthFailed = true;
              return;
            }
          }

          //"index.html" is the only valid target - the whole UI is one file now (BUGFIX_LOG.md #72).
          webUploadFilenameValid = (filename == "index.html");
          if (!webUploadFilenameValid) {
            SerialPrintln("Web file upload rejected: filename not in allowlist: " + filename);
            request->send(400, "text/plain", "Filename must be one of the known static assets");
            return;
          }

          SerialPrintln("DEBUG: Web file upload starting: " + filename);
          webUploadFile = LittleFS.open("/" + filename, "w");
          if (!webUploadFile) {
            SerialPrintln("Web file upload: failed to open /" + filename + " for writing");
          }
        }

        if (webFileUploadAuthFailed || !webUploadFilenameValid) {
          return;
        }

        if (webUploadFile) {
          webUploadFile.write(data, len);
        }

        if (final) {
          if (webUploadFile) {
            webUploadFile.close();
          }
          SerialPrintln("DEBUG: Web file upload finished: " + filename + ", total bytes: " + String(index + len));
        }
      }
    );

#if UNIT_BOOTLOADER_ENABLE == true
    // Admin: upload a compiled Unit.ino firmware image (raw AVR flash .bin, NOT the ESPMaster's own
    // .bin) to be pushed to a unit afterwards via /api/units/firmware-push. See Unit/BOOTLOADER_SETUP.md.
    // Requires X-OTA-Password, same as /api/firmware - anyone on the LAN could otherwise push
    // arbitrary code to the unit's AVR.
    webServer.on("/api/units/firmware", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        if (unitFirmwareUploadAuthFailed) {
          unitFirmwareUploadAuthFailed = false;
          request->send(401, "text/plain", "Invalid password");
          return;
        }
        request->send(200, "text/plain", "OK - firmware stored, ready to push to a unit");
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File unitFirmwareFile;

        if (index == 0) {
          unitFirmwareUploadAuthFailed = false;
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (providedPassword != String(otaPassword)) {
              SerialPrintln("Unit firmware upload rejected: invalid password");
              unitFirmwareUploadAuthFailed = true;
              return;
            }
          }

          SerialPrintln("DEBUG: Unit firmware upload starting: " + filename);
          unitFirmwareFile = LittleFS.open(unitFirmwarePath, "w");
          if (!unitFirmwareFile) {
            SerialPrintln("Unit firmware upload: failed to open " + String(unitFirmwarePath) + " for writing");
          }
        }

        if (unitFirmwareUploadAuthFailed) {
          return; //Password rejected, ignore the rest of the upload - final response already handled above
        }

        if (unitFirmwareFile) {
          unitFirmwareFile.write(data, len);
        }

        if (final) {
          if (unitFirmwareFile) {
            unitFirmwareFile.close();
          }
          SerialPrintln("DEBUG: Unit firmware upload finished. Total bytes: " + String(index + len));
        }
      }
    );

    // Admin: push the currently stored unit firmware to a single unit over I2C (twiboot).
    // Deferred to loop(), same reasoning as pendingRescanRequest above. Requires X-OTA-Password.
    //BUG FIX, 2026-08-22 (see BUGFIX_LOG.md #86): this used to be "/api/units/firmware/push" -
    //a URL that is a strict prefix-extension of "/api/units/firmware" (the upload endpoint just
    //above, registered with the two-callback upload form). ESPAsyncWebServer's route matching for
    //that form intercepted every request to this path too, so this handler was completely
    //unreachable (silently "succeeding" with the upload endpoint's response instead) ever since
    //the two endpoints were renamed from the old, non-overlapping "/unit-firmware-upload"/
    //"/unit-firmware-push" scheme. Renamed to a sibling path instead of a sub-path so the two
    //can never prefix-collide again.
    webServer.on("/api/units/firmware-push", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (String(otaPassword) != "") {
        String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
        if (providedPassword != String(otaPassword)) {
          SerialPrintln("Unit firmware push rejected: invalid password");
          request->send(401, "text/plain", "Invalid password");
          return;
        }
      }

      if (!request->hasParam(PARAM_ID, true)) {
        request->send(400, "text/plain", "'id' is required");
        return;
      }

      String idValue = request->getParam(PARAM_ID, true)->value();
      if (!isNumber(idValue)) {
        request->send(400, "text/plain", "'id' must be a number");
        return;
      }

      int unitId = idValue.toInt();
      if (unitId < 0 || unitId >= unitsAmount) {
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(unitsAmount - 1));
        return;
      }

      if (!LittleFS.exists(unitFirmwarePath)) {
        request->send(400, "text/plain", "No firmware uploaded yet. Upload a .bin first.");
        return;
      }

      if (pendingFirmwarePushRequest != nullptr) {
        request->send(503, "text/plain", "Another firmware push is already in progress, try again shortly");
        return;
      }

      pendingFirmwarePushUnitIndex = unitId;
      pendingFirmwarePushRequestDisconnected = false;
      pendingFirmwarePushRequest = request;
      //Note: an in-flight pushFirmwareToUnit() always runs to completion regardless of this flag
      //(by design) - it just has nowhere to report the result if the client already left.
      request->onDisconnect([]() {
        pendingFirmwarePushRequest = nullptr;
        pendingFirmwarePushRequestDisconnected = true;
      });
    });

    // Admin/diagnostic: send a unit into bootloader mode and report which I2C address it answers
    // on, without writing anything to flash. Used to verify the DIP-jumper address is read live
    // by twiboot (see BOOTLOADER_SETUP.md's jumper-live-test) without risking a real firmware
    // push. Deferred to loop() for the same reason as /unit-firmware-push above.
    webServer.on("/api/units/bootloader-probe", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam(PARAM_ID, true)) {
        request->send(400, "text/plain", "'id' is required");
        return;
      }

      String idValue = request->getParam(PARAM_ID, true)->value();
      if (!isNumber(idValue)) {
        request->send(400, "text/plain", "'id' must be a number");
        return;
      }

      int unitId = idValue.toInt();
      if (unitId < 0 || unitId >= unitsAmount) {
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(unitsAmount - 1));
        return;
      }

      if (pendingBootloaderProbeRequest != nullptr) {
        request->send(503, "text/plain", "Another bootloader probe is already in progress, try again shortly");
        return;
      }

      pendingBootloaderProbeUnitIndex = unitId;
      pendingBootloaderProbeRequestDisconnected = false;
      pendingBootloaderProbeRequest = request;
      request->onDisconnect([]() {
        pendingBootloaderProbeRequest = nullptr;
        pendingBootloaderProbeRequestDisconnected = true;
      });
    });
#endif

#if WEB_FIRMWARE_UPDATE_ENABLE == true
    // Admin: browser-based firmware upload (no Arduino IDE required).
    // Password (if otaPassword is set) is passed via the X-OTA-Password header so it's available
    // before the file data starts streaming in, regardless of multipart field order.
    //
    // BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #79): Update.begin()/write()/end() used to be called
    // directly from the upload callback below - live-confirmed to crash the ESP on the very first
    // chunk every time (`Panic core_esp8266_main.cpp:191 __yield`, ctx: sys - a stack overflow,
    // this callback's stack is too small for what Update.begin()'s flash erase needs). Fix: this
    // callback ONLY copies bytes into otaRingBuffer; loop() does all the actual Update.* calls in
    // its own large, safe stack. See the ring-buffer globals near firmwareUpdateAuthFailed above.
    webServer.on("/api/firmware", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Runs once AsyncWebServer has received the whole body (every chunk already handed to the
        //upload callback below). The actual flash write may still be draining in loop() at this
        //point - don't respond here, just hand off the request; loop() sends the real response
        //once Update.end() has actually run (see the pendingFirmwareUploadRequest block below).
        if (firmwareUpdateAuthFailed) {
          firmwareUpdateAuthFailed = false;
          request->send(401, "text/plain", "Invalid password");
          return;
        }
        pendingFirmwareUploadRequestDisconnected = false;
        pendingFirmwareUploadRequest = request;
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          SerialPrintln("DEBUG: Firmware upload starting: " + filename);
          firmwareUpdateAuthFailed = false;
          //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): request->contentLength() is the whole
          //multipart body size (a few hundred bytes more than the raw firmware, for the boundary/
          //headers) - close enough once rounded up to a flash sector below. Used so loop() only
          //erases what this upload actually needs, not the full OTA slot (which grew from ~438KB
          //to ~1019KB in this same change - erasing the full slot took long enough to overflow the
          //ring buffer before Update.begin() even returned, live-reproduced testing this).
          otaUploadContentLength = request->contentLength();
          otaRingHead = 0;
          otaRingTail = 0;
          otaRingCount = 0;
          otaUpdateBegun = false;
          otaFinalChunkQueued = false;
          otaClientDisconnected = false;
          otaResultReady = false;
          //Freed by loop() once it's done with the upload (success or not) - see below. Allocated
          //fresh per upload rather than held statically, see the comment on the declaration above.
          if (otaRingBuffer != nullptr) {
            delete[] otaRingBuffer; //shouldn't happen (loop() always frees before the next upload
                                    //could start), but don't leak if it somehow does
          }
          otaRingBuffer = new (std::nothrow) uint8_t[OTA_RING_SIZE];
          otaRingOverflowed = (otaRingBuffer == nullptr);
          if (otaRingOverflowed) {
            SerialPrintln("Firmware upload: could not allocate ring buffer, aborting");
          }

          //Only enforce the password if one has actually been configured
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (providedPassword != String(otaPassword)) {
              SerialPrintln("Firmware upload rejected: invalid password");
              firmwareUpdateAuthFailed = true;
              return;
            }
          }

          //Registered here (not just in the completion callback above) so a client that
          //disconnects mid-upload - before AsyncWebServer ever considers the request "complete" -
          //still gets noticed by loop(), instead of leaving otaUpdateActive stuck forever.
          request->onDisconnect([]() {
            otaClientDisconnected = true;
            pendingFirmwareUploadRequest = nullptr;
            pendingFirmwareUploadRequestDisconnected = true;
          });

          otaUpdateActive = true; //loop() calls Update.begin() itself, see below
        }

        if (firmwareUpdateAuthFailed || !otaUpdateActive || otaRingOverflowed) {
          return; //Password rejected, ring buffer alloc failed, or a stray chunk after we gave up
        }

        if (len > 0) {
          if (otaRingCount + len > OTA_RING_SIZE) {
            SerialPrintln("Firmware upload: ring buffer overflow, aborting");
            otaRingOverflowed = true;
          } else {
            size_t firstPart = OTA_RING_SIZE - otaRingHead;
            if (firstPart >= len) {
              memcpy(otaRingBuffer + otaRingHead, data, len);
            } else {
              memcpy(otaRingBuffer + otaRingHead, data, firstPart);
              memcpy(otaRingBuffer, data + firstPart, len - firstPart);
            }
            otaRingHead = (otaRingHead + len) % OTA_RING_SIZE;
            otaRingCount += len;
          }
        }

        if (final) {
          otaFinalChunkQueued = true;
        }
      }
    );
#endif

#if DEBUG_ENABLE == true
    //Used to be two CSRF-via-GET endpoints (/exit-debug-mode, /enable-debug-mode) - merged into
    //one POST with an "enabled" param (see BUGFIX_LOG.md #74).
    webServer.on("/api/debug-mode", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("enabled", true)) {
        request->send(400, "text/plain", "'enabled' is required (true/false)");
        return;
      }

      bool enabled = request->getParam("enabled", true)->value() == "true";
      showDebugPage = enabled;
      writeFile(LittleFS, debugModePath, enabled ? "true" : "false");

      if (enabled) {
        SerialPrintln("DEBUG: Request to enable debug mode received");
        request->send(200, "text/plain", "OK - Debug mode enabled. This enables: startup debug page, error status panel, page load debug log, and serial debug log. Refresh page to see changes.");
      } else {
        SerialPrintln("DEBUG: Request to exit debug mode received");
        request->send(200, "text/plain", "OK - Debug mode disabled. Startup debug page, error status, and debug logs will be hidden.");
      }
    });
#endif
    
    //This and reset-units/ota/reset-wifi below are POST, not GET - a GET link is triggerable
    //cross-site (e.g. an <img> tag on any page) without user interaction (CSRF-via-GET).
    webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reboot Received");
      
      //Create HTML page to explain the system is rebooting
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - Rebooting</h1>";
      html += "<p>Reboot is pending now...<p>";
      html += "<p>This can take anywhere between 10-20 seconds<p>";
      html += "<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "</font>";
      html += "</div>";
      
      request->send(200, "text/html", html);
      isPendingReboot = true;
    });
    
    webServer.on("/api/units/reset", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reset Units Received");

      //This will be picked up in the loop
      isPendingUnitsReset = true;

      request->send(200, "text/plain", "Units are resetting/recalibrating now.");
    });

    webServer.on("/api/scheduled-messages", HTTP_DELETE, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Remove Scheduled Message Received");
      
      if (request->hasParam(PARAM_ID)) {
        bool removedScheduledMessage = false;
        String idValue = request->getParam(PARAM_ID)->value();

        if (isNumber(idValue)) {
          long parsedIdValue = atol(idValue.c_str());
          bool removed = removeScheduledMessage(parsedIdValue);
          
          if (removed) {
            request->send(202, "text/plain", "Removed");
          }
          else {
            request->send(400, "text/plain", "Unable to find message with ID specified. Id: " + idValue);
          }
        }
        else {
          SerialPrintln("Invalid Delete Scheduled Message ID Received");
          request->send(400, "text/plain", "Invalid ID value");
        }
      } 
      else {
          SerialPrintln("Delete Scheduled Message Received with no ID");
          request->send(400, "text/plain", "No ID specified");
      }
    });

    webServer.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request Post of Form Received");    

      bool submissionError = false;
      
      //Separate declarations - `bool a, b = false;` only initializes b, not a.
      bool newMessageScheduleEnabledValue = false;
      bool newMessageScheduleShowIndefinitely = false;
      long newMessageScheduleDateTimeUnixValue = -1;
      String newAlignmentValue, newDeviceModeValue, newFlapSpeedValue, newInputTextValue, newCountdownToDateUnixValue = "";
      bool hasNewInputTextValue = false;
      
      int params = request->params();
      for (int paramIndex = 0; paramIndex < params; paramIndex++) {
        AsyncWebParameter* p = request->getParam(paramIndex);
        if (p->isPost()) {
          //HTTP POST alignment value
          if (p->name() == PARAM_ALIGNMENT) {
            String receivedValue = p->value();
            if (receivedValue == ALIGNMENT_MODE_LEFT || receivedValue == ALIGNMENT_MODE_CENTER || receivedValue == ALIGNMENT_MODE_RIGHT) {
              newAlignmentValue = receivedValue;
            }
            else {
              SerialPrintln("Alignment provided was not valid. Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST device mode value
          if (p->name() == PARAM_DEVICEMODE) {
            String receivedValue = p->value();
            if (receivedValue == DEVICE_MODE_TEXT || receivedValue == DEVICE_MODE_CLOCK || receivedValue == DEVICE_MODE_DATE || receivedValue == DEVICE_MODE_COUNTDOWN) {
              newDeviceModeValue = receivedValue;          
            }
            else {
              SerialPrintln("Device Mode provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST Flap Speed Slider value
          if (p->name() == PARAM_FLAP_SPEED) {
            //SECURITY/BUG FIX, 2026-08-13: the <input type="range" min="1" max="100"> bound in
            //index.html is client-side only. convertSpeed() feeds this straight into map(), which
            //extrapolates rather than clamps outside its input range - a handcrafted request with
            //e.g. flapSpeed=-1000 produces a negative "speed", which then gets truncated to a
            //uint8_t when written over I2C (Wire.write()) and wraps around to an arbitrary large
            //byte value. The unit would call stepper.setSpeed() with that garbage RPM value - a
            //real (if low-probability) safety concern for the physical motor. Validate server-side.
            String rawFlapSpeed = p->value();
            if (!isNumber(rawFlapSpeed) || rawFlapSpeed.toInt() < 1 || rawFlapSpeed.toInt() > 100) {
              SerialPrintln("flapSpeed provided was invalid (must be a number 1-100): " + rawFlapSpeed);
              submissionError = true;
            } else {
              newFlapSpeedValue = rawFlapSpeed.c_str();
            }
          }

          //HTTP POST inputText value
          if (p->name() == PARAM_INPUT_TEXT) {
            //SECURITY/BUG FIX, 2026-08-13: the <input maxlength="75"> limit in index.html is
            //client-side only - a request built by hand (or a malicious/broken client) can send
            //an arbitrarily long value. Nothing downstream (processSentenceToLines(),
            //centerString() and friends) has an unbounded failure mode anymore after this
            //session's other fixes, but an unbounded String here is still needless heap
            //pressure/slowness on a memory-constrained device for no legitimate benefit. Enforce
            //server-side too, generously above the UI's own limit.
            if (p->value().length() > 500) {
              SerialPrintln("inputText provided was too long (" + String(p->value().length()) + " chars, max 500)");
              submissionError = true;
            } else {
              newInputTextValue = p->value().c_str();
              hasNewInputTextValue = true;
            }
          }

          //HTTP POST Schedule Enabled
          if (p->name() == PARAM_SCHEDULE_ENABLED) {
            String newMessageScheduleEnabledString = p->value().c_str();
            newMessageScheduleEnabledValue = newMessageScheduleEnabledString == "on" ?
              true : 
              false;
          }
          
          //HTTP POST Schedule Show Indefinitely
          if (p->name() == PARAM_SCHEDULE_SHOW_INDEFINITELY) {
            String newMessageScheduleShowIndefinitelyString = p->value().c_str();
            newMessageScheduleShowIndefinitely = newMessageScheduleShowIndefinitelyString == "on" ?
              true : 
              false;
          }

          //HTTP POST Schedule Seconds
          if (p->name() == PARAM_SCHEDULE_DATE_TIME) {
            String receivedValue = p->value().c_str();
            if (isNumber(receivedValue)) {
              newMessageScheduleDateTimeUnixValue = atol(receivedValue.c_str());
            }
            else {
              SerialPrintln("Schedule date time provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }

          //HTTP POST Countdown Seconds
          if (p->name() == PARAM_COUNTDOWN_DATE) {
            String receivedValue = p->value().c_str();
            if (isNumber(receivedValue)) {
              newCountdownToDateUnixValue = receivedValue;
            }
            else {
              SerialPrintln("Countdown date provided was not valid. Invalid Value: " + receivedValue); 
              submissionError = true;
            }
          }
        }
      }    

      //If there was an error, report back to check what has been input
      if (submissionError) {
        SerialPrintln("Finished Processing Request with Error");
        request->send(400, "text/plain", "Invalid submission - check the values you entered (e.g. schedule/countdown dates must be in the future).");
      }
      else {
        SerialPrintln("Finished Processing Request Successfully");

        lastReceivedMessageDateTime = timezone.dateTime("d M y H:i:s");

        //Only if a new alignment value
        if (newAlignmentValue != "" && alignment != newAlignmentValue) {
          alignment = newAlignmentValue;
          alignmentUpdated = true;

          writeFile(LittleFS, alignmentPath, alignment.c_str());
          SerialPrintln("Alignment Updated: " + alignment);
        }

        //Only if a new flap speed value
        if (newFlapSpeedValue != "" && flapSpeed != newFlapSpeedValue) {
          flapSpeed = newFlapSpeedValue;

          writeFile(LittleFS, flapSpeedPath, flapSpeed.c_str());
          SerialPrintln("Flap Speed Updated: " + flapSpeed);
        }

        //Only if countdown date has changed
        if (newCountdownToDateUnixValue != "" && countdownToDateUnix != newCountdownToDateUnixValue) {
          countdownToDateUnix = newCountdownToDateUnixValue;

          writeFile(LittleFS, countdownPath, countdownToDateUnix.c_str());
          SerialPrintln("Countdown Date Time Unix Updated: " + countdownToDateUnix);
        }

        //If its a new scheduled message, add it to the backlog and proceed, don't want to change device mode
        //Else, we do want to change the device mode and clear out the input text
        if (newMessageScheduleEnabledValue) {
          addAndPersistScheduledMessage(newInputTextValue, newMessageScheduleDateTimeUnixValue, newMessageScheduleShowIndefinitely);
          SerialPrintln("New Scheduled Message added");
        }
        else {
          //Only if device mode has changed
          if (newDeviceModeValue != "" && deviceMode != newDeviceModeValue) {
            deviceMode = newDeviceModeValue;

            writeFile(LittleFS, deviceModePath, deviceMode.c_str());
            SerialPrintln("Device Mode Set: " + deviceMode);
          }

          //Only if we are showing text
          if (hasNewInputTextValue && deviceMode == DEVICE_MODE_TEXT) {
            inputText = newInputTextValue;
          }
        }

        request->send(200, "text/plain", "OK");
      }
    });

#if OTA_ENABLE == true
    webServer.on("/api/ota-mode", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to start OTA mode received");
      
      //Create HTML page to explain OTA
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - OTA Update Mode</h1>";
      html += "<p>OTA mode has been started. You can now update your module via WiFI. Open your Arduino IDE and select the new port in \"Tools\" menu and upload the your sketch as normal!<p>";
      html += "<p>Open your Arduino IDE and select the new port in \"Tools\" menu and upload the your sketch as normal!</p>";
      html += "<p>After you have carried out your update, the system will automatically be rebooted. You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p>You can take the system out of this mode by clicking the button to reboot below.</p>";
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "<form action=\"/api/reboot\" method=\"post\" style=\"display:inline\"><button type=\"submit\">Reboot</button></form>";
      html += "</font>";
      html += "</div>";

      request->send(200, "text/html", html);
  
      if (!isInOtaMode) {
        SerialPrintln("Setting OTA Hostname");
        ArduinoOTA.setHostname("Split-Flap-OTA");

        //If there is a password set, disabled by default for ease
        if (otaPassword != "") {
          ArduinoOTA.setPassword(otaPassword.c_str());
        }
        
        SerialPrintln("Starting OTA Mode");
        ArduinoOTA.begin();
        delay(100);
      
        ArduinoOTA.onStart([]() {
          LittleFS.end();
          if (ArduinoOTA.getCommand() == U_FLASH) {
            SerialPrintln("Start updating sketch");
          } 
          else {
            SerialPrintln("Start updating filesystem");
          }  
        });
        
        ArduinoOTA.onEnd([]() {
          SerialPrintln("Finished OTA Update - Rebooting");
          isPendingReboot = true;
        });
        
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
          SerialPrintf("OTA Progress: %u%%\r", (progress / (total / 100)));
        });
        
        ArduinoOTA.onError([](ota_error_t error) {
          SerialPrintf("Error[%u]: ", error);

          if (error == OTA_AUTH_ERROR) {
            SerialPrintln("Finished OTA Update - Rebooting");
          }
          else if (error == OTA_BEGIN_ERROR) {
            SerialPrintln("OTA Begin Failed");
          }
          else if (error == OTA_CONNECT_ERROR) {
            SerialPrintln("OTA Connect Failed");
          }
          else if (error == OTA_RECEIVE_ERROR) {
            SerialPrintln("OTA Receive Failed");
          }
          else if (error == OTA_END_ERROR) {
            SerialPrintln("OTA End Failed");
          }
        });
        
        //Put in OTA Mode
        isInOtaMode = true;
      }
      else {
        SerialPrintln("Already in OTA Mode");
      }
    });
#endif

#if WIFI_USE_DIRECT == false
    webServer.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reset WiFi Received");
      
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - Resetting WiFi</h1>";
      html += "<p>WiFi Settings have been erased. Device will now reboot...<p>";
      html += "<p>You will now be able to connect to this device in AP mode to configure the WiFi once more<p>";
      html += "<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "</font>";
      html += "</div>";
      
      request->send(200, "text/html", html);
      isPendingWifiReset = true;
    });
#endif

    //Freely readable without auth (this device has no separate login) - changing it still
    //requires the current password.
    webServer.on("/api/ota-password", HTTP_GET, [](AsyncWebServerRequest * request) {
      JsonDocument doc;
      doc["otaPassword"] = otaPassword;
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(doc, *response);
      request->send(response);
    });

    webServer.on("/api/ota-password", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("currentPassword", true) || !request->hasParam("newPassword", true)) {
        request->send(400, "text/plain", "'currentPassword' and 'newPassword' are required");
        return;
      }

      String currentPasswordValue = request->getParam("currentPassword", true)->value();
      if (currentPasswordValue != otaPassword) {
        SerialPrintln("OTA password change rejected: current password incorrect");
        request->send(401, "text/plain", "Current password is incorrect");
        return;
      }

      String newPasswordValue = request->getParam("newPassword", true)->value();
      if (newPasswordValue.length() == 0) {
        request->send(400, "text/plain", "New password cannot be empty");
        return;
      }

      otaPassword = newPasswordValue;
      writeFile(LittleFS, otaPasswordPath, otaPassword.c_str());
      SerialPrintln("OTA password changed");
      request->send(200, "text/plain", "OTA password changed");
    });

    webServer.on("/api/timezone", HTTP_GET, [](AsyncWebServerRequest * request) {
      JsonDocument doc;
      doc["timezone"] = timezoneSetting;
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(doc, *response);
      request->send(response);
    });

    webServer.on("/api/timezone", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("timezone", true)) {
        request->send(400, "text/plain", "'timezone' is required");
        return;
      }

      String newTimezoneValue = request->getParam("timezone", true)->value();
      if (newTimezoneValue.length() == 0) {
        request->send(400, "text/plain", "Timezone cannot be empty");
        return;
      }

      // setLocation() does a live lookup against ezTime's timezoned service, so an invalid/
      // unknown TZ database name is rejected here instead of silently persisting garbage.
      if (!timezone.setLocation(newTimezoneValue)) {
        SerialPrintln("Timezone change rejected: '" + newTimezoneValue + "' not recognized");
        request->send(400, "text/plain", "Timezone not recognized - use a TZ database name like Europe/Berlin");
        return;
      }

      timezoneSetting = newTimezoneValue;
      writeFile(LittleFS, timezonePath, timezoneSetting.c_str());
      SerialPrintln("Timezone changed to: " + timezoneSetting);
      request->send(200, "text/plain", "Timezone changed");
    });

    delay(250);
    webServer.begin();
    
    blinkLed(8, 200, 100); // 8 blinks = Web server ready
    debugStatus = "Ready";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    SerialPrintln("Split Flap Ready!");
    SerialPrintln("#######################################################");
  }
  else {
    if (isPendingReboot) {
      SerialPrintln("Reboot is pending to be able to continue device function. Hold please...");
      SerialPrintln("#######################################################");
    }
    else {
      SerialPrintln("Unable to connect to WiFi... Not starting web server");
      SerialPrintln("Please hard restart your device to try connect again");
      SerialPrintln("#######################################################");
      // Continuous blink to indicate error
      for (int i = 0; i < 10; i++) {
        ledOn();
        delay(100);
        ledOff();
        delay(100);
      }
    }
  }
}

/* .----------------------------------------------------. */
/* | ___                _             _                 | */
/* || _ \_  _ _ _  _ _ (_)_ _  __ _  | |   ___ ___ _ __ | */
/* ||   | || | ' \| ' \| | ' \/ _` | | |__/ _ / _ | '_ \| */
/* ||_|_\\_,_|_||_|_||_|_|_||_\__, | |____\___\___| .__/| */
/* |                          |___/               |_|   | */
/* '----------------------------------------------------' */
void loop() {
  //DIAGNOSTIC, 2026-08-16 (see BUGFIX_LOG.md #76): the first I2C scan of the Units, deliberately
  //delayed 30s after boot (see the comment in setup() for why) - runs here instead of blocking
  //setup() with delay(30000), so WiFi/the web server start immediately. Fires exactly once.
  static bool firstUnitScanDone = false;
  if (!firstUnitScanDone && millis() >= 30000) {
    firstUnitScanDone = true;
    SerialPrintln("");
    SerialPrintln("=== I2C Bus Scan on Startup (delayed 30s) ===");
    for (int scanAttempt = 0; scanAttempt < 3 && unitsAmount == 0; scanAttempt++) {
      if (scanAttempt > 0) {
        SerialPrintln("No units found, retrying scan...");
        delay(300);
      }
      scanI2CBus();
    }
    SerialPrintln("");
  }

  //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #40/#81): advances the non-blocking display-update
  //state machine by at most one small step (one I2C op at most) - called unconditionally, before
  //any of the deferred-request handling below, so a display update in progress can never again
  //prevent loop() from reaching this point (which is what used to make the deferred-request
  //pattern below starve for up to 90s at a time).
  updateDisplayJob();

  //Handle any I2C admin requests deferred here by the web handlers (see the comment on
  //pendingRescanRequest near unitsAmount for why this can't just run in the handler itself).
  //Checked first, before anything else in loop(), so a pending request gets served as soon as
  //possible.
  if (pendingRescanRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingRescanRequest;
    pendingRescanRequestDisconnected = false; //this run's flag starts clean - see the comment near unitsAmount

    int detectedAmount = scanI2CBus();

    //Guard slot released only now (not before scanI2CBus() ran) so a request arriving mid-scan
    //correctly gets 503 instead of silently overwriting this one. Same pattern below. See
    //BUGFIX_LOG.md #50.
    pendingRescanRequest = nullptr;

    //Client may have disconnected while scanI2CBus() ran, making deferredRequest a dangling pointer.
    if (!pendingRescanRequestDisconnected) {
      JsonDocument doc;
      doc["unitsAmount"] = detectedAmount;
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }

  if (pendingUnitsInfoRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingUnitsInfoRequest;
    pendingUnitsInfoRequestDisconnected = false;

    JsonDocument doc;
    doc["unitsAmount"] = unitsAmount;
    for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
      int status = -2;
      int offset = -1;
      bool online;
      //DIAGNOSTIC, 2026-08-16 (see BUGFIX_LOG.md #77): this used to also fetch phase/progress
      //(6-byte I2C read) - dropped in favor of the lightweight 3-byte status+offset read, since
      //the Administration page never displayed phase/progress anyway. Halves the number of TWI
      //interrupts this endpoint costs the Unit per poll. The display-update state machine (used by
      //showText(), unrelated to this endpoint - see ServiceDisplayJobFunctions.ino) still fetches
      //the full data - it genuinely needs it.
      //DIAGNOSTIC, 2026-08-22 (see BUGFIX_LOG.md #84): ?detail=1 opts back into the heavy read for
      //exactly this call - see the comment on pendingUnitsInfoRequestDetail above.
      if (pendingUnitsInfoRequestDetail) {
        int phase = -1;
        int progress = -1;
        online = readUnitInfo(unitIndex, status, offset, phase, progress);
        doc["units"][unitIndex]["phase"] = phase;
        doc["units"][unitIndex]["progress"] = progress;
      } else {
        online = readUnitInfo(unitIndex, status, offset);
      }

      doc["units"][unitIndex]["index"] = unitIndex;
      doc["units"][unitIndex]["online"] = online;
      doc["units"][unitIndex]["status"] = status;
      doc["units"][unitIndex]["offset"] = offset;

      yield();
    }

    pendingUnitsInfoRequest = nullptr;

    if (!pendingUnitsInfoRequestDisconnected) {
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }

  if (pendingLegacyI2CScanRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingLegacyI2CScanRequest;
    pendingLegacyI2CScanRequestDisconnected = false;

    JsonDocument doc;
    doc["scanTime"] = millis();

    int foundCount = 0;
    for (int address = 0; address < 16; address++) {
      Wire.beginTransmission(address);
      byte error = Wire.endTransmission();

      if (error == 0) {
        doc["devices"][foundCount]["address"] = address;

        Wire.requestFrom(address, 1, 1);
        if (Wire.available()) {
          int status = Wire.read();
          doc["devices"][foundCount]["status"] = status;
          if (status == 0) {
            doc["devices"][foundCount]["statusText"] = "ready";
          } else if (status == 1) {
            doc["devices"][foundCount]["statusText"] = "busy";
          } else {
            doc["devices"][foundCount]["statusText"] = "unknown";
          }
        } else {
          doc["devices"][foundCount]["status"] = -1;
          doc["devices"][foundCount]["statusText"] = "no response";
        }
        foundCount++;
      }

      yield();
      delay(10);
    }

    doc["foundCount"] = foundCount;
    doc["expectedCount"] = unitsAmount;

    pendingLegacyI2CScanRequest = nullptr;

    if (!pendingLegacyI2CScanRequestDisconnected) {
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }

#if WEB_FIRMWARE_UPDATE_ENABLE == true
  // Drains otaRingBuffer and does all the real Update.begin()/write()/end() work here, in loop()'s
  // large safe stack - see BUGFIX_LOG.md #79 for why this can't run in the upload callback itself.
  if (otaUpdateActive) {
    if (!otaUpdateBegun && !otaRingOverflowed) {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): only erase what this upload actually needs
      //(rounded up to a flash sector), not the whole OTA slot - Update.begin()'s erase is
      //synchronous and scales with the size passed in, and widening the OTA slot from ~438KB to
      //~1019KB (same change) made a full-slot erase take long enough to overflow the small ring
      //buffer before this call even returned. Falls back to the old full-slot behavior if
      //contentLength() wasn't available for some reason, and is always clamped to maxSketchSpace
      //as a safety ceiling either way.
      uint32_t updateSize = maxSketchSpace;
      if (otaUploadContentLength > 0) {
        uint32_t rounded = (otaUploadContentLength + 0xFFF) & 0xFFFFF000;
        if (rounded > 0 && rounded < maxSketchSpace) {
          updateSize = rounded;
        }
      }
      if (!Update.begin(updateSize, U_FLASH)) {
        SerialPrintln("Firmware upload: Update.begin() failed");
        Update.printError(Serial);
        otaRingOverflowed = true; //no dedicated "begin failed" flag - this also aborts cleanly
      }
      otaUpdateBegun = true;
    }

    if (otaRingCount > 0 && !otaRingOverflowed) {
      size_t lenToWrite = otaRingCount;
      size_t firstPart = OTA_RING_SIZE - otaRingTail;
      bool writeOk;
      if (firstPart >= lenToWrite) {
        writeOk = Update.write(otaRingBuffer + otaRingTail, lenToWrite) == lenToWrite;
      } else {
        writeOk = Update.write(otaRingBuffer + otaRingTail, firstPart) == firstPart;
        if (writeOk) {
          writeOk = Update.write(otaRingBuffer, lenToWrite - firstPart) == (lenToWrite - firstPart);
        }
      }
      if (!writeOk) {
        SerialPrintln("Firmware upload: Update.write() failed");
        Update.printError(Serial);
        otaRingOverflowed = true;
      }
      otaRingTail = (otaRingTail + lenToWrite) % OTA_RING_SIZE;
      otaRingCount -= lenToWrite;
    }

    if (!otaResultReady && (otaClientDisconnected || otaRingOverflowed || (otaFinalChunkQueued && otaRingCount == 0))) {
      bool success = false;
      if (!otaClientDisconnected && !otaRingOverflowed) {
        success = Update.end(true);
        if (!success) {
          SerialPrintln("Firmware upload: Update.end() failed");
          Update.printError(Serial);
        }
      } else {
        //Must still call end() here (ignoring its result - we already know success is false):
        //Update.begin() refuses to start a new update while the previous one is still marked
        //"running" internally, and only end()/a premature end() clears that - skipping this call
        //left every subsequent upload attempt failing with "Update.begin() failed" until a full
        //reboot, live-confirmed while testing this fix.
        if (otaUpdateBegun) {
          Update.end(false);
        }
        if (otaClientDisconnected) {
          SerialPrintln("Firmware upload: client disconnected mid-upload, aborting (old firmware stays active)");
        } else {
          SerialPrintln("Firmware upload: aborting (old firmware stays active)");
        }
      }
      otaResultReady = true;
      otaResultSuccess = success;
      otaUpdateActive = false;
      delete[] otaRingBuffer; //safe even if allocation itself failed - delete[] on nullptr is a no-op
      otaRingBuffer = nullptr;
    }
  }

  // Separate from the block above on purpose: the flash-side result and the deferred request can
  // become ready in either order (e.g. an overflow can finish before AsyncWebServer even calls the
  // completion callback that hands us pendingFirmwareUploadRequest).
  if (otaResultReady && pendingFirmwareUploadRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingFirmwareUploadRequest;
    bool wasDisconnected = pendingFirmwareUploadRequestDisconnected;
    pendingFirmwareUploadRequest = nullptr;

    if (!wasDisconnected) {
      AsyncWebServerResponse* response = deferredRequest->beginResponse(otaResultSuccess ? 200 : 500, "text/plain",
        otaResultSuccess ? "OK - Firmware written, rebooting now" : "Update failed, check the serial log for details");
      response->addHeader("Connection", "close");
      deferredRequest->send(response);
    }

    otaResultReady = false;
    if (otaResultSuccess) {
      SerialPrintln("Firmware update successful, rebooting...");
      isPendingReboot = true;
    } else {
      SerialPrintln("Firmware update failed");
    }
  }
#endif

#if UNIT_BOOTLOADER_ENABLE == true
  if (pendingFirmwarePushRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingFirmwarePushRequest;
    int unitIndex = pendingFirmwarePushUnitIndex;
    pendingFirmwarePushRequestDisconnected = false;

    SerialPrintln("Pushing firmware to Unit " + String(unitIndex) + "...");
    String resultJson;
    //Always runs to completion regardless of whether the client is still listening - see the
    //comment on the onDisconnect handler for /unit-firmware-push above.
    bool success = pushFirmwareToUnit(unitIndex, resultJson);

    pendingFirmwarePushRequest = nullptr;

    if (!pendingFirmwarePushRequestDisconnected) {
      deferredRequest->send(success ? 200 : 500, "application/json", resultJson);
    }
  }

  if (pendingBootloaderProbeRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingBootloaderProbeRequest;
    int unitIndex = pendingBootloaderProbeUnitIndex;
    pendingBootloaderProbeRequestDisconnected = false;

    SerialPrintln("Bootloader probe: requesting Unit " + String(unitIndex) + " to enter bootloader...");
    requestUnitEnterBootloader(unitIndex);

    //Same retry/backoff as pushFirmwareToUnit() - give the watchdog reset + twiboot startup
    //time to happen.
    TwibootChipInfo chipInfo;
    int bootAddress = -1;
    for (int attempt = 0; attempt < 5 && bootAddress == -1; attempt++) {
      delay(attempt == 0 ? 300 : 150);
      yield();
      bootAddress = scanForBootloaderAddress(chipInfo);
    }

    JsonDocument doc;
    doc["unitIndex"] = unitIndex;
    if (bootAddress == -2) {
      doc["success"] = false;
      doc["error"] = "More than one unit answered as a bootloader right now - refusing to guess which one.";
    } else if (bootAddress < 0) {
      doc["success"] = false;
      doc["error"] = "No unit responded in bootloader mode. Nothing was written.";
    } else {
      doc["success"] = true;
      doc["bootAddress"] = bootAddress;
      doc["bootAddressHex"] = "0x" + String(bootAddress, HEX);
      doc["addressOffsetFromBase"] = bootAddress - UNIT_BOOTLOADER_BASE_ADDRESS;
      char sigBuf[16];
      snprintf(sigBuf, sizeof(sigBuf), "%02X %02X %02X", chipInfo.signature[0], chipInfo.signature[1], chipInfo.signature[2]);
      doc["signature"] = sigBuf;
      doc["pageSize"] = chipInfo.pageSize;
      doc["bootloaderStart"] = chipInfo.bootloaderStart;

      //Unconditional re-init: repeated-start I2C reads (what the scan above just did) can leave
      //the ESP8266 core's *software* I2C state stuck (NACK on every later transaction) even when
      //the physical bus is idle - recoverI2CBus() alone doesn't catch that case. See BUGFIX_LOG.md #53.
      Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
      Wire.setClock(400000);
#if UNIT_BOOTLOADER_ENABLE == true
      Wire.setClockStretchLimit(25000);
#endif

      //Explicitly asked to return to the app - twiboot's own ~1000ms idle-timeout wasn't reliable
      //in practice (unit stayed in the bootloader 44+s once, needed a power cycle). BUGFIX_LOG.md #53.
      bool returnedToApp = twibootStartApplication(bootAddress);
      doc["returnedToApp"] = returnedToApp;
      doc["note"] = returnedToApp
        ? "Nothing was written. The unit was explicitly told to return to its app."
        : "Nothing was written, but the explicit return-to-app command failed - the unit may still be sitting in the bootloader. Check /admin/units, or power-cycle the unit if it doesn't come back on its own.";
    }

    pendingBootloaderProbeRequest = nullptr;

    if (!pendingBootloaderProbeRequestDisconnected) {
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }
#endif

  //Reboot in here as if we restart within a request handler, no response is returned
  if (isPendingReboot) {
    SerialPrintln("Rebooting Now... Fairwell!");
    SerialPrintln("#######################################################");
    delay(100);

    ESP.restart();
    return;
  }

#if WIFI_USE_DIRECT == false
  //Clear off the WiFi Manager Settings
  if (isPendingWifiReset) {
    SerialPrintln("Removing WiFi settings");
    wifiManager.resetSettings();
    delay(100);

    isPendingReboot = true;
    return;
  }
#endif

  // BUG FIX, 2026-08-14: removed loop()'s entire custom WiFi reconnect-monitoring block
  // (an addition on top of the original upstream project, not present in the "stable" baseline
  // the user started from - github.com/scientress/split-flap - verified directly by fetching
  // its ESPMaster.ino/loop(): it contains *no* WiFi.status()/disconnect()/begin() calls at all,
  // relying entirely on wifiManager.setWiFiAutoReconnect(true) set once in initWiFi() and the
  // ESP8266 SDK's own background auto-reconnect from then on).
  //
  // Today's session spent a long time fixing bugs *within* that added block (wrong hardcoded
  // credentials, permanently giving up after 3 failures, insufficient radio reset before
  // retrying) and it kept resurfacing new failure modes each time (most recently: 90+
  // consecutive failed reconnect attempts over 18+ minutes even with all those fixes applied).
  // Stepping back per the user's direct suggestion: this custom block calls WiFi.disconnect()
  // and WiFi.begin() *on top of* the SDK's own already-running background auto-reconnect - a
  // very plausible mechanism for actively fighting/interrupting that background process rather
  // than helping it, which would explain why patching the block's *details* never fully
  // resolved the underlying instability. Removed entirely rather than patched further -
  // isWifiConfigured is now purely a boot-time flag again (set once in initWiFi() on success,
  // exactly as in the original), matching the simpler, previously-stable design. The
  // WIFI_NONE_SLEEP fix (see ServiceWifiFunctions.ino) is kept - it's an independent radio
  // power-mode setting, not something that can conflict with the SDK's own reconnect logic, and
  // is a genuine improvement over both the original and this project's prior state.

  //Do nothing if WiFi is not configured
  if (!isWifiConfigured) {
    //Show there is an error via text on display
    deviceMode = DEVICE_MODE_TEXT;
    alignment = ALIGNMENT_MODE_CENTER;
    flapSpeed = "80";

    showText("OFFLINE");
    delay(100);
    return;
  }

  //If no units have been detected (see scanI2CBus()), there's nothing to display to - skip
  //anything unit-related below. Web server, OTA and settings all stay fully usable; use the
  //Admin page's "Rescan Units" once wiring/addressing is fixed, no reboot required.
  bool unitsAvailable = (unitsAmount > 0);

  //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #40/#81): this used to be "showText(blank1);
  //delay(2000); showText(blank2);", relying on showText() blocking until fully done to guarantee
  //ordering. Now that showText() returns immediately, a real delay(2000) here would just freeze
  //loop() (and updateDisplayJob() with it) for 2 real seconds - undermining the whole point of
  //this change. updateUnitsResetSequence() (ServiceDisplayJobFunctions.ino) replicates the same
  //blank1 -> pause -> blank2 ordering non-blockingly instead.
  if (unitsAvailable && isPendingUnitsReset) {
    updateUnitsResetSequence();
  } else if (!unitsAvailable) {
    isPendingUnitsReset = false; //nothing to reset, don't leave this stuck pending
  }
  
#if OTA_ENABLE == true
  //If System is in OTA, try handle!
  if(isInOtaMode) {
    ArduinoOTA.handle();
    delay(1);
  }
#endif

  //ezTime library sync
  events(); 
  
  //Process every second
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;

    checkScheduledMessages();
    checkCountdown();

    // Skip display updates if a web request is active to prevent blocking.
    // (showText() itself is a no-op when no units are detected, see scanI2CBus())
    if (!webRequestActive) {
      //Mode Selection
      if (deviceMode == DEVICE_MODE_TEXT || deviceMode == DEVICE_MODE_COUNTDOWN) {
        showText(inputText);
      }
      else if (deviceMode == DEVICE_MODE_DATE) {
        showText(timezone.dateTime(dateFormat));
      }
      else if (deviceMode == DEVICE_MODE_CLOCK) {
        showText(timezone.dateTime(clockFormat));
      }
    } else {
      // Web request is active, skip display update to keep web server responsive
      yield(); // Give web server time to process
    }
  }
}

// Scans the I2C bus, reports all found devices, and (re-)determines unitsAmount: the number of
// units found sequentially starting at address 0, stopping at the first missing address - this
// mirrors the DIP switches' own "must be sequential from 0" requirement (see README "Set Unit
// Address"), so a gap here means a wiring/configuration problem, not a legitimately sparse set
// of units. Sets the global unitsAmount as a side effect and also returns it for convenience.
int scanI2CBus() {
  SerialPrintln("========================================");
  SerialPrintln("I2C Bus Scan - Checking addresses 0-15");
  SerialPrintln("========================================");

  int foundCount = 0;
  bool foundUnits[16] = {false}; // Track which addresses are found
  int unitStatus[16] = {-2}; // Track status of each address (-2 = not found, -1 = sleeping, 0 = ready, 1 = busy)

  // Scan all possible I2C addresses (0-15)
  for (int address = 0; address < 16; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      foundUnits[address] = true;
      foundCount++;

      SerialPrint("✓ Found device at address ");
      SerialPrint(address);

      Wire.requestFrom(address, 1, 1);
      if (Wire.available()) {
        int status = Wire.read();
        unitStatus[address] = status;

        SerialPrint(" - Status: ");
        SerialPrint(status);

        if (status == 0) {
          SerialPrintln(" (READY)");
        } else if (status == 1) {
          SerialPrintln(" (BUSY/MOVING)");
        } else if (status == -1) {
          SerialPrintln(" (SLEEPING)");
        } else {
          SerialPrintln(" (UNKNOWN)");
        }
      } else {
        SerialPrintln(" - No status response");
        unitStatus[address] = -3; // No response
      }
    } else if (error == 2) {
      // NACK on address - device not found (normal for unused addresses)
      // Don't log this to reduce noise
    } else if (error == 4) {
      SerialPrint("⚠ Unknown I2C error at address ");
      SerialPrintln(address);
    }

    yield();
    delay(10);
  }

  // Determine unitsAmount: count contiguous "found" addresses starting at 0
  int detectedAmount = 0;
  while (detectedAmount < 16 && foundUnits[detectedAmount]) {
    detectedAmount++;
  }
  unitsAmount = detectedAmount;

  // Summary report
  SerialPrintln("");
  SerialPrintln("--- Scan Summary ---");
  SerialPrint("Total devices found: ");
  SerialPrintln(foundCount);
  SerialPrint("Detected unitsAmount (sequential from address 0): ");
  SerialPrintln(unitsAmount);
  SerialPrintln("");

  SerialPrintln("Unit Status:");
  for (int i = 0; i < unitsAmount; i++) {
    SerialPrint("  Unit ");
    SerialPrint(i);
    SerialPrint(" (address ");
    SerialPrint(i);
    SerialPrint("): ✓ CONNECTED");

    if (unitStatus[i] == 0) {
      SerialPrintln(" - Ready");
    } else if (unitStatus[i] == 1) {
      SerialPrintln(" - Busy");
    } else if (unitStatus[i] == -1) {
      SerialPrintln(" - Sleeping");
    } else if (unitStatus[i] == -3) {
      SerialPrintln(" - No status response");
    } else {
      SerialPrint(" - Status code: ");
      SerialPrintln(unitStatus[i]);
    }
  }

  // Anything found beyond the contiguous run is likely a gap earlier in the sequence
  // (a missing/misconfigured unit) rather than genuinely "extra" devices
  if (foundCount > unitsAmount) {
    SerialPrintln("");
    SerialPrintln("⚠ Devices found beyond the contiguous 0..N-1 range - check for a gap (a missing/misconfigured unit earlier in the sequence?):");
    for (int i = unitsAmount; i < 16; i++) {
      if (foundUnits[i]) {
        SerialPrint("  Address ");
        SerialPrint(i);
        SerialPrintln(" - responds, but out of sequence");
      }
    }
  }

  SerialPrintln("");
  if (unitsAmount == 0) {
    SerialPrintln("⚠ WARNING: No units detected at address 0. Display functionality is unavailable until this is fixed. Check:");
    SerialPrintln("  1. DIP switch settings (address 0 must be assigned to a unit)");
    SerialPrintln("  2. I2C wiring (SDA/SCL connections)");
    SerialPrintln("  3. Power connections");
    SerialPrintln("  Use the Admin page's \"Rescan Units\" button once fixed, no reboot required.");
  } else {
    SerialPrintln("✓ " + String(unitsAmount) + " unit(s) detected and ready.");
  }

  SerialPrintln("========================================");
  return unitsAmount;
}

//Gets all the currently stored calues from memory in a JSON object
String getCurrentSettingValues() {
  unsigned long funcStart = millis();
  const unsigned long MAX_FUNCTION_TIME = 5000; // 5 second max for this function (reduced from 10s)
  SerialPrintln("DEBUG: getCurrentSettingValues() - Starting");
  
  // Yield immediately to allow web server to process
  yield();
  
  JsonDocument document;
  unsigned long afterDoc = millis();
  SerialPrintln("DEBUG: getCurrentSettingValues() - JsonDocument created (" + String(afterDoc - funcStart) + "ms)");

  document["timezoneOffset"] = timezone.getOffset();
  document["unitCount"] = unitsAmount;
  document["alignment"] = alignment;
  document["flapSpeed"] = flapSpeed;
  document["deviceMode"] = deviceMode;
  document["version"] = espVersion;
  document["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
  document["lastWrittenText"] = lastWrittenText;
  document["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());
  
  // WiFi status information - read RSSI quickly (reduced readings for faster response)
  if (WiFi.status() == WL_CONNECTED) {
    document["wifiStatus"] = "Connected";
    // Read RSSI 2 times for faster response (was 5, but adds delay)
    long rssiSum = 0;
    int rssiReadings = 2;
    for (int i = 0; i < rssiReadings; i++) {
      rssiSum += WiFi.RSSI();
      if (i < rssiReadings - 1) {
        delay(5); // Reduced from 10ms
        yield();
      }
    }
    document["wifiRssi"] = rssiSum / rssiReadings;
    document["wifiIp"] = WiFi.localIP().toString();
  } else {
    document["wifiStatus"] = "Disconnected";
    document["wifiRssi"] = 0;
    document["wifiIp"] = "";
  }
  unsigned long afterBasicFields = millis();
  SerialPrintln("DEBUG: getCurrentSettingValues() - Basic fields set (" + String(afterBasicFields - afterDoc) + "ms)");

  int scheduledCount = scheduledMessages.size();
  SerialPrintln("DEBUG: getCurrentSettingValues() - Processing " + String(scheduledCount) + " scheduled messages");
  unsigned long beforeScheduled = millis();
  
  // Process scheduled messages with yields every few items to keep web server responsive
  // Also check for timeout to prevent hanging
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    // Check for timeout - if we're taking too long, skip remaining messages
    if (millis() - funcStart > MAX_FUNCTION_TIME) {
      SerialPrintln("DEBUG: WARNING - getCurrentSettingValues() timeout, skipping remaining scheduled messages");
      break;
    }
    
    ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];
    
    document["scheduledMessages"][scheduledMessageIndex]["scheduledDateTimeUnix"] = scheduledMessage.ScheduledDateTimeUnix;
    document["scheduledMessages"][scheduledMessageIndex]["message"] = scheduledMessage.Message;
    document["scheduledMessages"][scheduledMessageIndex]["showIndefinitely"] = scheduledMessage.ShowIndefinitely;
    
    // Yield every 5 messages to keep web server responsive, or on every message if < 10 total
    if (scheduledMessageIndex % 5 == 0 || scheduledCount < 10) {
      yield(); // Allow web server to process requests during loop
    }
  }
  unsigned long afterScheduled = millis();
  SerialPrintln("DEBUG: getCurrentSettingValues() - Scheduled messages processed (" + String(afterScheduled - beforeScheduled) + "ms)");

#if OTA_ENABLE == true
  document["otaEnabled"] = true;
  document["isInOtaMode"] = isInOtaMode;
#else
  document["otaEnabled"] = false;
#endif

#if WIFI_USE_DIRECT == false
  document["wifiSettingsResettable"] = true;
#else
  document["wifiSettingsResettable"] = false;
#endif
  
  unsigned long beforeSerialize = millis();
  
  // Check timeout before serialization
  if (millis() - funcStart > MAX_FUNCTION_TIME) {
    SerialPrintln("DEBUG: WARNING - getCurrentSettingValues() timeout before serialization, returning minimal response");
    JsonDocument minimalDoc;
    minimalDoc["timezoneOffset"] = timezone.getOffset();
    minimalDoc["unitCount"] = unitsAmount;
    minimalDoc["alignment"] = alignment;
    minimalDoc["flapSpeed"] = flapSpeed;
    minimalDoc["deviceMode"] = deviceMode;
    minimalDoc["version"] = espVersion;
    minimalDoc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
    minimalDoc["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    minimalDoc["wifiIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    minimalDoc["scheduledMessages"] = JsonArray(); // Empty array
    String minimalJson;
    serializeJson(minimalDoc, minimalJson);
    return minimalJson;
  }
  
  yield(); // Yield before potentially long serialization
  String jsonString;
  serializeJson(document, jsonString);
  yield(); // Yield after serialization to allow web server to process
  unsigned long afterSerialize = millis();
  SerialPrintln("DEBUG: getCurrentSettingValues() - JSON serialized (" + String(afterSerialize - beforeSerialize) + "ms, " + String(jsonString.length()) + " bytes)");
  SerialPrintln("DEBUG: getCurrentSettingValues() - Total time: " + String(afterSerialize - funcStart) + "ms");

  return jsonString;
}
