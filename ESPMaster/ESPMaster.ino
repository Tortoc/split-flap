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
#define USE_MULTICAST       true   //broadcast a "<mdnsName>.local" hostname on the local network
                                   //(found missing vs. the real upstream, JonnyBooker/split-flap
                                   //commit b3465b1/PR #30 - our fork branched off an earlier point
                                   //via the scientress fork and never picked this up). If running
                                   //more than one display on the same network, mdnsName below must
                                   //be unique per device.
//Numbered boot-progress blink codes (1-8, one per setup() stage) were removed 2026-09-10 on user
//request - they blocked every boot for ~8.7s of delay() regardless of outcome, and (since
//ledOn() only actually lights up while showDebugPage is on, see its own comment) usually didn't
//even visibly blink in normal production use. LED_ENABLE now only gates the "WiFi totally failed
//to connect" continuous error blink at the end of setup() - see that block's own comment.
#define LED_ENABLE          true
#define DEBUG_ENABLE true  //startup debug page, error panel, page-load log, serial debug log -
                           //gated at runtime by showDebugPage (POST /api/debug-mode, persisted
                           //in /debugmode.txt)
#define MQTT_ENABLE true   //MQTT + Home Assistant auto-discovery (found 2026-09-10 in two
                           //independent forks, see
                           //ServiceMqttFunctions.ino). This flag alone decides whether the
                           //feature exists in the firmware AT ALL - PubSubClient's own buffer is
                           //malloc()'d from its constructor, so a compile-time false here is the
                           //only way to avoid that cost entirely. The separate, persisted
                           //mqttEnabled (runtime, via POST /api/mqtt) controls whether it actually
                           //connects - false by default, changeable without a reflash.

#define WIFI_STATIC_IP      false //try a router-assigned static IP instead of DHCP
#define FAE_MOD                   //modified PCB with an ESP-12F module

//ESP01 GPIO1/TX is used for I2C SDA and GPIO3/RX for I2C SCL (Wire.begin(1, 3)) - so the debug LED
//goes on GPIO2 instead of the built-in GPIO1 LED. VCC must be 3.3V only.
#define LED_DEBUG_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

//The numbered per-stage startup blink codes this comment used to document were removed
//2026-09-10 (see LED_ENABLE's own comment) - the only LED behavior left is a continuous blink at
//the end of setup() if WiFi never connects at all, only visible while debug mode is on
//(see ledOn()'s own comment).

//--- Constants - only change if you know what you're doing ---
#define ANSWER_SIZE         6 //status + offset + phase + progress bytes, must match Unit.ino's requestEvent()
#define ANSWER_SIZE_LIGHT   3 //status + offset only, see readUnitInfo()'s 3-arg overload
#define ANSWER_SIZE_DIAG    8 //adds the two diagnostic counters (found 2026-09-10, inspired by
                               //scottbez1/splitflap) - see readUnitInfo()'s 7-arg overload. Unit's
                               //requestEvent() always writes all 8 bytes now; a master still using
                               //ANSWER_SIZE or ANSWER_SIZE_LIGHT just reads fewer of them, same
                               //backwards-compatible relationship those two already had. IMPORTANT
                               //(see readUnitInfo()'s 7-arg overload comment): a unit not yet
                               //updated to the newer Unit.ino still answers an 8-byte request
                               //without error - the last 2 bytes just read back as 255 (idle bus),
                               //not a detectable "unsupported" marker.
#define FLAP_AMOUNT         45
#define MIN_SPEED           1
#define MAX_SPEED           12
#define UNIT_STEPS          2038    //must match STEPS in Unit.ino
#define UNIT_BOOTLOADER_CMD 255     //must match CMD_ENTER_BOOTLOADER in Unit.ino
#define UNIT_BOOTLOADER_BASE_ADDRESS 0x29 //start of the twiboot bootloader address range (0x29-0x68) -
                                           //NOTE: 0x68 is also RTC_I2C_ADDRESS below - no real conflict
                                           //at the current 10-unit installation (bootloader addresses
                                           //only reach 0x29+9=0x32), but would collide if ever expanded
                                           //to near the structural 64-unit ceiling (UNITS_AMOUNT_MAX).
#define CRASH_LOG_PATH "/crash-log.jsonl"
//Optional DS3231 RTC module on the same I2C bus (added 2026-09-11 on user request, see
//ServiceRtcFunctions.ino) - fixed address per the DS3231 datasheet, not configurable. Entirely
//optional/auto-detected (probeRtc()) - every other build/device without one attached behaves
//exactly as before this feature existed.
#define RTC_I2C_ADDRESS 0x68

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
#if USE_MULTICAST == true
#include <ESP8266mDNS.h>
#endif
#include <ezTime.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#if MQTT_ENABLE == true
#include <PubSubClient.h>
#endif
#include <Wire.h>
//twi.h's raw twi_writeTo() is used for twiboot flash-page writes instead of Wire - a page write is
//132 bytes, which exceeds Wire's 128-byte TX buffer and silently drops the tail.
#include <twi.h>
#include <memory> //std::shared_ptr - PendingRequestSlot's per-generation disconnect flag, CODE_REVIEW_V2.md #1
#include <atomic> //std::atomic<bool> - same reason
#include <user_interface.h> //struct rst_info, for the crash reporter
#include <lwip/priv/tcp_priv.h> //exposes tcp_active_pcbs/etc. for /api/tcp-status
#include "Classes.h"
#include "LittleFS.h"

//--- User-facing settings ---
//Hostname broadcast via mDNS when USE_MULTICAST is enabled - reachable as "<mdnsHostname>.local"
//on networks whose clients support mDNS (most desktop OSes and phones; not universal on all
//routers/IoT-only networks). Persisted, not a compile-time constant (found 2026-09-10: with only
//one device on this repo's hardware, a fixed name was fine at first, but the user asked to be
//able to change it themselves - e.g. once a second display joins the same network and needs a
//unique name) - see POST /api/wifi's "set-hostname" action (Netzwerk tab in the UI).
String mdnsHostname = "split-flap";

//Used if connecting via "WIFI_USE_DIRECT" of "true" - Otherwise, leave blank
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";

//Runtime-changeable OTA password, persisted to LittleFS (see /api/ota-password). Default "splitflap".
//Empty string = password protection globally OFF - every endpoint below (including firmware/
//flash ones) is then unauthenticated, no exceptions, regardless of the toggles below (found
//2026-09-09, CODE_REVIEW.md #29 - the previous design applied this check to only 4 firmware-
//related endpoints, left the password itself readable unauthenticated via this same field over
//GET /api/settings and a dedicated GET /api/ota-password [both removed], AND separately exposed
//every persisted config file - including this very password - directly via serveStatic()'s
//filesystem-wide static route [also fixed, see the comment there]).
String otaPassword = "";
const char* otaPasswordPath = "/otapassword.txt";
const char* mdnsHostnamePath = "/mdnsname.txt";

#if MQTT_ENABLE == true
//WiFiClient/PubSubClient objects declared here, not in ServiceMqttFunctions.ino - the Arduino
//build concatenates .ino files in filename order (ESPMaster.ino before ServiceMqttFunctions.ino),
//so a global used by code IN this file (GET /api/settings and POST /api/mqtt below both read
//mqttClient.connected()) must be declared before that point, not in a later file.
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

//MQTT broker settings - all persisted, all runtime-changeable via POST /api/mqtt (System ->
//MQTT tab in the UI), same pattern as mdnsHostname/otaPassword above. mqttEnabled defaults to
//off - MQTT_ENABLE (the compile-time flag above) only controls whether the feature EXISTS in
//this firmware at all, not whether it's actively connecting.
bool mqttEnabled = false;
String mqttBrokerHost = "";
int mqttBrokerPort = 1883;
String mqttUsername = "";
//Same never-sent-back-over-GET convention as otaPassword - see that comment.
String mqttPassword = "";
const char* mqttEnabledPath = "/mqttenabled.txt";
const char* mqttBrokerHostPath = "/mqttbrokerhost.txt";
const char* mqttBrokerPortPath = "/mqttbrokerport.txt";
const char* mqttUsernamePath = "/mqttusername.txt";
const char* mqttPasswordPath = "/mqttpassword.txt";
#endif

//Per-category "reachable without the password" toggles - only consulted when otaPassword is
//non-empty (an empty password already means everything is open, see its own comment). Each
//gates a specific, named group of state-changing endpoints - see isRequestAllowedWithoutPassword()
//(ServiceAccessControlFunctions.ino) for exactly which. Firmware/flash endpoints
//(/api/web-file, /api/units/firmware*, /api/firmware-*) and this access-control configuration
//itself deliberately have NO toggle - always protected whenever a password is set, per explicit
//user requirement. Default false (protected) for every toggle - a newly-added category defaults
//to the safe state, not accidentally-public.
bool accessControlTextPublic = false;      //POST /api/text
bool accessControlSettingsPublic = false;  //POST /api/settings
bool accessControlUnitsPublic = false;     //POST /api/units?action=offset|reset
bool accessControlWifiPublic = false;      //POST /api/wifi
bool accessControlTimezonePublic = false;  //POST /api/timezone
bool accessControlSystemPublic = false;    //POST /api/reboot, POST /api/debug-mode
const char* accessControlTextPublicPath = "/accesscontrol-text.txt";
const char* accessControlSettingsPublicPath = "/accesscontrol-settings.txt";
const char* accessControlUnitsPublicPath = "/accesscontrol-units.txt";
const char* accessControlWifiPublicPath = "/accesscontrol-wifi.txt";
const char* accessControlTimezonePublicPath = "/accesscontrol-timezone.txt";
const char* accessControlSystemPublicPath = "/accesscontrol-system.txt";

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
//
//Generation-tracked, not a bare pointer+bool: ESPAsyncWebServer deletes the request object on
//client disconnect, even mid-flight, and loop() frees the slot for a new request the moment it
//starts working on the current one (so a second request isn't needlessly rejected while the
//first is still being processed). Found 2026-09-09 (CODE_REVIEW.md #4): with a bare pointer+bool,
//an old request's disconnect firing during that window would unconditionally null out whatever
//NEWER request has since taken the slot, and flag it as disconnected too - a real, reachable race
//between two genuinely unrelated requests, not just a single slow one. accept()/take()/
//stillConnected() below scope every disconnect to the specific request generation it belongs to.
struct PendingRequestSlot {
  AsyncWebServerRequest* request = nullptr;
  uint32_t generation = 0;             //bumped every time a new request is accepted into this slot

  //Per-generation disconnect flag, NOT a single "last disconnected generation" field like before
  //(found 2026-09-09, CODE_REVIEW_V2.md #1, live-reproducible with the extracted struct): a single
  //shared "disconnectedGeneration" field only remembers the MOST RECENT disconnect - if generation
  //N disconnects (disconnectedGeneration=N), then a NEWER generation M gets accepted into the
  //now-freed slot and ALSO disconnects (disconnectedGeneration=M) BEFORE N's own
  //stillConnected(N) check ever runs, that check would wrongly see "last disconnected was M, not
  //N" and report N as still connected - a stale, already-deleted request pointer then gets used.
  //Each accept() now creates its own dedicated flag instead, captured by that generation's own
  //onDisconnect closure AND handed out by take() to the caller - kept alive via shared_ptr
  //reference counting independent of whatever the slot itself does afterward (a newer accept()
  //replaces the slot's own copy in disconnectedFlag below, but a caller who already took() an
  //older generation is holding its own reference to the OLD flag, unaffected by that).
  std::shared_ptr<std::atomic<bool>> disconnectedFlag;

  //Call from the HTTP request handler when accepting a new request into this (assumed-empty) slot.
  //Registers the disconnect callback itself, correctly scoped to this specific generation's own flag.
  void accept(AsyncWebServerRequest* newRequest) {
    uint32_t myGeneration = ++generation;
    request = newRequest;
    auto myFlag = std::make_shared<std::atomic<bool>>(false);
    disconnectedFlag = myFlag;
    newRequest->onDisconnect([this, myGeneration, myFlag]() {
      myFlag->store(true);
      if (generation == myGeneration) { //still the current occupant - safe to clear the slot too
        request = nullptr;
      }
    });
  }

  //Call from loop() when starting to process this slot - returns the request to use and frees the
  //slot immediately so a new request can be accepted while this one's work is still in progress.
  //flagOut is this specific request's own disconnect flag, for the stillConnected() check below -
  //hold onto it (it's a shared_ptr, safe to keep past this call) until that check is done.
  AsyncWebServerRequest* take(std::shared_ptr<std::atomic<bool>> &flagOut) {
    AsyncWebServerRequest* result = request;
    flagOut = disconnectedFlag;
    request = nullptr;
    return result;
  }

  //Call from loop() right before using the request returned by take() (e.g. ->send()) - false
  //means that specific request's connection is gone and the pointer is no longer safe to touch.
  //Pass the exact flag take() gave you for this same request, not the slot's current one (which
  //may since belong to a different, newer generation).
  static bool stillConnected(const std::shared_ptr<std::atomic<bool>> &flag) {
    return flag && !flag->load();
  }
};

PendingRequestSlot pendingRescanSlot;
PendingRequestSlot pendingUnitsInfoSlot;
PendingRequestSlot pendingLegacyI2CScanSlot;
bool pendingUnitsInfoRequestDetail = false; //captured from the "detail" query param at request time

//writeOffsetToUnit() used to be called directly from the /api/units?action=offset handler - the
//only I2C write anywhere in this file NOT deferred to loop() like every other one, for no
//documented reason (found 2026-09-09, CODE_REVIEW.md #30). Same deferred-request pattern as
//pendingRescanSlot above.
PendingRequestSlot pendingUnitOffsetSlot;
int pendingUnitOffsetUnitId = -1;
uint16_t pendingUnitOffsetValue = 0;

//timezone.setLocation() does a synchronous network lookup against ezTime's timezoned service -
//was called directly from the /api/timezone POST handler, blocking that AsyncWebServer callback
//on network I/O instead of the response-stream/deferred-to-loop() pattern used everywhere else in
//this file (found 2026-09-09, CODE_REVIEW.md #30). Same deferred-request pattern as
//pendingRescanSlot/pendingUnitOffsetSlot above.
PendingRequestSlot pendingTimezoneSlot;
String pendingTimezoneValue = "";
//The actual lookup (checkTimezoneLookup(), ServiceAsyncTimeSyncFunctions.ino) now spans MULTIPLE
//loop() ticks instead of completing within the one tick that takes the request out of
//pendingTimezoneSlot - these hold the deferred request across that whole span. See the loop()
//block that processes them for the full explanation.
AsyncWebServerRequest* timezoneLookupDeferredRequest = nullptr;
std::shared_ptr<std::atomic<bool>> timezoneLookupDeferredFlag;
//Snapshot of pendingTimezoneValue taken at kickoff time, NOT re-read from that shared global at
//finish time (found 2026-09-09, CODE_REVIEW_V2.md #14/Nachtrag: pendingTimezoneValue is a single
//global - a second request accepted into the slot while the first's async lookup is still running
//would overwrite it before the first one's result gets persisted, saving the SECOND request's
//value under the FIRST request's successful lookup). The lookup itself (startTimezoneLookup())
//already took its own internal copy correctly; this is specifically for what gets written to
//timezoneSetting/the settings file once the lookup completes.
String timezoneLookupDeferredValue = "";
//Declared here (not in ServiceAsyncTimeSyncFunctions.ino, where they're actually driven from) -
//ESPMaster.ino is concatenated/compiled before that file in the sketch, and loop() above
//references them directly; global variables (unlike functions) aren't auto-forward-declared
//across .ino files, only within the same one (same reasoning as debugSimulateWriteFailureForUnit
//elsewhere in this file, from an earlier P2 fix).
bool tzLookupDone = false;
bool tzLookupResult = false;

