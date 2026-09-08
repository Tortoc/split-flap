/*
  SplitFlap ESPMaster firmware.
  Based on https://github.com/JonnyBooker/split-flap, built on the work of David Königsmann
  (https://github.com/Dave19171/split-flap). Licensed under GNU:
  https://github.com/JonnyBooker/split-flap/blob/master/LICENSE
*/

//--- Feature flags ---
#define SERIAL_ENABLE       true   //enable serial debug messages
#define UNIT_CALLS_DISABLE  false  //disable I2C calls to units (debug the ESP alone)
#define WEB_FIRMWARE_UPDATE_ENABLE true //browser-based firmware upload from the Admin page
#define UNIT_BOOTLOADER_ENABLE true //push Unit.ino updates over I2C via twiboot - needs every
                                    //unit pre-flashed with twiboot, see Unit/BOOTLOADER_SETUP.md
#define UNITS_AMOUNT_MAX    64     //matches the 6-bit DIP switch addressing range; actual amount
                                    //is auto-detected at startup via scanI2CBus()
#define SERIAL_BAUDRATE     115200
#define WIFI_USE_DIRECT     false  //false = AP config portal (WiFiManager), true = direct connect
#define LED_ENABLE          true
#define DEBUG_ENABLE true  //startup debug page, error panel, page-load log, serial debug log,
                           //and whether LED_ENABLE actually blinks - gated at runtime by
                           //showDebugPage (POST /api/debug-mode, persisted in /debugmode.txt)

#define WIFI_STATIC_IP      false //try a router-assigned static IP instead of DHCP
#define FAE_MOD                   //modified PCB with an ESP-12F module

//ESP01 GPIO1/TX is used for I2C SDA and GPIO3/RX for I2C SCL (Wire.begin(1, 3)) - so the debug LED
//goes on GPIO2 instead of the built-in GPIO1 LED. VCC must be 3.3V only.
#define LED_DEBUG_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

//LED blink codes during startup (only while debug mode is on):
//1=WiFi connecting, 2=WiFi connected, 3=NTP starting, 4=NTP success, 5=NTP failed,
//6=filesystem init, 7=web server starting, 8=ready. Continuous=error.

//--- Constants - only change if you know what you're doing ---
#define ANSWER_SIZE         6 //status + offset + phase + progress bytes, must match Unit.ino's requestEvent()
#define ANSWER_SIZE_LIGHT   3 //status + offset only, see readUnitInfo()'s 3-arg overload
#define FLAP_AMOUNT         45
#define MIN_SPEED           1
#define MAX_SPEED           12
#define UNIT_STEPS          2038    //must match STEPS in Unit.ino
#define UNIT_BOOTLOADER_CMD 255     //must match CMD_ENTER_BOOTLOADER in Unit.ino
#define UNIT_BOOTLOADER_BASE_ADDRESS 0x29 //start of the twiboot bootloader address range (0x29-0x68)
#define CRASH_LOG_PATH "/crash-log.jsonl"

//Named so ServiceI2CRecoveryFunctions.ino's bus-recovery routine can reuse the exact same pins.
#ifdef FAE_MOD
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#else
#define I2C_SDA_PIN 1
#define I2C_SCL_PIN 3
#endif

//--- Libraries ---
#if WIFI_USE_DIRECT == false
//WEBSERVER_H guard needed for WiFiManager compatibility, see
//https://github.com/me-no-dev/ESPAsyncWebServer/issues/418#issuecomment-667976368
#define WEBSERVER_H
#include <WiFiManager.h>
#endif

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
//twi.h's raw twi_writeTo() is used for twiboot flash-page writes instead of Wire - a page write is
//132 bytes, which exceeds Wire's 128-byte TX buffer and silently drops the tail.
#include <twi.h>
#include <user_interface.h> //struct rst_info, for the crash reporter
#include <lwip/priv/tcp_priv.h> //exposes tcp_active_pcbs/etc. for /api/tcp-status
#include "Classes.h"
#include "LittleFS.h"

//--- User-facing settings ---
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

//Quiet Hours - parks the display (blank, no motor movement) during a daily time window, see
//isQuietHoursActive() (ServiceQuietHoursFunctions.ino). Start/end are "HH:mm" 24h local time;
//the window may span midnight (e.g. 22:00-07:00).
bool quietHoursEnabled = false;
String quietHoursStart = "22:00";
String quietHoursEnd = "07:00";
const char* quietHoursEnabledPath = "/quiethoursenabled.txt";
const char* quietHoursStartPath = "/quiethoursstart.txt";
const char* quietHoursEndPath = "/quiethoursend.txt";

//User-facing date/clock format (e.g. "dd.MM.yyyy"), translated to ezTime's syntax at render time
//by convertUserDateFormatToEzTime().
String dateFormat = "dd.MM.yyyy";
String clockFormat = "HH:mm";

//Countdown display prefix/suffix, shared by standalone Countdown mode and Rotation's countdown
//items. Empty means "don't show that part" - see buildCountdownText().
String countdownPrefix = "";
String countdownSuffix = "Tage";

//Countup display prefix/suffix - same idea as Countdown above, but for days elapsed *since* a
//date instead of days remaining *until* one. See buildCountupText().
String countupPrefix = "";
String countupSuffix = "Tage";

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

//--- Core state - normal running of the system, be careful changing these ---
//The current version of code to display on the UI
const char* espVersion = "2.3.0-Scott";

//All the letters on the units that we have to be displayed. You can change these if it so pleases at your own risk
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
int displayState[UNITS_AMOUNT_MAX];
unsigned long previousMillis = 0;
int unitsAmount = 0; //auto-detected via I2C scan, sequential from 0 - see scanI2CBus()

//Calling Wire directly from an AsyncWebServer callback crashes the ESP (too little stack for I2C
//clock-stretch handling) - these just stash the request, loop() does the I2C work and sends the
//response itself. One in flight at a time; a second request while one's pending gets 503.
AsyncWebServerRequest* pendingRescanRequest = nullptr;
AsyncWebServerRequest* pendingUnitsInfoRequest = nullptr;
AsyncWebServerRequest* pendingLegacyI2CScanRequest = nullptr;

//ESPAsyncWebServer deletes the request object on client disconnect, even mid-flight - each pending
//request gets a matching *Disconnected flag so loop() knows its pointer is no longer safe to use.
bool pendingRescanRequestDisconnected = false;
bool pendingUnitsInfoRequestDisconnected = false;
bool pendingLegacyI2CScanRequestDisconnected = false;
bool pendingUnitsInfoRequestDetail = false; //captured from the "detail" query param at request time

//Same reasoning, for the twiboot/bootloader endpoints (UNIT_BOOTLOADER_ENABLE).
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
const char* PARAM_CLOCK_FORMAT = "clockFormat";
const char* PARAM_DATE_FORMAT = "dateFormat";
const char* PARAM_ROTATION_ITEMS = "rotationItems";
const char* PARAM_COUNTDOWN_PREFIX = "countdownPrefix";
const char* PARAM_COUNTDOWN_SUFFIX = "countdownSuffix";
const char* PARAM_COUNTUP_DATE = "countupDateTimeUnix";
const char* PARAM_COUNTUP_PREFIX = "countupPrefix";
const char* PARAM_COUNTUP_SUFFIX = "countupSuffix";
const char* PARAM_ID = "id";
const char* PARAM_UNIT_OFFSET = "offset";
const char* PARAM_TEXT_PUSH_TEXT = "text";
const char* PARAM_TEXT_PUSH_DURATION = "duration";

//Device Modes
const char* DEVICE_MODE_TEXT = "text";
const char* DEVICE_MODE_CLOCK = "clock";
const char* DEVICE_MODE_DATE = "date";
const char* DEVICE_MODE_COUNTDOWN = "countdown";
const char* DEVICE_MODE_COUNTUP = "countup";
const char* DEVICE_MODE_ROTATION = "rotation";

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
const char* clockFormatPath = "/clockformat.txt";
const char* dateFormatPath = "/dateformat.txt";
const char* rotationItemsPath = "/rotation-items.txt";
const char* countdownPrefixPath = "/countdownprefix.txt";
const char* countdownSuffixPath = "/countdownsuffix.txt";
const char* countupPath = "/countup.txt";
const char* countupPrefixPath = "/countupprefix.txt";
const char* countupSuffixPath = "/countupsuffix.txt";

//Variables for storing things for checking and use in normal running
String alignment = "";
String flapSpeed = "";
String inputText = "";
String deviceMode = "";
String countdownToDateUnix = "";
String countupSinceDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
bool alignmentUpdated = false;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
bool isWifiConfigured = false;
bool showDebugPage = false; //defaults off - the startup debug page can render blank on fresh boots
volatile bool webRequestActive = false; //skip display updates while a web request is in flight
LList<ScheduledMessage> scheduledMessages;
//Fixed array, not LList<RotationItem>: LinkedList's operator[] can dereference a NULL node under
//interleaved access - a real, reproduced crash. Direct indexing avoids it.
#define ROTATION_ITEMS_MAX 20
RotationItem rotationItems[ROTATION_ITEMS_MAX];
int rotationItemCount = 0;
int rotationCurrentIndex = 0;
unsigned long rotationItemStartedAtSeconds = 0; //0 = not started yet (real timestamps are never 0)
//Set by checkRotation() from the current item's Alignment field ("" = inherit the default) and
//threaded through to showText() as an override.
String rotationCurrentAlignment = "";
Timezone timezone;

AsyncWebServer webServer(80);

#if WIFI_USE_DIRECT == false
WiFiManager wifiManager;
bool isPendingWifiReset = false;

