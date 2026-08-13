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
#define OTA_ENABLE          false    //Option to enable OTA functionality via the Arduino IDE (same WiFi network + IDE required)
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
//DIAGNOSTIC, 2026-08-13: was 3 (status byte + 2 offset bytes). Unit.ino's requestEvent() now
//also sends a phase byte + 2 progress bytes (see currentPhase there) so we can tell "still
//legitimately busy, making progress" from "actually stuck" - must match Unit.ino exactly, a
//unit still running the old 3-byte firmware will fail every read until it's reflashed too.
#define ANSWER_SIZE         6
#define FLAP_AMOUNT         45      //Amount of Flaps in each unit
#define MIN_SPEED           1       //Min Speed
#define MAX_SPEED           12      //Max Speed
#define UNIT_STEPS          2038    //Must match STEPS in Unit.ino - used to validate offset values sent from the Admin UI
#define UNIT_BOOTLOADER_CMD 255     //Must match CMD_ENTER_BOOTLOADER in Unit.ino
#define UNIT_BOOTLOADER_BASE_ADDRESS 0x29 //Start of the range scanned for a unit's twiboot bootloader (0x29-0x38).
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

//Change if you want to have an Over The Air (OTA) Password for updates
const char* otaPassword = "0424";

//Change this to your timezone, use the TZ database name
//https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
const char* timezoneString = "America/Los_Angeles";

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

//Bench-tested on real hardware (2026-08-11): triggering an I2C bus scan/read directly from
//inside an AsyncWebServer request callback reliably hangs the ESP for several seconds and then
//crashes/reboots it - even for a scan that finds zero devices (pure address probes, no actual
//data read). The *identical* scanI2CBus()/readUnitInfo() calls are rock solid when made from
//setup() or loop(). Suspected cause: AsyncTCP callbacks run nested inside loop()'s own call
//stack with much less headroom, and Wire's clock-stretch handling wants more of it than that
//leaves free. Fix: handlers that need to touch the I2C bus never call Wire themselves - they
//just remember the pending AsyncWebServerRequest here, and loop() (top-level stack) does the
//actual bus work and calls request->send() itself once done. Only one of each can be in flight
//at a time (single-operator admin page); a second request while one is pending gets an
//immediate 503 rather than silently replacing/dropping the first.
AsyncWebServerRequest* pendingRescanRequest = nullptr;
AsyncWebServerRequest* pendingUnitsInfoRequest = nullptr;
AsyncWebServerRequest* pendingLegacyI2CScanRequest = nullptr;

//Same reasoning, for the twiboot/bootloader endpoints (only relevant when
//UNIT_BOOTLOADER_ENABLE is true - see ServiceUnitFirmwareFunctions.ino).
AsyncWebServerRequest* pendingFirmwarePushRequest = nullptr;
int pendingFirmwarePushUnitIndex = -1;
AsyncWebServerRequest* pendingBootloaderProbeRequest = nullptr;
int pendingBootloaderProbeUnitIndex = -1;

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
bool showDebugPage = true; // Default to showing debug page on first boot
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
#endif
//SECURITY FIX, 2026-08-13: same reasoning, for /unit-firmware-upload - see the comment there.
#if UNIT_BOOTLOADER_ENABLE == true
bool unitFirmwareUploadAuthFailed = false;
#endif

// LED Debugging variables
String debugStatus = "";

// Serial log buffer for web interface (circular buffer, stores last 100 messages).
// DIAGNOSTIC FIX, 2026-08-13: message used to be a String. addToSerialLog() runs on
// *every single* SerialPrint()/SerialPrintln() call (regardless of SERIAL_ENABLE - see
// HelpersSerialHandling.ino), cycling through 100 slots with wildly varying message
// lengths - a textbook heap-fragmentation pattern on the ESP8266's small heap, and the
// leading remaining suspect for this session's "crashes eventually, at a seemingly
// random point" bug (after the /log stack-overflow fix in 1483f66 measurably improved
// but didn't fully eliminate it). Switched to a fixed-size char buffer: no allocation,
// no fragmentation, at the cost of a flat ~15KB of static RAM (100 * 150 bytes) instead
// of scattered heap churn. Longer messages are truncated, not an error for a debug log.
#define SERIAL_LOG_SIZE 100
#define SERIAL_LOG_MESSAGE_MAX 150
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
#if SERIAL_ENABLE == true
  //Setup so we can see serial messages
  Serial.begin(SERIAL_BAUDRATE);