//Retry tracking for the ONE-SHOT boot-time timezone lookup (started once in setup(), entirely
//fire-and-forget - see the comment there) - found 2026-09-10, CODE_REVIEW_V3.md #12: a transient
//DNS/network failure during that single attempt permanently left the active timezone RULE
//unresolved (ezTime's own 'UTC' default) until either an explicit POST /api/timezone or a luckier
//boot - the later periodic NTP resync only refreshes the CLOCK, never re-resolves the zone RULE.
//bootTimezoneLookupPending tracks "a boot/retry attempt is currently in flight, its own result not
//yet consumed" so the retry logic below reacts to each completion exactly once (tzLookupDone
//itself stays true indefinitely once set, so it alone can't distinguish a fresh completion from
//an already-handled one - same reasoning as timezoneLookupDeferredRequest's nulling above, just
//without an HTTP request to null).
bool bootTimezoneRuleResolved = false;
bool bootTimezoneLookupPending = false;
unsigned long nextBootTimezoneRetryAt = 0; //0 = no retry currently scheduled
#define BOOT_TIMEZONE_RETRY_MS 60000UL //not urgent - just needs to eventually succeed

//Same reasoning, for the twiboot/bootloader endpoints (UNIT_BOOTLOADER_ENABLE).
PendingRequestSlot pendingFirmwarePushSlot;
int pendingFirmwarePushUnitIndex = -1;
PendingRequestSlot pendingBootloaderProbeSlot;
int pendingBootloaderProbeUnitIndex = -1;

//Search for parameter in HTTP POST request
const char* PARAM_ALIGNMENT = "alignment";
const char* PARAM_FLAP_SPEED = "flapSpeed";
const char* PARAM_ANIMATION_MODE = "animationMode";
const char* PARAM_ANIMATION_STAGGER_MS = "animationStaggerMs";
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

//Animation options ("Animationen", found 2026-09-10 on user request) - see
//computeUnitAnimationDelayMs() (ServiceDisplayJobFunctions.ino) for what each one actually does.
//Defined here, not in that file, despite being used there too - the Arduino build concatenates
//.ino files in filename order (ESPMaster.ino before ServiceDisplayJobFunctions.ino), so a #define
//used by code in THIS file (the ANIMATION_MODES array right below, and POST /api/settings'
//validation further down) must be declared before that point, same reason mqttClient etc. had to
//move for the MQTT feature.
#define ANIMATION_MODE_SIMULTANEOUS   "simultaneous"
#define ANIMATION_MODE_LEFT_TO_RIGHT  "left-to-right"
#define ANIMATION_MODE_RIGHT_TO_LEFT  "right-to-left"
#define ANIMATION_MODE_OUTSIDE_IN     "outside-in"
#define ANIMATION_MODE_INSIDE_OUT     "inside-out"
const char* ANIMATION_MODES[] = {
  ANIMATION_MODE_SIMULTANEOUS, ANIMATION_MODE_LEFT_TO_RIGHT, ANIMATION_MODE_RIGHT_TO_LEFT,
  ANIMATION_MODE_OUTSIDE_IN, ANIMATION_MODE_INSIDE_OUT
};
#define ANIMATION_MODES_COUNT 5
#define ANIMATION_STAGGER_MS_MAX 1000 //see the plan's own reasoning - 10 units * 1000ms is already
                                       //a ~9s spread for the last unit; more than that stops being
                                       //a display effect and starts being just slow.

//Shared by POST /api/settings' own validation and replaceRotationItemsFromJson()'s per-item
//validation (found 2026-09-10, "Animation pro Rotation-Item") - one place for the "which values
//are valid" list rather than two independently-maintained copies. Functions (unlike the #defines/
//globals above) are auto-prototyped by the Arduino build regardless of file order, so this is
//safe to call from any other .ino file despite being defined here.
bool isValidAnimationMode(String value) {
  for (int modeIndex = 0; modeIndex < ANIMATION_MODES_COUNT; modeIndex++) {
    if (value == ANIMATION_MODES[modeIndex]) {
      return true;
    }
  }
  return false;
}

//File paths to save input values permanently
const char* alignmentPath = "/alignment.txt";
const char* flapSpeedPath = "/flapspeed.txt";
const char* animationModePath = "/animationmode.txt";
const char* animationStaggerMsPath = "/animationstagger.txt";
const char* deviceModePath = "/devicemode.txt";
const char* countdownPath = "/countdown.txt";
const char* scheduledMessagesPath = "/scheduled-messages.txt";
const char* debugModePath = "/debugmode.txt";
const char* unitFirmwarePath = "/unit-firmware.bin";
//Temp path for the unit-firmware upload's write-then-atomic-rename() pattern - see the comment on
//that handler (CODE_REVIEW.md #5).
const char* unitFirmwareTmpPath = "/unit-firmware.bin.tmp";
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
String animationMode = ANIMATION_MODE_SIMULTANEOUS; //default - see the user's own confirmation
unsigned long animationStaggerMs = 150; //ms between consecutive units' start, see ANIMATION_MODES
String inputText = "";
String deviceMode = "";
String countdownToDateUnix = "";
String countupSinceDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
//Together with lastWrittenText and displayRenderGeneration below, what showText() compares a new
//request against to decide whether a redraw is actually needed - see the long comment on
//displayRenderGeneration (found 2026-09-09, CODE_REVIEW.md #14). Replaces the old bare
//alignmentUpdated bool, which only ever reflected the GLOBAL default alignment setting, not the
//per-call alignmentOverride Rotation items use - two Rotation entries with identical text but
//different per-item alignment never triggered a redraw. lastWrittenAlignment stores whichever
//alignment (override or global) was ACTUALLY used for the last completed render, so it's directly
//comparable against a new call's own effective alignment regardless of which one it came from.
String lastWrittenAlignment = "";
//Bumped whenever something could make the PHYSICAL display state diverge from what
//lastWrittenText/lastWrittenAlignment claim, without the requested text or alignment itself
//necessarily changing - the global alignment setting changing (replaces the old alignmentUpdated
//bool), a rescan (a unit may have restarted and lost its displayed letter), or a per-unit
//calibration offset change (recalibration re-homes the unit, so its previously-displayed letter
//is no longer guaranteed correct). showText() redraws whenever this doesn't match
//lastRenderedGeneration (the generation that was current at the last completed render, set by
//finishDisplayJob()), even if lastWrittenText and the effective alignment are both unchanged.
unsigned long displayRenderGeneration = 0;
unsigned long lastRenderedGeneration = 0;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
bool isWifiConfigured = false;
//Optional DS3231 RTC (ServiceRtcFunctions.ino) - declared here, not in that file, since
//setup()/GET /api/diag below (both in this earlier-concatenated main sketch file) need them; see
//probeRtc()'s own comment for the full reasoning.
bool rtcPresent = false;
bool rtcOscillatorStopFlag = false;
bool showDebugPage = false; //defaults off - the startup debug page can render blank on fresh boots
volatile bool webRequestActive = false; //skip display updates while a web request is in flight
LList<ScheduledMessage> scheduledMessages;
//Fixed array, not LList<RotationItem>: LinkedList's operator[] can dereference a NULL node under
//interleaved access - a real, reproduced crash. Direct indexing avoids it.
#define ROTATION_ITEMS_MAX 20
RotationItem rotationItems[ROTATION_ITEMS_MAX];
int rotationItemCount = 0;
int rotationCurrentIndex = 0;
//Counts seconds the CURRENT rotation item was actually accepted by showText() (loop()'s
//mode-dispatch block increments this after a successful call), not wall-clock seconds since its
//turn started - found 2026-09-09, CODE_REVIEW_V2.md #13: a pure wall-clock timer kept
//advancing/wrapping the index every item's durationSeconds regardless of whether the display was
//even free to actually show it (a busy multi-line job or a temporary /api/text push both silently
//swallow showText() calls made while they're running), so a slow-to-render or often-blocked item
//could get skipped almost entirely while later items absorbed all the real visible time. Reset to
//0 whenever the current item changes (checkRotation() below) or rotation restarts.
unsigned long rotationItemAcceptedSeconds = 0;
//Counts EVERY tick the current item has been current, regardless of whether showText() actually
//accepted it - unlike rotationItemAcceptedSeconds above, which only counts genuinely
//accepted/settled ticks. Used purely as a "how long has this item been stuck" defense in
//checkRotation() (CODE_REVIEW_V3.md #13) - a permanently-unrenderable item (too many lines,
//whether a pre-existing text item saved before the upfront validation in
//replaceRotationItemsFromJson() existed, or a dynamic clock/date/countdown/countup item whose
//content unpredictably grows too long) would otherwise leave rotationItemAcceptedSeconds stuck
//below durationSeconds forever, permanently blocking every item behind it too. Reset alongside
//rotationItemAcceptedSeconds, at every one of its own reset points.
unsigned long rotationItemElapsedRealSeconds = 0;
//Set by checkRotation() from the current item's Alignment field ("" = inherit the default) and
//threaded through to showText() as an override.
String rotationCurrentAlignment = "";
//Same pattern as rotationCurrentAlignment above, for the per-item animation override (found
//2026-09-10, "Animation pro Rotation-Item") - "" / -1 = inherit the global animationMode/
//animationStaggerMs.
String rotationCurrentAnimationMode = "";
int rotationCurrentAnimationStaggerMs = -1;
Timezone timezone;

//Bug (found while fixing #117): despite the name/parameter, neither timezone.now() nor
//timezone.tzTime(TIME_NOW, UTC_TIME) actually return UTC - both return local time (verified
//empirically via a temporary /api/diag?type=timetest probe: both were identical to each other and
//exactly getOffset() minutes ahead of a known-correct external UTC clock). Reversing the offset
//by hand is the only way found to get real UTC seconds out of this library - needed everywhere
//this compares against a client-supplied Unix timestamp (scheduled messages, countdown/countup,
//rotation items), since the client always sends real UTC seconds (e.g. JS Date.now()/1000).
long getRealUtcNow() {
  return timezone.now() + (long)timezone.getOffset() * 60;
}

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
//Which request currently "owns" otaChunkBuffer/otaChunkLen while its body is still streaming in -
//found 2026-09-10, CODE_REVIEW_V3.md #1 (live-reproduced with the extracted body handler): the
//existing otaPendingPhase!=OTA_PHASE_NONE check in the body handler only catches a SECOND request
//starting once a FIRST request's entire body has already been fully received and its outer
//handler has run (that's the only thing that ever sets otaPendingPhase away from NONE). Two
//requests whose bodies are BOTH still streaming in concurrently - fully possible on this
//single-threaded event loop, since AsyncTCP services multiple open connections' incoming data in
//whatever order packets arrive - see otaPendingPhase stay OTA_PHASE_NONE the entire time either
//body is mid-transfer, so neither ever rejects the other; repro: buffer holding "AA" (chunk A's
//first fragment) ends up "BBaa" once B's index==0 fragment resets otaChunkLen out from under A's
//still-in-flight remainder. otaChunkOwner closes that gap directly: claimed by whichever request's
//body arrives at index==0 first, checked (not just otaPendingPhase) by every later request's own
//index==0, and released once its owning request's outer handler runs (body fully received) or by
//the abandoned-upload watchdog if the connection vanishes before that.
AsyncWebServerRequest* otaChunkOwner = nullptr;
volatile size_t otaChunkLen = 0;
enum OtaPhase { OTA_PHASE_NONE, OTA_PHASE_BEGIN, OTA_PHASE_WRITE, OTA_PHASE_FINISH };
volatile OtaPhase otaPendingPhase = OTA_PHASE_NONE; //what loop() should do next
volatile OtaPhase otaCompletedPhase = OTA_PHASE_NONE;
volatile uint32_t otaBeginSize = 0;
//Bytes actually handed to Update.write() so far, tracked ourselves rather than trusting
//Update.progress() at finish time (found 2026-09-09, live-verified against a real firmware image
//- see the comment at the finish-phase comparison below for why Update.progress() alone is wrong).
volatile size_t otaBytesWritten = 0;
//Removed 2026-09-10, CODE_REVIEW_V4.md #1: this used to be a single shared/global "reject this
//chunk" flag, set by EITHER the true owner's own body OR a losing concurrent request's body, and
//cleared by whichever request's outer handler happened to run next - regardless of which one
//actually caused/deserved it. Repro: A owns a chunk, B loses the ownership race and sets the
//shared flag, A's own next fragment gets silently dropped because of it, A's outer handler then
//reports 400 and (wrongly) clears the flag on B's behalf, letting B's later fragment fall through
//and get appended onto A's leftover buffer - a Frankenstein "AAbb" chunk gets accepted for
//writing. Fixed by never letting a losing request's body touch ANY shared state at all (see the
//per-fragment "otaChunkOwner != request" check below) and having the outer handler derive its
//response purely from "was I (still) the owner when my body finished" (a request-pointer
//comparison, inherently request-scoped) plus statelessly re-checked facts (password,
//request->contentLength()) instead of a mutable shared flag.
volatile bool otaResultReady = false;
volatile bool otaResultSuccess = false;
String otaResultMessage = "";
volatile unsigned long otaLastActivityAt = 0; //watchdog for an abandoned upload (client vanished)
//90s: a client retrying a dropped chunk over flaky WiFi can genuinely take a while even though the
//upload itself is fine.
#define OTA_ABANDONED_TIMEOUT_MS 90000
PendingRequestSlot pendingFirmwareUploadSlot; //reused in turn by all three /api/firmware-* steps
#endif
#if UNIT_BOOTLOADER_ENABLE == true
//Set when the upload couldn't be written/moved into place (open() or write() failure, or the
//final rename() failing) - see the temp-file+atomic-rename pattern on that handler
//(CODE_REVIEW.md #5). Safe to keep global/shared despite CODE_REVIEW_V4.md #1/#2 (unlike the
//removed ...AuthFailed/...BusyFailed flags below): only ever SET inside the "am I the current
//owner" gated section of the body handler, and only ever READ/cleared by the outer handler after
//confirming (via wasOwner) that THIS request still is that same owner - and reset to false the
//moment a NEW request claims ownership, so a vanished previous owner's leftover true can never
//leak into the next one's result.
bool unitFirmwareUploadWriteFailed = false;
//Which request currently "owns" the shared static unitFirmwareFile/temp-file state while its body
//is still streaming in - found 2026-09-10, CODE_REVIEW_V3.md #2 (live-reproduced against the
//extracted callback): the earlier CODE_REVIEW_V2.md #3 fix only reordered the password check
//ahead of the cleanup step, so an UNAUTHENTICATED request could no longer disrupt an
//authenticated one - but two AUTHENTICATED uploads (e.g. an accidental double-submit, or two
//admin sessions) still shared the exact same static File/temp-path with no per-request isolation
//at all, so their fragments could still interleave: A writes "AA", B starts and closes/replaces
//A's temp file, A's later "aa" fragment then writes through B's now-reassigned handle - repro
//confirmed: published file contains "BBaa". Claimed by whichever request's body arrives at
//index==0 first, checked by every later request's own index==0 (in addition to, not instead of,
//the existing password check), and released once its owning request's outer handler runs (body
//fully received) or immediately via onDisconnect if the connection vanishes before that.
//
//unitFirmwareUploadAuthFailed/unitFirmwareUploadBusyFailed (removed 2026-09-10,
//CODE_REVIEW_V4.md #2) used to be shared/global flags set by ANY request's body handler (including
//a losing/rejected one) and cleared by ANY request's outer handler - so a concurrent request B's
//bad password or busy-rejection could silently poison the flags a completely different, legitimate
//owner A's own outer handler then read, e.g. making A's real fragment look "already failed" and
//get dropped, or making A's outer handler clear B's flag on B's behalf, letting a later fragment of
//B's fall through and get appended onto A's file. Repro (see CODE_REVIEW_V4.md #2): file ends up
//"AAzz" (missing "aa"), A gets 200, B gets 401. Fixed by never touching shared state for a request
//that fails before claiming ownership, and by having the outer handler re-derive its response from
//data that's either independently stateless (the password, re-checked directly) or inherently
//request-scoped (wasOwner, a request-pointer comparison against unitFirmwareUploadOwner) instead of
//a mutable flag any other request's callback could also write to.
AsyncWebServerRequest* unitFirmwareUploadOwner = nullptr;
#endif
//Same reasoning/lifetime as unitFirmwareUploadWriteFailed above (CODE_REVIEW_V4.md #2) - only set
//within the "am I owner" gated section, only read/cleared once wasOwner is confirmed true, reset
//to false the moment a new request claims ownership.
bool webFileUploadWriteFailed = false;
//Same ownership pattern as unitFirmwareUploadOwner above (CODE_REVIEW_V3.md #2). The former
//webFileUploadAuthFailed/webFileUploadBusyFailed shared flags were removed for the identical
//cross-request-poisoning reason documented on unitFirmwareUploadOwner above (CODE_REVIEW_V4.md #2).
AsyncWebServerRequest* webFileUploadOwner = nullptr;

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