//Lets the user scan for and connect to a WiFi network from the running web UI, in addition to the
//"Reset WiFi -> reboot into AP portal" flow.
bool wifiScanRequested = false;

enum WifiConnectPhase {
  WIFI_CONNECT_NONE,
  WIFI_CONNECT_PENDING,          //POST /api/wifi/connect received, loop() hasn't called WiFi.begin() yet
  WIFI_CONNECT_TESTING,          //WiFi.begin() called with the new credentials, waiting for a result
  WIFI_CONNECT_SUCCESS,
  WIFI_CONNECT_FAILED_REVERTED   //new credentials didn't connect in time, reverted to the old ones
};
WifiConnectPhase wifiConnectPhase = WIFI_CONNECT_NONE;
String wifiConnectNewSsid = "";
String wifiConnectNewPassword = "";
String wifiConnectOldSsid = "";
String wifiConnectOldPassword = "";
unsigned long wifiConnectPhaseChangedAt = 0;
//The ESP can only be associated with one AP at a time, so WiFi.begin() drops the current connection
//immediately - this bounds how long a bad new network gets before reverting to the old one.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
#endif

//Used to reject a browser firmware upload part-way through if the password didn't match
#if WEB_FIRMWARE_UPDATE_ENABLE == true
bool firmwareUpdateAuthFailed = false;

//Client-paced chunked upload (/api/firmware-start/-chunk/-finish) - the browser sends the next
//chunk only after the ESP confirms the previous one is flashed, giving natural backpressure since
//the ESP8266 can't otherwise slow down incoming TCP data. Update.* runs from loop(), not the
//AsyncWebServer callback (too little stack there).
#define OTA_CHUNK_SIZE 4096 //must match CHUNK_SIZE in index.html's uploadFirmware()
uint8_t* otaChunkBuffer = nullptr; //allocated only while an upload is in progress
volatile size_t otaChunkLen = 0;
enum OtaPhase { OTA_PHASE_NONE, OTA_PHASE_BEGIN, OTA_PHASE_WRITE, OTA_PHASE_FINISH };
volatile OtaPhase otaPendingPhase = OTA_PHASE_NONE; //what loop() should do next
volatile OtaPhase otaCompletedPhase = OTA_PHASE_NONE;
volatile uint32_t otaBeginSize = 0;
volatile bool otaChunkRejected = false;
volatile bool otaResultReady = false;
volatile bool otaResultSuccess = false;
String otaResultMessage = "";
volatile unsigned long otaLastActivityAt = 0; //watchdog for an abandoned upload (client vanished)
//90s: a client retrying a dropped chunk over flaky WiFi can genuinely take a while even though the
//upload itself is fine.
#define OTA_ABANDONED_TIMEOUT_MS 90000
AsyncWebServerRequest* pendingFirmwareUploadRequest = nullptr;
bool pendingFirmwareUploadRequestDisconnected = false;
#endif
#if UNIT_BOOTLOADER_ENABLE == true
bool unitFirmwareUploadAuthFailed = false;
#endif
bool webFileUploadAuthFailed = false;

String debugStatus = "";

void ledOn() {
#if LED_ENABLE == true
  //Only lights up while debug mode is on; ledOff() stays ungated so a mid-sequence toggle can't
  //leave the LED stuck lit.
  if (showDebugPage) {
    digitalWrite(LED_DEBUG_PIN, LED_ON);
  }
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
  //Pull SDA/SCL to idle-high before anything can glitch them - the Unit's TWI hardware doesn't
  //reset when the ESP does and stays armed/listening.
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);

  //Load the debug-mode flag before the first blinkLed() call - some blink codes fire before the
  //normal filesystem-init point.
#if DEBUG_ENABLE == true
  if (LittleFS.begin()) {
    showDebugPage = (readFile(LittleFS, debugModePath, "false") == "true");
  }
#endif

#if SERIAL_ENABLE == true
  Serial.begin(SERIAL_BAUDRATE);
#elif !defined(FAE_MOD)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); //ESP01 only
#endif

#ifdef FAE_MOD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#endif

  Wire.setClock(400000); //400kHz - units run at 16MHz with 4.7k pull-ups, comfortably support this

#if UNIT_BOOTLOADER_ENABLE == true
  //twiboot holds SCL low during a flash page write - give it more time than the worst-case
  //erase+write duration so it never gets misread as a bus error.
  Wire.setClockStretchLimit(25000);
#endif

  //Proactive bus recovery, not just reactive after a failed read - cheap, no-op if already idle.
  recoverI2CBus();

  // Initialize LED for debugging
#if LED_ENABLE == true
  pinMode(LED_DEBUG_PIN, OUTPUT);
  ledOff();
  blinkLed(1, 100, 50); // Quick blink to show startup
#endif

  // First I2C scan is deferred to loop() (firstUnitScanDone block there), 30s after boot to give
  // Units time to finish their own homing - WiFi/web server start immediately regardless.
  SerialPrintln(F(""));
  SerialPrintln(F("#######################################################"));
  SerialPrintln(F("..............Split Flap Display Starting.............."));
  SerialPrintln(F("#######################################################"));
  debugStatus = "Starting";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);

  //Load and read all the things
  debugStatus = "WiFi Init";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);
  blinkLed(1, 200, 100); // 1 blink = WiFi connecting
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    blinkLed(2, 200, 100); // 2 blinks = WiFi connected
    debugStatus = "WiFi Connected";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    
    //ezTime initialization - NON-BLOCKING with timeout
    debugStatus = "NTP Sync Starting";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    blinkLed(3, 200, 100); // 3 blinks = NTP syncing
    
    setInterval(21600); //sync every 6h - a wall clock doesn't need minute-level freshness

    //Not setDebug(INFO) - ezTime's own internal logging fires extremely often around WiFi
    //hiccups, wasting loop()/UART time. Our own SerialPrintln() logging is unaffected.
    setDebug(NONE);
    
    // Try to sync with timeout (non-blocking)
    unsigned long ntpStartTime = millis();
    unsigned long ntpTimeout = 30000; // 30 second timeout
    
    SerialPrintln(F("DEBUG: Starting NTP sync (non-blocking, 30s timeout)"));
    
    // Wait for sync with timeout and yield
    while (timeStatus() == timeNotSet && (millis() - ntpStartTime) < ntpTimeout) {
      events(); // Process ezTime events
      yield();  // Allow other tasks to run
      delay(100);
    }
    
    if (timeStatus() == timeSet) {
      blinkLed(4, 200, 100); // 4 blinks = NTP sync success
      debugStatus = "NTP Sync Success";
      SerialPrintln(F("DEBUG: Status = ") + debugStatus);
      SerialPrintln(F("DEBUG: NTP sync successful!"));
    } else {
      blinkLed(5, 200, 100); // 5 blinks = NTP sync failed
      debugStatus = "NTP Sync Failed";
      SerialPrintln(F("DEBUG: Status = ") + debugStatus);
      SerialPrintln(F("DEBUG: WARNING - NTP sync failed or timed out, continuing anyway"));
    }
    
    //Load various variables
    debugStatus = "File System Init";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    blinkLed(6, 200, 100); // 6 blinks = File system init
    initialiseFileSystem();
    loadValuesFromFileSystem();
    checkAndPersistCrashLog(); // see ServiceCrashLogFunctions.ino - must run after LittleFS mounts

    // Must come after loadValuesFromFileSystem() - needs timezoneSetting, populated from there.
    timezone.setLocation(timezoneSetting);
    SerialPrintln(F("DEBUG: Timezone set to: ") + timezoneSetting);
    
    // Debug mode was already loaded at the very top of setup(), before the earlier blinkLed()
    // calls - just log the already-known value here.
#if DEBUG_ENABLE == true
    SerialPrintln(F("DEBUG: Debug mode: ") + String(showDebugPage ? F("enabled") : F("disabled")) + F(" (controls startup page, error status, page load log, LED blink codes, and serial log)"));