#elif !defined(FAE_MOD)
  //For ESP01 only
  Wire.begin(1, 3); 
  
  //De-activate I2C if debugging the ESP, otherwise serial does not work
  //Wire.begin(D1, D2); //For NodeMCU testing only SDA=D1 and SCL=D2
#endif

#ifdef FAE_MOD
  //Fae Mod
  Wire.begin(4, 5);
#endif

#if UNIT_BOOTLOADER_ENABLE == true
  //twiboot (compiled with USE_CLOCKSTRETCH=1, see Unit/BOOTLOADER_SETUP.md) holds SCL low
  //while a flash page write is in progress. Give it comfortably more time than the ATmega328p's
  //worst-case erase+write duration so a page write never gets misread as a bus error.
  Wire.setClockStretchLimit(25000);
#endif

  // Initialize LED for debugging
#if LED_ENABLE == true
  pinMode(LED_DEBUG_PIN, OUTPUT);
  ledOff();
  blinkLed(1, 100, 50); // Quick blink to show startup
#endif

  // I2C bus scan on startup - determines unitsAmount. Retry a few times in case the bus
  // hasn't fully settled yet (500ms should normally be plenty, this is just extra slack).
  delay(500); // Give I2C bus time to stabilize
  SerialPrintln("");
  SerialPrintln("=== I2C Bus Scan on Startup ===");
  for (int scanAttempt = 0; scanAttempt < 3 && unitsAmount == 0; scanAttempt++) {
    if (scanAttempt > 0) {
      SerialPrintln("No units found, retrying scan...");
      delay(300);
    }
    scanI2CBus();
  }
  SerialPrintln("");

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
    
    // Set sync interval but don't block
    setInterval(60); // Sync every 60 seconds
    setDebug(INFO); // Set to INFO level for debugging
    
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
    
    timezone.setLocation(timezoneString);
    SerialPrintln("DEBUG: Timezone set to: " + String(timezoneString));
    
    //Load various variables
    debugStatus = "File System Init";
    SerialPrintln("DEBUG: Status = " + debugStatus);
    blinkLed(6, 200, 100); // 6 blinks = File system init
    initialiseFileSystem();
    loadValuesFromFileSystem();
    
    // Load debug mode setting