void setup() {
  //Pull SDA/SCL to idle-high before anything can glitch them - the Unit's TWI hardware doesn't
  //reset when the ESP does and stays armed/listening.
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);

  //Load the debug-mode flag as early as possible - gates the startup debug page/error panel/
  //page-load log further down, all of which can otherwise render before this is known.
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

  //Optional DS3231 RTC (see ServiceRtcFunctions.ino) - probed and, if present and trustworthy,
  //used to seed the clock RIGHT HERE, deliberately before initWiFi() below and unconditional on
  //its outcome (not just in the WiFi-succeeded branch further down) - the whole point is a
  //correct-ish clock independent of WiFi/NTP, so it must not itself depend on WiFi succeeding.
  //ntpSyncApplyResult() (ServiceAsyncTimeSyncFunctions.ino) is reused as-is rather than
  //duplicating its careful offset math - same reasoning there applies to a raw UTC value from
  //the RTC as it does to one from NTP. The timezone RULE (DST-aware offset) isn't resolved from
  //the network yet at this point - exactly the same brief "shows UTC until the async timezone
  //lookup catches up" window the existing NTP path already has and already treats as harmless
  //(see startTimezoneLookup()'s own call site comment further down), no new limitation here.
  probeRtc();
  if (rtcPresent) {
    time_t rtcUtc;
    if (readRtcTime(rtcUtc)) {
      ntpSyncApplyResult(rtcUtc);
      SerialPrintln(F("RTC: seeded clock from DS3231"));
    }
  }

  // Initialize LED for debugging
#if LED_ENABLE == true
  pinMode(LED_DEBUG_PIN, OUTPUT);
  ledOff();
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
  initWiFi();
  
  //Helpful if want to force reset WiFi settings for testing
  //wifiManager.resetSettings();

  if (isWifiConfigured && !isPendingReboot) {
    debugStatus = "WiFi Connected";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);

    //ezTime initialization - NON-BLOCKING with timeout
    debugStatus = "NTP Sync Starting";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);

    //ezTime's own auto-sync (events()-driven updateNTP()) is fully disabled - setInterval(0)
    //makes events() a permanent no-op (see ezTime.cpp: "if (_ntp_interval) updateNTP();"). Our own
    //ServiceAsyncTimeSyncFunctions.ino (checkNtpSync()) takes over that job instead, ticked from
    //loop() further down - a genuinely non-blocking replacement, see that file's header comment
    //for why (CODE_REVIEW.md #30 / BUGFIX_LOG #170 - the original blocking DNS+UDP calls could
    //crash the device under network stress).
    setInterval(0);

    //Not setDebug(INFO) - ezTime's own internal logging fires extremely often around WiFi
    //hiccups, wasting loop()/UART time. Our own SerialPrintln() logging is unaffected.
    setDebug(NONE);

    // Try to sync with timeout (non-blocking)
    unsigned long ntpStartTime = millis();
    unsigned long ntpTimeout = 30000; // 30 second timeout

    SerialPrintln(F("DEBUG: Starting NTP sync (non-blocking, 30s timeout)"));

    // Wait for sync with timeout and yield - startNtpSync()/checkNtpSync() (see
    // ServiceAsyncTimeSyncFunctions.ino) never block internally, so this loop's own delay(100)
    // is the only wait happening here, same as before.
    startNtpSync();
    while (timeStatus() == timeNotSet && (millis() - ntpStartTime) < ntpTimeout) {
      checkNtpSync();
      yield();  // Allow other tasks to run
      delay(100);
    }
    
    if (timeStatus() == timeSet) {
      debugStatus = "NTP Sync Success";
      SerialPrintln(F("DEBUG: Status = ") + debugStatus);
      SerialPrintln(F("DEBUG: NTP sync successful!"));
    } else {
      debugStatus = "NTP Sync Failed";
      SerialPrintln(F("DEBUG: Status = ") + debugStatus);
      SerialPrintln(F("DEBUG: WARNING - NTP sync failed or timed out, continuing anyway"));
    }

    //Load various variables
    debugStatus = "File System Init";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    initialiseFileSystem();
    loadValuesFromFileSystem();
    //See ServiceCrashLogFunctions.ino - must run after LittleFS mounts. Also called from the
    //"WiFi didn't come up" else-branch below (found 2026-09-10) - this call here only covers the
    //case where WiFi succeeded on this exact boot.
    checkAndPersistCrashLog();

    // Must come after loadValuesFromFileSystem() - needs timezoneSetting, populated from there.
    // Was a blocking timezone.setLocation() call - missed in the first pass of the CODE_REVIEW.md
    // #30 async rewrite (only the /api/timezone endpoint's own call was replaced at first; this
    // separate boot-time one, setting the timezone RULE the persisted setting refers to rather
    // than the current time-of-day, was still calling the blocking version and still showed up
    // live as "DEBUG: Timezone set to: ..." printing only after the old synchronous network wait -
    // found by checking the serial boot log for it, not by inspection alone). Fire-and-forget here
    // instead of waiting for it: checkTimezoneLookup() (ticked every loop() tick, same as the
    // endpoint's own lookups) picks it up and applies it (timezone.setPosix()) within the first
    // couple of loop() iterations once the network's up - nothing meaningful is shown on the
    // display before the 30s startup I2C scan delay anyway, so a brief window with the default
    // (UTC, no offset) rule at the very start of boot is harmless, unlike blocking setup() on a
    // network round-trip that can itself trip the software watchdog under network stress (see
    // ServiceAsyncTimeSyncFunctions.ino's header comment, BUGFIX_LOG #170).
    startTimezoneLookup(timezoneSetting);
    bootTimezoneLookupPending = true; //see its own comment - lets loop() retry this specific attempt on failure
    SerialPrintln(F("DEBUG: Timezone lookup started (non-blocking) for: ") + timezoneSetting);
    
    // Debug mode was already loaded at the very top of setup() - just log the already-known
    // value here.
#if DEBUG_ENABLE == true
    SerialPrintln(F("DEBUG: Debug mode: ") + String(showDebugPage ? F("enabled") : F("disabled")) + F(" (controls startup page, error status, page load log, and serial log)"));
#endif

    //Web Server Endpoint configuration
    debugStatus = "Web Server Setup";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);

    //Was `webServer.serveStatic("/", LittleFS, "/")` - a filesystem-wide static route. The only
    //real static asset is index.html(.gz), already served explicitly below (GET "/") - the
    //favicon is an embedded base64 data URI in that same HTML, no separate file. This catch-all
    //route served NO legitimate purpose beyond that: it exposed every runtime config file in
    //LittleFS root directly and unauthenticated, INCLUDING the OTA password itself
    //(GET /otapassword.txt returned it in plaintext, completely bypassing every password check
    //in this file - found 2026-09-09, CODE_REVIEW.md #29). Removed entirely rather than adding an
    //allowlist filter - there was nothing legitimate left to allow through it.

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
        html += "<button onclick='loadDiag()'>Refresh Diagnostics</button>";
        html += "<div class='status'>Current Status: " + debugStatus + "</div>";
        html += "<p>Debug mode is enabled. This page shows current crash log and heap diagnostics. When you continue to normal mode, you'll also see error status panel and page load debug log features.</p>";
        html += "<div class='log-container' id='logContainer'>";
        html += "<div style='color: #888;'>Loading diagnostics...</div>";
        html += "</div>";
        html += "<script>";
        //Was a 1s-interval poll of /log - that route (and its RAM ring buffer) was removed in an
        //earlier session (BUGFIX_LOG #41-43, freeing heap headroom), leaving this page polling a
        //dead endpoint (404 every second) whenever debug mode was on (found 2026-09-09,
        //CODE_REVIEW.md #27). Switched to the diagnostic sources that actually exist now
        //(/api/crash-log, /api/diag?type=heap) - a one-shot fetch on load plus a manual refresh
        //button, not a re-added interval poll, to avoid reintroducing the same kind of steady-state
        //cost that #41-43 removed.
        html += "function loadDiag() {";
        html += "  var container = document.getElementById('logContainer');";
        html += "  container.innerHTML = '<div style=\"color: #888;\">Loading diagnostics...</div>';";
        html += "  var xhrCrash = new XMLHttpRequest();";
        html += "  xhrCrash.onreadystatechange = function() {";
        html += "    if (this.readyState != 4) return;";
        html += "    var crashHtml;";
        html += "    if (this.status == 200) {";
        html += "      var lines = this.responseText.split('\\n').filter(function(l) { return l.trim() != ''; });";
        html += "      if (lines.length == 0) {";
        html += "        crashHtml = '<div class=\"debug\">Crash log: empty</div>';";
        html += "      } else {";
        html += "        crashHtml = '<div class=\"error\">Crash log (' + lines.length + ' entr' + (lines.length == 1 ? 'y' : 'ies') + '):</div>';";
        html += "        for (var i = 0; i < lines.length; i++) {";
        html += "          crashHtml += '<div class=\"log-entry\">' + escapeHtml(lines[i]) + '</div>';";
        html += "        }";
        html += "      }";
        html += "    } else {";
        html += "      crashHtml = '<div class=\"error\">Error loading crash log: Status ' + this.status + '</div>';";
        html += "    }";
        html += "    document.getElementById('crashLogSection').innerHTML = crashHtml;";
        html += "  };";
        html += "  xhrCrash.onerror = function() {";
        html += "    document.getElementById('crashLogSection').innerHTML = '<div class=\"error\">Network error loading crash log</div>';";
        html += "  };";
        html += "  xhrCrash.open('GET', '/api/crash-log', true);";
        html += "  var xhrHeap = new XMLHttpRequest();";
        html += "  xhrHeap.onreadystatechange = function() {";
        html += "    if (this.readyState != 4) return;";
        html += "    var heapHtml = this.status == 200";
        html += "      ? '<div class=\"debug\">Heap: ' + escapeHtml(this.responseText) + '</div>'";
        html += "      : '<div class=\"error\">Error loading heap diagnostics: Status ' + this.status + '</div>';";
        html += "    document.getElementById('heapSection').innerHTML = heapHtml;";
        html += "  };";
        html += "  xhrHeap.onerror = function() {";
        html += "    document.getElementById('heapSection').innerHTML = '<div class=\"error\">Network error loading heap diagnostics</div>';";
        html += "  };";
        html += "  xhrHeap.open('GET', '/api/diag?type=heap', true);";
        html += "  container.innerHTML = '<div id=\"heapSection\">Loading...</div><div id=\"crashLogSection\"></div>';";
        html += "  xhrCrash.send();";
        html += "  xhrHeap.send();";
        html += "}";
        html += "function escapeHtml(text) {";
        html += "  var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;', \"'\": '&#039;' };";
        html += "  return text.replace(/[&<>\"']/g, function(m) { return map[m]; });";
        html += "}";
        html += "function continueToNormal() {";
        //Same sessionStorage keys as index.html's getAdminPassword() (CODE_REVIEW.md #29) - this
        //page is served from the same origin, so a password already entered on the main page this
        //browser tab's session is reused here without asking again, and vice versa.
        html += "  var password = '';";
        html += "  if (sessionStorage.getItem('splitflapAdminPasswordKnown') === '1') {";
        html += "    password = sessionStorage.getItem('splitflapAdminPassword') || '';";
        html += "  } else {";
        html += "    var entered = prompt('Administrator-Passwort (leer lassen, falls keines gesetzt ist):', '');";
        html += "    if (entered !== null) {";
        html += "      password = entered;";
        html += "      sessionStorage.setItem('splitflapAdminPassword', entered);";
        html += "      sessionStorage.setItem('splitflapAdminPasswordKnown', '1');";
        html += "    }";
        html += "  }";
        html += "  var xhr = new XMLHttpRequest();";
        html += "  xhr.onreadystatechange = function() {";
        html += "    if (this.readyState == 4 && this.status == 200) {";
        html += "      window.location.href = '/';";
        html += "    } else if (this.readyState == 4 && this.status == 401) {";
        html += "      sessionStorage.removeItem('splitflapAdminPassword');";
        html += "      sessionStorage.removeItem('splitflapAdminPasswordKnown');";
        html += "      alert('Falsches Administrator-Passwort.');";
        html += "    }";
        html += "  };";
        html += "  xhr.open('POST', '/api/debug-mode', true);";
        html += "  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');";
        html += "  xhr.setRequestHeader('X-OTA-Password', password);";
        html += "  xhr.send('enabled=false');";
        html += "}";
        html += "loadDiag();";
        html += "</script>";
        html += "</body></html>";
        
        request->send(200, "text/html", html);
        return;
      }