#endif

    //Web Server Endpoint configuration
    debugStatus = "Web Server Setup";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    blinkLed(7, 200, 100); // 7 blinks = Web server starting
    
    //No .setCacheControl() - drove heap fragmentation up fast on repeated requests when tried.
    //Hard-refresh needed after updating a web file.
    webServer.serveStatic("/", LittleFS, "/");

    //All API routes below live under a single "/api/" prefix - full endpoint reference in API.md.
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
      unsigned long pageRequestStart = millis();
      SerialPrintln(F("DEBUG: Request Home Page Received"));
      
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
      //Actually stored gzip-compressed as "/index.html.gz" - AsyncFileResponse falls back to that
      //automatically and adds Content-Encoding: gzip itself.
      unsigned long beforeSend = millis();
      SerialPrintln(F("DEBUG: Serving index.html (took ") + String(beforeSend - pageRequestStart) + F("ms to get here)"));
      request->send(LittleFS, "/index.html", "text/html");
      SerialPrintln(F("DEBUG: index.html sent (total: ") + String(millis() - pageRequestStart) + F("ms)"));
    });

    webServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest * request) {
      unsigned long settingsStart = millis();
      SerialPrintln(F("DEBUG: Request for Settings Received at ") + String(settingsStart) + F("ms"));
      
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
      minimalDoc["countupSinceDateUnix"] = atol(countupSinceDateUnix.c_str());
      minimalDoc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
      minimalDoc["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      minimalDoc["wifiIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
      minimalDoc["scheduledMessages"] = JsonArray();
      minimalDoc["wifiSettingsResettable"] = true;
      //otaPassword/timezone included so Administration can populate from this one fetch, given
      //this device's hard 5-connection TCP limit.
      minimalDoc["otaPassword"] = otaPassword;
      minimalDoc["timezone"] = timezoneSetting;
      minimalDoc["clockFormat"] = clockFormat;
      minimalDoc["dateFormat"] = dateFormat;
      minimalDoc["countdownPrefix"] = countdownPrefix;
      minimalDoc["countdownSuffix"] = countdownSuffix;
      minimalDoc["countupPrefix"] = countupPrefix;
      minimalDoc["countupSuffix"] = countupSuffix;
      minimalDoc["quietHoursEnabled"] = quietHoursEnabled;
      minimalDoc["quietHoursStart"] = quietHoursStart;
      minimalDoc["quietHoursEnd"] = quietHoursEnd;

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

      //Rotation items themselves aren't built here - looping over them in this handler caused a
      //stack-overflow panic. Just the count (cheap); the client fetches the full list from
      //GET /api/rotation-items only when the Rotation tab is opened.
      minimalDoc["rotationItemCount"] = rotationItemCount;

      //beginResponseStream() avoids building the JSON into one contiguous String first.
      SerialPrintln(F("DEBUG: Settings response ready in ") + String(millis() - settingsStart) + F("ms"));

      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(minimalDoc, *response);
      request->send(response);

      SerialPrintln(F("DEBUG: Settings response sent (total: ") + String(millis() - settingsStart) + F("ms)"));

      // Clear flag after request completes
      webRequestActive = false;
    });

    //Attempted merging this with the GET handler above into one HTTP_GET|HTTP_POST registration
    //(same pattern as /api/wifi etc., see BUGFIX_LOG #120) - reproducibly broken on real hardware:
    //a genuine POST request received the GET handler's JSON response instead of "OK", and invalid
    //POST bodies got 200 instead of 400. Root cause not chased down (this handler's combined body
    //is far larger than every other successfully-merged pair - likely relevant, but unconfirmed);
    //reverted to two separate registrations, verified working again. See BUGFIX_LOG #121.
    webServer.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("DEBUG: Request Post of Form Received"));

      bool submissionError = false;

      //Separate declarations - `bool a, b = false;` only initializes b, not a.
      bool newMessageScheduleEnabledValue = false;
      bool newMessageScheduleShowIndefinitely = false;
      long newMessageScheduleDateTimeUnixValue = -1;
      String newAlignmentValue, newDeviceModeValue, newFlapSpeedValue, newInputTextValue, newCountdownToDateUnixValue = "";
      bool hasNewInputTextValue = false;
      String newClockFormatValue = "";
      String newDateFormatValue = "";
      bool hasNewRotationItems = false;
      String newRotationItemsJson = "";
      //Distinct hasNew*/empty-allowed pattern (unlike clockFormat/dateFormat above) - "" is a
      //meaningful, deliberate value here (show the bare day count), not "field not submitted".
      bool hasNewCountdownPrefixValue = false;
      String newCountdownPrefixValue = "";
      bool hasNewCountdownSuffixValue = false;
      String newCountdownSuffixValue = "";
      String newCountupSinceDateUnixValue = "";
      bool hasNewCountupPrefixValue = false;
      String newCountupPrefixValue = "";
      bool hasNewCountupSuffixValue = false;
      String newCountupSuffixValue = "";

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
              SerialPrintln(F("Alignment provided was not valid. Value: ") + receivedValue);
              submissionError = true;
            }
          }

          //HTTP POST device mode value
          if (p->name() == PARAM_DEVICEMODE) {
            String receivedValue = p->value();
            if (receivedValue == DEVICE_MODE_TEXT || receivedValue == DEVICE_MODE_CLOCK || receivedValue == DEVICE_MODE_DATE || receivedValue == DEVICE_MODE_COUNTDOWN || receivedValue == DEVICE_MODE_COUNTUP || receivedValue == DEVICE_MODE_ROTATION) {
              newDeviceModeValue = receivedValue;
            }
            else {
              SerialPrintln(F("Device Mode provided was not valid. Invalid Value: ") + receivedValue);
              submissionError = true;
            }
          }

          //HTTP POST Flap Speed Slider value
          if (p->name() == PARAM_FLAP_SPEED) {
            //Client-side min/max isn't enough - convertSpeed() extrapolates out-of-range values,
            //and a garbage speed reaching the unit is a real motor safety concern.
            String rawFlapSpeed = p->value();
            if (!isNumber(rawFlapSpeed) || rawFlapSpeed.toInt() < 1 || rawFlapSpeed.toInt() > 100) {
              SerialPrintln(F("flapSpeed provided was invalid (must be a number 1-100): ") + rawFlapSpeed);
              submissionError = true;
            } else {
              newFlapSpeedValue = rawFlapSpeed.c_str();
            }
          }

          //HTTP POST inputText value
          if (p->name() == PARAM_INPUT_TEXT) {
            //Enforce a max length server-side too - an unbounded String is needless heap pressure.
            if (p->value().length() > 500) {
              SerialPrintln(F("inputText provided was too long (") + String(p->value().length()) + F(" chars, max 500)"));
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
              SerialPrintln(F("Schedule date time provided was not valid. Invalid Value: ") + receivedValue);
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
              SerialPrintln(F("Countdown date provided was not valid. Invalid Value: ") + receivedValue);
              submissionError = true;
            }
          }

          //HTTP POST Countup Seconds (the date to count days elapsed since)
          if (p->name() == PARAM_COUNTUP_DATE) {
            String receivedValue = p->value().c_str();
            if (isNumber(receivedValue)) {
              newCountupSinceDateUnixValue = receivedValue;
            }
            else {
              SerialPrintln(F("Countup date provided was not valid. Invalid Value: ") + receivedValue);
              submissionError = true;
            }
          }

          //Clock/date format, e.g. "HH:mm:ss" - see convertUserDateFormatToEzTime().
          if (p->name() == PARAM_CLOCK_FORMAT) {
            if (p->value().length() > 40) {
              SerialPrintln(F("clockFormat provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newClockFormatValue = p->value();
            }
          }
          if (p->name() == PARAM_DATE_FORMAT) {
            if (p->value().length() > 40) {
              SerialPrintln(F("dateFormat provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newDateFormatValue = p->value();
            }
          }

          //HTTP POST Countdown prefix/suffix - "" is a valid, deliberate value (show the bare day
          //count), so tracked via hasNew*/empty-allowed rather than the newX != "" pattern above.
          if (p->name() == PARAM_COUNTDOWN_PREFIX) {
            if (p->value().length() > 40) {
              SerialPrintln(F("countdownPrefix provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newCountdownPrefixValue = p->value();
              hasNewCountdownPrefixValue = true;
            }
          }
          if (p->name() == PARAM_COUNTDOWN_SUFFIX) {
            if (p->value().length() > 40) {
              SerialPrintln(F("countdownSuffix provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newCountdownSuffixValue = p->value();
              hasNewCountdownSuffixValue = true;
            }
          }

          //HTTP POST Countup prefix/suffix - same "" is deliberate reasoning as Countdown above.
          if (p->name() == PARAM_COUNTUP_PREFIX) {
            if (p->value().length() > 40) {
              SerialPrintln(F("countupPrefix provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newCountupPrefixValue = p->value();
              hasNewCountupPrefixValue = true;
            }
          }
          if (p->name() == PARAM_COUNTUP_SUFFIX) {
            if (p->value().length() > 40) {
              SerialPrintln(F("countupSuffix provided was too long (max 40 chars)"));
              submissionError = true;
            } else {
              newCountupSuffixValue = p->value();
              hasNewCountupSuffixValue = true;
            }
          }

          //HTTP POST Rotation Items (JSON array, the client always submits the whole list) -
          //bounded well above any realistic sequence, still a needless-heap-pressure safeguard.
          if (p->name() == PARAM_ROTATION_ITEMS) {
            if (p->value().length() > 8000) {
              SerialPrintln(F("rotationItems provided was too long (max 8000 chars)"));
              submissionError = true;
            } else {
              newRotationItemsJson = p->value();
              hasNewRotationItems = true;
            }
          }
        }
      }

      //If there was an error, report back to check what has been input
      if (submissionError) {
        SerialPrintln(F("Finished Processing Request with Error"));
        request->send(400, "text/plain", "Invalid submission - check the values you entered (e.g. schedule/countdown dates must be in the future).");
      }
      else {
        SerialPrintln(F("Finished Processing Request Successfully"));

        lastReceivedMessageDateTime = timezone.dateTime("d M y H:i:s");

        //Only if a new alignment value
        if (newAlignmentValue != "" && alignment != newAlignmentValue) {
          alignment = newAlignmentValue;
          alignmentUpdated = true;

          writeFile(LittleFS, alignmentPath, alignment.c_str());
          SerialPrintln(F("Alignment Updated: ") + alignment);
        }

        //Only if a new flap speed value
        if (newFlapSpeedValue != "" && flapSpeed != newFlapSpeedValue) {
          flapSpeed = newFlapSpeedValue;

          writeFile(LittleFS, flapSpeedPath, flapSpeed.c_str());
          SerialPrintln(F("Flap Speed Updated: ") + flapSpeed);
        }

        //Only if countdown date has changed - gated to the countdown tab's own submission (its
        //hidden field is only ever recomputed by that tab's onsubmit case; submitting from a
        //different tab would otherwise resubmit that field's stale/default "0" and wipe the
        //stored date, since the field is a form-wide value regardless of which tab is visible).
        if (newDeviceModeValue == DEVICE_MODE_COUNTDOWN && newCountdownToDateUnixValue != "" && countdownToDateUnix != newCountdownToDateUnixValue) {
          countdownToDateUnix = newCountdownToDateUnixValue;

          writeFile(LittleFS, countdownPath, countdownToDateUnix.c_str());
          SerialPrintln(F("Countdown Date Time Unix Updated: ") + countdownToDateUnix);
        }

        //Only if countup date has changed - same tab-gating reasoning as Countdown above.
        if (newDeviceModeValue == DEVICE_MODE_COUNTUP && newCountupSinceDateUnixValue != "" && countupSinceDateUnix != newCountupSinceDateUnixValue) {
          countupSinceDateUnix = newCountupSinceDateUnixValue;

          writeFile(LittleFS, countupPath, countupSinceDateUnix.c_str());
          SerialPrintln(F("Countup Since Date Unix Updated: ") + countupSinceDateUnix);
        }

        //Only if the clock/date format has changed
        if (newClockFormatValue != "" && clockFormat != newClockFormatValue) {
          clockFormat = newClockFormatValue;

          writeFile(LittleFS, clockFormatPath, clockFormat.c_str());
          SerialPrintln(F("Clock Format Updated: ") + clockFormat);
        }
        if (newDateFormatValue != "" && dateFormat != newDateFormatValue) {
          dateFormat = newDateFormatValue;

          writeFile(LittleFS, dateFormatPath, dateFormat.c_str());
          SerialPrintln(F("Date Format Updated: ") + dateFormat);
        }
        if (hasNewCountdownPrefixValue && countdownPrefix != newCountdownPrefixValue) {
          countdownPrefix = newCountdownPrefixValue;

          writeFile(LittleFS, countdownPrefixPath, countdownPrefix.c_str());
          SerialPrintln(F("Countdown Prefix Updated: ") + countdownPrefix);
        }
        if (hasNewCountdownSuffixValue && countdownSuffix != newCountdownSuffixValue) {
          countdownSuffix = newCountdownSuffixValue;

          writeFile(LittleFS, countdownSuffixPath, countdownSuffix.c_str());
          SerialPrintln(F("Countdown Suffix Updated: ") + countdownSuffix);
        }
        if (hasNewCountupPrefixValue && countupPrefix != newCountupPrefixValue) {
          countupPrefix = newCountupPrefixValue;

          writeFile(LittleFS, countupPrefixPath, countupPrefix.c_str());
          SerialPrintln(F("Countup Prefix Updated: ") + countupPrefix);
        }
        if (hasNewCountupSuffixValue && countupSuffix != newCountupSuffixValue) {
          countupSuffix = newCountupSuffixValue;

          writeFile(LittleFS, countupSuffixPath, countupSuffix.c_str());
          SerialPrintln(F("Countup Suffix Updated: ") + countupSuffix);
        }

        //Rotation items are always submitted as the complete list (see index.html) - a full
        //replace, not an incremental add.
        if (hasNewRotationItems) {
          replaceRotationItemsFromJson(newRotationItemsJson);
          SerialPrintln(F("Rotation Items Updated, count: ") + String(rotationItemCount));
        }

        //If its a new scheduled message, add it to the backlog and proceed, don't want to change device mode
        //Else, we do want to change the device mode and clear out the input text
        if (newMessageScheduleEnabledValue) {
          addAndPersistScheduledMessage(newInputTextValue, newMessageScheduleDateTimeUnixValue, newMessageScheduleShowIndefinitely);
          SerialPrintln(F("New Scheduled Message added"));
        }
        else {
          //Only if device mode has changed
          if (newDeviceModeValue != "" && deviceMode != newDeviceModeValue) {
            deviceMode = newDeviceModeValue;

            //Always (re-)start a sequence from the beginning when switching into Rotation mode.
            if (deviceMode == DEVICE_MODE_ROTATION) {
              rotationCurrentIndex = 0;
              rotationItemStartedAtSeconds = 0;
            }

            writeFile(LittleFS, deviceModePath, deviceMode.c_str());
            SerialPrintln(F("Device Mode Set: ") + deviceMode);
          }

          //Only if we are showing text
          if (hasNewInputTextValue && deviceMode == DEVICE_MODE_TEXT) {
            inputText = newInputTextValue;
          }
        }

        request->send(200, "text/plain", "OK");
      }
    });

    //Own small handler, not sharing a stack frame with /api/settings - only fetched when the
    //Rotation tab is opened.
    webServer.on("/api/rotation-items", HTTP_GET, [](AsyncWebServerRequest * request) {
      webRequestActive = true;

      JsonDocument rotationDoc;
      JsonArray rotationArray = rotationDoc.to<JsonArray>();
      unsigned long rotationRequestStart = millis();

      //No yield() here - pure in-memory struct->JSON copying, finishes in microseconds; a yield()
      //here previously overflowed the AsyncWebServer callback's stack. Time budget below is a
      //defensive fallback only.
      for (int i = 0; i < rotationItemCount && (millis() - rotationRequestStart) < 1000; i++) {
        RotationItem &item = rotationItems[i]; //plain array indexing, see rotationItems' declaration
        JsonObject itemObject = rotationArray.add<JsonObject>();
        itemObject["type"] = item.Type;
        itemObject["value"] = item.Value;
        itemObject["format"] = item.Format;
        itemObject["targetDateTimeUnix"] = item.TargetDateTimeUnix;
        itemObject["prefix"] = item.Prefix;
        itemObject["suffix"] = item.Suffix;
        itemObject["durationSeconds"] = item.DurationSeconds;
        itemObject["alignment"] = item.Alignment;
      }

      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(rotationDoc, *response);
      request->send(response);

      webRequestActive = false;
    });

    //Consolidates the four pure-diagnostic endpoints (former /api/health, /api/heap,
    ///api/tcp-status, /api/test) into one registered route, dispatched by ?type= - each
    //registered handler costs a small permanent heap allocation that's never freed, and this
    //device has very little room to spare.
    webServer.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest * request) {
      String type = request->hasParam("type") ? request->getParam("type")->value() : "";

      if (type == "health") {
        request->send(200, "text/plain", "Healthy");
        return;
      }

      if (type == "heap") {
        char buf[220];
        //getFlashChipRealSize() = physical chip's own SPI ID (ground truth); getFlashChipSize() =
        //what the eesz board-menu choice assumes - a mismatch means the compiled layout is wrong.
        snprintf(buf, sizeof(buf), "free=%u frag=%u%% maxBlock=%u flashReal=%u flashConfigured=%u flashChipId=0x%08X espChipId=0x%08X sdk=%s core=%s",
          ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize(),
          ESP.getFlashChipRealSize(), ESP.getFlashChipSize(), ESP.getFlashChipId(), ESP.getChipId(),
          ESP.getSdkVersion(), ESP.getCoreVersion().c_str());
        request->send(200, "text/plain", buf);
        return;
      }

      if (type == "tcp") {
        //This board's lwIP build hard-caps MEMP_NUM_TCP_PCB at 5 - only 5 simultaneously *active*
        //TCP connections, with a separate 5-slot TIME-WAIT pool (every request opens a fresh
        //connection, "Connection: close" on every response).
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
        return;
      }

      if (type == "test") {
        request->send(200, "application/json", "{\"status\":\"ok\",\"time\":" + String(millis()) + "}");
        return;
      }

      request->send(400, "text/plain", "Unknown 'type' - use health, heap, tcp, or test");
    });

    // On-device crash log (see ServiceCrashLogFunctions.ino) - survives even without a live
    // serial connection at the moment of a crash. One JSON object per line, oldest first.
    // GET+DELETE merged into one registration (heap-saving pattern established for /api/wifi,
    // see BUGFIX_LOG #120). DELETE (not GET) clears it, to avoid a CSRF-via-GET state change.
    webServer.on("/api/crash-log", HTTP_GET | HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        if (!LittleFS.exists(CRASH_LOG_PATH)) {
          request->send(200, "application/jsonl", "");
          return;
        }
        request->send(LittleFS, CRASH_LOG_PATH, "application/jsonl");
        return;
      }

      // request->method() == HTTP_DELETE
      if (LittleFS.exists(CRASH_LOG_PATH)) {
        LittleFS.remove(CRASH_LOG_PATH);
      }
      request->send(200, "text/plain", "Crash log cleared");
    });

    // I2C bus scanner endpoint for diagnostics
    webServer.on("/api/i2c-scan", HTTP_GET, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("DEBUG: I2C scan requested"));
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

#if UNIT_BOOTLOADER_ENABLE == true
    // Must be registered before "/api/units" below: ESPAsyncWebServer's canHandle() treats any
    // request path starting with "<registered uri>/" as a match too, and _attachHandler() picks
    // the FIRST registered handler that matches - so with "/api/units" registered first, it was
    // silently swallowing every request to "/api/units/firmware"/"/api/units/firmware-push" (a
    // real, undetected regression introduced when "/api/units" itself gained a POST handler in the
    // units-actions consolidation, see BUGFIX_LOG #119/#122 - found while testing this merge, not
    // caused by it). Registering the more specific paths first fixes it for good.

    // Admin: upload a compiled Unit.ino firmware image (raw AVR .bin, NOT ESPMaster's own) to be
    // pushed to a unit via /api/units/firmware-push. Requires X-OTA-Password, same as /api/firmware.
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
              SerialPrintln(F("Unit firmware upload rejected: invalid password"));
              unitFirmwareUploadAuthFailed = true;
              return;
            }
          }

          SerialPrintln(F("DEBUG: Unit firmware upload starting: ") + filename);
          unitFirmwareFile = LittleFS.open(unitFirmwarePath, "w");
          if (!unitFirmwareFile) {
            SerialPrintln(F("Unit firmware upload: failed to open ") + String(unitFirmwarePath) + F(" for writing"));
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
          SerialPrintln(F("DEBUG: Unit firmware upload finished. Total bytes: ") + String(index + len));
        }
      }
    );

    //Pushes the currently stored unit firmware to a single unit over I2C (twiboot), or just probes
    //which I2C address it answers on without writing anything - both deferred to loop(), both take
    //the same 'id' param, so merged into one registration dispatched by 'action' (heap-saving
    //pattern established for /api/wifi etc., see BUGFIX_LOG #120/#121) - unlike the /api/settings
    //attempt in #121, both branches here are short and structurally near-identical, not a large,
    //disjoint pair of handlers, so the same risk doesn't apply. Deliberately its own path, not
    //merged into "/api/units/firmware" - that's a multipart upload registration (different
    //ESPAsyncWebServer callback signature), and that upload route would otherwise intercept a
    //sub-path anyway.
    webServer.on("/api/units/firmware-push", HTTP_POST, [](AsyncWebServerRequest * request) {
      String action = request->hasParam("action", true) ? request->getParam("action", true)->value() : "";

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

      if (action == "push") {
        if (String(otaPassword) != "") {
          String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
          if (providedPassword != String(otaPassword)) {
            SerialPrintln(F("Unit firmware push rejected: invalid password"));
            request->send(401, "text/plain", "Invalid password");
            return;
          }
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
        return;
      }

      if (action == "probe") {
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
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use push or probe");
    });
#endif

    // Admin: read live status + calibration offset for every unit (GET, deferred to loop()), or
    // trigger a unit write-action (POST, dispatched by ?action=: rescan/offset/reset - former
    // /api/units/rescan, /api/units/offset, /api/units/reset). Merged into one registration
    // (heap-saving pattern established for /api/wifi etc., see BUGFIX_LOG #120/#124) - the two
    // branches stay structurally small and independent (unlike the failed /api/settings attempt
    // in #121), so the same size-related risk doesn't apply here.
    webServer.on("/api/units", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        SerialPrintln(F("DEBUG: Admin units request received"));
        if (pendingUnitsInfoRequest != nullptr) {
          request->send(503, "text/plain", "Another units request is already in progress, try again shortly");
          return;
        }
        //Opt-in heavy 6-byte read (adds phase/progress) only via ?detail=1 - routine polling stays cheap.
        pendingUnitsInfoRequestDetail = request->hasParam("detail");
        pendingUnitsInfoRequestDisconnected = false;
        pendingUnitsInfoRequest = request;
        request->onDisconnect([]() {
          pendingUnitsInfoRequest = nullptr;
          pendingUnitsInfoRequestDisconnected = true;
        });
        return;
      }

      // request->method() == HTTP_POST
      String action = request->hasParam("action") ? request->getParam("action")->value() : "";

      if (action == "rescan") {
        SerialPrintln(F("DEBUG: Admin rescan-units request received"));
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
        return;
      }

      if (action == "offset") {
        if (!request->hasParam(PARAM_ID, true) || !request->hasParam(PARAM_UNIT_OFFSET, true)) {
          SerialPrintln(F("Admin unit-offset request missing parameters"));
          request->send(400, "text/plain", "Both 'id' and 'offset' are required");
          return;
        }

        String idValue = request->getParam(PARAM_ID, true)->value();
        String offsetValue = request->getParam(PARAM_UNIT_OFFSET, true)->value();

        if (!isNumber(idValue) || !isNumber(offsetValue)) {
          SerialPrintln(F("Admin unit-offset request had non-numeric values"));
          request->send(400, "text/plain", "'id' and 'offset' must be numbers");
          return;
        }

        int unitId = idValue.toInt();
        long offset = atol(offsetValue.c_str());

        if (unitId < 0 || unitId >= unitsAmount) {
          SerialPrintln(F("Admin unit-offset request had out of range unit id: ") + idValue);
          request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(unitsAmount - 1));
          return;
        }

        if (offset < 0 || offset >= UNIT_STEPS) {
          SerialPrintln(F("Admin unit-offset request had out of range offset: ") + offsetValue);
          request->send(400, "text/plain", "Offset out of range. Valid range: 0 to " + String(UNIT_STEPS - 1));
          return;
        }

        SerialPrintln(F("Setting Unit ") + String(unitId) + F(" offset to ") + String(offset));
        writeOffsetToUnit(unitId, (uint16_t)offset);

        request->send(202, "text/plain", "Offset sent. Unit is recalibrating now, this can take a few seconds.");
        return;
      }

      if (action == "reset") {
        SerialPrintln(F("Request to Reset Units Received"));
        isPendingUnitsReset = true; //picked up in loop()
        request->send(200, "text/plain", "Units are resetting/recalibrating now.");
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use rescan, offset, or reset");
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
              SerialPrintln(F("Web file upload rejected: invalid password"));
              webFileUploadAuthFailed = true;
              return;
            }
          }

          //"index.html.gz" is the only valid target - the whole UI is one gzip-compressed file,
          //served automatically with Content-Encoding: gzip.
          webUploadFilenameValid = (filename == "index.html.gz");
          if (!webUploadFilenameValid) {
            SerialPrintln(F("Web file upload rejected: filename not in allowlist: ") + filename);
            request->send(400, "text/plain", "Filename must be one of the known static assets");
            return;
          }

          //A leftover uncompressed "/index.html" would take precedence over "/index.html.gz".
          if (LittleFS.exists("/index.html")) {
            LittleFS.remove("/index.html");
          }

          SerialPrintln(F("DEBUG: Web file upload starting: ") + filename);
          webUploadFile = LittleFS.open("/" + filename, "w");
          if (!webUploadFile) {
            SerialPrintln(F("Web file upload: failed to open /") + filename + F(" for writing"));
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
          SerialPrintln(F("DEBUG: Web file upload finished: ") + filename + F(", total bytes: ") + String(index + len));
        }
      }
    );