#if DEBUG_ENABLE == true
    String debugModeSetting = readFile(LittleFS, debugModePath, "true");
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
    
    webServer.serveStatic("/", LittleFS, "/");
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
        html += "  xhr.open('GET', '/exit-debug-mode', true);";
        html += "  xhr.send();";
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

    webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
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
      minimalDoc["otaEnabled"] = false;
      
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
      
      String minimalJson;
      serializeJson(minimalDoc, minimalJson);
      
      unsigned long totalTime = millis() - settingsStart;
      SerialPrintln("DEBUG: Settings response ready in " + String(totalTime) + "ms, JSON size: " + String(minimalJson.length()) + " bytes");
      
      request->send(200, "application/json", minimalJson);
      minimalJson = String();
      
      SerialPrintln("DEBUG: Settings response sent (total: " + String(millis() - settingsStart) + "ms)");
      
      // Clear flag after request completes
      webRequestActive = false;
    });
    
    webServer.on("/health", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request for Health Check Received");
      request->send(200, "text/plain", "Healthy");
    });
    
    // Simple test endpoint that responds immediately
    webServer.on("/test", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Test endpoint called");
      request->send(200, "application/json", "{\"status\":\"ok\",\"time\":" + String(millis()) + "}");
    });
    
    webServer.on("/log", HTTP_GET, [](AsyncWebServerRequest * request) {
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
      
      String jsonString;
      serializeJson(document, jsonString);
      
      // Check if serialization succeeded
      if (jsonString.length() == 0) {
        SerialPrintln("ERROR: JSON serialization failed for /log endpoint");
        request->send(500, "application/json", "{\"error\":\"Serialization failed\"}");
        return;
      }
      
      request->send(200, "application/json", jsonString);
    });
    
    // I2C bus scanner endpoint for diagnostics
    webServer.on("/i2c-scan", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: I2C scan requested");
      //Deferred to loop() - see the comment on pendingLegacyI2CScanRequest near unitsAmount.
      if (pendingLegacyI2CScanRequest != nullptr) {
        request->send(503, "text/plain", "Another I2C scan is already in progress, try again shortly");
        return;
      }
      pendingLegacyI2CScanRequest = request;
    });

    // Admin: read live status + calibration offset for every unit.
    //Deferred to loop() - see the comment on pendingUnitsInfoRequest near unitsAmount.
    webServer.on("/admin/units", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Admin units request received");
      if (pendingUnitsInfoRequest != nullptr) {
        request->send(503, "text/plain", "Another units request is already in progress, try again shortly");
        return;
      }
      pendingUnitsInfoRequest = request;
    });

    // Admin: re-run the I2C scan to pick up units that were connected/fixed after boot,
    // without needing a full reboot. Updates the global unitsAmount used everywhere else.
    //Deferred to loop() - see the comment on pendingRescanRequest near unitsAmount.
    webServer.on("/admin/rescan-units", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Admin rescan-units request received");
      if (pendingRescanRequest != nullptr) {
        request->send(503, "text/plain", "Another rescan is already in progress, try again shortly");
        return;
      }
      pendingRescanRequest = request;
    });

    // Admin: set a new calibration offset on a single unit, unit recalibrates immediately
    webServer.on("/admin/unit-offset", HTTP_POST, [](AsyncWebServerRequest * request) {
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

#if UNIT_BOOTLOADER_ENABLE == true
    // Admin: upload a compiled Unit.ino firmware image (raw AVR flash .bin, NOT the ESPMaster's own
    // .bin) to be pushed to a unit afterwards via /unit-firmware-push. See Unit/BOOTLOADER_SETUP.md.
    //SECURITY FIX, 2026-08-13: this (and /unit-firmware-push below) had no access control at all,
    //unlike /firmware-update (the ESPMaster's own OTA endpoint), which already requires
    //X-OTA-Password when one is configured. Anyone on the LAN could push arbitrary code to run on
    //the Unit's AVR - now gated the same way, reusing the same password/header for consistency.
    webServer.on("/unit-firmware-upload", HTTP_POST,
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
    // Deferred to loop() - see the comment on pendingRescanRequest near unitsAmount; this does
    // I2C reads (verify-after-write) just like the endpoints fixed there, so the same handler-
    // stack crash applies here too.
    //SECURITY FIX, 2026-08-13: was unauthenticated - see the comment on /unit-firmware-upload.
    webServer.on("/unit-firmware-push", HTTP_POST, [](AsyncWebServerRequest * request) {
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
      pendingFirmwarePushRequest = request;
    });

    // Admin/diagnostic: send a unit into bootloader mode and report which I2C address it answers
    // on, without writing anything to flash. Used to verify the DIP-jumper address is read live
    // by twiboot (see BOOTLOADER_SETUP.md's jumper-live-test) without risking a real firmware
    // push. Deferred to loop() for the same reason as /unit-firmware-push above.
    webServer.on("/admin/unit-bootloader-probe", HTTP_POST, [](AsyncWebServerRequest * request) {
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
      pendingBootloaderProbeRequest = request;
    });
#endif

#if WEB_FIRMWARE_UPDATE_ENABLE == true
    // Admin: browser-based firmware upload (no Arduino IDE required).
    // Password (if otaPassword is set) is passed via the X-OTA-Password header so it's available
    // before the file data starts streaming in, regardless of multipart field order.
    webServer.on("/firmware-update", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Runs once the full upload has been received and handled by the upload callback below
        if (firmwareUpdateAuthFailed) {
          firmwareUpdateAuthFailed = false;
          request->send(401, "text/plain", "Invalid password");
          return;
        }

        bool success = !Update.hasError();
        AsyncWebServerResponse* response = request->beginResponse(success ? 200 : 500, "text/plain",
          success ? "OK - Firmware written, rebooting now" : "Update failed, check the serial log for details");
        response->addHeader("Connection", "close");
        request->send(response);

        if (success) {
          SerialPrintln("Firmware update successful, rebooting...");
          isPendingReboot = true;
        } else {
          SerialPrintln("Firmware update failed");
        }
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          SerialPrintln("DEBUG: Firmware upload starting: " + filename);
          firmwareUpdateAuthFailed = false;

          //Only enforce the password if one has actually been configured
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (providedPassword != String(otaPassword)) {
              SerialPrintln("Firmware upload rejected: invalid password");
              firmwareUpdateAuthFailed = true;
              return;
            }
          }

          uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            SerialPrintln("Firmware upload: Update.begin() failed");
            Update.printError(Serial);
          }
        }

        if (firmwareUpdateAuthFailed) {
          return; //Password rejected, ignore the rest of the upload - final response already handled above
        }

        if (Update.write(data, len) != len) {
          SerialPrintln("Firmware upload: Update.write() failed");
          Update.printError(Serial);
        }

        if (final) {
          if (Update.end(true)) {
            SerialPrintln("DEBUG: Firmware upload finished successfully. Total bytes: " + String(index + len));
          } else {
            SerialPrintln("Firmware upload: Update.end() failed");
            Update.printError(Serial);
          }
        }
      }
    );