#endif
      //Defensive heap guard (BUGFIX_LOG #51/#44 pattern, moved here from the now-removed
      //serveStatic() filter that used to cover this same LittleFSImpl::open() call - see its own
      //comment): a low-heap open() is one of two confirmed abort() sites on this device.
      if (ESP.getFreeHeap() < 6000) {
        request->send(503, "text/plain", "Low memory, try again shortly");
        return;
      }

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
      minimalDoc["animationMode"] = animationMode;
      minimalDoc["animationStaggerMs"] = animationStaggerMs;
      minimalDoc["deviceMode"] = deviceMode;
      //Found 2026-09-10 (live user report): a browser reload always left the main Text field
      //empty - this response never included inputText at all, so the client had nothing to
      //repopulate it with even though the server-side value was still fully intact. inputText is
      //a SHARED variable every mode's own per-second update (clock/date/countdown/countup/
      //rotation) also writes its own rendered content into - only meaningful as "what the user
      //actually typed for Text mode" while deviceMode is really "text" (see how the /api/settings
      //POST handler above only ever assigns to it under that same condition); the client gates its
      //use on deviceMode too, so sending it unconditionally here is harmless.
      minimalDoc["inputText"] = inputText;
      minimalDoc["version"] = espVersion;
      minimalDoc["lastTimeReceivedMessageDateTime"] = lastReceivedMessageDateTime;
      minimalDoc["lastWrittenText"] = lastWrittenText;
      minimalDoc["countdownToDateUnix"] = atol(countdownToDateUnix.c_str());
      minimalDoc["countupSinceDateUnix"] = atol(countupSinceDateUnix.c_str());
      minimalDoc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected");
      minimalDoc["wifiRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
      minimalDoc["wifiIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
      minimalDoc["wifiSettingsResettable"] = true;
      minimalDoc["mdnsHostname"] = mdnsHostname; //user-editable, see POST /api/wifi's "set-hostname" action
#if MQTT_ENABLE == true
      //Folded in here too, same "one fetch covers the whole System modal" reasoning as
      //mdnsHostname above - the password itself is never included, see POST /api/mqtt's own GET.
      minimalDoc["mqttEnabled"] = mqttEnabled;
      minimalDoc["mqttBrokerHost"] = mqttBrokerHost;
      minimalDoc["mqttBrokerPort"] = mqttBrokerPort;
      minimalDoc["mqttUsername"] = mqttUsername;
      minimalDoc["mqttPasswordSet"] = mqttPassword != "";
      minimalDoc["mqttConnected"] = mqttEnabled && mqttClient.connected();
#endif
      //timezone included so Administration can populate from this one fetch, given this device's
      //hard 5-connection TCP limit. The password itself is NEVER sent back (CODE_REVIEW.md #29) -
      //only whether one is currently set, plus the (non-secret) per-category toggle states, both
      //safe to expose unauthenticated same as everything else on this GET.
      minimalDoc["otaPasswordSet"] = otaPassword != "";
      minimalDoc["accessControlTextPublic"] = accessControlTextPublic;
      minimalDoc["accessControlSettingsPublic"] = accessControlSettingsPublic;
      minimalDoc["accessControlUnitsPublic"] = accessControlUnitsPublic;
      minimalDoc["accessControlWifiPublic"] = accessControlWifiPublic;
      minimalDoc["accessControlTimezonePublic"] = accessControlTimezonePublic;
      minimalDoc["accessControlSystemPublic"] = accessControlSystemPublic;
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

      //Bug #117: scheduled messages used to be serialized right here, and this handler would
      //hang/crash the whole device (reason 254/abort, stack trace through umm_free_core - heap
      //corruption) whenever a message actually sat in the queue. Two things were changed to fix
      //it, following the exact same pattern already proven safe below for rotationItemCount/
      //GET /api/rotation-items: (1) an isolated minimal repro (a standalone JsonDocument outside
      //this handler) showed assigning an unbound JsonArray() to a field and then writing into it
      //via nested [i][field] indexing corrupts the heap on this ArduinoJson version; switching to
      //the documented-safe .to<JsonArray>()+.add<JsonObject>() pattern alone did NOT fix the crash
      //here, though - (2) the yield() this loop used (for a >1s time budget across many messages)
      //turned out to be the same known trap noted below for rotation items: yield() from inside
      //this handler, combined with JSON-document operations, overflows the AsyncWebServer
      //callback's small stack. Moved the whole thing to its own handler (GET /api/scheduled-
      //messages) that mirrors rotation-items exactly: no yield() in the loop, just the item count
      //here so the client knows there's something to fetch.
      minimalDoc["scheduledMessageCount"] = scheduledMessages.size();

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
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      SerialPrintln(F("DEBUG: Request Post of Form Received"));

      bool submissionError = false;
      //Set if replaceRotationItemsFromJson() rejects invalid JSON - see CODE_REVIEW.md #24. Kept
      //separate from submissionError above: unlike that flag (which aborts the whole request
      //with 400 before touching anything), the rest of this request still gets processed
      //normally either way - only the final response wording changes.
      bool rotationItemsUpdateRejected = false;
      //Set if a submitted scheduled message needs more lines than fit and could therefore never
      //actually be shown - checked at acceptance time now, not just discovered (and then stuck
      //forever) once due (found 2026-09-09, CODE_REVIEW_V2.md #10). Same "reject, don't silently
      //drop/accept" pattern as rotationItemsUpdateRejected above.
      bool scheduledMessageRejected = false;
      //Distinct human-readable detail for scheduledMessageRejected, since it's now set for two
      //different reasons (too long to render, or a past-due timestamp - see
      //addAndPersistScheduledMessage()'s new return value below, CODE_REVIEW_V4.md #12) that
      //deserve different explanations in the final response.
      String scheduledMessageRejectionReason = "";

      //Separate declarations - `bool a, b = false;` only initializes b, not a.
      bool newMessageScheduleEnabledValue = false;
      bool newMessageScheduleShowIndefinitely = false;
      long newMessageScheduleDateTimeUnixValue = -1;
      String newAlignmentValue, newDeviceModeValue, newFlapSpeedValue, newInputTextValue, newCountdownToDateUnixValue = "";
      String newAnimationModeValue = "";
      String newAnimationStaggerMsValue = "";
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

          //HTTP POST Animation mode value ("Animationen", found 2026-09-10 on user request)
          if (p->name() == PARAM_ANIMATION_MODE) {
            String receivedValue = p->value();
            if (isValidAnimationMode(receivedValue)) {
              newAnimationModeValue = receivedValue;
            } else {
              SerialPrintln(F("animationMode provided was not valid. Value: ") + receivedValue);
              submissionError = true;
            }
          }

          //HTTP POST Animation stagger (ms between consecutive units' start) value
          if (p->name() == PARAM_ANIMATION_STAGGER_MS) {
            String rawAnimationStaggerMs = p->value();
            if (!isNumber(rawAnimationStaggerMs) || rawAnimationStaggerMs.toInt() < 0 || rawAnimationStaggerMs.toInt() > ANIMATION_STAGGER_MS_MAX) {
              SerialPrintln(F("animationStaggerMs provided was invalid (must be a number 0-") + String(ANIMATION_STAGGER_MS_MAX) + F("): ") + rawAnimationStaggerMs);
              submissionError = true;
            } else {
              newAnimationStaggerMsValue = rawAnimationStaggerMs;
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
          displayRenderGeneration++; //see the comment on this variable - CODE_REVIEW.md #14

          writeFile(LittleFS, alignmentPath, alignment.c_str());
          SerialPrintln(F("Alignment Updated: ") + alignment);
        }

        //Only if a new flap speed value
        if (newFlapSpeedValue != "" && flapSpeed != newFlapSpeedValue) {
          flapSpeed = newFlapSpeedValue;

          writeFile(LittleFS, flapSpeedPath, flapSpeed.c_str());
          SerialPrintln(F("Flap Speed Updated: ") + flapSpeed);
        }

        //Only if a new animation mode - doesn't bump displayRenderGeneration, same reasoning as
        //flapSpeed above: this only affects the TIMING of future writes, not what content counts
        //as already-shown/cached.
        if (newAnimationModeValue != "" && animationMode != newAnimationModeValue) {
          animationMode = newAnimationModeValue;

          writeFile(LittleFS, animationModePath, animationMode.c_str());
          SerialPrintln(F("Animation Mode Updated: ") + animationMode);
        }

        if (newAnimationStaggerMsValue != "" && String(animationStaggerMs) != newAnimationStaggerMsValue) {
          animationStaggerMs = (unsigned long)newAnimationStaggerMsValue.toInt();

          writeFile(LittleFS, animationStaggerMsPath, newAnimationStaggerMsValue.c_str());
          SerialPrintln(F("Animation Stagger Updated: ") + newAnimationStaggerMsValue);
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
        //replace, not an incremental add. Gated to the rotation tab's own submission, same
        //tab-gating reasoning as Countdown/Countup above (found 2026-09-09, CODE_REVIEW.md #6):
        //the hidden field is only ever recomputed by collectRotationItemsForSubmit(), called only
        //from the rotation tab's onsubmit case - submitting from any other tab resubmits its
        //stale/default "[]" and would otherwise silently wipe an existing, loaded rotation list.
        if (newDeviceModeValue == DEVICE_MODE_ROTATION && hasNewRotationItems) {
          //Return value checked now (found 2026-09-09, CODE_REVIEW.md #24) - invalid rotation
          //JSON no longer wipes the existing list, but the response below should say so too,
          //rather than unconditionally claiming success while quietly discarding the submission.
          //The rest of this request (scheduled messages, mode switch, etc.) still gets processed
          //normally - a broken rotation submission doesn't need to block unrelated fields in the
          //same request.
          if (replaceRotationItemsFromJson(newRotationItemsJson)) {
            SerialPrintln(F("Rotation Items Updated, count: ") + String(rotationItemCount));
          } else {
            rotationItemsUpdateRejected = true;
          }
        }

        //If its a new scheduled message, add it to the backlog and proceed, don't want to change device mode
        //Else, we do want to change the device mode and clear out the input text
        if (newMessageScheduleEnabledValue) {
          if (isMessageTooLongToRender(newInputTextValue)) {
            SerialPrintln(F("Scheduled Message rejected: needs more lines than fit, would never actually show"));
            scheduledMessageRejected = true;
            scheduledMessageRejectionReason = "the scheduled message needs more lines than fit and was NOT scheduled - it would never actually show.";
          } else if (!addAndPersistScheduledMessage(newInputTextValue, newMessageScheduleDateTimeUnixValue, newMessageScheduleShowIndefinitely)) {
            //Found 2026-09-10, CODE_REVIEW_V4.md #12: addAndPersistScheduledMessage()'s return
            //value used to be ignored entirely - a timestamp that's already due/in the past (the
            //request only checked it was numeric, never that it was actually in the future) was
            //silently dropped while this handler still answered 200 "OK" as if it had been
            //scheduled. Can happen with a maliciously/accidentally crafted request, or legitimately
            //if a very soon deadline passes in the time between the browser prefilling the field
            //and this request actually being processed (see CODE_REVIEW_V4.md #9's related fix).
            SerialPrintln(F("Scheduled Message rejected: date/time is not in the future"));
            scheduledMessageRejected = true;
            scheduledMessageRejectionReason = "the scheduled date/time has already passed and was NOT scheduled - pick a time in the future.";
          } else {
            SerialPrintln(F("New Scheduled Message added"));
          }
        }
        else {
          //Only if device mode has changed
          if (newDeviceModeValue != "" && deviceMode != newDeviceModeValue) {
            deviceMode = newDeviceModeValue;

            //Always (re-)start a sequence from the beginning when switching into Rotation mode.
            if (deviceMode == DEVICE_MODE_ROTATION) {
              rotationCurrentIndex = 0;
              rotationItemAcceptedSeconds = 0;
              rotationItemElapsedRealSeconds = 0;
            }

            writeFile(LittleFS, deviceModePath, deviceMode.c_str());
            SerialPrintln(F("Device Mode Set: ") + deviceMode);
          }

          //Only if we are showing text
          if (hasNewInputTextValue && deviceMode == DEVICE_MODE_TEXT) {
            inputText = newInputTextValue;
          }
        }

        if (rotationItemsUpdateRejected && scheduledMessageRejected) {
          request->send(400, "text/plain", "OK, but the submitted rotation items were rejected (invalid JSON, or a text item needs more lines than fit) AND " + scheduledMessageRejectionReason);
        } else if (rotationItemsUpdateRejected) {
          request->send(400, "text/plain", "OK, but the submitted rotation items were rejected (invalid JSON, or a text item needs more lines than fit to ever display) and were NOT applied - the existing rotation list is unchanged.");
        } else if (scheduledMessageRejected) {
          //scheduledMessageRejectionReason (found 2026-09-10, CODE_REVIEW_V4.md #12) now carries
          //one of two distinct explanations - see where it's set above.
          request->send(400, "text/plain", "OK, but " + scheduledMessageRejectionReason);
        } else {
          request->send(200, "text/plain", "OK");
        }
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
        itemObject["animationMode"] = item.AnimationMode;
        itemObject["animationStaggerMs"] = item.AnimationStaggerMs;
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
        char buf[250];
        //getFlashChipRealSize() = physical chip's own SPI ID (ground truth); getFlashChipSize() =
        //what the eesz board-menu choice assumes - a mismatch means the compiled layout is wrong.
        //rtc/rtcOsf (added 2026-09-11, see ServiceRtcFunctions.ino) - the only way to verify the
        //optional DS3231's detection/trustworthiness without physically observing the display.
        snprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(F("free=%u frag=%u%% maxBlock=%u flashReal=%u flashConfigured=%u flashChipId=0x%08X espChipId=0x%08X sdk=%s core=%s rtc=%s rtcOsf=%u")),
          ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize(),
          ESP.getFlashChipRealSize(), ESP.getFlashChipSize(), ESP.getFlashChipId(), ESP.getChipId(),
          ESP.getSdkVersion(), ESP.getCoreVersion().c_str(),
          rtcPresent ? "present" : "absent", rtcOscillatorStopFlag ? 1 : 0);
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
        snprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(F("active=%d/5 timeWait=%d/5 bound=%d listen=%d")), activeCount, timeWaitCount, boundCount, listenCount);
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
      if (pendingLegacyI2CScanSlot.request != nullptr) {
        request->send(503, "text/plain", "Another I2C scan is already in progress, try again shortly");
        return;
      }
      pendingLegacyI2CScanSlot.accept(request);
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
        //Capture ownership BEFORE releasing it - the one reliable, request-scoped (pointer
        //comparison) way to know whether THIS request's body ever won the shared static file
        //state, independent of whatever any other concurrent request's body did in the meantime
        //(CODE_REVIEW_V4.md #2 - replaces the old shared unitFirmwareUploadAuthFailed/
        //unitFirmwareUploadBusyFailed flags, see unitFirmwareUploadOwner's declaration comment).
        bool wasOwner = (unitFirmwareUploadOwner == request);
        if (wasOwner) {
          unitFirmwareUploadOwner = nullptr;
        }
        //Password is stateless and independently re-checked here, same as checkOtaPassword() for
        //the ESPMaster OTA endpoints above - never gated behind a shared flag another request's
        //callback could set or clear.
        if (String(otaPassword) != "") {
          String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
          if (!constantTimeEquals(providedPassword, String(otaPassword))) {
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }
        if (!wasOwner) {
          request->send(503, "text/plain", "Another firmware upload is already in progress, try again shortly");
          return;
        }
        if (unitFirmwareUploadWriteFailed) {
          unitFirmwareUploadWriteFailed = false;
          request->send(500, "text/plain", "Upload failed while writing the file. The previously stored firmware (if any) is unchanged.");
          return;
        }
        request->send(200, "text/plain", "OK - firmware stored, ready to push to a unit");
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File unitFirmwareFile;

        if (index == 0) {
          //Check the password FIRST, before touching any previous-upload state at all. Found
          //2026-09-09, CODE_REVIEW_V2.md #3: this used to close the previous upload's (possibly
          //still-legitimately-in-progress) file handle and delete its temp file BEFORE checking
          //this new request's own password - an unauthenticated POST here could already interrupt
          //and corrupt a different, correctly-authenticated upload that was still streaming in,
          //purely by virtue of also hitting index==0 on the shared static state. On failure, just
          //return WITHOUT touching any shared state (found 2026-09-10, CODE_REVIEW_V4.md #2) - the
          //outer handler above re-checks the password itself and never trusts a shared flag for it.
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (!constantTimeEquals(providedPassword, String(otaPassword))) {
              SerialPrintln(F("Unit firmware upload rejected: invalid password"));
              return;
            }
          }

          //Reject a second, DIFFERENT authenticated request trying to start while this channel's
          //shared static state is already claimed by another still-in-flight upload - found
          //2026-09-10, CODE_REVIEW_V3.md #2, see unitFirmwareUploadOwner's own declaration comment
          //for the full mechanism/repro. Must come before any of the cleanup/touching of
          //unitFirmwareFile below - a losing request must never get anywhere near it. Again, just
          //return - no shared flag to set (CODE_REVIEW_V4.md #2).
          if (unitFirmwareUploadOwner != nullptr && unitFirmwareUploadOwner != request) {
            SerialPrintln(F("Unit firmware upload rejected: another upload is already in progress"));
            return;
          }
          unitFirmwareUploadOwner = request;
          //Fresh ownership period starts here - discard any leftover true from a PREVIOUS owner
          //whose connection vanished before its own outer handler ran to consume/clear it (that
          //handler is the only other place this gets cleared). Without this reset, a vanished
          //owner's failure could otherwise get blamed on this brand new, unrelated request.
          unitFirmwareUploadWriteFailed = false;
          //Releases this claim immediately if the connection vanishes before the outer handler
          //ever runs to release it normally (e.g. WiFi drops mid-transfer) - otherwise every
          //future upload would be permanently rejected as "busy" by a request that never finishes.
          request->onDisconnect([request]() {
            if (unitFirmwareUploadOwner == request) {
              unitFirmwareUploadOwner = nullptr;
            }
          });

          //Only now, with this request's own credentials confirmed AND exclusive ownership
          //claimed, touch the shared static upload state. If a previous upload's connection just
          //vanished mid-transfer, "final" never fired for it and this handle was never closed -
          //found live 2026-09-09 while testing the fix below (a dangling handle measurably cost
          //heap/fragmentation across the next upload). Close it defensively before reusing the
          //static variable for this attempt.
          if (unitFirmwareFile) {
            unitFirmwareFile.close();
          }
          //Same reasoning for a leftover temp file from that vanished upload - LittleFS.open(...,
          //"w") below truncates it anyway, but remove it explicitly so a request that fails later
          //(e.g. a write error) doesn't leave stale bytes behind from an even earlier attempt.
          if (LittleFS.exists(unitFirmwareTmpPath)) {
            LittleFS.remove(unitFirmwareTmpPath);
          }

          SerialPrintln(F("DEBUG: Unit firmware upload starting: ") + filename);
          //Write to a temp path, atomically rename() onto the real path only once the whole
          //upload is confirmed complete and error-free - an aborted/failed upload must never
          //leave the previously-stored (working) firmware truncated or corrupted (found
          //2026-09-09, CODE_REVIEW.md #5).
          unitFirmwareFile = LittleFS.open(unitFirmwareTmpPath, "w");
          if (!unitFirmwareFile) {
            SerialPrintln(F("Unit firmware upload: failed to open ") + String(unitFirmwareTmpPath) + F(" for writing"));
            unitFirmwareUploadWriteFailed = true;
          }
        }

        //A per-fragment ownership check, not a shared "was rejected" flag (CODE_REVIEW_V4.md #2) -
        //without this, a LATER fragment (index>0) of a rejected request would fall straight
        //through to unitFirmwareFile.write() below, writing its bytes into the WINNING request's
        //file. Comparing live ownership on every fragment, instead of any shared boolean, means a
        //rejected request's fragments are dropped purely because they're not the current owner -
        //nothing it does can ever affect a different, legitimate owner's own fragments.
        if (unitFirmwareUploadOwner != request) {
          return;
        }

        if (unitFirmwareFile) {
          size_t written = unitFirmwareFile.write(data, len);
          if (written != len) {
            SerialPrintln(F("Unit firmware upload: short write (") + String(written) + F(" of ") + String(len) + F(" bytes) - aborting"));
            unitFirmwareUploadWriteFailed = true;
          }
        }

        if (final) {
          if (unitFirmwareFile) {
            unitFirmwareFile.close();
          }
          if (unitFirmwareUploadWriteFailed) {
            LittleFS.remove(unitFirmwareTmpPath); //don't leave a corrupt temp file lying around
            SerialPrintln(F("DEBUG: Unit firmware upload failed, previously stored firmware (if any) left untouched"));
          } else if (LittleFS.rename(unitFirmwareTmpPath, unitFirmwarePath)) {
            SerialPrintln(F("DEBUG: Unit firmware upload finished. Total bytes: ") + String(index + len));
          } else {
            SerialPrintln(F("Unit firmware upload: failed to move temp file into place"));
            unitFirmwareUploadWriteFailed = true;
          }
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

      //Bounds against UNITS_AMOUNT_MAX (the structural 6-bit-DIP address ceiling), not the
      //cached unitsAmount - unlike every other unit-id check in this file, this endpoint's whole
      //point is to reach a unit that's already stuck in the bootloader (BUGFIX_LOG #139, local),
      //which by definition doesn't answer at its app address and therefore drops out of
      //unitsAmount on the next scan. Checking against unitsAmount here would block exactly the
      //recovery this endpoint exists for. scanForBootloaderAddress() below is the real safety
      //net regardless (refuses to proceed unless exactly one unit answers as a bootloader).
      int unitId = idValue.toInt();
      if (unitId < 0 || unitId >= UNITS_AMOUNT_MAX) {
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(UNITS_AMOUNT_MAX - 1));
        return;
      }

      if (action == "push") {
        if (String(otaPassword) != "") {
          String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
          if (!constantTimeEquals(providedPassword, String(otaPassword))) {
            SerialPrintln(F("Unit firmware push rejected: invalid password"));
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }

        if (!LittleFS.exists(unitFirmwarePath)) {
          request->send(400, "text/plain", "No firmware uploaded yet. Upload a .bin first.");
          return;
        }

        if (pendingFirmwarePushSlot.request != nullptr) {
          request->send(503, "text/plain", "Another firmware push is already in progress, try again shortly");
          return;
        }

        pendingFirmwarePushUnitIndex = unitId;
        //Note: an in-flight pushFirmwareToUnit() always runs to completion regardless of a
        //disconnect (by design) - it just has nowhere to report the result if the client already left.
        pendingFirmwarePushSlot.accept(request);
        return;
      }

      if (action == "probe") {
        if (pendingBootloaderProbeSlot.request != nullptr) {
          request->send(503, "text/plain", "Another bootloader probe is already in progress, try again shortly");
          return;
        }

        pendingBootloaderProbeUnitIndex = unitId;
        pendingBootloaderProbeSlot.accept(request);
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
        if (pendingUnitsInfoSlot.request != nullptr) {
          request->send(503, "text/plain", "Another units request is already in progress, try again shortly");
          return;
        }
        //Opt-in heavy 6-byte read (adds phase/progress) only via ?detail=1 - routine polling stays cheap.
        pendingUnitsInfoRequestDetail = request->hasParam("detail");
        pendingUnitsInfoSlot.accept(request);
        return;
      }

      // request->method() == HTTP_POST
      String action = request->hasParam("action") ? request->getParam("action")->value() : "";

      if (action == "rescan") {
        SerialPrintln(F("DEBUG: Admin rescan-units request received"));
        if (pendingRescanSlot.request != nullptr) {
          request->send(503, "text/plain", "Another rescan is already in progress, try again shortly");
          return;
        }
        pendingRescanSlot.accept(request);
        return;
      }

      if (action == "offset") {
        if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return;

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

        if (pendingUnitOffsetSlot.request != nullptr) {
          request->send(503, "text/plain", "Another offset change is already in progress, try again shortly");
          return;
        }

        SerialPrintln(F("Setting Unit ") + String(unitId) + F(" offset to ") + String(offset));
        //Deferred to loop() (see pendingUnitOffsetSlot's comment) - not written here directly.
        pendingUnitOffsetUnitId = unitId;
        pendingUnitOffsetValue = (uint16_t)offset;
        pendingUnitOffsetSlot.accept(request);
        return;
      }

      if (action == "reset") {
        if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return;

        SerialPrintln(F("Request to Reset Units Received"));
        isPendingUnitsReset = true; //picked up in loop()
        //updateUnitsResetSequence() writes its own blank1/pause/blank2 sequence directly, bypassing
        //showText()/lastWrittenText entirely - without this, the cache would still believe
        //whatever text was shown before the reset is still correctly displayed. See
        //displayRenderGeneration's comment - CODE_REVIEW.md #14.
        displayRenderGeneration++;
        request->send(200, "text/plain", "Units are resetting/recalibrating now.");
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use rescan, offset, or reset");
    });

    //Replaces a single static web UI file in place, without a full littlefs.bin reflash (which
    //would wipe runtime settings files). Filename allowlisted, not an arbitrary path.
    webServer.on("/api/web-file", HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Capture ownership BEFORE releasing it - same request-scoped derivation as
        ///api/units/firmware above (CODE_REVIEW_V4.md #2).
        bool wasOwner = (webFileUploadOwner == request);
        if (wasOwner) {
          webFileUploadOwner = nullptr;
        }
        //Password is stateless and independently re-checked here - never gated behind a shared
        //flag another request's callback could set or clear.
        if (String(otaPassword) != "") {
          String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
          if (!constantTimeEquals(providedPassword, String(otaPassword))) {
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }
        if (!wasOwner) {
          //Distinguish "this request's OWN filename was invalid" from "genuinely lost the
          //ownership race to a concurrent upload" - found while implementing CODE_REVIEW_V4.md
          //#2 (not itself part of that review): the body handler used to send its own 400
          //directly for an invalid filename AND give up ownership, so the outer handler here
          //would then ALSO try to send a (wrong, generic) 503 for the same request - a double
          //response. Re-derive the uploaded filename statelessly from this request's own file
          //param instead (ESPAsyncWebServer adds one with isFile()==true during multipart
          //parsing, regardless of the field name used) rather than relying on any shared flag.
          String uploadedFilename = "";
          for (size_t i = 0; i < request->params(); i++) {
            const AsyncWebParameter* p = request->getParam(i);
            if (p->isFile()) {
              uploadedFilename = p->value();
              break;
            }
          }
          if (uploadedFilename != "" && uploadedFilename != "index.html.gz") {
            request->send(400, "text/plain", "Filename must be one of the known static assets");
          } else {
            request->send(503, "text/plain", "Another web file upload is already in progress, try again shortly");
          }
          return;
        }
        if (webFileUploadWriteFailed) {
          webFileUploadWriteFailed = false;
          request->send(500, "text/plain", "Upload failed while writing the file. The previous file (if any) is unchanged.");
          return;
        }
        request->send(200, "text/plain", "OK - file updated");
      },
      [](AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File webUploadFile;
        static String webUploadTmpPath;

        if (index == 0) {
          //Check the password FIRST, before touching any previous-upload state at all - same
          //reasoning as /api/units/firmware above (found 2026-09-09, CODE_REVIEW_V2.md #3): an
          //unauthenticated POST hitting index==0 must not be able to close/delete a different,
          //still-legitimately-in-progress upload's file just by arriving first. On failure, just
          //return - no shared flag to set (CODE_REVIEW_V4.md #2), the outer handler re-checks the
          //password itself.
          if (String(otaPassword) != "") {
            String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
            if (!constantTimeEquals(providedPassword, String(otaPassword))) {
              SerialPrintln(F("Web file upload rejected: invalid password"));
              return;
            }
          }

          //Reject a second, DIFFERENT authenticated request trying to start while this channel's
          //shared static state is already claimed by another still-in-flight upload - found
          //2026-09-10, CODE_REVIEW_V3.md #2 (same underlying issue as /api/units/firmware above,
          //confirmed here by static analysis of the identical shared-static-state pattern). Must
          //come before any cleanup/touching of webUploadFile below. Again, just return
          //(CODE_REVIEW_V4.md #2) - no shared flag.
          if (webFileUploadOwner != nullptr && webFileUploadOwner != request) {
            SerialPrintln(F("Web file upload rejected: another upload is already in progress"));
            return;
          }
          webFileUploadOwner = request;
          //Fresh ownership period - discard any leftover true from a previous owner whose
          //connection vanished before its own outer handler ran to consume/clear it (same
          //reasoning as unitFirmwareUploadWriteFailed's reset above).
          webFileUploadWriteFailed = false;
          //Releases this claim immediately if the connection vanishes before the outer handler
          //ever runs to release it normally - see the identical /api/units/firmware comment above.
          request->onDisconnect([request]() {
            if (webFileUploadOwner == request) {
              webFileUploadOwner = nullptr;
            }
          });

          //Only now, with this request's own credentials confirmed AND exclusive ownership
          //claimed, touch the shared static upload state. Same defensive cleanup as
          ///api/units/firmware above, for a previous upload whose connection vanished mid-transfer
          //("final" never fired, so this never ran) - see that handler's comment.
          if (webUploadFile) {
            webUploadFile.close();
          }
          if (webUploadTmpPath != "" && LittleFS.exists(webUploadTmpPath)) {
            LittleFS.remove(webUploadTmpPath);
          }

          //"index.html.gz" is the only valid target - the whole UI is one gzip-compressed file,
          //served automatically with Content-Encoding: gzip. An invalid filename gives up
          //ownership again immediately (found 2026-09-10, CODE_REVIEW_V4.md #2 - this used to be a
          //separate shared static `webUploadFilenameValid` bool with the identical
          //any-request-can-clobber-it problem as the auth/busy flags above; ownership alone
          //already fully covers it, since a request that never keeps ownership automatically
          //fails every later fragment's "webFileUploadOwner != request" check below too). Does
          //NOT send a response itself (found while implementing #2, not itself part of that
          //review): this body handler running to completion doesn't stop the separate outer
          //(onRequest) handler above from ALSO running once the body finishes arriving over the
          //wire - sending a response from both would be a double response for one request. The
          //outer handler re-derives "was my own filename invalid" itself and sends the one actual
          //reply.
          if (filename != "index.html.gz") {
            SerialPrintln(F("Web file upload rejected: filename not in allowlist: ") + filename);
            webFileUploadOwner = nullptr;
            return;
          }

          SerialPrintln(F("DEBUG: Web file upload starting: ") + filename);
          //Same temp-file + atomic rename() pattern as /api/units/firmware above - an
          //aborted/failed upload must never leave the live web UI file truncated (found
          //2026-09-09, CODE_REVIEW.md #5). The stale-"/index.html"-removal below moved to right
          //before the swap too, so a failed upload can't leave neither file in place.
          webUploadTmpPath = "/" + filename + ".tmp";
          webUploadFile = LittleFS.open(webUploadTmpPath, "w");
          if (!webUploadFile) {
            SerialPrintln(F("Web file upload: failed to open ") + webUploadTmpPath + F(" for writing"));
            webFileUploadWriteFailed = true;
          }
        }

        //Per-fragment ownership check, not a sticky flag - see the identical /api/units/firmware
        //comment above for why this must compare live ownership on every fragment. Also the only
        //thing needed to drop a rejected (bad password/busy/bad filename) request's later
        //fragments, now that none of those paths touch any shared flag any more.
        if (webFileUploadOwner != request) {
          return;
        }

        if (webUploadFile) {
          size_t written = webUploadFile.write(data, len);
          if (written != len) {
            SerialPrintln(F("Web file upload: short write (") + String(written) + F(" of ") + String(len) + F(" bytes) - aborting"));
            webFileUploadWriteFailed = true;
          }
        }

        if (final) {
          if (webUploadFile) {
            webUploadFile.close();
          }
          if (webFileUploadWriteFailed) {
            LittleFS.remove(webUploadTmpPath); //don't leave a corrupt temp file lying around
            SerialPrintln(F("DEBUG: Web file upload failed, previous file (if any) left untouched"));
          } else {
            //A leftover uncompressed "/index.html" would take precedence over "/index.html.gz" -
            //only now, right before the atomic swap, not eagerly at the start.
            if (LittleFS.exists("/index.html")) {
              LittleFS.remove("/index.html");
            }
            if (LittleFS.rename(webUploadTmpPath, "/" + filename)) {
              SerialPrintln(F("DEBUG: Web file upload finished: ") + filename + F(", total bytes: ") + String(index + len));
            } else {
              SerialPrintln(F("Web file upload: failed to move temp file into place"));
              webFileUploadWriteFailed = true;
            }
          }
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
      return constantTimeEquals(providedPassword, String(otaPassword));
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
      pendingFirmwareUploadSlot.accept(request);
      otaPendingPhase = OTA_PHASE_BEGIN; //loop() calls Update.begin()
    });

    //Step 2: one chunk of firmware data - the browser waits for this to return before sending
    //the next one, giving natural backpressure.
    webServer.on("/api/firmware-chunk", HTTP_POST,
      [checkOtaPassword](AsyncWebServerRequest * request) {
        //Capture ownership BEFORE releasing it - this is the one and only thing that reliably
        //tells THIS specific outer-handler invocation whether its own body ever got to claim the
        //shared buffer, independent of anything any other concurrent request's body did (see
        //otaChunkOwner's declaration comment and CODE_REVIEW_V4.md #1 for the bug this replaces).
        bool wasOwner = (otaChunkOwner == request);
        if (wasOwner) {
          otaChunkOwner = nullptr;
        }
        if (!checkOtaPassword(request)) {
          request->send(401, "text/plain", "Invalid password");
          return;
        }
        if (otaChunkBuffer == nullptr) {
          request->send(400, "text/plain", "No firmware upload in progress - call /api/firmware-start first");
          return;
        }
        if (!wasOwner) {
          //This request's body never claimed the shared buffer - either its declared size (Content-
          //Length) exceeded OTA_CHUNK_SIZE, or another chunk was already mid-transfer/still pending
          //when it arrived. Distinguish the two from data this request owns outright
          //(contentLength()), not from any shared mutable flag another request could have touched.
          if (request->contentLength() > OTA_CHUNK_SIZE) {
            request->send(400, "text/plain", "Chunk too large");
          } else {
            request->send(503, "text/plain", "Another chunk upload is already in progress, try again shortly");
          }
          return;
        }
        //Reject an overlapping request outright rather than silently overwriting the still-pending
        //one's state - same "already busy" pattern used for every other pending-request slot in
        //this file (found 2026-09-09, CODE_REVIEW.md #3: nothing previously stopped a second chunk
        //or a finish from clobbering pendingFirmwareUploadRequest/otaPendingPhase while loop()
        //hadn't finished processing an earlier one yet).
        if (otaPendingPhase != OTA_PHASE_NONE) {
          request->send(503, "text/plain", "Still processing the previous chunk, try again shortly");
          return;
        }
        otaLastActivityAt = millis();
        pendingFirmwareUploadSlot.accept(request);
        otaPendingPhase = OTA_PHASE_WRITE; //loop() calls Update.write()
      },
      nullptr, //no multipart upload needed - this is a plain raw-body request
      [checkOtaPassword](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0) {
          //Check auth HERE too, not just in the request handler above - that one only runs once
          //the ENTIRE body has already streamed in, but this body handler runs incrementally AS
          //each piece of the raw POST body arrives, writing straight into the shared, global
          //otaChunkBuffer. Found 2026-09-09, CODE_REVIEW.md #3: without this, an unauthenticated
          //POST here could already corrupt an in-progress, correctly-authenticated upload's
          //buffered data before its own password is ever checked (which only happens afterward,
          //in the request handler above, by which point the damage is already done). On failure,
          //just return WITHOUT touching any shared state at all (found 2026-09-10,
          //CODE_REVIEW_V4.md #1) - this request simply never becomes otaChunkOwner, which the
          //per-fragment check below and the outer handler's wasOwner re-derivation are both built
          //to handle on their own, with nothing to leak into a different, legitimate request.
          if (!checkOtaPassword(request)) {
            return;
          }
          //Reject an overlapping chunk's body HERE too, before touching otaChunkLen/otaChunkBuffer
          //at all - not just in the outer request handler above, which only runs once this ENTIRE
          //body has already streamed in and overwritten them. Found 2026-09-09,
          //CODE_REVIEW_V2.md #2, live-reproduced with the extracted body handler: a buffer holding
          //"AAAA" contained "BBBB" after a second body ran while the first chunk was still
          //pending (loop() hadn't called Update.write() for it yet).
          if (otaPendingPhase != OTA_PHASE_NONE) {
            return;
          }
          //Reject a SECOND body that's ALSO still mid-transfer, not just one whose outer handler
          //has already run - found 2026-09-10, CODE_REVIEW_V3.md #1: the otaPendingPhase check
          //above only ever changes once a body is FULLY received, so two bodies that are BOTH
          //still streaming in concurrently (their fragments interleaved by AsyncTCP servicing
          //multiple open connections) previously passed both checks and corrupted each other -
          //see otaChunkOwner's own declaration comment for the full mechanism and repro.
          if (otaChunkOwner != nullptr && otaChunkOwner != request) {
            return;
          }
          if (otaChunkBuffer == nullptr || total > OTA_CHUNK_SIZE) {
            //This request's own chunk is invalid (no upload started, or too large) - never claim
            //ownership for it; the outer handler re-derives the precise reason from
            //request->contentLength() once this body finishes arriving.
            return;
          }
          otaChunkOwner = request;
          otaChunkLen = 0;
        }
        //Per-fragment ownership check (not a shared "rejected" flag, see CODE_REVIEW_V4.md #1) -
        //a request that never claimed ownership above, or that has since lost it (watchdog
        //timeout, or its own overflow guard below), always fails this and is silently dropped -
        //by construction this can never affect a DIFFERENT request's legitimate buffer, since the
        //comparison is against this specific fragment's own `request` pointer.
        if (otaChunkOwner != request) {
          return;
        }
        //Re-check on EVERY fragment, not just index==0 (found 2026-09-09, CODE_REVIEW_V2.md #4,
        //statically traced): the abandoned-upload watchdog (loop(), see otaLastActivityAt below)
        //could previously free/null otaChunkBuffer mid-transfer, between two fragments of one
        //slow-arriving body, since otaLastActivityAt was only updated once the FULL chunk (and
        //its outer handler) had completed - a later fragment's memcpy would then write through a
        //dangling/null pointer. Updating otaLastActivityAt on every fragment (not just at the
        //end) additionally means a genuinely still-arriving, slow-but-active body no longer looks
        //"abandoned" to that same watchdog in the first place.
        otaLastActivityAt = millis();
        if (otaChunkBuffer == nullptr || otaChunkLen + len > OTA_CHUNK_SIZE) {
          //Shouldn't happen given the total check at index==0 above, but never overflow the buffer.
          //Give up ownership outright rather than setting a shared flag - the outer handler's
          //wasOwner check then correctly reports this request as rejected (503) without needing
          //any extra state, and a fresh request is free to claim the slot immediately after.
          otaChunkOwner = nullptr;
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
      //Same "already busy" guard as /api/firmware-chunk above (CODE_REVIEW.md #3).
      if (otaPendingPhase != OTA_PHASE_NONE) {
        request->send(503, "text/plain", "Still processing a previous chunk, try again shortly");
        return;
      }
      otaLastActivityAt = millis();
      pendingFirmwareUploadSlot.accept(request);
      otaPendingPhase = OTA_PHASE_FINISH; //loop() calls Update.end()
    });
#endif

#if DEBUG_ENABLE == true
    webServer.on("/api/debug-mode", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;

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
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;

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

    //See BUGFIX_LOG #117 - moved out of GET /api/settings, mirrors GET /api/rotation-items exactly
    //(own small JsonDocument, .to<JsonArray>()+.add<JsonObject>(), deliberately no yield() in the
    //loop - a yield() here previously overflowed the AsyncWebServer callback's small stack).
    //Deliberately a separate registration, not merged with DELETE below - not worth stacking any
    //more risk onto this fix given the other GET/POST routing quirk found this session.
    webServer.on("/api/scheduled-messages", HTTP_GET, [](AsyncWebServerRequest * request) {
      JsonDocument scheduledDoc;
      JsonArray scheduledArray = scheduledDoc.to<JsonArray>();
      unsigned long scheduledRequestStart = millis();

      for (int i = 0; i < scheduledMessages.size() && (millis() - scheduledRequestStart) < 1000; i++) {
        ScheduledMessage msg = scheduledMessages.get(i);
        JsonObject entry = scheduledArray.add<JsonObject>();
        entry["scheduledDateTimeUnix"] = msg.ScheduledDateTimeUnix;
        entry["message"] = msg.Message;
        entry["showIndefinitely"] = msg.ShowIndefinitely;
      }

      AsyncResponseStream *response = request->beginResponseStream("application/json");
      serializeJson(scheduledDoc, *response);
      request->send(response);
    });

    webServer.on("/api/scheduled-messages", HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

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
      if (rejectIfPasswordRequired(request, accessControlTextPublic)) return;

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

      //Checked up front, before either branch below, not just discovered via showText()'s bool
      //return value (found 2026-09-09, CODE_REVIEW_V2.md #10): the permanent branch below never
      //even looked at showText()'s result at all, so a too-long text silently got "OK" back and
      //was then retried forever, uselessly, by the normal 1s mode-dispatch loop. A clear, honest
      //400 here is strictly better than that for both branches.
      if (isMessageTooLongToRender(newText)) {
        request->send(400, "text/plain", "text needs more lines than fit to display - shorten it or add fewer line breaks");
        return;
      }

      if (durationSeconds > 0) {
        //Temporary: deviceMode/inputText are deliberately left untouched, same pattern
        //checkScheduledMessages() already uses for a non-indefinite scheduled message - once the
        //hold time elapses, the next 1s tick's normal mode loop redisplays whatever deviceMode
        //currently is on its own, no extra "revert" logic needed here. Because of that, there's no
        //other code path that will ever retry this specific call - unlike the permanent branch
        //below, a busy display here means the message is genuinely lost, not just delayed. Report
        //that honestly instead of claiming success (found 2026-09-09, CODE_REVIEW.md #10).
        //The "too many lines" reason is already ruled out above (CODE_REVIEW_V2.md #10) - a false
        //here can now only mean a busy display.
        if (!showText(newText, (int)(durationSeconds * 1000))) {
          request->send(503, "text/plain", "Nothing was displayed - the display is busy right now. Try again shortly.");
          return;
        }
      } else {
        //Permanent: switches the display into Text mode, same net effect as submitting the main
        //form with deviceMode=text&inputText=<text>. Unlike the temporary branch above, a busy
        //display here isn't a loss - deviceMode/inputText are set regardless, so the normal 1s
        //mode-dispatch loop keeps retrying this exact message on its own until it succeeds.
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
        //Found 2026-09-10: unlike every POST action on this endpoint (all gated on
        //accessControlWifiPublic), these two GET polling reads were never checked at all - leaked
        //nearby-network scan results (SSID/RSSI/security) and the device's own currently-connected
        //SSID/IP to anyone unauthenticated on the LAN, regardless of the Wifi category toggle.
        //Consistent to gate here too: the same browser/session that started a scan or connect via
        //the (already-gated) POST action already has the password, so this doesn't block normal
        //polling of an attempt that was actually allowed to start.
        if (rejectIfPasswordRequired(request, accessControlWifiPublic)) return;

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
            doc["error"] = F("Scan failed");
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
            case WIFI_CONNECT_NONE: doc["phase"] = F("none"); break;
            case WIFI_CONNECT_PENDING: doc["phase"] = F("pending"); break;
            case WIFI_CONNECT_TESTING: doc["phase"] = F("testing"); break;
            case WIFI_CONNECT_SUCCESS: doc["phase"] = F("success"); break;
            case WIFI_CONNECT_FAILED_REVERTED: doc["phase"] = F("failed_reverted"); break;
          }
          doc["wifiStatus"] = WiFi.status() == WL_CONNECTED ? F("Connected") : F("Disconnected");
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
      if (rejectIfPasswordRequired(request, accessControlWifiPublic)) return;

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

      if (action == "set-hostname") {
        //Shares this endpoint's registration rather than getting a dedicated route - see
        //POST /api/quiet-hours' comment above ("every registered handler costs a small permanent
        //heap allocation that's never freed") for why that's a deliberate choice here, not an
        //oversight. Gated by the same accessControlWifiPublic as every other action on this route
        //(the "Netzwerk" tab in the UI) - the hostname isn't a secret, but changing it is a
        //device-state change like the others here.
        if (!request->hasParam("hostname", true)) {
          request->send(400, "text/plain", "'hostname' is required");
          return;
        }

        String newHostname = request->getParam("hostname", true)->value();
        if (!isValidMdnsHostname(newHostname)) {
          request->send(400, "text/plain", "Invalid hostname - only letters, digits, and hyphens (not leading/trailing), 1-63 characters");
          return;
        }

        mdnsHostname = newHostname;
        writeFile(LittleFS, mdnsHostnamePath, mdnsHostname.c_str());

#if USE_MULTICAST == true
        //MDNS.begin() only sets up the responder once, in setup() - calling it again without
        //end()ing the previous one first would just silently keep answering under the old name
        //alongside (or instead of) the new one, depending on the library's internal state.
        MDNS.end();
        if (MDNS.begin(mdnsHostname.c_str())) {
          SerialPrintln(F("mDNS hostname changed - now reachable as ") + mdnsHostname + F(".local"));
        } else {
          SerialPrintln(F("mDNS hostname changed but responder failed to restart"));
        }
#endif

        request->send(200, "text/plain", "Hostname updated");
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use reset, scan, connect, or set-hostname");
    });
#endif

    //CODE_REVIEW.md #29: this endpoint used to also have a GET returning the password itself in
    //plaintext, unauthenticated "by design" (this device has no separate login) - removed
    //entirely. The per-category access-control toggle STATES (not the password) are public,
    //non-secret info and are read via GET /api/settings instead (see minimalDoc there).
    //
    //POST changes the password and/or the access-control toggles together, always gated behind
    //the CURRENT password matching - this is the one place in the whole access-control system
    //that is never itself toggleable-public (see accessControlTextPublic's comment in
    //ESPMaster.ino), otherwise a category opened via "settings public" could reopen every other
    //category including firmware/flash through this same endpoint.
    webServer.on("/api/ota-password", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("currentPassword", true)) {
        request->send(400, "text/plain", "'currentPassword' is required");
        return;
      }

      String currentPasswordValue = request->getParam("currentPassword", true)->value();
      if (!constantTimeEquals(currentPasswordValue, String(otaPassword))) {
        SerialPrintln(F("OTA password/access-control change rejected: current password incorrect"));
        request->send(401, "text/plain", "Current password is incorrect");
        return;
      }

      //newPassword is optional and MAY be empty - an empty value deliberately disables password
      //protection globally (CODE_REVIEW.md #29's "muss komplett deaktivierbar sein" requirement).
      //Omitting the param entirely (not present at all) leaves the password unchanged, so a
      //request that only wants to update the access-control toggles doesn't have to resend it.
      if (request->hasParam("newPassword", true)) {
        otaPassword = request->getParam("newPassword", true)->value();
        writeFile(LittleFS, otaPasswordPath, otaPassword.c_str());
        SerialPrintln(otaPassword == "" ? F("OTA password cleared - protection now globally OFF") : F("OTA password changed"));
      }

      //Every accessControl* toggle is optional per-request (a plain checkbox form only submits
      //checked boxes) - an ABSENT param means "unchecked"/false, matching normal HTML form
      //semantics, not "leave unchanged". The frontend always sends the full set together.
      accessControlTextPublic = request->hasParam("accessControlText", true);
      accessControlSettingsPublic = request->hasParam("accessControlSettings", true);
      accessControlUnitsPublic = request->hasParam("accessControlUnits", true);
      accessControlWifiPublic = request->hasParam("accessControlWifi", true);
      accessControlTimezonePublic = request->hasParam("accessControlTimezone", true);
      accessControlSystemPublic = request->hasParam("accessControlSystem", true);
      writeFile(LittleFS, accessControlTextPublicPath, accessControlTextPublic ? "true" : "false");
      writeFile(LittleFS, accessControlSettingsPublicPath, accessControlSettingsPublic ? "true" : "false");
      writeFile(LittleFS, accessControlUnitsPublicPath, accessControlUnitsPublic ? "true" : "false");
      writeFile(LittleFS, accessControlWifiPublicPath, accessControlWifiPublic ? "true" : "false");
      writeFile(LittleFS, accessControlTimezonePublicPath, accessControlTimezonePublic ? "true" : "false");
      writeFile(LittleFS, accessControlSystemPublicPath, accessControlSystemPublic ? "true" : "false");

      request->send(200, "text/plain", "Security settings updated");
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
      if (rejectIfPasswordRequired(request, accessControlTimezonePublic)) return;

      if (!request->hasParam("timezone", true)) {
        request->send(400, "text/plain", "'timezone' is required");
        return;
      }

      String newTimezoneValue = request->getParam("timezone", true)->value();
      if (newTimezoneValue.length() == 0) {
        request->send(400, "text/plain", "Timezone cannot be empty");
        return;
      }

      //isTimezoneLookupBusy() covers a PREVIOUS request whose slot was already taken but whose
      //async lookup (checkTimezoneLookup(), ServiceAsyncTimeSyncFunctions.ino) hasn't finished
      //yet - pendingTimezoneSlot.request alone would miss that window. timezoneLookupDeferredRequest
      //additionally covers the narrow gap AFTER the lookup finishes (tzLookupState already back to
      //IDLE, so isTimezoneLookupBusy() alone would already say "no") but BEFORE loop()'s own finish
      //block below has actually persisted the result and responded - found 2026-09-09,
      //CODE_REVIEW_V2.md #14/Nachtrag: without this, a request accepted during exactly that gap
      //could overwrite pendingTimezoneValue before the first request's own result is saved under
      //it. (timezoneLookupDeferredValue's own snapshot, taken at kickoff below, closes the second
      //half of that same finding - even if a new request somehow still slipped in here.)
      if (pendingTimezoneSlot.request != nullptr || isTimezoneLookupBusy() || timezoneLookupDeferredRequest != nullptr) {
        request->send(503, "text/plain", "Another timezone change is already in progress, try again shortly");
        return;
      }

      //The actual lookup is deferred to loop() (see pendingTimezoneSlot's comment) via a
      //genuinely non-blocking state machine (ServiceAsyncTimeSyncFunctions.ino), not run here.
      pendingTimezoneValue = newTimezoneValue;
      pendingTimezoneSlot.accept(request);
    });

#if MQTT_ENABLE == true
    //Own registration, like /api/timezone above - a genuinely new settings category, no existing
    //route it naturally shares with (see the "every registered handler costs a small permanent
    //heap allocation" comment on /api/quiet-hours below for why that's a deliberate, not-free
    //choice here, not an oversight).
    webServer.on("/api/mqtt", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == HTTP_GET) {
        JsonDocument doc;
        doc["mqttEnabled"] = mqttEnabled;
        doc["mqttBrokerHost"] = mqttBrokerHost;
        doc["mqttBrokerPort"] = mqttBrokerPort;
        doc["mqttUsername"] = mqttUsername;
        //Password itself never sent back, same convention as otaPassword - only whether one is
        //set at all is exposed, not the value.
        doc["mqttPasswordSet"] = mqttPassword != "";
        doc["mqttConnected"] = mqttEnabled && mqttClient.connected();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // request->method() == HTTP_POST
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      //Validate EVERYTHING into local temporaries before touching any persisted/live state -
      //found 2026-09-10, CODE_REVIEW_V4.md #4: this used to mutate and persist mqttEnabled/
      //mqttBrokerHost, THEN validate brokerPort - a request with a valid host but an invalid port
      //got 400, yet the host change (and its file write) had already happened. Worse,
      //applyMqttSettingsChange() (the only place that re-points the live PubSubClient's
      //setServer() at mqttBrokerHost's CURRENT buffer) never ran on that early return, so the
      //client kept using whatever raw pointer an earlier setServer() call had captured - if
      //mqttBrokerHost's String reallocated its buffer on that partial reassignment (as String
      //commonly does), the client was left holding a dangling pointer into freed memory. Now: on
      //any validation failure, nothing has changed at all; on success, every field is applied and
      //persisted together, immediately followed by the one applyMqttSettingsChange() call that
      //refreshes the client's server pointer against the final, current buffer.
      bool newMqttEnabled = request->hasParam("enabled", true); //same checkbox semantics as accessControl*
      String newBrokerHost = request->hasParam("brokerHost", true) ? request->getParam("brokerHost", true)->value() : mqttBrokerHost;
      uint16_t newBrokerPort = mqttBrokerPort;
      if (request->hasParam("brokerPort", true)) {
        String rawPort = request->getParam("brokerPort", true)->value();
        if (!isNumber(rawPort) || rawPort.toInt() < 1 || rawPort.toInt() > 65535) {
          request->send(400, "text/plain", "'brokerPort' must be a number 1-65535");
          return;
        }
        newBrokerPort = rawPort.toInt();
      }
      String newUsername = request->hasParam("username", true) ? request->getParam("username", true)->value() : mqttUsername;
      //Same "present decides, absent means unchanged, empty explicitly clears" convention as
      //otaPassword's newPassword - see that comment.
      bool passwordProvided = request->hasParam("password", true);
      String newPassword = passwordProvided ? request->getParam("password", true)->value() : mqttPassword;

      mqttEnabled = newMqttEnabled;
      writeFile(LittleFS, mqttEnabledPath, mqttEnabled ? "true" : "false");
      mqttBrokerHost = newBrokerHost;
      writeFile(LittleFS, mqttBrokerHostPath, mqttBrokerHost.c_str());
      mqttBrokerPort = newBrokerPort;
      writeFile(LittleFS, mqttBrokerPortPath, String(mqttBrokerPort).c_str());
      mqttUsername = newUsername;
      writeFile(LittleFS, mqttUsernamePath, mqttUsername.c_str());
      if (passwordProvided) {
        mqttPassword = newPassword;
        writeFile(LittleFS, mqttPasswordPath, mqttPassword.c_str());
      }

      applyMqttSettingsChange();
      request->send(200, "text/plain", "MQTT settings updated");
    });
#endif

    //No dedicated GET here - quietHoursEnabled/Start/End are already folded into GET /api/settings
    //(see minimalDoc above). Adding a second registered route on top of that one reproducibly
    //broke /api/settings' own response handling on this heap-constrained device - every registered
    //handler costs a small permanent heap allocation that's never freed, and there's very little
    //room to spare.
    webServer.on("/api/quiet-hours", HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

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
    //Same low-heap guard as GET "/"'s index.html serving (see its own comment) for consistency,
    //though nothing else in this file serves a LittleFS file from a route that could land here.
    webServer.onNotFound([](AsyncWebServerRequest * request) {
      if (ESP.getFreeHeap() < 6000) {
        request->send(503, "text/plain", "Low memory, try again shortly");
        return;
      }
      request->send(404, "text/plain", "Not Found");
    });

    delay(250);
    webServer.begin();

#if USE_MULTICAST == true
    if (MDNS.begin(mdnsHostname.c_str())) {
      SerialPrintln(F("DEBUG: mDNS responder started - reachable as ") + mdnsHostname + F(".local"));
    } else {
      SerialPrintln(F("DEBUG: mDNS responder failed to start"));
    }
#endif

#if MQTT_ENABLE == true
    initMqtt();
#endif

    debugStatus = "Ready";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);
    SerialPrintln(F("Split Flap Ready!"));
    SerialPrintln(F("#######################################################"));
  }
  else {
    //Crash-log persistence (and loading the rest of the persisted config) must not depend on WiFi
    //having succeeded - found 2026-09-10 while auditing previously-unreviewed corners of the
    //codebase. custom_crash_callback() can only ever hold ONE crash in RTC memory at a time (a
    //later crash unconditionally overwrites it) - if this boot's own path here is reached because
    //the crash itself was WiFi-related (e.g. the AUTH_EXPIRE/WiFiManager reconnect issue under
    //separate investigation), every subsequent boot could ALSO fail to reconnect for the same
    //underlying reason, and the one crash worth diagnosing would keep getting silently discarded
    //before ever reaching LittleFS - never actually visible via GET /api/crash-log, since the web
    //server itself only starts in the branch above. Both calls are pure local filesystem
    //operations with no WiFi dependency at all, so there's no reason they were ever gated on it in
    //the first place - this just extends the exact same calls the success branch above already
    //makes (see the "File System Init" step there) to this path too.
    initialiseFileSystem();
    loadValuesFromFileSystem();
    checkAndPersistCrashLog(); // see ServiceCrashLogFunctions.ino - must run after LittleFS mounts

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
#if USE_MULTICAST == true
  MDNS.update();
#endif
#if MQTT_ENABLE == true
  mqttLoop();
#endif

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

  //I2C admin requests deferred here by the web handlers (see pendingRescanSlot's comment near
  //unitsAmount) - checked first so a pending request gets served as soon as possible.
  if (pendingRescanSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingRescanSlot.take(disconnected);

    int detectedAmount = scanI2CBus();

    //Client may have disconnected while scanI2CBus() ran, making deferredRequest a dangling pointer.
    if (PendingRequestSlot::stillConnected(disconnected)) {
      JsonDocument doc;
      doc["unitsAmount"] = detectedAmount;
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }

  if (pendingUnitOffsetSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingUnitOffsetSlot.take(disconnected);

    bool writeOk = writeOffsetToUnit(pendingUnitOffsetUnitId, pendingUnitOffsetValue);
    //Recalibration re-homes the unit - its previously-displayed letter is no longer
    //guaranteed correct even though the cache's text/alignment are unchanged. See
    //displayRenderGeneration's comment - CODE_REVIEW.md #14.
    displayRenderGeneration++;

    if (PendingRequestSlot::stillConnected(disconnected)) {
      if (writeOk) {
        deferredRequest->send(202, "text/plain", "Offset sent. Unit is recalibrating now, this can take a few seconds.");
      } else {
        deferredRequest->send(503, "text/plain", "Failed to write offset to unit (I2C write failed) - try again");
      }
    }
  }

  //Was a single-tick block calling timezone.setLocation() directly (a blocking DNS+UDP network
  //call) - live-reproduced as a genuine software-watchdog crash under real network stress
  //(2026-09-09, CODE_REVIEW.md #30/BUGFIX_LOG #170 - a wdtDisable()/wdtEnable() mitigation was
  //tried and reverted after it caused an even WORSE crash type on this ESP8266 core/SDK version).
  //Replaced with a genuinely non-blocking implementation (checkTimezoneLookup(),
  //ServiceAsyncTimeSyncFunctions.ino) that never blocks loop() - which means the lookup now spans
  //multiple loop() ticks instead of finishing within the one tick that takes the request out of
  //pendingTimezoneSlot, so the deferred request has to be held across that whole span instead of
  //just this one block (timezoneLookupDeferredRequest/_Generation, declared near
  //pendingTimezoneSlot). checkTimezoneLookup() itself already ran earlier this loop() tick (near
  //the top, where events() used to be) - by the time control reaches here, tzLookupDone may
  //already be true for the SAME tick a lookup finishes on.
  if (pendingTimezoneSlot.request != nullptr && timezoneLookupDeferredRequest == nullptr && !isTimezoneLookupBusy()) {
    timezoneLookupDeferredRequest = pendingTimezoneSlot.take(timezoneLookupDeferredFlag);
    //Snapshot NOW, not re-read from the shared pendingTimezoneValue global at finish time below -
    //see timezoneLookupDeferredValue's own comment (CODE_REVIEW_V2.md #14/Nachtrag).
    timezoneLookupDeferredValue = pendingTimezoneValue;
    startTimezoneLookup(timezoneLookupDeferredValue);
  }

  if (timezoneLookupDeferredRequest != nullptr && tzLookupDone) {
    AsyncWebServerRequest* deferredRequest = timezoneLookupDeferredRequest;
    std::shared_ptr<std::atomic<bool>> disconnectedFlag = timezoneLookupDeferredFlag;
    timezoneLookupDeferredRequest = nullptr;

    bool timezoneOk = tzLookupResult;
    if (timezoneOk) {
      timezoneSetting = timezoneLookupDeferredValue;
      writeFile(LittleFS, timezonePath, timezoneSetting.c_str());
      SerialPrintln(F("Timezone changed to: ") + timezoneSetting);
      //The active rule is now confirmed resolved regardless of how the original boot attempt
      //went - stops a still-pending boot-retry (CODE_REVIEW_V3.md #12) from redundantly
      //re-resolving a value that's already correct (harmless either way, just unnecessary).
      bootTimezoneRuleResolved = true;
    } else {
      SerialPrintln(F("Timezone change rejected: '") + timezoneLookupDeferredValue + F("' not recognized"));
    }

    if (PendingRequestSlot::stillConnected(disconnectedFlag)) {
      if (timezoneOk) {
        deferredRequest->send(200, "text/plain", "Timezone changed");
      } else {
        deferredRequest->send(400, "text/plain", "Timezone not recognized - use a TZ database name like Europe/Berlin");
      }
    }
  }

  //Consumes the boot-time (or a retry of it) lookup's own result exactly once - see
  //bootTimezoneRuleResolved's own comment (CODE_REVIEW_V3.md #12). Only reacts to a completion
  //that has NO associated HTTP request (timezoneLookupDeferredRequest == nullptr) - an explicit
  //POST /api/timezone's own outcome is fully handled by the block directly above and must never
  //be reinterpreted as a boot/retry result here.
  if (bootTimezoneLookupPending && timezoneLookupDeferredRequest == nullptr && tzLookupDone) {
    bootTimezoneLookupPending = false;
    if (tzLookupResult) {
      bootTimezoneRuleResolved = true;
      SerialPrintln(F("Boot timezone lookup succeeded"));
    } else {
      nextBootTimezoneRetryAt = millis() + BOOT_TIMEZONE_RETRY_MS;
      SerialPrintln(F("Boot timezone lookup failed - retrying in ") + String(BOOT_TIMEZONE_RETRY_MS / 1000) + F("s"));
    }
  }

  //Fires the scheduled retry once due - deliberately steps aside if a real user-initiated
  ///api/timezone request is currently pending (harmless either way, isTimezoneLookupBusy() would
  //also block it, but avoiding the race entirely means a real request never has to wait behind an
  //automatic background retry it didn't ask for).
  if (!bootTimezoneRuleResolved && !bootTimezoneLookupPending && nextBootTimezoneRetryAt != 0 &&
      millisReached(nextBootTimezoneRetryAt) && !isTimezoneLookupBusy() && pendingTimezoneSlot.request == nullptr) {
    SerialPrintln(F("Retrying boot timezone lookup for: ") + timezoneSetting);
    startTimezoneLookup(timezoneSetting);
    bootTimezoneLookupPending = true;
  }

  if (pendingUnitsInfoSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingUnitsInfoSlot.take(disconnected);

    JsonDocument doc;
    doc["unitsAmount"] = unitsAmount;
    for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
      int status = -2;
      int offset = -1;
      bool online;
      //Default is the light 3-byte status+offset read, not the 8-byte one with phase/progress/
      //diagnostics - the Administration page never displays those. ?detail=1 opts into the heavy
      //read (added the two diagnostic counters 2026-09-10 alongside phase/progress, rather than a
      //separate query param - two extra I2C bytes is negligible next to the existing 6).
      if (pendingUnitsInfoRequestDetail) {
        int phase = -1;
        int progress = -1;
        int unexpectedHomeTriggerCount = -1;
        int slowHomeSearchCount = -1;
        online = readUnitInfo(unitIndex, status, offset, phase, progress, unexpectedHomeTriggerCount, slowHomeSearchCount);
        doc["units"][unitIndex]["phase"] = phase;
        doc["units"][unitIndex]["progress"] = progress;
        doc["units"][unitIndex]["unexpectedHomeTriggerCount"] = unexpectedHomeTriggerCount;
        doc["units"][unitIndex]["slowHomeSearchCount"] = slowHomeSearchCount;
      } else {
        online = readUnitInfo(unitIndex, status, offset);
      }

      doc["units"][unitIndex]["index"] = unitIndex;
      doc["units"][unitIndex]["online"] = online;
      doc["units"][unitIndex]["status"] = status;
      doc["units"][unitIndex]["offset"] = offset;

      yield();
    }

    if (PendingRequestSlot::stillConnected(disconnected)) {
      AsyncResponseStream *response = deferredRequest->beginResponseStream("application/json");
      serializeJson(doc, *response);
      deferredRequest->send(response);
    }
  }

  if (pendingLegacyI2CScanSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingLegacyI2CScanSlot.take(disconnected);

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
            doc["devices"][foundCount]["statusText"] = F("ready");
          } else if (status == 1) {
            doc["devices"][foundCount]["statusText"] = F("busy");
          } else {
            doc["devices"][foundCount]["statusText"] = F("unknown");
          }
        } else {
          doc["devices"][foundCount]["status"] = -1;
          doc["devices"][foundCount]["statusText"] = F("no response");
        }
        foundCount++;
      }

      yield();
      delay(10);
    }

    doc["foundCount"] = foundCount;
    doc["expectedCount"] = unitsAmount;

    if (PendingRequestSlot::stillConnected(disconnected)) {
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
      otaBytesWritten = 0;
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
    //No otaChunkRejected check needed here (removed, CODE_REVIEW_V4.md #1) - otaPendingPhase only
    //ever transitions to OTA_PHASE_WRITE from the /api/firmware-chunk outer handler above, which by
    //now only does so once it has confirmed THIS request was the buffer's sole, still-current owner.
    bool ok = (Update.write(otaChunkBuffer, otaChunkLen) == otaChunkLen);
    if (!ok) {
      SerialPrintln(F("Firmware upload: chunk write failed"));
      Update.printError(Serial);
      //Must still call end() here (ignoring its result): Update.begin() refuses to start a new
      //update while the previous one is still marked "running" internally, and only end() clears
      //that - skipping this leaves every subsequent upload failing until a full reboot.
      Update.end(false);
      delete[] otaChunkBuffer; //abort the whole upload - a missing chunk buffer makes the other
      otaChunkBuffer = nullptr; //two endpoints cleanly reject anything further
    } else {
      otaBytesWritten += otaChunkLen; //see the finish-phase comment for why not Update.progress()
    }

    otaResultSuccess = ok;
    otaResultMessage = ok ? "OK" : "Chunk write failed, check the serial log for details";
    otaCompletedPhase = OTA_PHASE_WRITE;
    otaResultReady = true;
    otaPendingPhase = OTA_PHASE_NONE;

  } else if (otaPendingPhase == OTA_PHASE_FINISH) {
    //Verify every declared byte actually arrived before accepting this as a complete update -
    //Update.end(true)'s evenIfRemaining=true deliberately skips the library's own isFinished()
    //check (see Updater.cpp), which alone would let /api/firmware-finish activate a truncated
    //image if called early (found 2026-09-09, CODE_REVIEW.md #2). evenIfRemaining still has to
    //stay true for the real end() call below, though - Update.begin() rounds its internal target
    //size UP to the next flash sector, always larger than the exact byte count actually
    //transferred, so the library's own isFinished() would never be satisfied even for a genuinely
    //complete, correctly-sized upload.
    //
    //Deliberately otaBytesWritten here, NOT Update.progress(): live-tested 2026-09-09 against a
    //real firmware image (518944 bytes, not a multiple of 4096) and it reproducibly rejected every
    //single upload as "incomplete", short by exactly the size of the final chunk. Root cause is in
    //Updater.cpp - write() only advances progress() when a full SPI_FLASH_SEC_SIZE (4096) buffer
    //gets flushed to flash; a trailing partial sector stays in Updater's internal _buffer and is
    //only flushed inside end(), i.e. AFTER this comparison would run. So Update.progress() legitimately
    //under-reports by design for any image whose size isn't an exact multiple of 4096 - which is
    //effectively every real image. otaBytesWritten instead counts bytes actually handed to (and
    //accepted by) Update.write() call-by-call, independent of Updater's internal flush timing.
    size_t received = otaBytesWritten;
    bool sizeMatches = (received == otaBeginSize);
    bool ok = false;
    if (!sizeMatches) {
      SerialPrintln(F("Firmware upload: size mismatch at finish (received ") + String(received) +
        F(" of ") + String(otaBeginSize) + F(" declared bytes) - refusing to activate a truncated image"));
      Update.end(false); //release the "running" lock without activating anything
    } else {
      ok = Update.end(true);
      if (!ok) {
        SerialPrintln(F("Firmware upload: Update.end() failed"));
        Update.printError(Serial);
      }
    }
    delete[] otaChunkBuffer;
    otaChunkBuffer = nullptr;

    otaResultSuccess = ok;
    otaResultMessage = ok ? "OK - Firmware written, rebooting now"
      : (sizeMatches ? "Update failed, check the serial log for details"
        : "Firmware upload incomplete (" + String(received) + " of " + String(otaBeginSize) + " bytes received) - refusing to activate a truncated image");
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
    //A body that claimed ownership but then vanished mid-transfer (connection dropped before its
    //outer handler ever ran) would otherwise leave otaChunkOwner permanently stuck, since only
    //that request's own outer handler normally clears it - the same watchdog window that already
    //frees the buffer for this case releases the claim too.
    otaChunkOwner = nullptr;
  }

  // Separate from the block above on purpose: the flash-side result and the deferred request can
  // become ready in either order.
  //
  // Gated on otaResultReady ALONE now, not "otaResultReady && pendingFirmwareUploadSlot.request !=
  // nullptr" (found 2026-09-10, CODE_REVIEW_V4.md #13): /api/firmware-finish's client can disconnect
  // right after the request is accepted but before this loop() tick runs - PendingRequestSlot's own
  // onDisconnect callback (see its accept() comment) already nulls the slot's request pointer at
  // that point, which used to skip this WHOLE block, reboot-flagging included, even though
  // Update.end() had already genuinely succeeded and the new firmware was already active. Whether
  // to actually SEND the HTTP response legitimately depends on a request still being there to send
  // it to - whether to flag the reboot must not, since the flash write itself doesn't care whether
  // anyone is still listening for its result.
  if (otaResultReady) {
    if (otaResultSuccess && otaCompletedPhase == OTA_PHASE_FINISH) {
      SerialPrintln(F("Firmware update successful, rebooting..."));
      isPendingReboot = true;
    } else if (!otaResultSuccess) {
      SerialPrintln(F("Firmware update failed"));
    }

    if (pendingFirmwareUploadSlot.request != nullptr) {
      std::shared_ptr<std::atomic<bool>> disconnected;
      AsyncWebServerRequest* deferredRequest = pendingFirmwareUploadSlot.take(disconnected);

      if (PendingRequestSlot::stillConnected(disconnected)) {
        AsyncWebServerResponse* response = deferredRequest->beginResponse(otaResultSuccess ? 200 : 500, "text/plain", otaResultMessage);
        response->addHeader("Connection", "close");
        deferredRequest->send(response);
      }
    }

    otaResultReady = false;
  }