#if WEB_FIRMWARE_UPDATE_ENABLE == true
    //Browser-based firmware upload in three chunked steps. Shared password check for all three.
    auto checkOtaPassword = [](AsyncWebServerRequest * request) -> bool {
      if (String(otaPassword) == "") {
        return true; //no password configured, nothing to check
      }
      String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
      return providedPassword == String(otaPassword);
    };

    // Step 1: tell the ESP how big the firmware is and let it erase/prepare the flash region.
    // Body: size=<bytes> (form-encoded).
    webServer.on("/api/firmware-start", HTTP_POST, [checkOtaPassword](AsyncWebServerRequest * request) {
      if (!checkOtaPassword(request)) {
        request->send(401, "text/plain", "Invalid password");
        return;
      }
      if (otaChunkBuffer != nullptr || otaPendingPhase != OTA_PHASE_NONE) {
        request->send(503, "text/plain", "Another firmware upload is already in progress");
        return;
      }
      if (!request->hasParam("size", true)) {
        request->send(400, "text/plain", "'size' is required");
        return;
      }
      uint32_t size = (uint32_t)request->getParam("size", true)->value().toInt();
      if (size == 0) {
        request->send(400, "text/plain", "'size' must be > 0");
        return;
      }

      SerialPrintln(F("DEBUG: Firmware upload starting, size=") + String(size));
      otaBeginSize = size;
      otaLastActivityAt = millis();
      otaResultReady = false;
      pendingFirmwareUploadRequestDisconnected = false;
      pendingFirmwareUploadRequest = request;
      request->onDisconnect([]() {
        pendingFirmwareUploadRequest = nullptr;
        pendingFirmwareUploadRequestDisconnected = true;
      });
      otaPendingPhase = OTA_PHASE_BEGIN; //loop() calls Update.begin()
    });

    //Step 2: one chunk of firmware data - the browser waits for this to return before sending
    //the next one, giving natural backpressure.
    webServer.on("/api/firmware-chunk", HTTP_POST,
      [checkOtaPassword](AsyncWebServerRequest * request) {
        if (!checkOtaPassword(request)) {
          request->send(401, "text/plain", "Invalid password");
          return;
        }
        if (otaChunkRejected) {
          otaChunkRejected = false;
          request->send(400, "text/plain", "Chunk too large or no upload in progress");
          return;
        }
        if (otaChunkBuffer == nullptr) {
          request->send(400, "text/plain", "No firmware upload in progress - call /api/firmware-start first");
          return;
        }
        otaLastActivityAt = millis();
        pendingFirmwareUploadRequestDisconnected = false;
        pendingFirmwareUploadRequest = request;
        request->onDisconnect([]() {
          pendingFirmwareUploadRequest = nullptr;
          pendingFirmwareUploadRequestDisconnected = true;
        });
        otaPendingPhase = OTA_PHASE_WRITE; //loop() calls Update.write()
      },
      nullptr, //no multipart upload needed - this is a plain raw-body request
      [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0) {
          otaChunkLen = 0;
          otaChunkRejected = (otaChunkBuffer == nullptr) || (total > OTA_CHUNK_SIZE);
        }
        if (otaChunkRejected) {
          return;
        }
        if (otaChunkLen + len > OTA_CHUNK_SIZE) {
          otaChunkRejected = true; //shouldn't happen given the total check above, but never overflow
          return;
        }
        memcpy(otaChunkBuffer + otaChunkLen, data, len);
        otaChunkLen += len;
      }
    );

    // Step 3: no body needed - finalizes the update (Update.end()) and reboots on success.
    webServer.on("/api/firmware-finish", HTTP_POST, [checkOtaPassword](AsyncWebServerRequest * request) {
      if (!checkOtaPassword(request)) {
        request->send(401, "text/plain", "Invalid password");
        return;
      }
      if (otaChunkBuffer == nullptr) {
        request->send(400, "text/plain", "No firmware upload in progress");
        return;
      }
      otaLastActivityAt = millis();
      pendingFirmwareUploadRequestDisconnected = false;
      pendingFirmwareUploadRequest = request;
      request->onDisconnect([]() {
        pendingFirmwareUploadRequest = nullptr;
        pendingFirmwareUploadRequestDisconnected = true;
      });
      otaPendingPhase = OTA_PHASE_FINISH; //loop() calls Update.end()
    });