#endif

#if DEBUG_ENABLE == true
    webServer.on("/exit-debug-mode", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request to exit debug mode received");
      showDebugPage = false;
      writeFile(LittleFS, debugModePath, "false");
      request->send(200, "text/plain", "OK - Debug mode disabled. Startup debug page, error status, and debug logs will be hidden.");
    });
    
    webServer.on("/enable-debug-mode", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request to enable debug mode received");
      showDebugPage = true;
      writeFile(LittleFS, debugModePath, "true");
      request->send(200, "text/plain", "OK - Debug mode enabled. This enables: startup debug page, error status panel, page load debug log, and serial debug log. Refresh page to see changes.");
    });
#endif
    
    //SECURITY FIX, 2026-08-13: this and reset-units/ota/reset-wifi below used to be HTTP_GET,
    //reachable via a plain <a href> link with only a client-side confirm() as a safety net.
    //That's not real protection - any page loaded in the same browser (e.g. an <img> tag
    //pointing here on a totally unrelated site) could trigger these state-changing actions
    //without any user interaction at all (classic CSRF-via-GET). Switched to POST; index.html's
    //links became small <form method="post"> submissions instead, which browsers still
    //navigate on submit exactly like the old GET links did, so the response pages are unchanged.
    webServer.on("/reboot", HTTP_POST, [](AsyncWebServerRequest * request) {
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
    
    webServer.on("/reset-units", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to Reset Units Received");
      
      //This will be picked up in the loop
      isPendingUnitsReset = true;
      
      request->redirect("/?is-resetting-units=true");
    });

    webServer.on("/scheduled-message/remove", HTTP_DELETE, [](AsyncWebServerRequest * request) {
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

    webServer.on("/", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("DEBUG: Request Post of Form Received");    

      bool submissionError = false;
      
      //DIAGNOSTIC FIX, 2026-08-13: this used to be
      //`bool newMessageScheduleEnabledValue, newMessageScheduleShowIndefinitely = false;` -
      //in C++ that only initializes the *second* variable; newMessageScheduleEnabledValue was
      //left as uninitialized stack garbage, so `if (newMessageScheduleEnabledValue)` further
      //down could unpredictably take the "it's a scheduled message" branch on a plain text
      //update, depending on whatever was previously on this handler's stack frame.
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
            newFlapSpeedValue = p->value().c_str();
          }

          //HTTP POST inputText value
          if (p->name() == PARAM_INPUT_TEXT) {
            newInputTextValue = p->value().c_str();
            hasNewInputTextValue = true;
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
        request->redirect("/?invalid-submission=" + true);
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

        //Redirect so that we don't have the "re-submit form" problem in browser for refresh
        request->redirect("/");
      }
    });

#if OTA_ENABLE == true
    webServer.on("/ota", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln("Request to start OTA mode received");
      
      //Create HTML page to explain OTA
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += "<font face='arial'><h1>Split Flap - OTA Update Mode</h1>";
      html += "<p>OTA mode has been started. You can now update your module via WiFI. Open your Arduino IDE and select the new port in \"Tools\" menu and upload the your sketch as normal!<p>";
      html += "<p>Open your Arduino IDE and select the new port in \"Tools\" menu and upload the your sketch as normal!</p>";
      html += "<p>After you have carried out your update, the system will automatically be rebooted. You can go to the main home page after this time by clicking the button below or going to '/'.</p>";
      html += "<p>You can take the system out of this mode by clicking the button to reboot below.</p>";
      //BUG FIX, 2026-08-13: both links here used to end with a stray `\")\"` before the closing
      //quote (e.g. href="http://<ip>")" ) - a malformed URL with a literal trailing `")`. Also,
      ///reboot is POST-only now (see the comment on that handler) so a plain <a href> link to it
      //would just get a 404 - using a tiny inline form instead, same pattern as index.html.
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += "<form action=\"/reboot\" method=\"post\" style=\"display:inline\"><button type=\"submit\">Reboot</button></form>";
      html += "</font>";
      html += "</div>";

      request->send(200, "text/html", html);
  
      if (!isInOtaMode) {
        SerialPrintln("Setting OTA Hostname");
        ArduinoOTA.setHostname("Split-Flap-OTA");

        //If there is a password set, disabled by default for ease
        if (otaPassword != "") {
          ArduinoOTA.setPassword(otaPassword);
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
    webServer.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest * request) {
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
  //Handle any I2C admin requests deferred here by the web handlers (see the comment on
  //pendingRescanRequest near unitsAmount for why this can't just run in the handler itself).
  //Checked first, before anything else in loop(), so a pending request gets served as soon as
  //possible.
  if (pendingRescanRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingRescanRequest;
    pendingRescanRequest = nullptr;

    int detectedAmount = scanI2CBus();

    JsonDocument doc;
    doc["unitsAmount"] = detectedAmount;
    String jsonString;
    serializeJson(doc, jsonString);
    deferredRequest->send(200, "application/json", jsonString);
  }

  if (pendingUnitsInfoRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingUnitsInfoRequest;
    pendingUnitsInfoRequest = nullptr;

    JsonDocument doc;
    doc["unitsAmount"] = unitsAmount;
    for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
      int status = -2;
      int offset = -1;
      int phase = -1;
      int progress = -1;
      bool online = readUnitInfo(unitIndex, status, offset, phase, progress);

      doc["units"][unitIndex]["index"] = unitIndex;
      doc["units"][unitIndex]["online"] = online;
      doc["units"][unitIndex]["status"] = status;
      doc["units"][unitIndex]["offset"] = offset;
      //DIAGNOSTIC, 2026-08-13: phase (0=idle, 1=calibrating, 2=rotating) + progress (steps done
      //in the current phase) - see currentPhase in Unit.ino.
      doc["units"][unitIndex]["phase"] = phase;
      doc["units"][unitIndex]["progress"] = progress;

      yield();
    }
    String jsonString;
    serializeJson(doc, jsonString);
    deferredRequest->send(200, "application/json", jsonString);
  }

  if (pendingLegacyI2CScanRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingLegacyI2CScanRequest;
    pendingLegacyI2CScanRequest = nullptr;

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
    String jsonString;
    serializeJson(doc, jsonString);
    deferredRequest->send(200, "application/json", jsonString);
  }

#if UNIT_BOOTLOADER_ENABLE == true
  if (pendingFirmwarePushRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingFirmwarePushRequest;
    int unitIndex = pendingFirmwarePushUnitIndex;
    pendingFirmwarePushRequest = nullptr;

    SerialPrintln("Pushing firmware to Unit " + String(unitIndex) + "...");
    String resultJson;
    bool success = pushFirmwareToUnit(unitIndex, resultJson);

    deferredRequest->send(success ? 200 : 500, "application/json", resultJson);
  }

  if (pendingBootloaderProbeRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingBootloaderProbeRequest;
    int unitIndex = pendingBootloaderProbeUnitIndex;
    pendingBootloaderProbeRequest = nullptr;

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
      doc["note"] = "Nothing was written. The unit returns to its normal app on its own within about a second of being left idle.";
    }

    String jsonString;
    serializeJson(doc, jsonString);
    deferredRequest->send(200, "application/json", jsonString);
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

  // Monitor WiFi connection and attempt reconnection if lost
  if (isWifiConfigured) {
    static unsigned long lastWiFiCheck = 0;
    static int reconnectAttempts = 0;
    const unsigned long wifiCheckInterval = 10000; // Check every 10 seconds
    
    if (millis() - lastWiFiCheck > wifiCheckInterval) {
      lastWiFiCheck = millis();
      
      if (WiFi.status() != WL_CONNECTED) {
        SerialPrintln("DEBUG: WiFi connection lost! Status: " + String(WiFi.status()));
        reconnectAttempts++;
        
        if (reconnectAttempts <= 3) {
          SerialPrintln("DEBUG: Attempting to reconnect (attempt " + String(reconnectAttempts) + "/3)...");
          WiFi.disconnect();
          delay(100);
          yield();

          //BUG FIX, 2026-08-13: this used to unconditionally call
          //`WiFi.begin(wifiDirectSsid, wifiDirectPassword)` here, regardless of WIFI_USE_DIRECT.
          //Those two constants are only meant for the WIFI_USE_DIRECT==true code path in
          //initWiFi() - in the (default, and this project's actual) WIFI_USE_DIRECT==false/
          //WiFiManager setup, they're just leftover placeholder values for a different network
          //entirely, so every reconnect attempt was guaranteed to fail. That silently turned
          //any ordinary transient WiFi drop into a permanent one: 3 doomed attempts (up to ~30s
          //blocked) followed by isWifiConfigured=false with nothing left to ever retry, until a
          //manual power cycle - this was very likely responsible for a lot of what looked like
          //ESP crashes/hangs this session. Use WiFi.begin() with no arguments instead, which
          //reconnects with the ESP8266 SDK's own persisted last-used credentials (set correctly
          //by WiFiManager's autoConnect() at boot) - works for both WiFi modes.
#if WIFI_USE_DIRECT == true
          WiFi.begin(wifiDirectSsid, wifiDirectPassword);
#else
          WiFi.begin();
#endif
          
          int reconnectTimeout = 10; // 10 second timeout for reconnection
          int reconnectCount = 0;
          while (WiFi.status() != WL_CONNECTED && reconnectCount < reconnectTimeout) {
            delay(500);
            yield();
            delay(500);
            yield();
            reconnectCount++;
          }
          
          if (WiFi.status() == WL_CONNECTED) {
            SerialPrintln("DEBUG: WiFi reconnected successfully! IP: " + WiFi.localIP().toString());
            reconnectAttempts = 0;
          } else {
            SerialPrintln("DEBUG: Reconnection attempt " + String(reconnectAttempts) + " failed");
            if (reconnectAttempts >= 3) {
              SerialPrintln("DEBUG: Max reconnection attempts reached. Marking WiFi as not configured.");
              isWifiConfigured = false;
            }
          }
        }
      } else {
        // Connection is good, reset reconnect counter
        if (reconnectAttempts > 0) {
          reconnectAttempts = 0;
        }
      }
    }
  }
  
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

  if (unitsAvailable && isPendingUnitsReset) {
    SerialPrintln("Reseting Units now...");

    //Blank out the message
    String blankOutText1 = createRepeatingString('-');
    showText(blankOutText1);
    delay(2000);

    //Do just enough to do a full iteration which triggers the re-calibration
    String blankOutText2 = createRepeatingString('.');
    showText(blankOutText2);

    //We did a reset!
    isPendingUnitsReset = false;

    SerialPrintln("Done Units Reset!");
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