#endif

#if UNIT_BOOTLOADER_ENABLE == true
  if (pendingFirmwarePushSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingFirmwarePushSlot.take(disconnected);
    int unitIndex = pendingFirmwarePushUnitIndex;

    SerialPrintln(F("Pushing firmware to Unit ") + String(unitIndex) + F("..."));
    String resultJson;
    //Always runs to completion regardless of whether the client is still listening - see the
    //comment on the onDisconnect handler for /unit-firmware-push above.
    bool success = pushFirmwareToUnit(unitIndex, resultJson);
    //Invalidate the display cache unconditionally, not just on success - found 2026-09-10,
    //CODE_REVIEW_V3.md #10: pushFirmwareToUnit()'s very first step (requestUnitEnterBootloader())
    //already interrupts the unit's running app and reboots it into the bootloader regardless of
    //whether the push that follows ultimately succeeds - the unit's actual physical position may
    //no longer match whatever the cache still believes, either way. Same reasoning as
    //forceUnitRecoveryReset()'s identical fix (ServiceI2CRecoveryFunctions.ino).
    displayRenderGeneration++;

    if (PendingRequestSlot::stillConnected(disconnected)) {
      deferredRequest->send(success ? 200 : 500, "application/json", resultJson);
    }
  }

  if (pendingBootloaderProbeSlot.request != nullptr) {
    std::shared_ptr<std::atomic<bool>> disconnected;
    AsyncWebServerRequest* deferredRequest = pendingBootloaderProbeSlot.take(disconnected);
    int unitIndex = pendingBootloaderProbeUnitIndex;
    int expectedBootAddress = UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex;

    JsonDocument doc;
    doc["unitIndex"] = unitIndex;

    //Same structural-collision guard as pushFirmwareToUnit() - see its comment
    //(ServiceUnitFirmwareFunctions.ino, CODE_REVIEW_V2.md #16). This probe is read-only so a
    //collision here can't corrupt anything, but a "successful" read against a colliding address
    //could still be silently-wrong garbage from whichever unit happens to win the bus - reject
    //clearly instead, without ever sending the enter-bootloader command.
    bool collisionRisk = expectedBootAddress < unitsAmount;

    if (collisionRisk) {
      doc["success"] = false;
      doc["error"] = F("Refusing to probe Unit ") + String(unitIndex) + F(": its bootloader address would collide with "
        "another unit's live app address at this installation's current size. See CODE_REVIEW_V2.md #16.");
    } else {
      SerialPrintln(F("Bootloader probe: requesting Unit ") + String(unitIndex) + F(" to enter bootloader..."));
      unsigned long bootloaderSearchStart = millis();
      uint8_t enterBootloaderResult = requestUnitEnterBootloader(unitIndex);
      SerialPrintln(F("  requestUnitEnterBootloader() Wire.endTransmission() = ") + String(enterBootloaderResult) + F(" (0 = success)"));
      //Invalidate the display cache - found 2026-09-10, CODE_REVIEW_V3.md #10: this interrupts and
      //reboots the unit's running app regardless of whether the probe below actually finds it in
      //the bootloader afterward - see the identical fix/comment on pushFirmwareToUnit()'s call
      //site above. Not reached in the collisionRisk branch above, which deliberately never sends
      //this command at all - nothing to invalidate there.
      displayRenderGeneration++;

      //Same retry/backoff as pushFirmwareToUnit() - give the watchdog reset + twiboot startup
      //time to happen. Probes ONLY unitIndex's own expected address, not a full scan of the whole
      //bootloader address range - see findExpectedBootloaderAddress()'s comment
      //(ServiceUnitFirmwareFunctions.ino, CODE_REVIEW_V2.md #15).
      TwibootChipInfo chipInfo;
      int bootAddress = findExpectedBootloaderAddress(unitIndex, chipInfo, bootloaderSearchStart) ? expectedBootAddress : -1;

      if (bootAddress < 0) {
        doc["success"] = false;
        doc["error"] = F("No unit responded in bootloader mode. Nothing was written.");
      } else {
      doc["success"] = true;
      doc["bootAddress"] = bootAddress;
      doc["bootAddressHex"] = F("0x") + String(bootAddress, HEX);
      doc["addressOffsetFromBase"] = bootAddress - UNIT_BOOTLOADER_BASE_ADDRESS;
      char sigBuf[16];
      snprintf_P(sigBuf, sizeof(sigBuf), reinterpret_cast<PGM_P>(F("%02X %02X %02X")), chipInfo.signature[0], chipInfo.signature[1], chipInfo.signature[2]);
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
    }

    if (PendingRequestSlot::stillConnected(disconnected)) {
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
  
  //ezTime's own auto-sync is disabled (setInterval(0) in setup()) - events() is now a permanent
  //no-op, not called here anymore. checkNtpSync() (ServiceAsyncTimeSyncFunctions.ino) replaces it
  //with a genuinely non-blocking implementation instead of the wdtDisable()/wdtEnable() bracket
  //tried and reverted earlier (2026-09-09, CODE_REVIEW.md #30/BUGFIX_LOG #170 - that bracket
  //caused a NEW, worse crash type on this ESP8266 core/SDK version when applied to this
  //every-tick call site). checkTimezoneLookup() is the equivalent replacement for the
  ///api/timezone endpoint's own network lookup - see pendingTimezoneSlot's processing block
  //further down for how it's driven. Neither call below ever blocks - both are cheap, immediate
  //checks on every tick where nothing is in flight.
  checkNtpSync();
  checkTimezoneLookup();

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
#if MQTT_ENABLE == true
    checkMqttState();
#endif

    // Skip display updates if a web request is active to prevent blocking.
    // (showText() itself is a no-op when no units are detected, see scanI2CBus())
    if (!webRequestActive) {
      //Mode Selection
      if (deviceMode == DEVICE_MODE_ROTATION) {
        //rotationCurrentAlignment ("" = inherit the default) is set every tick by checkRotation().
        //Only count this second toward the current item's duration if it was actually accepted -
        //see rotationItemAcceptedSeconds' own comment (CODE_REVIEW_V2.md #13). Additionally
        //requires the display to have actually SETTLED (!isDisplayJobBusy()) before counting -
        //found 2026-09-10, CODE_REVIEW_V3.md #9: showText() already returns true the moment a
        //job merely STARTS toward this item (matching CODE_REVIEW_V2.md #9's "already in progress"
        //check), well before the flaps have actually finished moving - counting from there meant
        //part of an item's configured duration was consumed by motor movement time rather than
        //genuinely stationary, readable display time, especially for a large flap-position change
        //or a multi-line item.
        bool rotationTextAccepted = showText(inputText, 0, rotationCurrentAlignment, rotationCurrentAnimationMode, rotationCurrentAnimationStaggerMs);
        if (rotationTextAccepted && !isDisplayJobBusy()) {
          rotationItemAcceptedSeconds++;
        }
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
  //A rescan can mean a unit restarted and lost its displayed letter (or the unit count itself
  //changed) - either way the physical display may no longer match the text-content cache, even
  //though the cache's own text/alignment never changed. See displayRenderGeneration's comment -
  //CODE_REVIEW.md #14. Bumped unconditionally (including at boot, harmless there) rather than
  //only when detectedAmount differs from before, since a unit restarting mid-scan with the SAME
  //final count is exactly the case this needs to catch.
  displayRenderGeneration++;

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