#endif

#if DEBUG_ENABLE == true
    webServer.on("/api/debug-mode", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("enabled", true)) {
        request->send(400, "text/plain", "'enabled' is required (true/false)");
        return;
      }

      bool enabled = request->getParam("enabled", true)->value() == "true";
      showDebugPage = enabled;
      writeFile(LittleFS, debugModePath, enabled ? "true" : "false");

      if (enabled) {
        SerialPrintln(F("DEBUG: Request to enable debug mode received"));
        request->send(200, "text/plain", "OK - Debug mode enabled. This enables: startup debug page, error status panel, page load debug log, and serial debug log. Refresh page to see changes.");
      } else {
        SerialPrintln(F("DEBUG: Request to exit debug mode received"));
        request->send(200, "text/plain", "OK - Debug mode disabled. Startup debug page, error status, and debug logs will be hidden.");
      }
    });
#endif
    
    //This and reset-units/ota/reset-wifi below are POST, not GET - a GET link is triggerable
    //cross-site (e.g. an <img> tag on any page) without user interaction (CSRF-via-GET).
    webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Reboot Received"));
      
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

    webServer.on("/api/scheduled-messages", HTTP_DELETE, [](AsyncWebServerRequest * request) {
      SerialPrintln(F("Request to Remove Scheduled Message Received"));
      
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
          SerialPrintln(F("Invalid Delete Scheduled Message ID Received"));
          request->send(400, "text/plain", "Invalid ID value");
        }
      } 
      else {
          SerialPrintln(F("Delete Scheduled Message Received with no ID"));
          request->send(400, "text/plain", "No ID specified");
      }
    });

    //Lightweight text-push endpoint for automations (webhooks, Home Assistant, shortcuts, etc.) -
    //just a message and an optional hold time, instead of the full /api/settings form. Same
    //unauthenticated design as /api/settings (this device has no separate login system).
    webServer.on("/api/text", HTTP_POST, [](AsyncWebServerRequest * request) {
      String newText = "";
      bool hasText = false;
      long durationSeconds = 0;

      int params = request->params();
      for (int paramIndex = 0; paramIndex < params; paramIndex++) {
        AsyncWebParameter* p = request->getParam(paramIndex);
        if (p->isPost()) {
          if (p->name() == PARAM_TEXT_PUSH_TEXT) {
            if (p->value().length() > 500) {
              request->send(400, "text/plain", "text too long (max 500 chars)");
              return;
            }
            newText = p->value();
            hasText = true;
          }
          if (p->name() == PARAM_TEXT_PUSH_DURATION) {
            String rawDuration = p->value();
            if (!isNumber(rawDuration) || rawDuration.toInt() < 0 || rawDuration.toInt() > 3600) {
              request->send(400, "text/plain", "duration must be a number 0-3600 (seconds)");
              return;
            }
            durationSeconds = rawDuration.toInt();
          }
        }
      }

      if (!hasText) {
        request->send(400, "text/plain", "Missing required 'text' parameter");
        return;
      }

      if (durationSeconds > 0) {
        //Temporary: deviceMode/inputText are deliberately left untouched, same pattern
        //checkScheduledMessages() already uses for a non-indefinite scheduled message - once the
        //hold time elapses, the next 1s tick's normal mode loop redisplays whatever deviceMode
        //currently is on its own, no extra "revert" logic needed here.
        showText(newText, (int)(durationSeconds * 1000));
      } else {
        //Permanent: switches the display into Text mode, same net effect as submitting the main
        //form with deviceMode=text&inputText=<text>.
        if (deviceMode != DEVICE_MODE_TEXT) {
          deviceMode = DEVICE_MODE_TEXT;
          writeFile(LittleFS, deviceModePath, deviceMode.c_str());
        }
        inputText = newText;
        showText(newText);
      }

      request->send(200, "text/plain", "OK");
    });


#if WIFI_USE_DIRECT == false
    //Consolidates all five former WiFi endpoints (/api/wifi/reset, /api/wifi/scan,
    ///api/wifi/scan-result, /api/wifi/connect, /api/wifi/connect-status) into a single registered
    //route - GET dispatches by ?type=, POST by ?action=. One registration handling both methods,
    //rather than two separate GET/POST registrations on the same path (tried that first; GET
    //requests were reproducibly routed to the POST handler instead - not chasing that library
    //quirk down, this single-registration form sidesteps it and saves even more).
    webServer.on("/api/wifi", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        String type = request->hasParam("type") ? request->getParam("type")->value() : "";

        if (type == "scan-result") {
          //WiFi.scanComplete() returns WIFI_SCAN_RUNNING (-1) while still scanning,
          //WIFI_SCAN_FAILED (-2) on failure, otherwise the network count.
          int8_t scanResult = WiFi.scanComplete();

          JsonDocument doc;
          if (scanResult == WIFI_SCAN_RUNNING || wifiScanRequested) {
            doc["scanning"] = true;
          } else if (scanResult == WIFI_SCAN_FAILED) {
            doc["scanning"] = false;
            doc["error"] = "Scan failed";
          } else {
            doc["scanning"] = false;
            JsonArray networks = doc["networks"].to<JsonArray>();

            //Dedupe by SSID (the same network can show up once per BSSID/channel/repeater) - keep
            //only the strongest-signal entry for each name, sorted strongest-first by scan order.
            for (int i = 0; i < scanResult; i++) {
              String ssid = WiFi.SSID(i);
              if (ssid == "") {
                continue; //hidden network - nothing to show/select by name
              }

              int32_t rssi = WiFi.RSSI(i);
              bool secure = WiFi.encryptionType(i) != ENC_TYPE_NONE;

              bool alreadyListed = false;
              for (JsonObject existingNetwork : networks) {
                if (existingNetwork["ssid"] == ssid) {
                  alreadyListed = true;
                  if (rssi > (int32_t)existingNetwork["rssi"]) {
                    existingNetwork["rssi"] = rssi;
                  }
                  break;
                }
              }
              if (!alreadyListed) {
                JsonObject network = networks.add<JsonObject>();
                network["ssid"] = ssid;
                network["rssi"] = rssi;
                network["secure"] = secure;
              }
            }
            WiFi.scanDelete();
          }

          AsyncResponseStream *response = request->beginResponseStream("application/json");
          serializeJson(doc, *response);
          request->send(response);
          return;
        }

        if (type == "connect-status") {
          //Polled to find out how a connect attempt went. A terminal result is only reported
          //once - the phase resets to "none" right after being read.
          JsonDocument doc;
          switch (wifiConnectPhase) {
            case WIFI_CONNECT_NONE: doc["phase"] = "none"; break;
            case WIFI_CONNECT_PENDING: doc["phase"] = "pending"; break;
            case WIFI_CONNECT_TESTING: doc["phase"] = "testing"; break;
            case WIFI_CONNECT_SUCCESS: doc["phase"] = "success"; break;
            case WIFI_CONNECT_FAILED_REVERTED: doc["phase"] = "failed_reverted"; break;
          }
          doc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";
          doc["wifiSsid"] = WiFi.SSID();
          doc["wifiIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";

          if (wifiConnectPhase == WIFI_CONNECT_SUCCESS || wifiConnectPhase == WIFI_CONNECT_FAILED_REVERTED) {
            wifiConnectPhase = WIFI_CONNECT_NONE;
          }

          AsyncResponseStream *response = request->beginResponseStream("application/json");
          serializeJson(doc, *response);
          request->send(response);
          return;
        }

        request->send(400, "text/plain", "Unknown 'type' - use scan-result or connect-status");
        return;
      }

      // request->method() == HTTP_POST
      String action = request->hasParam("action") ? request->getParam("action")->value() : "";

      if (action == "reset") {
        SerialPrintln(F("Request to Reset WiFi Received"));

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
        return;
      }

      if (action == "scan") {
        //Kicks off an async network scan - see wifiScanRequested's comment above. Just sets a
        //flag, the actual WiFi.scanNetworksAsync() call happens in loop().
        if (WiFi.scanComplete() == WIFI_SCAN_RUNNING || wifiScanRequested) {
          request->send(409, "text/plain", "A scan is already in progress");
          return;
        }
        SerialPrintln(F("WiFi scan requested"));
        wifiScanRequested = true;
        request->send(200, "text/plain", "Scan started");
        return;
      }

      if (action == "connect") {
        //Connects to a new WiFi network without rebooting - drops the current connection
        //immediately, before the new credentials are known to work; checkWifiConnectAttempt()
        //reverts on failure.
        if (wifiConnectPhase != WIFI_CONNECT_NONE) {
          request->send(409, "text/plain", "A connection attempt is already in progress");
          return;
        }
        if (!request->hasParam("ssid", true) || request->getParam("ssid", true)->value() == "") {
          request->send(400, "text/plain", "'ssid' is required");
          return;
        }

        //Captured now, fast/synchronous - by the time loop() actually calls WiFi.begin() with the
        //new credentials, WiFi.SSID()/psk() would already reflect them, not the old ones anymore.
        wifiConnectOldSsid = WiFi.SSID();
        wifiConnectOldPassword = WiFi.psk();
        wifiConnectNewSsid = request->getParam("ssid", true)->value();
        wifiConnectNewPassword = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";

        SerialPrintln(F("WiFi connect requested: SSID=\"") + wifiConnectNewSsid + F("\""));
        wifiConnectPhase = WIFI_CONNECT_PENDING;
        request->send(200, "text/plain", "Connection attempt started");
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use reset, scan, or connect");
    });
#endif

    //Freely readable without auth (this device has no separate login) - changing it still
    //requires the current password. GET+POST merged into one registration (heap-saving pattern
    //established for /api/wifi - also sidesteps the unresolved separate-GET/POST routing quirk
    //found there, see BUGFIX_LOG #120).
    webServer.on("/api/ota-password", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        JsonDocument doc;
        doc["otaPassword"] = otaPassword;
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // request->method() == HTTP_POST
      if (!request->hasParam("currentPassword", true) || !request->hasParam("newPassword", true)) {
        request->send(400, "text/plain", "'currentPassword' and 'newPassword' are required");
        return;
      }

      String currentPasswordValue = request->getParam("currentPassword", true)->value();
      if (currentPasswordValue != otaPassword) {
        SerialPrintln(F("OTA password change rejected: current password incorrect"));
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
      SerialPrintln(F("OTA password changed"));
      request->send(200, "text/plain", "OTA password changed");
    });

    webServer.on("/api/timezone", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        JsonDocument doc;
        doc["timezone"] = timezoneSetting;
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // request->method() == HTTP_POST
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
        SerialPrintln(F("Timezone change rejected: '") + newTimezoneValue + F("' not recognized"));
        request->send(400, "text/plain", "Timezone not recognized - use a TZ database name like Europe/Berlin");
        return;
      }

      timezoneSetting = newTimezoneValue;
      writeFile(LittleFS, timezonePath, timezoneSetting.c_str());
      SerialPrintln(F("Timezone changed to: ") + timezoneSetting);
      request->send(200, "text/plain", "Timezone changed");
    });

    //No dedicated GET here - quietHoursEnabled/Start/End are already folded into GET /api/settings
    //(see minimalDoc above). Adding a second registered route on top of that one reproducibly
    //broke /api/settings' own response handling on this heap-constrained device - every registered
    //handler costs a small permanent heap allocation that's never freed, and there's very little
    //room to spare.
    webServer.on("/api/quiet-hours", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("enabled", true) || !request->hasParam("start", true) || !request->hasParam("end", true)) {
        request->send(400, "text/plain", "'enabled', 'start', and 'end' are all required");
        return;
      }

      String newStartValue = request->getParam("start", true)->value();
      String newEndValue = request->getParam("end", true)->value();
      if (parseTimeOfDayToMinutes(newStartValue) < 0 || parseTimeOfDayToMinutes(newEndValue) < 0) {
        request->send(400, "text/plain", "'start'/'end' must be 24h time in HH:mm format, e.g. 22:00");
        return;
      }

      bool newEnabledValue = request->getParam("enabled", true)->value() == "true";

      quietHoursEnabled = newEnabledValue;
      quietHoursStart = newStartValue;
      quietHoursEnd = newEndValue;
      recomputeQuietHoursMinutes();
      writeFile(LittleFS, quietHoursEnabledPath, quietHoursEnabled ? "true" : "false");
      writeFile(LittleFS, quietHoursStartPath, quietHoursStart.c_str());
      writeFile(LittleFS, quietHoursEndPath, quietHoursEnd.c_str());
      SerialPrintln(F("Quiet Hours updated: enabled=") + String(quietHoursEnabled ? F("true") : F("false")) +
        F(" start=") + quietHoursStart + F(" end=") + quietHoursEnd);
      request->send(200, "text/plain", "Quiet Hours updated");
    });

    //Without this, ESPAsyncWebSrv's catch-all handler returns 500 instead of 404 for unknown routes.
    webServer.onNotFound([](AsyncWebServerRequest * request) {
      request->send(404, "text/plain", "Not Found");
    });

    delay(250);
    webServer.begin();
    
    blinkLed(8, 200, 100); // 8 blinks = Web server ready
    debugStatus = "Ready";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    SerialPrintln(F("Split Flap Ready!"));
    SerialPrintln(F("#######################################################"));
  }
  else {
    if (isPendingReboot) {
      SerialPrintln(F("Reboot is pending to be able to continue device function. Hold please..."));
      SerialPrintln(F("#######################################################"));
    }
    else {
      SerialPrintln(F("Unable to connect to WiFi... Not starting web server"));
      SerialPrintln(F("Please hard restart your device to try connect again"));
      SerialPrintln(F("#######################################################"));
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

void loop() {
  //First I2C scan, delayed 30s after boot (see setup()) - runs here instead of blocking setup(),
  //so WiFi/the web server start immediately. Fires exactly once.
  static bool firstUnitScanDone = false;
  if (!firstUnitScanDone && millis() >= 30000) {
    firstUnitScanDone = true;
    SerialPrintln(F(""));
    SerialPrintln(F("=== I2C Bus Scan on Startup (delayed 30s) ==="));
    for (int scanAttempt = 0; scanAttempt < 3 && unitsAmount == 0; scanAttempt++) {
      if (scanAttempt > 0) {
        SerialPrintln(F("No units found, retrying scan..."));
        delay(300);
      }
      scanI2CBus();
    }
    SerialPrintln(F(""));
  }

  //Advances the display-update state machine by at most one I2C op, unconditionally and before
  //any deferred-request handling below, so an in-progress display update can't block loop().
  updateDisplayJob();

  //I2C admin requests deferred here by the web handlers (see pendingRescanRequest's comment near
  //unitsAmount) - checked first so a pending request gets served as soon as possible.
  if (pendingRescanRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingRescanRequest;
    pendingRescanRequestDisconnected = false; //this run's flag starts clean - see the comment near unitsAmount

    int detectedAmount = scanI2CBus();

    //Guard slot released only now (not before scanI2CBus() ran) so a request arriving mid-scan
    //correctly gets 503 instead of silently overwriting this one. Same pattern below.
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
      //Default is the light 3-byte status+offset read, not the 6-byte one with phase/progress -
      //the Administration page never displays those. ?detail=1 opts into the heavy read.
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
    for (int address = 0; address < UNITS_AMOUNT_MAX; address++) {
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
  //Does the pending Update.* call from loop()'s stack - Update.begin()/write()/end() need more
  //stack than an AsyncWebServer callback has available.
  if (otaPendingPhase == OTA_PHASE_BEGIN) {
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    uint32_t rounded = (otaBeginSize + 0xFFF) & 0xFFFFF000; //round up to a flash sector
    uint32_t updateSize = (rounded > 0 && rounded < maxSketchSpace) ? rounded : maxSketchSpace;

    bool ok = Update.begin(updateSize, U_FLASH);
    if (ok) {
      //Allocated here, not statically - free heap on this device is chronically tight, no reason
      //to hold 4KB for a rarely-used feature the other 99% of the time.
      if (otaChunkBuffer != nullptr) {
        delete[] otaChunkBuffer; //shouldn't happen (always freed below before this runs again), don't leak if it somehow does
      }
      otaChunkBuffer = new (std::nothrow) uint8_t[OTA_CHUNK_SIZE];
      ok = (otaChunkBuffer != nullptr);
      if (!ok) {
        SerialPrintln(F("Firmware upload: could not allocate chunk buffer"));
        Update.end(false); //release Update's internal "running" lock - see the #79-era lesson below
      }
    } else {
      SerialPrintln(F("Firmware upload: Update.begin() failed"));
      Update.printError(Serial);
    }

    otaResultSuccess = ok;
    otaResultMessage = ok ? "OK - ready for chunks" : "Failed to start update, check the serial log for details";
    otaCompletedPhase = OTA_PHASE_BEGIN;
    otaResultReady = true;
    otaPendingPhase = OTA_PHASE_NONE;

  } else if (otaPendingPhase == OTA_PHASE_WRITE) {
    bool ok = !otaChunkRejected && Update.write(otaChunkBuffer, otaChunkLen) == otaChunkLen;
    if (!ok) {
      SerialPrintln(F("Firmware upload: chunk write failed"));
      Update.printError(Serial);
      //Must still call end() here (ignoring its result): Update.begin() refuses to start a new
      //update while the previous one is still marked "running" internally, and only end() clears
      //that - skipping this leaves every subsequent upload failing until a full reboot.
      Update.end(false);
      delete[] otaChunkBuffer; //abort the whole upload - a missing chunk buffer makes the other
      otaChunkBuffer = nullptr; //two endpoints cleanly reject anything further
    }

    otaResultSuccess = ok;
    otaResultMessage = ok ? "OK" : "Chunk write failed, check the serial log for details";
    otaCompletedPhase = OTA_PHASE_WRITE;
    otaResultReady = true;
    otaPendingPhase = OTA_PHASE_NONE;

  } else if (otaPendingPhase == OTA_PHASE_FINISH) {
    bool ok = Update.end(true);
    if (!ok) {
      SerialPrintln(F("Firmware upload: Update.end() failed"));
      Update.printError(Serial);
    }
    delete[] otaChunkBuffer;
    otaChunkBuffer = nullptr;

    otaResultSuccess = ok;
    otaResultMessage = ok ? "OK - Firmware written, rebooting now" : "Update failed, check the serial log for details";
    otaCompletedPhase = OTA_PHASE_FINISH;
    otaResultReady = true;
    otaPendingPhase = OTA_PHASE_NONE;
  }

  // Abandoned-upload watchdog - see the comment on otaLastActivityAt above.
  if (otaChunkBuffer != nullptr && otaPendingPhase == OTA_PHASE_NONE &&
      (millis() - otaLastActivityAt) > OTA_ABANDONED_TIMEOUT_MS) {
    SerialPrintln(F("Firmware upload: no activity for ") + String(OTA_ABANDONED_TIMEOUT_MS / 1000) + F("s, aborting (old firmware stays active)"));
    Update.end(false);
    delete[] otaChunkBuffer;
    otaChunkBuffer = nullptr;
  }

  // Separate from the block above on purpose: the flash-side result and the deferred request can
  // become ready in either order.
  if (otaResultReady && pendingFirmwareUploadRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingFirmwareUploadRequest;
    bool wasDisconnected = pendingFirmwareUploadRequestDisconnected;
    pendingFirmwareUploadRequest = nullptr;

    if (!wasDisconnected) {
      AsyncWebServerResponse* response = deferredRequest->beginResponse(otaResultSuccess ? 200 : 500, "text/plain", otaResultMessage);
      response->addHeader("Connection", "close");
      deferredRequest->send(response);
    }

    otaResultReady = false;
    if (otaResultSuccess && otaCompletedPhase == OTA_PHASE_FINISH) {
      SerialPrintln(F("Firmware update successful, rebooting..."));
      isPendingReboot = true;
    } else if (!otaResultSuccess) {
      SerialPrintln(F("Firmware update failed"));
    }
  }
#endif

#if UNIT_BOOTLOADER_ENABLE == true
  if (pendingFirmwarePushRequest != nullptr) {
    AsyncWebServerRequest* deferredRequest = pendingFirmwarePushRequest;
    int unitIndex = pendingFirmwarePushUnitIndex;
    pendingFirmwarePushRequestDisconnected = false;

    SerialPrintln(F("Pushing firmware to Unit ") + String(unitIndex) + F("..."));
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

    SerialPrintln(F("Bootloader probe: requesting Unit ") + String(unitIndex) + F(" to enter bootloader..."));
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

      //Unconditional re-init: repeated-start I2C reads can leave the software I2C state stuck even
      //when the physical bus is idle - recoverI2CBus() alone doesn't catch that.
      Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
      Wire.setClock(400000);
#if UNIT_BOOTLOADER_ENABLE == true
      Wire.setClockStretchLimit(25000);
#endif

      //Explicitly ask to return to the app - twiboot's own ~1000ms idle-timeout wasn't reliable in
      //practice (unit stayed in the bootloader 44+s once, needed a power cycle).
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
    SerialPrintln(F("Rebooting Now... Fairwell!"));
    SerialPrintln(F("#######################################################"));
    delay(100);

    ESP.restart();
    return;
  }

#if WIFI_USE_DIRECT == false
  //Clear off the WiFi Manager Settings
  if (isPendingWifiReset) {
    SerialPrintln(F("Removing WiFi settings"));
    wifiManager.resetSettings();
    delay(100);

    isPendingReboot = true;
    return;
  }
#endif

  //No custom WiFi reconnect-monitoring here deliberately - a prior version fought the SDK's own
  //auto-reconnect and caused instability.

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

  //If no units detected, skip anything unit-related - web server/OTA/settings stay usable; use
  //"Rescan Units" once wiring/addressing is fixed.
  bool unitsAvailable = (unitsAmount > 0);

  //updateUnitsResetSequence() does a non-blocking blank1 -> pause -> blank2 sequence.
  if (unitsAvailable && isPendingUnitsReset) {
    updateUnitsResetSequence();
  } else if (!unitsAvailable) {
    isPendingUnitsReset = false; //nothing to reset, don't leave this stuck pending
  }
  
  //ezTime library sync
  events(); 
  
  //Process every second
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;

    checkScheduledMessages();
    checkCountdown();
    checkCountup();
    checkRotation();
#if WIFI_USE_DIRECT == false
    checkWifiScanRequest();
    checkWifiConnectAttempt();
#endif

    // Skip display updates if a web request is active to prevent blocking.
    // (showText() itself is a no-op when no units are detected, see scanI2CBus())
    if (!webRequestActive) {
      //Mode Selection
      if (deviceMode == DEVICE_MODE_ROTATION) {
        //rotationCurrentAlignment ("" = inherit the default) is set every tick by checkRotation().
        showText(inputText, 0, rotationCurrentAlignment);
      }
      else if (deviceMode == DEVICE_MODE_TEXT || deviceMode == DEVICE_MODE_COUNTDOWN || deviceMode == DEVICE_MODE_COUNTUP) {
        showText(inputText);
      }
      else if (deviceMode == DEVICE_MODE_DATE) {
        showText(timezone.dateTime(convertUserDateFormatToEzTime(dateFormat)));
      }
      else if (deviceMode == DEVICE_MODE_CLOCK) {
        showText(timezone.dateTime(convertUserDateFormatToEzTime(clockFormat)));
      }
    } else {
      // Web request is active, skip display update to keep web server responsive
      yield(); // Give web server time to process
    }
  }
}

//Scans the I2C bus and (re-)determines unitsAmount: units found sequentially from address 0,
//stopping at the first gap. Sets the global as a side effect.
int scanI2CBus() {
  SerialPrintln(F("========================================"));
  SerialPrintln(F("I2C Bus Scan - Checking addresses 0-") + String(UNITS_AMOUNT_MAX - 1));
  SerialPrintln(F("========================================"));

  int foundCount = 0;
  bool foundUnits[UNITS_AMOUNT_MAX] = {false}; // Track which addresses are found
  int unitStatus[UNITS_AMOUNT_MAX]; // Track status of each address (-2 = not found, -1 = sleeping, 0 = ready, 1 = busy)
  for (int i = 0; i < UNITS_AMOUNT_MAX; i++) unitStatus[i] = -2; // {-2} only zero-initializes element 0 for a non-constant size

  // Scan all possible I2C addresses (0..UNITS_AMOUNT_MAX-1)
  for (int address = 0; address < UNITS_AMOUNT_MAX; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      foundUnits[address] = true;
      foundCount++;

      SerialPrint(F("✓ Found device at address "));
      SerialPrint(address);

      Wire.requestFrom(address, 1, 1);
      if (Wire.available()) {
        int status = Wire.read();
        unitStatus[address] = status;

        SerialPrint(F(" - Status: "));
        SerialPrint(status);

        if (status == 0) {
          SerialPrintln(F(" (READY)"));
        } else if (status == 1) {
          SerialPrintln(F(" (BUSY/MOVING)"));
        } else if (status == -1) {
          SerialPrintln(F(" (SLEEPING)"));
        } else {
          SerialPrintln(F(" (UNKNOWN)"));
        }
      } else {
        SerialPrintln(F(" - No status response"));
        unitStatus[address] = -3; // No response
      }
    } else if (error == 2) {
      // NACK on address - device not found (normal for unused addresses)
      // Don't log this to reduce noise
    } else if (error == 4) {
      SerialPrint(F("⚠ Unknown I2C error at address "));
      SerialPrintln(address);
    }

    yield();
    delay(10);
  }

  // Determine unitsAmount: count contiguous "found" addresses starting at 0
  int detectedAmount = 0;
  while (detectedAmount < UNITS_AMOUNT_MAX && foundUnits[detectedAmount]) {
    detectedAmount++;
  }
  unitsAmount = detectedAmount;

  // Summary report
  SerialPrintln(F(""));
  SerialPrintln(F("--- Scan Summary ---"));
  SerialPrint(F("Total devices found: "));
  SerialPrintln(foundCount);
  SerialPrint(F("Detected unitsAmount (sequential from address 0): "));
  SerialPrintln(unitsAmount);
  SerialPrintln(F(""));

  SerialPrintln(F("Unit Status:"));
  for (int i = 0; i < unitsAmount; i++) {
    SerialPrint(F("  Unit "));
    SerialPrint(i);
    SerialPrint(F(" (address "));
    SerialPrint(i);
    SerialPrint(F("): ✓ CONNECTED"));

    if (unitStatus[i] == 0) {
      SerialPrintln(F(" - Ready"));
    } else if (unitStatus[i] == 1) {
      SerialPrintln(F(" - Busy"));
    } else if (unitStatus[i] == -1) {
      SerialPrintln(F(" - Sleeping"));
    } else if (unitStatus[i] == -3) {
      SerialPrintln(F(" - No status response"));
    } else {
      SerialPrint(F(" - Status code: "));
      SerialPrintln(unitStatus[i]);
    }
  }

  // Anything found beyond the contiguous run is likely a gap earlier in the sequence
  // (a missing/misconfigured unit) rather than genuinely "extra" devices
  if (foundCount > unitsAmount) {
    SerialPrintln(F(""));
    SerialPrintln(F("⚠ Devices found beyond the contiguous 0..N-1 range - check for a gap (a missing/misconfigured unit earlier in the sequence?):"));
    for (int i = unitsAmount; i < UNITS_AMOUNT_MAX; i++) {
      if (foundUnits[i]) {
        SerialPrint(F("  Address "));
        SerialPrint(i);
        SerialPrintln(F(" - responds, but out of sequence"));
      }
    }
  }

  SerialPrintln(F(""));
  if (unitsAmount == 0) {
    SerialPrintln(F("⚠ WARNING: No units detected at address 0. Display functionality is unavailable until this is fixed. Check:"));
    SerialPrintln(F("  1. DIP switch settings (address 0 must be assigned to a unit)"));
    SerialPrintln(F("  2. I2C wiring (SDA/SCL connections)"));
    SerialPrintln(F("  3. Power connections"));
    SerialPrintln(F("  Use the Admin page's \"Rescan Units\" button once fixed, no reboot required."));
  } else {
    SerialPrintln(F("✓ ") + String(unitsAmount) + F(" unit(s) detected and ready."));
  }

  SerialPrintln(F("========================================"));
  return unitsAmount;
}

