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
#define USE_MULTICAST       true   //broadcast a "<mdnsName>.local" hostname on the local network.
                                   //If running more than one display on the same network, mdnsName
                                   //below must be unique per device.
//LED_ENABLE only gates the "WiFi totally failed to connect" continuous error blink at the end of
//setup() - see that block's own comment.
#define LED_ENABLE          true
#define DEBUG_ENABLE true  //startup debug page, error panel, page-load log, serial debug log -
                           //gated at runtime by showDebugPage (POST /api/debug-mode, persisted
                           //in /debugmode.txt)
#define MQTT_ENABLE false  //MQTT + Home Assistant auto-discovery. This flag alone decides whether
                           //the feature exists in the firmware AT ALL - PubSubClient's own buffer
                           //is malloc()'d from its constructor, so a compile-time false here is
                           //the only way to avoid that cost entirely. Flip to true + rebuild if
                           //MQTT/Home-Assistant integration is ever wanted.

#define WIFI_STATIC_IP      false //try a router-assigned static IP instead of DHCP
#define FAE_MOD                   //modified PCB with an ESP-12F module

//ESP01 GPIO1/TX is used for I2C SDA and GPIO3/RX for I2C SCL (Wire.begin(1, 3)) - so the debug LED
//goes on GPIO2 instead of the built-in GPIO1 LED. VCC must be 3.3V only.
#define LED_DEBUG_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

//--- Constants - only change if you know what you're doing ---
#define ANSWER_SIZE         6 //status + offset + phase + progress bytes, must match Unit.ino's requestEvent()
#define ANSWER_SIZE_LIGHT   3 //status + offset only, see readUnitInfo()'s 3-arg overload
#define ANSWER_SIZE_DIAG    8 //adds the two diagnostic counters - see readUnitInfo()'s 7-arg
                               //overload. A unit not yet updated to the newer Unit.ino still
                               //answers without error - the last 2 bytes just read back as 255
                               //(idle bus), not a detectable "unsupported" marker.
#define ANSWER_SIZE_VERSION 11 //adds the 3 firmware-version bytes (major/minor/patch) - see
                               //readUnitInfo()'s 10-arg overload. Same 255-byte caveat as
                               //ANSWER_SIZE_DIAG above.
#define FLAP_AMOUNT         45
#define MIN_SPEED           1
#define MAX_SPEED           12
#define UNIT_STEPS          2038    //must match STEPS in Unit.ino
#define UNIT_BOOTLOADER_CMD 255     //must match CMD_ENTER_BOOTLOADER in Unit.ino
#define UNIT_BOOTLOADER_BASE_ADDRESS 0x29 //start of the twiboot bootloader address range (0x29-0x68) -
                                           //NOTE: 0x68 is also RTC_I2C_ADDRESS below - would collide
                                           //if ever expanded near the structural 64-unit ceiling.
#define CRASH_LOG_PATH "/crash-log.jsonl"
//Optional DS3231 RTC module on the same I2C bus - fixed address per the DS3231 datasheet, not
//configurable. Entirely optional/auto-detected (probeRtc()).
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
#include <ESP8266HTTPClient.h> //Primary->Extension push/health-check (ServiceExtensionFunctions.ino) -
                               //verified IRAM-neutral vs. plain WiFiClient before adding this
                               //dependency (byte-identical IRAM usage in isolated test builds).
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
#include <memory> //std::shared_ptr - PendingRequestSlot's per-generation disconnect flag
#include <atomic> //std::atomic<bool> - same reason
#include <user_interface.h> //struct rst_info, for the crash reporter
#include <umm_malloc/umm_heap_select.h> //HeapSelectIram - OTA buffers in the second heap (mmu=4816H)
#include <lwip/priv/tcp_priv.h> //exposes tcp_active_pcbs/etc. for /api/tcp-status
#include "Classes.h"
#include "ServiceMemoryReserveFunctions.h"
#include "LittleFS.h"

//Request method values for webServer.on() and request->method() - ESPAsyncWebSrv's own bit flags.
//The library's HTTP_POST/HTTP_DELETE names can't be used in this sketch: WiFiManager includes
//ESP8266WebServer, and ESPAsyncWebSrv.h only defines its own WebRequestMethod enum
//#ifndef WEBSERVER_H - so here HTTP_POST/HTTP_DELETE are ESP8266WebServer's HTTPMethod values (3/6)
//while the separately compiled library still sets and checks its bit values (2/4). An "HTTP_POST"
//registration therefore also accepted GET (3 = GET|POST bits), "HTTP_DELETE" also POST, and
//comparisons against HTTP_POST were never true. Only HTTP_GET (1) happened to match
//(BUILD_INFO Punkt 236).
const WebRequestMethodComposite ASYNC_HTTP_GET = 0b00000001;
const WebRequestMethodComposite ASYNC_HTTP_POST = 0b00000010;
const WebRequestMethodComposite ASYNC_HTTP_DELETE = 0b00000100;

//--- User-facing settings ---
//Hostname broadcast via mDNS when USE_MULTICAST is enabled - reachable as "<mdnsHostname>.local"
//on networks whose clients support mDNS. Persisted, not a compile-time constant - see POST
///api/wifi's "set-hostname" action (Netzwerk tab in the UI).
String mdnsHostname = "split-flap";

//Used if connecting via "WIFI_USE_DIRECT" of "true" - Otherwise, leave blank
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";

//Runtime-changeable OTA password, persisted to LittleFS (see /api/ota-password). Default "splitflap".
//Empty string = password protection globally OFF - every endpoint below (including firmware/
//flash ones) is then unauthenticated, no exceptions, regardless of the toggles below.
String otaPassword = "";
const char* otaPasswordPath = "/otapassword.txt";
//Only one delivery of the web page at a time - see GET "/"'s comment (BUILD_INFO Punkt 237).
bool pageDeliveryActive = false;
unsigned long pageDeliveryStartedAt = 0;
//Which delivery owns pageDeliveryActive, and its connection - only that delivery's own disconnect
//releases the guard, and an overdue one gets closed explicitly (CODE_REVIEW_V8.md #2). The client
//stays valid until that disconnect callback runs, which also clears it.
uint32_t pageDeliveryGeneration = 0;
AsyncClient* pageDeliveryClient = nullptr;

//POST /api/rotation-items body, collected into one buffer - see that handler's comment. 8000
//matches the old form field limit; the text budget (STORED_TEXT_BUDGET) is checked afterwards.
#define ROTATION_BODY_MAX 8000
char* rotationBodyBuffer = nullptr;
size_t rotationBodyLength = 0;
AsyncWebServerRequest* rotationBodyOwner = nullptr;
bool rotationBodyTooLarge = false;
void freeRotationBody(bool ownBuffer) {
  if (ownBuffer && rotationBodyBuffer != nullptr) {
    free(rotationBodyBuffer);
    rotationBodyBuffer = nullptr;
  }
}

//Web UI login sessions - see ServiceAccessControlFunctions.ino's own comment.
#define LOGIN_SESSIONS_MAX 8
#define LOGIN_SESSION_DAYS 90
#define LOGIN_SESSION_SECONDS (LOGIN_SESSION_DAYS * 86400UL)
#define LOGIN_SESSION_COOKIE "sfsession"
//Cookie lifetime in the browser - deliberately longer than any session; whether a session is
//still valid is decided on the device, so a renewed session never needs a fresh Set-Cookie.
#define LOGIN_SESSION_COOKIE_MAX_AGE 34560000UL //400 days, the most browsers accept
const char* loginSessionsPath = "/loginsessions.json";

unsigned long lastFailedLoginAt = 0; //throttles every admin-password check after a wrong one, see checkAdminPassword()
//checkAdminPassword() results (ServiceAccessControlFunctions.ino).
#define PASSWORD_OK 0
#define PASSWORD_WRONG 1
#define PASSWORD_THROTTLED 2
const char* mdnsHostnamePath = "/mdnsname.txt";

//Unit 63's bootloader address is UNIT_BOOTLOADER_BASE_ADDRESS(0x29) + 63 = 0x68 - EXACTLY
//RTC_I2C_ADDRESS. twibootReadChipInfo() sent to the RTC's address writes a register pointer of
//0x02 (hours) followed by three zero bytes via the DS3231's own register auto-increment - a
//"read-only diagnostic" against the wrong device that actually corrupts its time registers.
//Checked at every bootloader-address entry point, independent of unitsAmount.
//
//Must live here, AFTER this file's own #include block above - a real function/variable definition
//placed BEFORE those includes confuses arduino-cli's auto-prototype-generator into inserting its
//generated prototype block ahead of the #include lines that actually declare those types,
//breaking the entire sketch's compile. #defines don't have this problem.
bool isReservedI2CAddress(int address) {
  return address == RTC_I2C_ADDRESS;
}

//Registered as the very first web handler: claims - and answers with 400 - every request the
//handlers behind it must never see. Since it's a "trivial" handler (no onRequest), the library
//doesn't even parse such a request's body.
//
//1. Multipart boundary outside RFC 2046's 1-70 characters: ESPAsyncWebSrv's multipart parser
//   loops over the boundary with a uint8_t counter - 256+ characters never end that loop and the
//   watchdog resets the device, before any handler or password check runs (same bug as
//   CVE-2026-54571; security review 2026-10-12, OW-01). Mirrors the library's own boundary
//   extraction (WebRequest.cpp).
//2. Unknown Host header (DNS rebinding, OW-07): a web page from elsewhere that re-points its own
//   domain to this device's IP would otherwise read and call everything not behind the password.
//   Accepted: an IPv4 address, or this device's mDNS/DHCP name - alone, as "<name>.local" or with
//   any router domain ("<name>.fritz.box"). An extension device configured under a different DNS
//   alias isn't reachable that way any more.
class RequestGuard : public AsyncWebHandler {
  public:
    bool canHandle(AsyncWebServerRequest *request) override {
      return hasInvalidMultipartBoundary(request) || !isKnownHost(request->host());
    }
    void handleRequest(AsyncWebServerRequest *request) override {
      request->send(400, "text/plain", F("Bad request (unknown host name or multipart boundary)"));
    }

  private:
    static bool hasInvalidMultipartBoundary(AsyncWebServerRequest *request) {
      if (!request->hasHeader("Content-Type")) {
        return false;
      }
      String value = request->getHeader("Content-Type")->value();
      if (!value.startsWith("multipart/")) {
        return false;
      }
      String boundary = value.substring(value.indexOf('=') + 1);
      boundary.replace("\"", "");
      return value.indexOf('=') == -1 || boundary.length() == 0 || boundary.length() > 70;
    }

    //True for "<name>", "<name>.local", "<name>.<anything>" (case-insensitive).
    static bool isOwnName(const String &host, const String &name) {
      if (name.length() == 0 || host.length() < name.length() || !host.substring(0, name.length()).equalsIgnoreCase(name)) {
        return false;
      }
      return host.length() == name.length() || host.charAt(name.length()) == '.';
    }

    static bool isIpv4Address(const String &host) {
      IPAddress address;
      return host.length() > 0 && isDigit(host.charAt(0)) && address.fromString(host);
    }

    static bool isKnownHost(String host) {
      int colon = host.indexOf(':');
      if (colon != -1) {
        host = host.substring(0, colon);
      }
      //No Host header at all (HTTP/1.0 tools) - no browser sends that, so rebinding can't either.
      return host.length() == 0 || isIpv4Address(host) || isOwnName(host, mdnsHostname) || isOwnName(host, WiFi.hostname());
    }
};

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
//non-empty. Each gates a specific, named group of state-changing endpoints - see
//isRequestAllowedWithoutPassword() (ServiceAccessControlFunctions.ino) for exactly which.
//Firmware/flash endpoints and this access-control configuration itself deliberately have NO
//toggle - always protected whenever a password is set. Default false for every toggle.
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

//Cached result of the last successful timezoned.rop.nl lookup (a POSIX TZ rule string, e.g.
//"CET-1CEST,M3.5.0,M10.5.0/3"). Applied immediately at boot via timezone.setPosix() when
//non-empty, so Clock/Date mode's very first pre-WiFi render already shows correct LOCAL time
//instead of UTC - a POSIX rule encodes its own DST transition dates, so this stays correct across
//DST changes without a fresh network lookup on every boot. Written back after every successful
//fresh lookup.
String cachedTimezonePosix = "";
const char* timezonePosixCachePath = "/timezone-posix-cache.txt";

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
//The current version of code to display on the UI. "2.3.0-Scott" was JonnyBooker/split-flap's own
//version at the point this project's initial commit forked it (2026-08-09) - never actually bumped
//since. Reconstructed 2026-09-16 by walking every commit since that fork forward through semver
//rules (feature commit = minor, bugfix commit = patch, breaking/protocol change = major). This is
//now this project's own independent version line, no longer tracking JonnyBooker's upstream
//releases (hence dropping the inherited "-Scott" codename).
const char* espVersion = "4.10.13";

//All the letters on the units that we have to be displayed. You can change these if it so pleases at your own risk
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
unsigned long previousMillis = 0;
int unitsAmount = 0; //auto-detected via I2C scan, sequential from 0 - see scanI2CBus()

//Replaces a fixed 30s post-boot wait (found 2026-09-11, present since this project's very first
//commit) with adaptive polling for actual unit readiness - see the loop() block using this for
//the full reasoning. showText() is a complete no-op while unitsAmount<=0, so this state machine
//is what actually gates "how soon after boot can the previously configured mode first render".
enum UnitReadinessPhase { UNIT_READINESS_SCAN, UNIT_READINESS_POLLING, UNIT_READINESS_DONE };
UnitReadinessPhase unitReadinessPhase = UNIT_READINESS_SCAN;
unsigned long unitReadinessStartedAt = 0; //set in setup(); millisReached() below is overflow-safe
unsigned long unitReadinessLastPollAt = 0;
#define UNIT_READINESS_POLL_INTERVAL_MS 200
//Safety ceiling, identical to the fixed wait this replaces - guarantees the worst case (a unit
//that never finishes/never responds) is never slower than before, while the common case (units
//actually ready within a second or two) becomes dramatically faster.
#define UNIT_READINESS_MAX_WAIT_MS 30000

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
//only I2C write anywhere in this file NOT deferred to loop() like every other one. Same
//deferred-request pattern as pendingRescanSlot above.
PendingRequestSlot pendingUnitOffsetSlot;
int pendingUnitOffsetUnitId = -1;
uint16_t pendingUnitOffsetValue = 0;

//timezone.setLocation() does a synchronous network lookup against ezTime's timezoned service -
//was called directly from the /api/timezone POST handler, blocking that AsyncWebServer callback
//on network I/O. Same deferred-request pattern as pendingRescanSlot/pendingUnitOffsetSlot above.
PendingRequestSlot pendingTimezoneSlot;
String pendingTimezoneValue = "";
//The actual lookup (checkTimezoneLookup(), ServiceAsyncTimeSyncFunctions.ino) spans MULTIPLE
//loop() ticks instead of completing within the one tick that takes the request out of
//pendingTimezoneSlot - these hold the deferred request across that whole span.
AsyncWebServerRequest* timezoneLookupDeferredRequest = nullptr;
std::shared_ptr<std::atomic<bool>> timezoneLookupDeferredFlag;
//Snapshot of pendingTimezoneValue taken at kickoff time, NOT re-read from that shared global at
//finish time - a second request accepted into the slot while the first's async lookup is still
//running would otherwise overwrite it before the first one's result gets persisted.
String timezoneLookupDeferredValue = "";
//Declared here (not in ServiceAsyncTimeSyncFunctions.ino, where they're actually driven from) -
//ESPMaster.ino is concatenated before that file, and loop() above references them directly;
//global variables aren't auto-forward-declared across .ino files.
bool tzLookupDone = false;
bool tzLookupResult = false;

//Retry tracking for the ONE-SHOT boot-time timezone lookup (started once in setup(), entirely
//fire-and-forget) - a transient DNS/network failure during that single attempt would otherwise
//permanently leave the active timezone RULE unresolved until an explicit POST /api/timezone or a
//luckier boot, since the periodic NTP resync only refreshes the CLOCK, never the zone RULE.
//bootTimezoneLookupPending tracks "a boot/retry attempt is currently in flight" so the retry logic
//reacts to each completion exactly once.
bool bootTimezoneRuleResolved = false;
bool bootTimezoneLookupPending = false;
unsigned long nextBootTimezoneRetryAt = 0; //0 = no retry currently scheduled
#define BOOT_TIMEZONE_RETRY_MS 60000UL //not urgent - just needs to eventually succeed

//Same reasoning, for the twiboot/bootloader endpoints (UNIT_BOOTLOADER_ENABLE).
PendingRequestSlot pendingFirmwarePushSlot;
int pendingFirmwarePushUnitIndex = -1;
PendingRequestSlot pendingBootloaderProbeSlot;
int pendingBootloaderProbeUnitIndex = -1;

//Shared by ServiceExtensionFunctions.ino's health check/display push AND
//ServiceDisplayJobFunctions.ino's pollDisplayJobWaitAllExtensionsReady() - declared here, not in
//either of those files, since Arduino concatenates .ino files alphabetically and
//ServiceDisplayJobFunctions.ino comes before ServiceExtensionFunctions.ino (same reasoning as
//pendingTimezoneSlot's own comment above for why a shared value used by an earlier file can't be
//#define'd in a later one).
#define EXTENSION_HEALTH_CHECK_INTERVAL_MS (15UL * 1000UL)
#define EXTENSION_HTTP_TIMEOUT_MS 1500

//POST /api/extension-display only validates and queues the segment; tickExtensionDisplay()
//(ServiceExtensionFunctions.ino) writes it from loop() - I2C writes don't belong in an
//AsyncWebServer callback (CODE_REVIEW_V6.md #1). The handler acknowledges right away instead of
//holding the request until the animation is done - a staggered animation can easily outlast the
//primary's EXTENSION_HTTP_TIMEOUT_MS, and a timed-out push counts as failed (CODE_REVIEW_V6.md #4),
//so the primary would re-render the same text over and over. The primary's readiness poll
//(GET /api/units) still sees the real state via displayBusy/displayWriteFailed.
//A newer push replaces a still-queued older one - only the latest content matters.
volatile bool pendingExtensionDisplayQueued = false;
//Set by POST /api/device-role when the role actually changed - loop() then cancels both display
//controllers (cancelDisplayControllers()) before either runs again (CODE_REVIEW_V8.md #6). Not
//done in the handler itself: it can run while loop() is yielding in the middle of a display job.
volatile bool pendingDisplayControllersCancel = false;
String pendingExtensionDisplaySegment;
String pendingExtensionDisplayAnimationMode;
unsigned long pendingExtensionDisplayStaggerMs = 0;
int pendingExtensionDisplayGlobalOffset = 0;
int pendingExtensionDisplayCombinedWidth = 0;
unsigned long pendingExtensionDisplayStartOffsetMs = 0;

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

//Multi-device roles ("Primärgerät"/"Erweiterungsgerät", see multiple-units.md - Master/Slave
//terminology deliberately avoided). standalone (default) is exactly today's behavior, unaffected
//by anything below. primary owns the display logic and stretches it across its own units plus any
//configured, reachable extensionDevices[]. extension does no display-logic of its own - its units
//are driven entirely by POST /api/extension-display pushes from its configured primary, but it
//keeps its own full web UI/API usable throughout.
const char* DEVICE_ROLE_STANDALONE = "standalone";
const char* DEVICE_ROLE_PRIMARY = "primary";
const char* DEVICE_ROLE_EXTENSION = "extension";

//How a primary uses ALL its configured extensionDevices[] (one setting for the whole group -
//mixing both kinds in one group is deliberately not supported yet). extend (default) appends each
//extension's units after the primary's own, widening the display. mirror sends every extension
//the primary's own characters instead (same text, same animation timing) - cut off or padded with
//blanks if its unit count differs - and leaves the primary's display width unchanged.
const char* EXTENSION_LINK_MODE_EXTEND = "extend";
const char* EXTENSION_LINK_MODE_MIRROR = "mirror";

//Alignment options
const char* ALIGNMENT_MODE_LEFT = "left";
const char* ALIGNMENT_MODE_CENTER = "center";
const char* ALIGNMENT_MODE_RIGHT = "right";

//Animation options - see computeUnitAnimationDelayMs() (ServiceDisplayJobFunctions.ino) for what
//each one actually does. Defined here, not in that file, despite being used there too - a #define
//used by code in THIS file must be declared before that point in Arduino's concatenated build.
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
//validation - one place for the "which values are valid" list. Functions (unlike #defines/
//globals) are auto-prototyped regardless of file order, so this is safe to call from any file.
bool isValidAnimationMode(const String &value) {
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
const char* deviceRolePath = "/devicerole.txt";
const char* extensionLinkModePath = "/extensionlinkmode.txt";
//Own file, like rotationItemsPath/scheduledMessagesPath - a short JSON array of hostnames, not a
//scalar, so it doesn't belong in the boot-snapshot blob (see that constant's own comment).
const char* extensionDevicesPath = "/extensiondevices.json";
//Combined boot-time settings cache - this device's flash chip was live-measured taking ~800ms per
//LittleFS file open regardless of file size, and loadValuesFromFileSystem() used to make ~30 of
//those on every boot. This file mirrors those ~30 scalar settings into one JSON blob, cutting
//boot-time loading down to ONE file open in the common case. The individual files remain the
//actual source of truth, untouched by every existing write path - this is a derived, read-side
//cache only.
const char* bootSnapshotPath = "/boot-snapshot.json";
//Set once right after boot and kept current by checkBootSnapshotFreshness() (ticked periodically
//from loop()) - see computeSettingsSignature()'s own comment for why a signature comparison, not
//a dedicated per-setting dirty flag.
String lastSavedSettingsSignature = "";
#define BOOT_SNAPSHOT_CHECK_INTERVAL_MS 30000 //throttles the flash-wear-conscious periodic re-check
const char* countdownPath = "/countdown.txt";
const char* scheduledMessagesPath = "/scheduled-messages.txt";
const char* debugModePath = "/debugmode.txt";
const char* unitFirmwarePath = "/unit-firmware.bin";
//Temp path for the unit-firmware upload's write-then-atomic-rename() pattern.
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
String deviceRole = "standalone"; //standalone|primary|extension - see DEVICE_ROLE_* constants' own comment
String extensionLinkMode = "extend"; //extend|mirror - see EXTENSION_LINK_MODE_* constants' own comment
//A fixed, small array like rotationItems - no per-entry heap allocation, no LinkedList
//operator[] risk. ExtensionDevice itself is declared in Classes.h, not here - see that header's
//own comment on TwibootChipInfo for why a struct used as a function parameter type must be
//visible before Arduino's auto-generated prototype block.
#define EXTENSION_DEVICES_MAX 4
ExtensionDevice extensionDevices[EXTENSION_DEVICES_MAX];
int extensionDeviceCount = 0;
String countdownToDateUnix = "";
String countupSinceDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
//Together with lastWrittenText and displayRenderGeneration below, what showText() compares a new
//request against to decide whether a redraw is actually needed. Stores whichever alignment
//(override or global) was ACTUALLY used for the last completed render, so two Rotation entries
//with identical text but different per-item alignment still trigger a redraw.
String lastWrittenAlignment = "";
//Bumped whenever something could make the PHYSICAL display state diverge from what
//lastWrittenText/lastWrittenAlignment claim, without the requested text/alignment itself
//necessarily changing - the global alignment setting changing, a rescan (a unit may have
//restarted and lost its displayed letter), or a calibration offset change. showText() redraws
//whenever this doesn't match lastRenderedGeneration, even if text/alignment are both unchanged.
unsigned long displayRenderGeneration = 0;
unsigned long lastRenderedGeneration = 0;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
//True while WiFiManager's setup hotspot is open - its password is on the display then, and Quiet
//Hours must not blank it (see initWiFi(), ServiceWifiFunctions.ino).
bool wifiSetupPortalOpen = false;
bool isWifiConfigured = false;
//Background offline-recovery retry (loop()'s `if (!isWifiConfigured)` block) - see that block's
//own comment (Code_REVIEW_V5.md #02) for why this exists at all.
unsigned long lastOfflineReconnectAttempt = 0;
#define OFFLINE_RECONNECT_RETRY_MS (5UL * 60UL * 1000UL) //5 minutes
//Makes the WiFiManager/AUTH_EXPIRE disconnect question observable over HTTP - see
//ServiceWifiFunctions.ino's own comment (Punkt 210-adjacent, 2026-09-13) for the full reasoning.
//Declared here (not in that file) so /api/diag?type=heap (ESPMaster.ino, concatenated before
//ServiceWifiFunctions.ino by Arduino's alphabetical build order) can see them.
unsigned long wifiDisconnectCount = 0;
String lastWifiDisconnectReason = "";
unsigned long lastWifiDisconnectAt = 0;
//TEMPORARY DIAGNOSTIC (2026-10-11) - only to test whether WiFi trouble correlates with low heap.
//Lowest free heap / largest free block seen since boot (sampled once per loop() pass, so short
//dips inside a single callback can be missed) and the free heap at the moment of the last WiFi
//disconnect. Shown by GET /api/diag?type=heap. Safe to remove again once that question is settled
//(these four globals, their two update sites in loop()/ServiceWifiFunctions.ino, the diag fields).
uint32_t diagMinFreeHeap = UINT32_MAX;
uint32_t diagMinFreeHeapAtSec = 0;
uint32_t diagMinMaxBlock = UINT32_MAX;
uint32_t diagFreeHeapAtLastWifiDisconnect = 0;
//Optional DS3231 RTC (ServiceRtcFunctions.ino) - declared here, not in that file, since
//setup()/GET /api/diag below (both in this earlier-concatenated main sketch file) need them; see
//probeRtc()'s own comment for the full reasoning.
bool rtcPresent = false;
bool rtcOscillatorStopFlag = false;
bool showDebugPage = false; //defaults off - the startup debug page can render blank on fresh boots
volatile bool webRequestActive = false; //skip display updates while a web request is in flight
LList<ScheduledMessage> scheduledMessages;
//False while an out-of-memory read left scheduledMessages incomplete - saving that partial list
//would overwrite every entry never read (CODE_REVIEW_V8.md #1). writeScheduledMessagesToFile()
//refuses while it's false; ensureScheduledMessagesComplete() retries the load first.
bool scheduledMessagesComplete = true;
//Bumped on every change to scheduledMessages / rotationItems - a chunked GET response aborts if
//its list changes between two chunks instead of mixing old and new entries (CODE_REVIEW_V8.md #4).
uint32_t scheduledMessagesGeneration = 0;
uint32_t rotationItemsGeneration = 0;
//Same as scheduledMessagesComplete, for the rotation list: an incomplete one isn't served by
//GET /api/rotation-items, since the editor would save it back as the full list.
bool rotationItemsComplete = true;
//Fixed array, not LList<RotationItem>: LinkedList's operator[] can dereference a NULL node under
//interleaved access - a real, reproduced crash. Direct indexing avoids it.
#define ROTATION_ITEMS_MAX 20
RotationItem rotationItems[ROTATION_ITEMS_MAX];
int rotationItemCount = 0;

//Found via Code_REVIEW_V5.md #11: scheduledMessages (an LList<ScheduledMessage>, ServiceScheduledMessageFunctions.ino)
//had no size cap at all, unlike rotationItems above - each entry's own message text is already
//capped (same 500-byte limit as /api/text), but the COUNT of entries was unbounded, and
//writeScheduledMessagesToFile() builds one JsonDocument plus one full serialized String for the
//WHOLE list on every add/remove - real, unbounded heap pressure on this already heap-constrained
//device, reachable through normal, authenticated API use (or unauthenticated too, if
//accessControlSettingsPublic is toggled on). Same order of magnitude as ROTATION_ITEMS_MAX above -
//generous for the device's actual use case (a handful of upcoming scheduled messages), while
//keeping the worst-case JSON blob a small, bounded size.
#define SCHEDULED_MESSAGES_MAX 20
//Total characters of all scheduled message texts (and, separately, of all rotation item texts)
//kept in RAM. Every stored text stays on the heap permanently - a hardware test with 16-20
//messages of 440 characters (~7-9 KB) left only ~9-11 KB free: the device hung while saving the
//17th and an OTA update failed at the connection gate (BUILD_INFO Punkt 233). 4000 characters keep
//~16 KB free after boot. New/changed entries that would exceed it are rejected; a list already on
//flash that's bigger (saved by an older firmware) is still loaded in full.
#define STORED_TEXT_BUDGET 4000
//Results of addAndPersistScheduledMessage()/removeScheduledMessage()
//(ServiceScheduledMessageFunctions.ino) - declared here since this file is compiled first.
#define SCHEDULE_ADDED 0
#define SCHEDULE_REJECTED 1 //already due/in the past, or SCHEDULED_MESSAGES_MAX reached
#define SCHEDULE_SAVE_FAILED 2
#define SCHEDULE_OVER_BUDGET 3 //all texts together would exceed STORED_TEXT_BUDGET
#define SCHEDULE_REMOVED 0
#define SCHEDULE_NOT_FOUND 1
//Results of readJsonArrayFile() (ServiceFileSystemFunctions.ino) - see its own comment.
#define JSON_ARRAY_FILE_OK 0
#define JSON_ARRAY_FILE_MISSING 1
#define JSON_ARRAY_FILE_INVALID 2
#define JSON_ARRAY_FILE_NO_MEMORY 3
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
//rotationItemStarted: a render of the current item was accepted since it became current (or since
//a temporary message displaced it). rotationItemSettled: that render has finished - from then on
//every second counts toward the item's duration, even while a clock/date item refreshes its own
//changing content (CODE_REVIEW_V8.md #3: with HH:mm:ss every tick started a new job, so no second
//ever counted and the item only advanced via the stuck-item grace). Reset with
//rotationItemAcceptedSeconds.
bool rotationItemStarted = false;
bool rotationItemSettled = false;
//Set by checkRotation() from the current item's Alignment field ("" = inherit the default) and
//threaded through to showText() as an override.
String rotationCurrentAlignment = "";
//Same pattern as rotationCurrentAlignment above, for the per-item animation override -
//"" / -1 = inherit the global animationMode/animationStaggerMs.
String rotationCurrentAnimationMode = "";
int rotationCurrentAnimationStaggerMs = -1;
Timezone timezone;

//Neither timezone.now() nor timezone.tzTime(TIME_NOW, UTC_TIME) actually return UTC despite the
//name - both return local time. Reversing the offset by hand is the only way to get real UTC
//seconds, needed wherever this compares against a client-supplied Unix timestamp (scheduled
//messages, countdown/countup, rotation items).
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
//Which request currently "owns" otaChunkBuffer/otaChunkLen while its body is still streaming in.
//Two requests' bodies can stream in concurrently on this single-threaded event loop (AsyncTCP
//services connections in whatever order packets arrive), so a simple otaPendingPhase!=NONE check
//alone can't catch a second upload starting mid-transfer of a first. Claimed by whichever
//request's body arrives at index==0 first, checked by every later request's own index==0, and
//released once its owning request's outer handler runs (body fully received) or by the
//abandoned-upload watchdog if the connection vanishes before that.
AsyncWebServerRequest* otaChunkOwner = nullptr;
volatile size_t otaChunkLen = 0;
enum OtaPhase { OTA_PHASE_NONE, OTA_PHASE_BEGIN, OTA_PHASE_WRITE, OTA_PHASE_FINISH };
volatile OtaPhase otaPendingPhase = OTA_PHASE_NONE; //what loop() should do next
volatile OtaPhase otaCompletedPhase = OTA_PHASE_NONE;
volatile uint32_t otaBeginSize = 0;
//Bytes actually handed to Update.write() so far, tracked ourselves rather than trusting
//Update.progress() at finish time (see the finish-phase comparison below for why).
volatile size_t otaBytesWritten = 0;
//A single shared "reject this chunk" flag would let a losing concurrent request's body poison a
//legitimate owner's result (each side's outer handler could clear/set it on the other's behalf).
//Fixed by never letting a losing request's body touch shared state at all (see the per-fragment
//"otaChunkOwner != request" check below) and having the outer handler derive its response purely
//from "was I still the owner when my body finished" (a request-pointer comparison) plus
//statelessly re-checked facts (password, request->contentLength()).
volatile bool otaResultReady = false;
volatile bool otaResultSuccess = false;
String otaResultMessage = "";
volatile unsigned long otaLastActivityAt = 0; //watchdog for an abandoned upload (client vanished)
//90s: a client retrying a dropped chunk over flaky WiFi can genuinely take a while even though the
//upload itself is fine.
#define OTA_ABANDONED_TIMEOUT_MS 90000
PendingRequestSlot pendingFirmwareUploadSlot; //reused in turn by all three /api/firmware-* steps
#endif

//Whether a browser firmware upload is in progress - the multi-device health check/push must not
//contend with it (see their own comments). Always false when web OTA is compiled out
//(CODE_REVIEW_V7.md #6: those callers used to reference the OTA globals unconditionally).
bool webFirmwareUpdateInProgress() {
#if WEB_FIRMWARE_UPDATE_ENABLE == true
  return otaChunkBuffer != nullptr || otaPendingPhase != OTA_PHASE_NONE;
#else
  return false;
#endif
}
#if UNIT_BOOTLOADER_ENABLE == true
//Set when the upload couldn't be written/moved into place (open() or write() failure, or the
//final rename() failing) - see the temp-file+atomic-rename pattern on that handler. Safe to keep
//global/shared: only ever SET inside the "am I the current owner" gated section of the body
//handler, and only ever READ/cleared by the outer handler after confirming (via wasOwner) that
//THIS request still is that same owner - reset to false the moment a new request claims ownership,
//so a vanished previous owner's leftover true can never leak into the next one's result.
bool unitFirmwareUploadWriteFailed = false;
//Which request currently "owns" the shared static unitFirmwareFile/temp-file state while its body
//is still streaming in. Two AUTHENTICATED uploads (accidental double-submit, or two admin
//sessions) could otherwise share the same static File/temp-path with no per-request isolation, so
//their fragments interleave (A writes "AA", B starts and closes/replaces A's temp file, A's later
//"aa" fragment writes through B's now-reassigned handle). Claimed by whichever request's body
//arrives at index==0 first, checked by every later request's own index==0 (in addition to the
//password check), and released once its owning request's outer handler runs (body fully received)
//or immediately via onDisconnect if the connection vanishes before that.
//
//A single shared unitFirmwareUploadAuthFailed/BusyFailed flag would let any request's rejection
//(bad password, busy) poison the result a different, legitimate owner's outer handler then reads.
//Fixed by never touching shared state for a request that fails before claiming ownership, and by
//deriving the outer handler's response from stateless facts (password, re-checked directly) or
//request-scoped ones (wasOwner, a pointer comparison) instead of a flag any other request could
//also write to.
AsyncWebServerRequest* unitFirmwareUploadOwner = nullptr;
#endif
//Same reasoning/lifetime as unitFirmwareUploadWriteFailed above.
bool webFileUploadWriteFailed = false;
//Same ownership pattern as unitFirmwareUploadOwner above.
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
  //used to seed the clock RIGHT HERE, before initWiFi() and unconditional on its outcome, so the
  //clock is correct-ish independent of WiFi/NTP. Reuses ntpSyncApplyResult()'s offset math as-is.
  //The timezone RULE isn't resolved from the network yet - same brief "shows UTC until the async
  //timezone lookup catches up" window the NTP path already has (see startTimezoneLookup() below).
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

  //Anchors the safety ceiling for the unit-readiness wait below - captured here, at the earliest
  //practical point, so the 30s ceiling is measured from boot rather than from when the SCAN phase
  //happens to finish.
  unitReadinessStartedAt = millis();

  //The I2C scan/unit-readiness wait (unitReadinessPhase) is driven synchronously further down in
  //THIS function, right before initWiFi() - loop() cannot run until setup() returns, so deferring
  //it to loop() would still sit behind initWiFi()'s own blocking autoConnect() call regardless.
  SerialPrintln(F(""));
  SerialPrintln(F("#######################################################"));
  SerialPrintln(F("..............Split Flap Display Starting.............."));
  SerialPrintln(F("#######################################################"));
  debugStatus = "Starting";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);

  //Config load + crash-log persistence, unconditional and as early as possible, before initWiFi()
  //even runs - neither function has a WiFi dependency, and loadValuesFromFileSystem() is what
  //knows the previously configured deviceMode/inputText/rotationItems/alignment/etc., so the
  //previously configured mode can render (once units report ready, see unitReadinessPhase below)
  //as fast as possible regardless of WiFi outcome.
  debugStatus = "File System Init";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);
  initialiseFileSystem();
  loadValuesFromFileSystem();
  checkAndPersistCrashLog(); // see ServiceCrashLogFunctions.ino - must run after LittleFS mounts

  //Show the previously configured mode BEFORE WiFi: loop() (where the display update state
  //machine runs) cannot start until setup() returns, so nothing could ever render before
  //initWiFi()'s blocking autoConnect() call finished, however non-blocking the code after it is.
  //Deliberately not touching WiFiManager's own connect logic (a prior custom reconnect rewrite
  //made things worse and had to be reverted) - instead gets the unit-readiness wait and one real
  //display update to happen BEFORE initWiFi()'s blocking call, spun synchronously here since
  //nothing else in setup() is waiting on this yet. Safety ceiling below matches
  //UNIT_READINESS_MAX_WAIT_MS's own reasoning.
  //Apply the cached POSIX rule (if any) from the LAST successful timezoned.rop.nl lookup, before
  //rendering anything below - see cachedTimezonePosix's own comment. Skipped if empty (fresh
  //device); Clock/Date mode then falls back to showing UTC until the real network lookup resolves.
  if (cachedTimezonePosix != "") {
    timezone.setPosix(cachedTimezonePosix);
    SerialPrintln(F("DEBUG: Applied cached timezone POSIX rule (pre-WiFi): ") + cachedTimezonePosix);
  }

  debugStatus = "Pre-WiFi Display";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);
  while (unitReadinessPhase != UNIT_READINESS_DONE) {
    tickUnitReadiness();
    delay(10);
    yield();
  }
  performOneSecondDisplayTick();
  unsigned long preWifiDisplayPumpStart = millis();
  while (isDisplayJobBusy() && !millisReached(preWifiDisplayPumpStart + 20000)) {
    updateDisplayJob();
    delay(5);
    yield();
  }
  previousMillis = millis(); //avoid an immediate duplicate tick the moment loop() starts

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
    //makes events() a permanent no-op. Our own ServiceAsyncTimeSyncFunctions.ino (checkNtpSync())
    //takes over that job instead, ticked from loop() - a genuinely non-blocking replacement (the
    //original blocking DNS+UDP calls could crash the device under network stress).
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

    //Must come after loadValuesFromFileSystem() - needs timezoneSetting, populated from there.
    //Fire-and-forget rather than a blocking timezone.setLocation() call: checkTimezoneLookup()
    //(ticked every loop() tick) picks it up and applies it (timezone.setPosix()) within the first
    //couple of loop() iterations once the network's up - blocking setup() on a network round-trip
    //can itself trip the software watchdog under network stress. The first Clock/Date render
    //already happened before this (see the pre-WiFi display block above); with cachedTimezonePosix
    //populated (the common case) that render is already correct LOCAL time. Only a device that has
    //never once resolved its timezone still shows UTC until this lookup completes.
    startTimezoneLookup(timezoneSetting);
    bootTimezoneLookupPending = true; //see its own comment - lets loop() retry this specific attempt on failure
    SerialPrintln(F("DEBUG: Timezone lookup started (non-blocking) for: ") + timezoneSetting);
    
    //Debug mode was already loaded at the very top of setup() - just log the already-known value.
#if DEBUG_ENABLE == true
    SerialPrintln(F("DEBUG: Debug mode: ") + String(showDebugPage ? F("enabled") : F("disabled")) + F(" (controls startup page, error status, page load log, and serial log)"));
#endif

    //Web Server Endpoint configuration
    debugStatus = "Web Server Setup";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);

    //A filesystem-wide static route (`webServer.serveStatic("/", LittleFS, "/")`) was removed
    //entirely: the only real static asset is index.html(.gz), already served explicitly below
    //(GET "/") - the favicon is an embedded base64 data URI in that same HTML. The catch-all route
    //served no legitimate purpose beyond that and exposed every runtime config file in LittleFS
    //root unauthenticated, including the OTA password itself in plaintext.

    //Must stay the first handler - see RequestGuard.
    webServer.addHandler(new RequestGuard());

    //All API routes below live under a single "/api/" prefix - full endpoint reference in API.md.
    webServer.on("/", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      unsigned long pageRequestStart = millis();
      SerialPrintln(F("DEBUG: Request Home Page Received"));
      
#if DEBUG_ENABLE == true
      // If debug mode is enabled and we're still in debug mode, show debug page
      if (showDebugPage) {
        // Generate debug HTML page with live log
        IPAddress ip = WiFi.localIP();
        String html = "<!DOCTYPE html><html><head>";
        html += F("<title>Split Flap - Debug Mode</title>");
        html += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");
        html += F("<style>");
        html += F("body { font-family: Arial, sans-serif; margin: 20px; background: #1e1e1e; color: #d4d4d4; }");
        html += F("h1 { color: #4ec9b0; }");
        html += F(".log-container { background: #252526; border: 1px solid #3e3e42; padding: 15px; font-family: 'Courier New', monospace; font-size: 0.9em; white-space: pre-wrap; }");
        html += F(".log-entry { margin: 2px 0; }");
        html += F(".timestamp { color: #808080; margin-right: 10px; }");
        html += F(".debug { color: #4ec9b0; }");
        html += F(".error { color: #f48771; }");
        html += F(".warning { color: #dcdcaa; }");
        html += F("button { background: #0e639c; color: white; border: none; padding: 10px 20px; font-size: 16px; cursor: pointer; border-radius: 4px; margin-bottom: 20px; }");
        html += F("button:hover { background: #1177bb; }");
        html += F(".status { color: #4ec9b0; font-weight: bold; margin: 10px 0; }");
        html += F("</style>");
        html += F("</head><body>");
        html += F("<h1>Split Flap - Debug Mode</h1>");
        html += F("<button onclick='continueToNormal()'>Continue to Normal Mode</button>");
        html += F("<button onclick='loadDiag()'>Refresh Diagnostics</button>");
        html += "<div class='status'>Current Status: " + debugStatus + "</div>";
        html += F("<p>Debug mode is enabled. This page shows current crash log and heap diagnostics. When you continue to normal mode, you'll also see error status panel and page load debug log features.</p>");
        html += F("<div class='log-container' id='logContainer'>");
        html += F("<div style='color: #888;'>Loading diagnostics...</div>");
        html += F("</div>");
        html += F("<script>");
        //One-shot fetch of the diagnostic sources that exist (/api/crash-log, /api/diag?type=heap)
        //plus a manual refresh button - deliberately not an interval poll, to avoid steady-state cost.
        html += F("function loadDiag() {");
        html += F("  var container = document.getElementById('logContainer');");
        html += F("  container.innerHTML = '<div style=\"color: #888;\">Loading diagnostics...</div>';");
        html += F("  var xhrCrash = new XMLHttpRequest();");
        html += F("  xhrCrash.onreadystatechange = function() {");
        html += F("    if (this.readyState != 4) return;");
        html += F("    var crashHtml;");
        html += F("    if (this.status == 200) {");
        html += F("      var lines = this.responseText.split('\\n').filter(function(l) { return l.trim() != ''; });");
        html += F("      if (lines.length == 0) {");
        html += F("        crashHtml = '<div class=\"debug\">Crash log: empty</div>';");
        html += F("      } else {");
        html += F("        crashHtml = '<div class=\"error\">Crash log (' + lines.length + ' entr' + (lines.length == 1 ? 'y' : 'ies') + '):</div>';");
        html += F("        for (var i = 0; i < lines.length; i++) {");
        html += F("          crashHtml += '<div class=\"log-entry\">' + escapeHtml(lines[i]) + '</div>';");
        html += F("        }");
        html += F("      }");
        html += F("    } else {");
        html += F("      crashHtml = '<div class=\"error\">Error loading crash log: Status ' + this.status + '</div>';");
        html += F("    }");
        html += F("    document.getElementById('crashLogSection').innerHTML = crashHtml;");
        html += F("  };");
        html += F("  xhrCrash.onerror = function() {");
        html += F("    document.getElementById('crashLogSection').innerHTML = '<div class=\"error\">Network error loading crash log</div>';");
        html += F("  };");
        html += F("  xhrCrash.open('GET', '/api/crash-log', true);");
        //Both reads need a login since OW-08 - the header lets the session cookie count.
        html += F("  xhrCrash.setRequestHeader('X-Requested-With', 'splitflap');");
        html += F("  var xhrHeap = new XMLHttpRequest();");
        html += F("  xhrHeap.onreadystatechange = function() {");
        html += F("    if (this.readyState != 4) return;");
        html += F("    var heapHtml = this.status == 200");
        html += F("      ? '<div class=\"debug\">Heap: ' + escapeHtml(this.responseText) + '</div>'");
        html += F("      : '<div class=\"error\">Error loading heap diagnostics: Status ' + this.status + '</div>';");
        html += F("    document.getElementById('heapSection').innerHTML = heapHtml;");
        html += F("  };");
        html += F("  xhrHeap.onerror = function() {");
        html += F("    document.getElementById('heapSection').innerHTML = '<div class=\"error\">Network error loading heap diagnostics</div>';");
        html += F("  };");
        html += F("  xhrHeap.open('GET', '/api/diag?type=heap', true);");
        html += F("  xhrHeap.setRequestHeader('X-Requested-With', 'splitflap');");
        html += F("  container.innerHTML = '<div id=\"heapSection\">Loading...</div><div id=\"crashLogSection\"></div>';");
        html += F("  xhrCrash.send();");
        html += F("  xhrHeap.send();");
        html += F("}");
        html += F("function escapeHtml(text) {");
        html += F("  var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;', \"'\": '&#039;' };");
        html += F("  return text.replace(/[&<>\"']/g, function(m) { return map[m]; });");
        html += F("}");
        html += F("function continueToNormal() {");
        //Same sessionStorage keys as index.html's getAdminPassword() - same origin, so a password
        //already entered on the main page is reused here without asking again, and vice versa.
        html += F("  var password = '';");
        html += F("  if (sessionStorage.getItem('splitflapAdminPasswordKnown') === '1') {");
        html += F("    password = sessionStorage.getItem('splitflapAdminPassword') || '';");
        html += F("  } else {");
        html += F("    var entered = prompt('Administrator-Passwort (leer lassen, falls keines gesetzt ist):', '');");
        html += F("    if (entered !== null) {");
        html += F("      password = entered;");
        html += F("      sessionStorage.setItem('splitflapAdminPassword', entered);");
        html += F("      sessionStorage.setItem('splitflapAdminPasswordKnown', '1');");
        html += F("    }");
        html += F("  }");
        html += F("  var xhr = new XMLHttpRequest();");
        html += F("  xhr.onreadystatechange = function() {");
        html += F("    if (this.readyState == 4 && this.status == 200) {");
        html += F("      window.location.href = '/';");
        html += F("    } else if (this.readyState == 4 && this.status == 401) {");
        html += F("      sessionStorage.removeItem('splitflapAdminPassword');");
        html += F("      sessionStorage.removeItem('splitflapAdminPasswordKnown');");
        html += F("      alert('Falsches Administrator-Passwort.');");
        html += F("    }");
        html += F("  };");
        html += F("  xhr.open('POST', '/api/debug-mode', true);");
        html += F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');");
        html += F("  xhr.setRequestHeader('X-OTA-Password', password);");
        html += F("  xhr.send('enabled=false');");
        html += F("}");
        html += F("loadDiag();");
        html += F("</script>");
        html += F("</body></html>");
        
        request->send(200, "text/html", html);
        return;
      }
#endif
      //Defensive heap guard, moved here from the now-removed serveStatic() filter that used to
      //cover this same LittleFSImpl::open() call: a low-heap open() is one of two confirmed
      //abort() sites on this device.
      if (ESP.getFreeHeap() < 6000) {
        request->send(503, "text/plain", "Low memory, try again shortly");
        return;
      }

      //One page delivery at a time (BUILD_INFO Punkt 237): two AsyncFileResponses streaming
      //index.html.gz from LittleFS at the same time reproducibly hung the device (hardware
      //watchdog reset after ~8s) or crashed it in lfs_mlist_isopen() (LittleFS's list of open
      //files corrupted) - sequential deliveries, parallel non-file requests and uploads alone were
      //all stable. A second visitor during a delivery gets a tiny page that reloads itself after a
      //second. Released only when the delivering connection itself ends. A delivery still running
      //after 30s (very slow client) is closed for the next visitor - but that visitor waits for
      //the actual disconnect too, so two file responses never overlap (CODE_REVIEW_V8.md #2).
      if (pageDeliveryActive) {
        if (millis() - pageDeliveryStartedAt >= 30000 && pageDeliveryClient != nullptr) {
          pageDeliveryClient->close(); //deferred to its next poll; its onDisconnect then releases the guard and the file
        }
        AsyncWebServerResponse *busyResponse = request->beginResponse(503, "text/html",
          String(F("<!DOCTYPE html><meta http-equiv=\"refresh\" content=\"1\"><p>Einen Moment ...</p>")));
        busyResponse->addHeader("Retry-After", "1");
        request->send(busyResponse);
        return;
      }
      pageDeliveryActive = true;
      pageDeliveryStartedAt = millis();
      uint32_t deliveryGeneration = ++pageDeliveryGeneration;
      pageDeliveryClient = request->client();
      request->onDisconnect([deliveryGeneration]() {
        if (deliveryGeneration == pageDeliveryGeneration) {
          pageDeliveryActive = false;
          pageDeliveryClient = nullptr;
        }
      });

      //Actually stored gzip-compressed as "/index.html.gz" - AsyncFileResponse falls back to that
      //automatically and adds Content-Encoding: gzip itself.
      unsigned long beforeSend = millis();
      SerialPrintln(F("DEBUG: Serving index.html (took ") + String(beforeSend - pageRequestStart) + F("ms to get here)"));
      request->send(LittleFS, "/index.html", "text/html");
      SerialPrintln(F("DEBUG: index.html sent (total: ") + String(millis() - pageRequestStart) + F("ms)"));
    });

    webServer.on("/api/settings", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      //Reading needs the password too (security review 2026-10-12, OW-08), unless this category is public.
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;
      unsigned long settingsStart = millis();
      SerialPrintln(F("DEBUG: Request for Settings Received at ") + String(settingsStart) + F("ms"));
      
      // Set flag to prevent display updates during web request
      webRequestActive = true;
      
      // Build minimal response immediately (fast path)
      JsonDocument minimalDoc;
      minimalDoc["timezoneOffset"] = timezone.getOffset();
      minimalDoc["unitCount"] = unitsAmount;
      //Characters one display line really has - own units, plus reachable extensions on a primary in
      //extend mode. The web UI limits a rotation text item to this.
      minimalDoc["displayWidth"] = combinedUnitsAmount();
      minimalDoc["alignment"] = alignment;
      minimalDoc["flapSpeed"] = flapSpeed;
      minimalDoc["animationMode"] = animationMode;
      minimalDoc["animationStaggerMs"] = animationStaggerMs;
      minimalDoc["deviceMode"] = deviceMode;
      //inputText is a SHARED variable every mode's own per-second update (clock/date/countdown/
      //countup/rotation) also writes its rendered content into - only meaningful as "what the user
      //actually typed for Text mode" while deviceMode is really "text"; the client gates its use on
      //deviceMode too, so sending it unconditionally here is harmless.
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
      minimalDoc["deviceRole"] = deviceRole; //user-editable, see POST /api/device-role (multiple-units.md)
      minimalDoc["extensionLinkMode"] = extensionLinkMode; //same endpoint, optional 'linkMode' param
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
      //hard 5-connection TCP limit. The password itself is NEVER sent back - only whether one is
      //currently set, plus the (non-secret) per-category toggle states.
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

      //Scheduled messages used to be serialized right here and could crash the device (heap
      //corruption via nested JsonArray [i][field] indexing, plus a yield()-inside-handler stack
      //overflow). Moved to its own handler (GET /api/scheduled-messages) that mirrors
      //rotation-items below: no yield() in the loop, just the item count here.
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

    //Merging this with the GET handler above into one HTTP_GET|HTTP_POST registration (same
    //pattern as /api/wifi etc.) was reproducibly broken on real hardware: a genuine POST request
    //received the GET handler's JSON response instead of "OK", and invalid POST bodies got 200
    //instead of 400. Reverted to two separate registrations, verified working again.
    webServer.on("/api/settings", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      SerialPrintln(F("DEBUG: Request Post of Form Received"));

      bool submissionError = false;
      //Set if replaceRotationItemsFromJson() rejects invalid JSON. Kept separate from
      //submissionError above: unlike that flag (which aborts the whole request with 400 before
      //touching anything), the rest of this request still gets processed normally either way -
      //only the final response wording changes.
      bool rotationItemsUpdateRejected = false;
      //Set if a submitted scheduled message needs more lines than fit and could therefore never
      //actually be shown - checked at acceptance time, not just discovered (and then stuck
      //forever) once due. Same "reject, don't silently drop/accept" pattern as
      //rotationItemsUpdateRejected above.
      bool scheduledMessageRejected = false;
      //Distinct human-readable detail for scheduledMessageRejected, since it's set for two
      //different reasons (too long to render, or a past-due timestamp) that deserve different
      //explanations in the final response.
      String scheduledMessageRejectionReason = "";
      //Set if a direct (non-scheduled) text submission needs more lines than fit. Mutually
      //exclusive with scheduledMessageRejected (gated on the same newMessageScheduleEnabledValue
      //branch).
      bool directTextRejected = false;

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

        //Doesn't bump displayRenderGeneration, same reasoning as flapSpeed above: this only
        //affects the TIMING of future writes, not what content counts as already-shown/cached.
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
        //tab-gating reasoning as Countdown/Countup above: the hidden field is only ever
        //recomputed by collectRotationItemsForSubmit(), called only from the rotation tab's
        //onsubmit case - submitting from any other tab resubmits its stale/default "[]" and
        //would otherwise silently wipe an existing, loaded rotation list.
        if (newDeviceModeValue == DEVICE_MODE_ROTATION && hasNewRotationItems) {
          //Invalid rotation JSON no longer wipes the existing list, but the response below says
          //so too rather than unconditionally claiming success. The rest of this request
          //(scheduled messages, mode switch, etc.) still gets processed normally.
          //An EMPTY value isn't "nothing submitted" here (the web UI always sent at least "[]"): the
          //web server library assembles form values character by character and copies them twice,
          //and on a fragmented heap a value of a few KB silently arrives as "". Report that instead of
          //answering OK while keeping the old list (BUILD_INFO Punkt 235). The web UI now uses
          //POST /api/rotation-items instead.
          if (newRotationItemsJson.length() == 0) {
            SerialPrintln(F("Rotation items arrived empty (too large for the available memory) - rejected"));
            rotationItemsUpdateRejected = true;
          } else if (replaceRotationItemsFromJson(newRotationItemsJson.c_str())) {
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
          } else {
            int scheduleResult = addAndPersistScheduledMessage(newInputTextValue, newMessageScheduleDateTimeUnixValue, newMessageScheduleShowIndefinitely);
            if (scheduleResult == SCHEDULE_REJECTED) {
              //Either the timestamp is already due/in the past (can happen legitimately if a very
              //soon deadline passes between the browser prefilling the field and this request
              //being processed) or SCHEDULED_MESSAGES_MAX was already reached.
              SerialPrintln(F("Scheduled Message rejected: date/time is not in the future, or too many are already scheduled"));
              scheduledMessageRejected = true;
              scheduledMessageRejectionReason = "the scheduled date/time has already passed, or the maximum number of scheduled messages is already reached - it was NOT scheduled.";
            } else if (scheduleResult == SCHEDULE_OVER_BUDGET) {
              SerialPrintln(F("Scheduled Message rejected: stored text budget exceeded"));
              scheduledMessageRejected = true;
              scheduledMessageRejectionReason = String(F("all scheduled messages together would exceed ")) + String(STORED_TEXT_BUDGET) + F(" characters - delete or shorten some first. It was NOT scheduled.");
            } else if (scheduleResult == SCHEDULE_SAVE_FAILED) {
              SerialPrintln(F("Scheduled Message rejected: could not be saved"));
              scheduledMessageRejected = true;
              scheduledMessageRejectionReason = "the scheduled messages could not be saved (not enough memory - try fewer or shorter messages) - it was NOT scheduled.";
            } else {
              SerialPrintln(F("New Scheduled Message added"));
            }
          }
        }
        else {
          //What deviceMode will actually END UP as after this request, whether or not a new mode
          //was explicitly submitted - needed so the too-long-text check below applies whenever
          //Text mode is (or stays) active, not only on an explicit mode switch.
          String effectiveDeviceMode = (newDeviceModeValue != "") ? newDeviceModeValue : deviceMode;

          //Validated BEFORE deviceMode is assigned/persisted below (CODE_REVIEW_V6.md #6):
          //previously the mode was already switched and written to LittleFS before this check ever
          //ran, so a rejected too-long text left the device stuck in Text mode showing nothing new,
          //instead of leaving whichever mode was actually running beforehand untouched. A message
          //needing more than DisplayJob::MAX_DISPLAY_LINES lines must be rejected here too (same
          //check as the scheduled-message branch above) - otherwise it gets applied while
          //startDisplayJob() then refuses to ever actually render it, every single tick, forever.
          if (hasNewInputTextValue && effectiveDeviceMode == DEVICE_MODE_TEXT && isMessageTooLongToRender(newInputTextValue)) {
            SerialPrintln(F("Direct text rejected: needs more lines than fit to display"));
            directTextRejected = true;
          } else {
            //Only if device mode has changed
            if (newDeviceModeValue != "" && deviceMode != newDeviceModeValue) {
              deviceMode = newDeviceModeValue;

              //Always (re-)start a sequence from the beginning when switching into Rotation mode.
              if (deviceMode == DEVICE_MODE_ROTATION) {
                rotationCurrentIndex = 0;
                rotationItemAcceptedSeconds = 0;
                rotationItemElapsedRealSeconds = 0;
                rotationItemStarted = false;
                rotationItemSettled = false;
              }

              writeFile(LittleFS, deviceModePath, deviceMode.c_str());
              SerialPrintln(F("Device Mode Set: ") + deviceMode);
            }

            //Only if we are showing text
            if (hasNewInputTextValue && deviceMode == DEVICE_MODE_TEXT) {
              inputText = newInputTextValue;
            }
          }
        }

        //Re-sync the boot snapshot right after this request's writes, not just on the periodic
        //30s check - a snapshot only caught up by a background timer leaves a real, if narrow,
        //window where a confirmed-saved change is lost to a power cut before the next tick. A
        //no-op if nothing this request touched actually changed.
        checkBootSnapshotFreshness();

        //scheduledMessageRejected and directTextRejected can never both be true for the same
        //request (they're set in mutually exclusive if/else branches above) - safe to fold into
        //one combined reason.
        bool anyTextRejected = scheduledMessageRejected || directTextRejected;
        String textRejectionReason = scheduledMessageRejected
          ? scheduledMessageRejectionReason
          : "the text needs more lines than fit to display and was NOT applied - the previous mode/text is unchanged.";

        if (rotationItemsUpdateRejected && anyTextRejected) {
          request->send(400, "text/plain", String(F("OK, but the submitted rotation items were rejected (invalid JSON, a text item needs more lines than fit, all texts together over ")) + String(STORED_TEXT_BUDGET) + F(" characters, or not enough memory to save them) AND ") + textRejectionReason);
        } else if (rotationItemsUpdateRejected) {
          request->send(400, "text/plain", String(F("OK, but the submitted rotation items were rejected (invalid JSON, a text item needs more lines than fit to ever display, all texts together over ")) + String(STORED_TEXT_BUDGET) + F(" characters, or not enough memory to save them) and were NOT applied - the existing rotation list is unchanged."));
        } else if (anyTextRejected) {
          request->send(400, "text/plain", "OK, but " + textRejectionReason);
        } else {
          request->send(200, "text/plain", "OK");
        }
      }
    });

    //Own small handler, not sharing a stack frame with /api/settings - only fetched when the
    //Rotation tab is opened.
    //GET: the list, element by element - see sendJsonArrayChunked()'s comment for why.
    //POST: replaces the whole list from a raw JSON array body (Content-Type: application/json) -
    //collected into ONE buffer allocated at its final size, unlike a form field, which the web
    //server library grows character by character and copies twice (lists of a few KB got lost that
    //way, BUILD_INFO Punkt 235). One registration for both methods, same reason as /api/wifi's.
    webServer.on("/api/rotation-items", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return; //OW-08
        if (!ensureRotationItemsComplete()) {
          request->send(503, "text/plain", "Rotation items not fully loaded (low memory), try again shortly");
          return;
        }
        sendJsonArrayChunked(request, rotationItemCount, fillRotationItemJson, &rotationItemsGeneration);
        return;
      }
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;
      bool owner = rotationBodyOwner == request;
      if (owner) {
        rotationBodyOwner = nullptr;
      }
      if (!owner || rotationBodyBuffer == nullptr) {
        freeRotationBody(owner);
        request->send(rotationBodyTooLarge ? 413 : 400, "text/plain", rotationBodyTooLarge
          ? F("Body too large or not enough memory")
          : F("Expected a JSON array body (Content-Type: application/json)"));
        rotationBodyTooLarge = false;
        return;
      }
      bool applied = replaceRotationItemsFromJson(rotationBodyBuffer);
      freeRotationBody(true);
      if (!applied) {
        request->send(400, "text/plain", String(F("Rotation items rejected (invalid JSON, a text item needs more lines than fit, all texts together over ")) +
          String(STORED_TEXT_BUDGET) + F(" characters, or not enough memory to save them) - the existing list is unchanged."));
        return;
      }
      SerialPrintln(F("Rotation Items Updated via JSON body, count: ") + String(rotationItemCount));
      request->send(200, "text/plain", "OK");
    }, nullptr, [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        //Only one body at a time, only for a caller that may change settings, and bounded - so an
        //unauthenticated or oversized request never makes the device allocate anything.
        if (rotationBodyOwner != nullptr || request->method() != ASYNC_HTTP_POST ||
            !(isRequestAllowedWithoutPassword(accessControlSettingsPublic) || requestIsAuthenticated(request))) {
          return;
        }
        if (total > ROTATION_BODY_MAX) {
          rotationBodyTooLarge = true;
          return;
        }
        rotationBodyBuffer = (char*)malloc(total + 1);
        if (rotationBodyBuffer == nullptr) {
          rotationBodyTooLarge = true;
          return;
        }
        rotationBodyOwner = request;
        rotationBodyLength = 0;
        //A client that drops mid-body never reaches the request handler - release the buffer here
        //instead of keeping it (and this endpoint) blocked forever. After a normal response the
        //owner is already cleared, so this is a no-op then.
        request->onDisconnect([request]() {
          if (rotationBodyOwner == request) {
            rotationBodyOwner = nullptr;
            freeRotationBody(true);
          }
        });
      }
      if (rotationBodyOwner != request || rotationBodyBuffer == nullptr || rotationBodyLength + len > total) {
        return;
      }
      memcpy(rotationBodyBuffer + rotationBodyLength, data, len);
      rotationBodyLength += len;
      rotationBodyBuffer[rotationBodyLength] = '\0';
    });

    //Consolidates the four pure-diagnostic endpoints (former /api/health, /api/heap,
    ///api/tcp-status, /api/test) into one registered route, dispatched by ?type= - each
    //registered handler costs a small permanent heap allocation that's never freed, and this
    //device has very little room to spare.
    webServer.on("/api/diag", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      String type = request->hasParam("type") ? request->getParam("type")->value() : "";

      if (type == "health") {
        request->send(200, "text/plain", "Healthy");
        return;
      }
      //Everything but the health check needs the password (OW-08) - chip IDs, versions, network state.
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;

      if (type == "heap") {
        char buf[540]; //worst-case length checked by hand incl. the longest wifiDisconnectReason string, +100 for the temporary diag fields, +20 for iramFree
        //getFlashChipRealSize() = physical chip's own SPI ID (ground truth); getFlashChipSize() =
        //what the eesz board-menu choice assumes - a mismatch means the compiled layout is wrong.
        //rtc/rtcOsf: the only way to verify the optional DS3231's detection/trustworthiness
        //without physically observing the display. resetReason comes from
        //ESP.getResetInfoPtr()->reason, the SDK's own boot-time record of the true last reset
        //cause, independent of our crash reporter (which only captures genuine exceptions/panics
        //via custom_crash_callback() - a CLEAN reset such as external/brownout/hardware-watchdog
        //leaves no trace there). uptimeSec lets repeated external polls notice ANY reboot between
        //two checks (crashed or clean) by seeing a smaller uptime than expected.
        //wifiDisconnects/lastWifiDisconnectReason/lastWifiDisconnectSecAgo makes AUTH_EXPIRE/
        //ASSOC_EXPIRE disconnects observable over HTTP without a live serial capture.
        const char* lastDisconnectReasonForDiag = wifiDisconnectCount > 0 ? lastWifiDisconnectReason.c_str() : "none";
        long lastDisconnectSecAgo = wifiDisconnectCount > 0 ? (long)((millis() - lastWifiDisconnectAt) / 1000) : -1;
        //iramFree: second heap in IRAM (mmu=4816H, OTA buffers), 0 if the build has none.
        uint32_t iramFree = 0;
#ifdef UMM_HEAP_IRAM
        {
          HeapSelectIram iramHeap;
          iramFree = ESP.getFreeHeap();
        }
#endif
        snprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(F("free=%u frag=%u%% maxBlock=%u flashReal=%u flashConfigured=%u flashChipId=0x%08X espChipId=0x%08X sdk=%s core=%s rtc=%s rtcOsf=%u resetReason=%s uptimeSec=%lu wifiDisconnects=%lu lastWifiDisconnectReason=%s lastWifiDisconnectSecAgo=%ld minFreeHeap=%u minFreeHeapAtSec=%u minMaxBlock=%u freeHeapAtLastWifiDisconnect=%u iramFree=%u")),
          ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize(),
          ESP.getFlashChipRealSize(), ESP.getFlashChipSize(), ESP.getFlashChipId(), ESP.getChipId(),
          ESP.getSdkVersion(), ESP.getCoreVersion().c_str(),
          rtcPresent ? "present" : "absent", rtcOscillatorStopFlag ? 1 : 0,
          crashResetReasonText(ESP.getResetInfoPtr()->reason), millis() / 1000,
          wifiDisconnectCount, lastDisconnectReasonForDiag, lastDisconnectSecAgo,
          diagMinFreeHeap, diagMinFreeHeapAtSec, diagMinMaxBlock, diagFreeHeapAtLastWifiDisconnect, iramFree); //temporary diag, see diagMinFreeHeap
        request->send(200, "text/plain", buf);
        return;
      }

      if (type == "tcp") {
        //MEMP_NUM_TCP_PCB=5 in this board's lwIP build is NOT a hard cap on simultaneous
        //connections, corrected 2026-09-12 (Punkt 207 investigation) - MEMP_MEM_MALLOC=1 means
        //these PCBs come from malloc(), not a fixed 5-slot pool, so more than 5 can genuinely be
        //in flight at once (verified live: a burst of 15 simultaneous connection attempts got
        //well past 5 before the device crashed elsewhere - see connectionGateRejectedCount
        //below for the actual defense now in place against that).
        int activeCount = 0;
        for (struct tcp_pcb *p = tcp_active_pcbs; p != NULL; p = p->next) activeCount++;
        int timeWaitCount = 0;
        for (struct tcp_pcb *p = tcp_tw_pcbs; p != NULL; p = p->next) timeWaitCount++;
        int boundCount = 0;
        for (struct tcp_pcb *p = tcp_bound_pcbs; p != NULL; p = p->next) boundCount++;
        int listenCount = 0;
        for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p != NULL; p = p->next) listenCount++;

        char buf[160];
        snprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(F("active=%d timeWait=%d bound=%d listen=%d gateRejected=%lu")),
          activeCount, timeWaitCount, boundCount, listenCount, connectionGateRejectedCount);
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
    // GET+DELETE merged into one registration (heap-saving pattern established for /api/wifi).
    // DELETE (not GET) clears it, to avoid a CSRF-via-GET state change.
    webServer.on("/api/crash-log", ASYNC_HTTP_GET | ASYNC_HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return; //OW-08
        if (!LittleFS.exists(CRASH_LOG_PATH)) {
          request->send(200, "application/jsonl", "");
          return;
        }
        request->send(LittleFS, CRASH_LOG_PATH, "application/jsonl");
        return;
      }

      // request->method() == HTTP_DELETE
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;
      if (LittleFS.exists(CRASH_LOG_PATH)) {
        LittleFS.remove(CRASH_LOG_PATH);
      }
      request->send(200, "text/plain", "Crash log cleared");
    });

    // I2C bus scanner endpoint for diagnostics
    webServer.on("/api/i2c-scan", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return; //OW-08 - drives the I2C bus
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
    // the FIRST registered handler that matches - registering the more specific paths first
    // avoids "/api/units" silently swallowing requests to "/api/units/firmware*".

    // Admin: upload a compiled Unit.ino firmware image (raw AVR .bin, NOT ESPMaster's own) to be
    // pushed to a unit via /api/units/firmware-push. Requires X-OTA-Password, same as /api/firmware.
    webServer.on("/api/units/firmware", ASYNC_HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Capture ownership BEFORE releasing it - the one reliable, request-scoped (pointer
        //comparison) way to know whether THIS request's body ever won the shared static file
        //state, independent of whatever any other concurrent request's body did in the meantime.
        bool wasOwner = (unitFirmwareUploadOwner == request);
        if (wasOwner) {
          unitFirmwareUploadOwner = nullptr;
        }
        //Password is stateless and independently re-checked here, same as checkOtaPassword() for
        //the ESPMaster OTA endpoints above - never gated behind a shared flag another request's
        //callback could set or clear.
        if (String(otaPassword) != "") {
          if (!requestIsAuthenticated(request)) { //password header or web UI login session
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
          //Check the password FIRST, before touching any previous-upload state at all - an
          //unauthenticated POST hitting index==0 must not be able to interrupt/corrupt a
          //different, still-legitimately-in-progress upload just by arriving first. On failure,
          //just return WITHOUT touching any shared state - the outer handler re-checks the
          //password itself and never trusts a shared flag for it.
          if (String(otaPassword) != "") {
            if (!requestIsAuthenticated(request)) { //password header or web UI login session
              SerialPrintln(F("Unit firmware upload rejected: invalid password"));
              return;
            }
          }

          //Reject a second, DIFFERENT authenticated request trying to start while this channel's
          //shared static state is already claimed by another still-in-flight upload - must come
          //before any cleanup/touching of unitFirmwareFile below. Again, just return - no shared
          //flag to set.
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
          //vanished mid-transfer, "final" never fired for it and this handle was never closed
          //(a dangling handle measurably costs heap/fragmentation across the next upload) - close
          //it defensively before reusing the static variable for this attempt.
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
          //leave the previously-stored (working) firmware truncated or corrupted.
          unitFirmwareFile = LittleFS.open(unitFirmwareTmpPath, "w");
          if (!unitFirmwareFile) {
            SerialPrintln(F("Unit firmware upload: failed to open ") + String(unitFirmwareTmpPath) + F(" for writing"));
            unitFirmwareUploadWriteFailed = true;
          }
        }

        //A per-fragment ownership check, not a shared "was rejected" flag - without this, a LATER
        //fragment (index>0) of a rejected request would fall straight through to
        //unitFirmwareFile.write() below, writing its bytes into the WINNING request's file.
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
          //Authorized again right before the file replaces the stored one - a password change or
          //revoked session during the transfer must not still let it through (CR-03).
          if (!unitFirmwareUploadWriteFailed && String(otaPassword) != "" && !requestIsAuthenticated(request)) {
            SerialPrintln(F("Unit firmware upload: no longer authorized at the end - discarding it"));
            unitFirmwareUploadWriteFailed = true;
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
    //pattern established for /api/wifi etc.). Deliberately its own path, not merged into
    //"/api/units/firmware" - that's a multipart upload registration (different ESPAsyncWebServer
    //callback signature), and that upload route would otherwise intercept a sub-path anyway.
    webServer.on("/api/units/firmware-push", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
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

      //Bounds against UNITS_AMOUNT_MAX (the structural 6-bit-DIP address ceiling), not the cached
      //unitsAmount - unlike every other unit-id check in this file, this endpoint's whole point is
      //to reach a unit that's already stuck in the bootloader, which by definition doesn't answer
      //at its app address and drops out of unitsAmount on the next scan. scanForBootloaderAddress()
      //below is the real safety net regardless (refuses unless exactly one unit answers as a
      //bootloader).
      int unitId = idValue.toInt();
      if (unitId < 0 || unitId >= UNITS_AMOUNT_MAX) {
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(UNITS_AMOUNT_MAX - 1));
        return;
      }

      if (action == "push") {
        if (String(otaPassword) != "") {
          if (!requestIsAuthenticated(request)) { //password header or web UI login session
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
        //Same always-required password check as "push" above, NOT the toggleable
        //accessControlUnitsPublic - this calls requestUnitEnterBootloader(), which genuinely
        //reboots the unit's running app exactly like "push" does. A read-only-sounding "probe"
        //name doesn't make it a safe, unauthenticated diagnostic.
        if (String(otaPassword) != "") {
          if (!requestIsAuthenticated(request)) { //password header or web UI login session
            SerialPrintln(F("Unit bootloader probe rejected: invalid password"));
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }

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
    // (heap-saving pattern established for /api/wifi etc.) - the two branches stay structurally
    // small and independent (unlike the failed /api/settings merge attempt), so the same
    // size-related risk doesn't apply here.
    webServer.on("/api/units", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        //Stays readable without the password (OW-08 exception): a primary polls its extensions'
        //unit count here and sends no credentials for that.
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
        if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return; //OW-08 - drives the I2C bus
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
    webServer.on("/api/web-file", ASYNC_HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Capture ownership BEFORE releasing it - same request-scoped derivation as
        ///api/units/firmware above.
        bool wasOwner = (webFileUploadOwner == request);
        if (wasOwner) {
          webFileUploadOwner = nullptr;
        }
        //Password is stateless and independently re-checked here - never gated behind a shared
        //flag another request's callback could set or clear.
        if (String(otaPassword) != "") {
          if (!requestIsAuthenticated(request)) { //password header or web UI login session
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }
        if (!wasOwner) {
          //Distinguish "this request's OWN filename was invalid" from "genuinely lost the
          //ownership race to a concurrent upload" - re-derive the uploaded filename statelessly
          //from this request's own file param (ESPAsyncWebServer adds one with isFile()==true
          //during multipart parsing) rather than relying on any shared flag, to avoid a double
          //response (400 from the body handler AND a generic 503 from here).
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
          //reasoning as /api/units/firmware above: an unauthenticated POST hitting index==0 must
          //not be able to close/delete a different, still-legitimately-in-progress upload's file
          //just by arriving first. On failure, just return - no shared flag to set, the outer
          //handler re-checks the password itself.
          if (String(otaPassword) != "") {
            if (!requestIsAuthenticated(request)) { //password header or web UI login session
              SerialPrintln(F("Web file upload rejected: invalid password"));
              return;
            }
          }

          //Reject a second, DIFFERENT authenticated request trying to start while this channel's
          //shared static state is already claimed by another still-in-flight upload (same
          //underlying issue as /api/units/firmware above) - must come before any cleanup/touching
          //of webUploadFile below. Again, just return - no shared flag.
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
          //ownership again immediately (ownership alone fully covers it, since a request that
          //never keeps ownership automatically fails every later fragment's
          //"webFileUploadOwner != request" check below too). Does NOT send a response itself -
          //the separate outer (onRequest) handler above also runs once the body finishes
          //arriving, and re-derives "was my own filename invalid" itself to send the one reply.
          if (filename != "index.html.gz") {
            SerialPrintln(F("Web file upload rejected: filename not in allowlist: ") + filename);
            webFileUploadOwner = nullptr;
            return;
          }

          SerialPrintln(F("DEBUG: Web file upload starting: ") + filename);
          //Same temp-file + atomic rename() pattern as /api/units/firmware above - an
          //aborted/failed upload must never leave the live web UI file truncated. The stale-
          //"/index.html"-removal below happens right before the swap too, so a failed upload
          //can't leave neither file in place.
          webUploadTmpPath = "/" + filename + ".tmp";
          webUploadFile = LittleFS.open(webUploadTmpPath, "w");
          if (!webUploadFile) {
            SerialPrintln(F("Web file upload: failed to open ") + webUploadTmpPath + F(" for writing"));
            webFileUploadWriteFailed = true;
          }
        }

        //Per-fragment ownership check, not a sticky flag - see the identical /api/units/firmware
        //comment above for why this must compare live ownership on every fragment.
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
          //Authorized again right before the swap - see /api/units/firmware (CR-03).
          if (!webFileUploadWriteFailed && String(otaPassword) != "" && !requestIsAuthenticated(request)) {
            SerialPrintln(F("Web file upload: no longer authorized at the end - discarding it"));
            webFileUploadWriteFailed = true;
          }
          if (webFileUploadWriteFailed) {
            LittleFS.remove(webUploadTmpPath); //don't leave a corrupt temp file lying around
            SerialPrintln(F("DEBUG: Web file upload failed, previous file (if any) left untouched"));
          } else {
            //Rename the new file into place FIRST, only remove the old uncompressed "/index.html"
            //AFTER that's confirmed - if the rename failed first (e.g. a power cut between the
            //two removal-then-rename steps), the previously-working uncompressed UI would already
            //be gone while the new ".gz" never landed. The two paths never collide with each
            //other, so there's no need to clear the old one first.
            if (LittleFS.rename(webUploadTmpPath, "/" + filename)) {
              SerialPrintln(F("DEBUG: Web file upload finished: ") + filename + F(", total bytes: ") + String(index + len));
              //A leftover uncompressed "/index.html" would take precedence over "/index.html.gz" -
              //only remove it now that the new file is confirmed safely in place.
              if (LittleFS.exists("/index.html")) {
                LittleFS.remove("/index.html");
              }
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
      return requestIsAuthenticated(request); //password header or web UI login session
    };

    // Step 1: tell the ESP how big the firmware is and let it erase/prepare the flash region.
    // Body: size=<bytes> (form-encoded).
    webServer.on("/api/firmware-start", ASYNC_HTTP_POST, [checkOtaPassword](AsyncWebServerRequest * request) {
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
    webServer.on("/api/firmware-chunk", ASYNC_HTTP_POST,
      [checkOtaPassword](AsyncWebServerRequest * request) {
        //Capture ownership BEFORE releasing it - this reliably tells THIS specific outer-handler
        //invocation whether its own body ever got to claim the shared buffer, independent of
        //anything any other concurrent request's body did (see otaChunkOwner's declaration comment).
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
        //this file.
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
          //otaChunkBuffer. On failure, just return WITHOUT touching any shared state - this
          //request simply never becomes otaChunkOwner, which the per-fragment check below and the
          //outer handler's wasOwner re-derivation are both built to handle on their own.
          if (!checkOtaPassword(request)) {
            return;
          }
          //Reject an overlapping chunk's body HERE too, before touching otaChunkLen/otaChunkBuffer
          //at all - not just in the outer request handler above, which only runs once this ENTIRE
          //body has already streamed in and overwritten them.
          if (otaPendingPhase != OTA_PHASE_NONE) {
            return;
          }
          //Reject a SECOND body that's ALSO still mid-transfer, not just one whose outer handler
          //has already run - the otaPendingPhase check above only ever changes once a body is
          //FULLY received, so two bodies that are BOTH still streaming in concurrently (fragments
          //interleaved by AsyncTCP servicing multiple open connections) could otherwise pass both
          //checks and corrupt each other - see otaChunkOwner's own declaration comment.
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
        //Per-fragment ownership check (not a shared "rejected" flag) - a request that never
        //claimed ownership above, or that has since lost it (watchdog timeout, or its own
        //overflow guard below), always fails this and is silently dropped.
        if (otaChunkOwner != request) {
          return;
        }
        //Re-check on EVERY fragment, not just index==0: the abandoned-upload watchdog (loop(),
        //see otaLastActivityAt below) could otherwise free/null otaChunkBuffer mid-transfer,
        //between two fragments of one slow-arriving body, since otaLastActivityAt was only
        //updated once the FULL chunk had completed. Updating it on every fragment also means a
        //genuinely still-arriving, slow-but-active body no longer looks "abandoned" in the first
        //place.
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
    webServer.on("/api/firmware-finish", ASYNC_HTTP_POST, [checkOtaPassword](AsyncWebServerRequest * request) {
      if (!checkOtaPassword(request)) {
        request->send(401, "text/plain", "Invalid password");
        return;
      }
      if (otaChunkBuffer == nullptr) {
        request->send(400, "text/plain", "No firmware upload in progress");
        return;
      }
      //Same "already busy" guard as /api/firmware-chunk above.
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
    webServer.on("/api/debug-mode", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
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
    webServer.on("/api/reboot", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;

      SerialPrintln(F("Request to Reboot Received"));
      
      //Create HTML page to explain the system is rebooting
      IPAddress ip = WiFi.localIP();
      
      String html = "<div style='text-align:center'>";
      html += F("<font face='arial'><h1>Split Flap - Rebooting</h1>");
      html += F("<p>Reboot is pending now...<p>");
      html += F("<p>This can take anywhere between 10-20 seconds<p>");
      html += F("<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>");
      html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
      html += F("</font>");
      html += F("</div>");
      
      request->send(200, "text/html", html);
      checkBootSnapshotFreshness(); //see its own comment further down - keep the snapshot fresh before every intentional reboot
      isPendingReboot = true;
    });

    //Mirrors GET /api/rotation-items exactly (own small JsonDocument,
    //.to<JsonArray>()+.add<JsonObject>(), deliberately no yield() in the loop - a yield() here
    //previously overflowed the AsyncWebServer callback's small stack). Deliberately a separate
    //registration, not merged with DELETE below.
    webServer.on("/api/scheduled-messages", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return; //OW-08
      //An incomplete list (see scheduledMessagesComplete) isn't served as if it were the whole
      //schedule - checkScheduledMessages() keeps retrying the load from loop().
      if (!scheduledMessagesComplete) {
        request->send(503, "text/plain", "Scheduled messages not fully loaded (low memory), try again shortly");
        return;
      }
      //Element by element - see sendJsonArrayChunked()'s comment for why.
      sendJsonArrayChunked(request, scheduledMessages.size(), fillScheduledMessageJson, &scheduledMessagesGeneration);
    });

    webServer.on("/api/scheduled-messages", ASYNC_HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      SerialPrintln(F("Request to Remove Scheduled Message Received"));
      
      if (request->hasParam(PARAM_ID)) {
        bool removedScheduledMessage = false;
        String idValue = request->getParam(PARAM_ID)->value();

        if (isNumber(idValue)) {
          long parsedIdValue = atol(idValue.c_str());
          int removeResult = removeScheduledMessage(parsedIdValue);

          if (removeResult == SCHEDULE_REMOVED) {
            request->send(202, "text/plain", "Removed");
          }
          else if (removeResult == SCHEDULE_SAVE_FAILED) {
            request->send(500, "text/plain", "Could not save the change (not enough memory) - the message is still scheduled.");
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
    webServer.on("/api/text", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
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

      //Checked up front, before either branch below - the permanent branch never looks at
      //showText()'s result at all, so a too-long text would otherwise silently get "OK" back and
      //be retried forever, uselessly, by the normal 1s mode-dispatch loop.
      if (isMessageTooLongToRender(newText)) {
        request->send(400, "text/plain", "text needs more lines than fit to display - shorten it or add fewer line breaks");
        return;
      }

      if (durationSeconds > 0) {
        //Temporary: deviceMode/inputText are deliberately left untouched, same pattern
        //checkScheduledMessages() already uses for a non-indefinite scheduled message - once the
        //hold time elapses, the next 1s tick's normal mode loop redisplays whatever deviceMode
        //currently is on its own. Unlike the permanent branch below, no other code path will ever
        //retry this specific call - a busy display here means the message is genuinely lost, so
        //report that honestly instead of claiming success. The "too many lines" reason is already
        //ruled out above - a false here can only mean a busy display.
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
        checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after a confirmed deviceMode change, not just on the periodic 30s check
      }

      request->send(200, "text/plain", "OK");
    });

    //Extension-side receiver for a primary's already-sliced, already-aligned display segment plus
    //animation context - see multiple-units.md and ServiceExtensionFunctions.ino's own comments
    //(pushSegmentToExtension()/applyExtensionDisplaySegment()). Plain form-urlencoded POST, same
    //as every other endpoint in this file.
    webServer.on("/api/extension-display", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (String(otaPassword) != "") {
        String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
        int passwordCheck = checkAdminPassword(providedPassword);
        if (passwordCheck != PASSWORD_OK) {
          request->send(passwordCheck == PASSWORD_THROTTLED ? 429 : 401, "text/plain", "Invalid password");
          return;
        }
      }
      if (deviceRole != DEVICE_ROLE_EXTENSION) {
        request->send(400, "text/plain", "This device is not configured as an extension (System -> deviceRole)");
        return;
      }
      if (!request->hasParam("segment", true)) {
        request->send(400, "text/plain", "'segment' is required");
        return;
      }
      String segment = request->getParam("segment", true)->value();
      if ((int)segment.length() > UNITS_AMOUNT_MAX) {
        request->send(400, "text/plain", "Segment too long");
        return;
      }
      String animationMode = request->hasParam("animationMode", true) ? request->getParam("animationMode", true)->value() : ANIMATION_MODE_SIMULTANEOUS;
      String staggerMsRaw = request->hasParam("staggerMs", true) ? request->getParam("staggerMs", true)->value() : "0";
      String globalOffsetRaw = request->hasParam("globalOffset", true) ? request->getParam("globalOffset", true)->value() : "0";
      String combinedWidthRaw = request->hasParam("combinedWidth", true) ? request->getParam("combinedWidth", true)->value() : String(unitsAmount);
      //Optional (older primaries don't send it) - how far the primary's line already is, see
      //pushSegmentToExtension().
      String startOffsetMsRaw = request->hasParam("startOffsetMs", true) ? request->getParam("startOffsetMs", true)->value() : "0";
      //Same bounds the primary itself is held to (CODE_REVIEW_V7.md #3) - an unchecked staggerMs or
      //width could otherwise schedule a unit's start hours into the future.
      if (!isValidAnimationMode(animationMode) || !isNumber(staggerMsRaw) || !isNumber(globalOffsetRaw) || !isNumber(combinedWidthRaw) || !isNumber(startOffsetMsRaw)) {
        request->send(400, "text/plain", "Invalid animationMode/staggerMs/globalOffset/combinedWidth/startOffsetMs");
        return;
      }
      long staggerMs = staggerMsRaw.toInt();
      long globalOffset = globalOffsetRaw.toInt();
      long combinedWidth = combinedWidthRaw.toInt();
      long startOffsetMs = startOffsetMsRaw.toInt();
      if (staggerMs < 0 || staggerMs > ANIMATION_STAGGER_MS_MAX ||
          combinedWidth < 1 || combinedWidth > UNITS_AMOUNT_MAX * (EXTENSION_DEVICES_MAX + 1) ||
          globalOffset < 0 || globalOffset >= combinedWidth || startOffsetMs < 0) {
        request->send(400, "text/plain", "staggerMs/globalOffset/combinedWidth/startOffsetMs out of range");
        return;
      }
      //Clamped, not rejected: a late push (e.g. 0ms stagger, slow network) is still valid - every
      //unit is simply already due. Anything past the longest possible stagger means the same.
      startOffsetMs = min(startOffsetMs, staggerMs * combinedWidth);

      //Queued for loop() and acknowledged right away - see pendingExtensionDisplayQueued's comment.
      pendingExtensionDisplaySegment = segment;
      pendingExtensionDisplayAnimationMode = animationMode;
      pendingExtensionDisplayStaggerMs = staggerMs;
      pendingExtensionDisplayGlobalOffset = globalOffset;
      pendingExtensionDisplayCombinedWidth = combinedWidth;
      pendingExtensionDisplayStartOffsetMs = startOffsetMs;
      pendingExtensionDisplayQueued = true;
      request->send(200, "text/plain", "OK");
    });

    //Primary-side extension host list (multiple-units.md) - GET returns the configured hosts plus
    //their last-known health-check status, POST replaces the whole list (same "client always
    //submits the complete list" convention as /api/rotation-items).
    webServer.on("/api/extension-devices", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      //Changing the list always needs the password, whatever "settings public" says: a primary
      //sends its admin password to every listed host, so a public write would hand it to any
      //host an outsider adds (security review 2026-10-12, CR-01).
      if (rejectIfPasswordRequired(request, request->method() == ASYNC_HTTP_GET && accessControlSettingsPublic)) return;

      if (request->method() == ASYNC_HTTP_GET) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < extensionDeviceCount; i++) {
          JsonObject entry = arr.add<JsonObject>();
          entry["host"] = extensionDevices[i].Host;
          entry["reachable"] = extensionDevices[i].Reachable;
          entry["unitsAmount"] = extensionDevices[i].UnitsAmount;
          entry["lastCheckedSecAgo"] = extensionDevices[i].LastCheckedAt == 0 ? -1 : (long)((millis() - extensionDevices[i].LastCheckedAt) / 1000);
        }
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // HTTP_POST
      if (!request->hasParam("hosts", true)) {
        request->send(400, "text/plain", "'hosts' (JSON array of hostnames/IPs) is required");
        return;
      }
      if (!replaceExtensionDevicesFromJson(request->getParam("hosts", true)->value())) {
        request->send(400, "text/plain", F("'hosts' must be a JSON array of IP addresses or hostnames, optionally with :port"));
        return;
      }
      request->send(200, "text/plain", "OK");
    });

    //Device role (multiple-units.md) - own small endpoint, same pattern as mDNS hostname/quiet
    //hours/MQTT rather than folding a fourth, independent concern into the already-large
    ///api/settings handler.
    webServer.on("/api/device-role", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;
      if (!request->hasParam("role", true)) {
        request->send(400, "text/plain", "'role' is required");
        return;
      }
      String newRole = request->getParam("role", true)->value();
      if (newRole != DEVICE_ROLE_STANDALONE && newRole != DEVICE_ROLE_PRIMARY && newRole != DEVICE_ROLE_EXTENSION) {
        request->send(400, "text/plain", "'role' must be standalone, primary, or extension");
        return;
      }
      //Optional - omitting it keeps the current link mode (older clients only ever send 'role').
      String newLinkMode = request->hasParam("linkMode", true) ? request->getParam("linkMode", true)->value() : extensionLinkMode;
      if (newLinkMode != EXTENSION_LINK_MODE_EXTEND && newLinkMode != EXTENSION_LINK_MODE_MIRROR) {
        request->send(400, "text/plain", "'linkMode' must be extend or mirror");
        return;
      }
      bool changed = false;
      if (deviceRole != newRole) {
        deviceRole = newRole;
        writeFile(LittleFS, deviceRolePath, deviceRole.c_str());
        pendingDisplayControllersCancel = true;
        changed = true;
      }
      if (extensionLinkMode != newLinkMode) {
        extensionLinkMode = newLinkMode;
        writeFile(LittleFS, extensionLinkModePath, extensionLinkMode.c_str());
        changed = true;
      }
      if (changed) {
        checkBootSnapshotFreshness();
        //combinedUnitsAmount() changes the moment this device stops/starts counting extensions,
        //and a link-mode switch changes what every extension gets sent - see
        //displayRenderGeneration's own comment (CODE_REVIEW_V6.md #3).
        displayRenderGeneration++;
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
    webServer.on("/api/wifi", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        //Gated same as every POST action on this endpoint - these GET polling reads would
        //otherwise leak nearby-network scan results and the device's own currently-connected
        //SSID/IP to anyone unauthenticated on the LAN. The same browser/session that started a
        //scan or connect via the (already-gated) POST action already has the password.
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
        html += F("<font face='arial'><h1>Split Flap - Resetting WiFi</h1>");
        html += F("<p>WiFi Settings have been erased. Device will now reboot...<p>");
        html += F("<p>You will now be able to connect to this device in AP mode to configure the WiFi once more<p>");
        html += F("<p>You can go to the main home page after this time by clicking the button below or going to '/'.</p>");
        html += "<p><a href=\"http://" + ip.toString() + "\">Home</a></p>";
        html += F("</font>");
        html += F("</div>");

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
        //Shares this endpoint's registration rather than getting a dedicated route - each
        //registered handler costs a small permanent heap allocation that's never freed. Gated by
        //the same accessControlWifiPublic as every other action on this route - the hostname
        //isn't a secret, but changing it is a device-state change like the others here.
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

        checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after this confirmed change, not just on the periodic 30s check
        request->send(200, "text/plain", "Hostname updated");
        return;
      }

      request->send(400, "text/plain", "Unknown 'action' - use reset, scan, connect, or set-hostname");
    });
#endif

    //This endpoint has no GET - a password must never be returned in plaintext. The per-category
    //access-control toggle STATES (not the password) are public, non-secret info and are read via
    //GET /api/settings instead (see minimalDoc there).
    //
    //POST changes the password and/or the access-control toggles together, always gated behind
    //the CURRENT password matching - this is the one place in the whole access-control system
    //that is never itself toggleable-public (see accessControlTextPublic's comment in
    //ESPMaster.ino), otherwise a category opened via "settings public" could reopen every other
    //category including firmware/flash through this same endpoint.
    //Web UI login (see ServiceAccessControlFunctions.ino's session comment). GET /api/session is
    //public: it only says whether the page should show its login dialog.
    webServer.on("/api/session", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      String json = String(F("{\"loginRequired\":")) + (isLoginRequired() ? F("true") : F("false")) +
        F(",\"loggedIn\":") + (String(otaPassword) != "" && requestIsAuthenticated(request) ? F("true") : F("false")) + F("}");
      request->send(200, "application/json", json);
    });

    webServer.on("/api/login", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      //Same CSRF reasoning as requestIsAuthenticated(): only the web UI itself logs in this way.
      if (!request->hasHeader("X-Requested-With") || request->getHeader("X-Requested-With")->value() != "splitflap") {
        request->send(403, "text/plain", "Missing X-Requested-With header");
        return;
      }
      if (String(otaPassword) == "") {
        request->send(200, "text/plain", "No password set");
        return;
      }
      //Slows down guessing - one wrong attempt anywhere blocks further attempts for 2 seconds.
      String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
      int passwordCheck = checkAdminPassword(password);
      if (passwordCheck == PASSWORD_THROTTLED) {
        request->send(429, "text/plain", "Too many attempts, wait a moment");
        return;
      }
      if (passwordCheck == PASSWORD_WRONG) {
        SerialPrintln(F("Login rejected: wrong password"));
        request->send(401, "text/plain", "Wrong password");
        return;
      }
      String token = createLoginSession();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "OK");
      response->addHeader("Set-Cookie", String(F(LOGIN_SESSION_COOKIE "=")) + token + F("; Max-Age=") + String(LOGIN_SESSION_COOKIE_MAX_AGE) + F("; Path=/; HttpOnly; SameSite=Strict"));
      request->send(response);
    });

    webServer.on("/api/logout", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      endLoginSession(request);
      //Saved before answering - see /api/ota-password (CR-02). The cookie is cleared either way.
      bool saved = saveLoginSessionsNow();
      AsyncWebServerResponse *response = request->beginResponse(saved ? 200 : 500, "text/plain", saved ? "OK" : "Logout could not be saved");
      response->addHeader("Set-Cookie", F(LOGIN_SESSION_COOKIE "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict"));
      request->send(response);
    });

    webServer.on("/api/ota-password", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (!request->hasParam("currentPassword", true)) {
        request->send(400, "text/plain", "'currentPassword' is required");
        return;
      }

      String currentPasswordValue = request->getParam("currentPassword", true)->value();
      int passwordCheck = checkAdminPassword(currentPasswordValue);
      if (passwordCheck == PASSWORD_THROTTLED) {
        request->send(429, "text/plain", "Too many attempts, wait a moment");
        return;
      }
      if (passwordCheck == PASSWORD_WRONG) {
        SerialPrintln(F("OTA password/access-control change rejected: current password incorrect"));
        request->send(401, "text/plain", "Current password is incorrect");
        return;
      }

      //newPassword is optional and MAY be empty - an empty value deliberately disables password
      //protection globally (CODE_REVIEW.md #29's "muss komplett deaktivierbar sein" requirement).
      //Omitting the param entirely (not present at all) leaves the password unchanged, so a
      //request that only wants to update the access-control toggles doesn't have to resend it.
      if (request->hasParam("newPassword", true)) {
        //Empty still switches protection off; anything else needs 8+ characters (security review
        //2026-10-12, OW-04). Checked before any session is revoked.
        size_t newPasswordLength = request->getParam("newPassword", true)->value().length();
        if (newPasswordLength > 0 && newPasswordLength < 8) {
          request->send(400, "text/plain", F("New password must be at least 8 characters (or empty to switch protection off)"));
          return;
        }
        //No session may outlive the password it was created with - revoked and saved BEFORE the
        //new password is, so a power cut or failed write can't leave old sessions on flash that
        //a reboot would accept again (security review 2026-10-12, CR-02).
        endAllLoginSessions();
        if (!saveLoginSessionsNow()) {
          request->send(500, "text/plain", "Could not revoke existing sessions - password not changed");
          return;
        }
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

      //Especially important here (Code_REVIEW_V5.md #01) - without this, a power cut shortly after
      //a confirmed password change could let the OLD password become valid again on the next boot.
      checkBootSnapshotFreshness();
      request->send(200, "text/plain", "Security settings updated");
    });

    webServer.on("/api/timezone", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlTimezonePublic)) return; //OW-08
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
      //additionally covers the narrow gap AFTER the lookup finishes but BEFORE loop()'s own finish
      //block below has actually persisted the result and responded, so a request accepted during
      //exactly that gap can't overwrite pendingTimezoneValue before the first result is saved under it.
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
    //route it naturally shares with (each registered handler costs a small permanent heap
    //allocation that's never freed, see /api/quiet-hours below - a deliberate, not-free choice).
    webServer.on("/api/mqtt", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return; //OW-08
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

      //Validate EVERYTHING into local temporaries before touching any persisted/live state: a
      //partial mutate-then-validate (e.g. host applied, then port rejected) would leave
      //applyMqttSettingsChange() never called, so the live PubSubClient keeps a raw pointer into
      //mqttBrokerHost's OLD buffer - a String reassignment elsewhere can reallocate that buffer,
      //leaving the client holding a dangling pointer. On any validation failure, nothing changes
      //at all; on success, every field is applied and persisted together, immediately followed by
      //the one applyMqttSettingsChange() call that refreshes the pointer against the final buffer.
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
      checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after this confirmed change, not just on the periodic 30s check
      request->send(200, "text/plain", "MQTT settings updated");
    });
#endif

    //No dedicated GET here - quietHoursEnabled/Start/End are already folded into GET /api/settings
    //(see minimalDoc above). Adding a second registered route on top of that one reproducibly
    //broke /api/settings' own response handling on this heap-constrained device - every registered
    //handler costs a small permanent heap allocation that's never freed, and there's very little
    //room to spare.
    webServer.on("/api/quiet-hours", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
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
      checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after this confirmed change, not just on the periodic 30s check
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
    //Established BEFORE the server starts accepting connections - see
    //ServiceMemoryReserveFunctions.cpp's own comment (Punkt 207).
    maintainEmergencyMemoryReserve();
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
    //Config load + crash-log persistence is handled unconditionally before WiFi is even
    //attempted (see the single call site right before initWiFi() above), so nothing is needed
    //here for this branch - neither has any WiFi dependency.

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

//Adaptive unit-readiness wait, replacing a fixed 30s post-boot wait - the RTC fix solved "does the
//software know the right time", not "when can it physically show it".
//
//Factored out of loop() so setup() can also drive it synchronously, right before initWiFi() -
//loop() itself cannot run until setup() returns, and initWiFi()'s blocking autoConnect() call
//sits in setup() BEFORE loop() ever gets a chance to, so anything "non-blocking" written into
//loop() alone was always gated behind it regardless. A function called from both setup() (blocking
//here is fine - nothing else is waiting on setup() yet) and loop() (ticked once per iteration, a
//no-op once DONE) decouples unit readiness/the first display from however long WiFi ends up taking.
void tickUnitReadiness() {
  if (unitReadinessPhase == UNIT_READINESS_SCAN) {
    //One scan attempt (with its own internal not-found retry, unchanged) to learn which
    //addresses are actually present, before switching to lightweight per-unit polling below -
    //repeating the full 64-address sweep on every poll tick would be needlessly slow.
    SerialPrintln(F(""));
    SerialPrintln(F("=== I2C Bus Scan on Startup ==="));
    for (int scanAttempt = 0; scanAttempt < 3 && unitsAmount == 0; scanAttempt++) {
      if (scanAttempt > 0) {
        SerialPrintln(F("No units found, retrying scan..."));
        delay(300);
      }
      scanI2CBus();
    }
    SerialPrintln(F(""));
    if (unitsAmount > 0) {
      SerialPrintln(F("Unit readiness: waiting for all units to finish homing..."));
      unitReadinessPhase = UNIT_READINESS_POLLING;
      unitReadinessLastPollAt = millis();
    } else {
      //Nothing found at all - nothing to poll/wait ready for (showText() already no-ops with 0
      //units regardless, same as before this change).
      unitReadinessPhase = UNIT_READINESS_DONE;
    }
  } else if (unitReadinessPhase == UNIT_READINESS_POLLING) {
    if (millisReached(unitReadinessStartedAt + UNIT_READINESS_MAX_WAIT_MS)) {
      SerialPrintln(F("Unit readiness: safety ceiling reached, proceeding with whatever units report now"));
      unitReadinessPhase = UNIT_READINESS_DONE;
    } else if (millisReached(unitReadinessLastPollAt + UNIT_READINESS_POLL_INTERVAL_MS)) {
      unitReadinessLastPollAt = millis();
      bool anyBusy = false;
      for (int address = 0; address < unitsAmount; address++) {
        int status, offset;
        //No response yet counts as "not ready" too - a unit whose Wire responder only just came
        //up (or is deep in its own blocking calibrate() call) can legitimately miss a beat here.
        if (!readUnitInfo(address, status, offset) || status == 1) { //1 = BUSY/MOVING, see scanI2CBus()'s status comment
          anyBusy = true;
          break;
        }
      }
      if (!anyBusy) {
        SerialPrintln(F("Unit readiness: all units report ready"));
        unitReadinessPhase = UNIT_READINESS_DONE;
      }
    }
  }
}

//Periodic, throttled check (see BOOT_SNAPSHOT_CHECK_INTERVAL_MS) - rewrites the boot snapshot only
//if something actually changed since the last write, bounding how stale it can get after an
//UNCONTROLLED reset (crash, power loss - no isPendingReboot involved) without wearing this
//device's already-confirmed-slow flash chip with unconditional periodic writes. Also called
//directly right before every existing `isPendingReboot = true` site, so an INTENTIONAL reboot
//never loses a change from within the last check interval either.
unsigned long lastBootSnapshotCheckAt = 0;
void checkBootSnapshotFreshness() {
  String currentSignature = computeSettingsSignature();
  if (currentSignature != lastSavedSettingsSignature) {
    saveBootSnapshot(); //also updates lastSavedSettingsSignature
  }
}

//One tick of the mode-driven display update - scheduled messages, countdown/countup text
//recompute, rotation advance, then showText() for whichever deviceMode is active. Factored out of
//loop() so setup() can also run it once, synchronously, BEFORE initWiFi() - see
//tickUnitReadiness()'s comment for why that's necessary to actually show anything before WiFi
//connects. Deliberately excludes checkWifiScanRequest()/checkWifiConnectAttempt()/checkMqttState()
//- all meaningless before WiFi/the web server exist, and still called separately from loop().
void performOneSecondDisplayTick() {
  //An extension's own display content decisions are entirely ceded to whichever primary is
  //configured to reach it (multiple-units.md) - its units are driven only by
  //POST /api/extension-display, never by this device's own deviceMode/scheduled-messages/
  //rotation/etc. Everything below this stays fully live for every other role, and an extension's
  //own web UI/API remain fully usable - only this one per-second content-decision loop is skipped.
  if (deviceRole == DEVICE_ROLE_EXTENSION) {
    return;
  }

  checkScheduledMessages();
  checkCountdown();
  checkCountup();
  checkRotation();

  // Skip display updates if a web request is active to prevent blocking.
  // (showText() itself is a no-op when no units are detected, see scanI2CBus())
  if (!webRequestActive) {
    //Mode Selection
    if (deviceMode == DEVICE_MODE_ROTATION) {
      //rotationCurrentAlignment ("" = inherit the default) is set every tick by checkRotation().
      //Counts this second toward the current item's duration only once its first render has
      //SETTLED - showText() already returns true the moment a job merely STARTS, well before the
      //flaps have finished moving, and that movement time isn't readable display time. A
      //temporary message (only those hold, finalPauseMs > 0) displaces the item: it has to be
      //rendered and settle again afterwards. See rotationItemSettled's comment for why a running
      //job alone doesn't stop the count once settled.
      bool foreignJob = isTemporaryMessageJobBusy();
      if (foreignJob) {
        rotationItemStarted = false;
        rotationItemSettled = false;
      } else if (rotationItemStarted && !isDisplayJobBusy()) {
        rotationItemSettled = true;
      }
      bool rotationTextAccepted = showText(inputText, 0, rotationCurrentAlignment, rotationCurrentAnimationMode, rotationCurrentAnimationStaggerMs);
      if (rotationTextAccepted) {
        rotationItemStarted = true;
        if (!isDisplayJobBusy()) {
          rotationItemSettled = true; //already showing exactly this - nothing to move
        }
      }
      if (rotationItemSettled && !foreignJob) {
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

void loop() {
  //TEMPORARY DIAGNOSTIC - see diagMinFreeHeap's comment.
  uint32_t diagFreeHeapNow = ESP.getFreeHeap();
  if (diagFreeHeapNow < diagMinFreeHeap) {
    diagMinFreeHeap = diagFreeHeapNow;
    diagMinFreeHeapAtSec = millis() / 1000;
  }
  uint32_t diagMaxBlockNow = ESP.getMaxFreeBlockSize();
  if (diagMaxBlockNow < diagMinMaxBlock) {
    diagMinMaxBlock = diagMaxBlockNow;
  }

#if USE_MULTICAST == true
  MDNS.update();
#endif
#if MQTT_ENABLE == true
  mqttLoop();
#endif

  tickUnitReadiness();

  if (pendingDisplayControllersCancel) {
    pendingDisplayControllersCancel = false;
    cancelDisplayControllers();
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

  //Extension-side display push (see pendingExtensionDisplayQueued's comment) - writes whichever
  //units are due this pass, never blocks.
  tickExtensionDisplay();

  //Login sessions created/renewed/ended by web requests - persisted here, not in the callbacks.
  saveLoginSessionsIfDirty();

  //A direct timezone.setLocation() call (blocking DNS+UDP) could trip the software watchdog under
  //real network stress, so this uses a genuinely non-blocking implementation
  //(checkTimezoneLookup(), ServiceAsyncTimeSyncFunctions.ino) instead - the lookup spans multiple
  //loop() ticks, so the deferred request has to be held across that whole span (
  //timezoneLookupDeferredRequest/_Generation, declared near pendingTimezoneSlot).
  //checkTimezoneLookup() itself already ran earlier this loop() tick, so tzLookupDone may already
  //be true for the SAME tick a lookup finishes on.
  if (pendingTimezoneSlot.request != nullptr && timezoneLookupDeferredRequest == nullptr && !isTimezoneLookupBusy()) {
    timezoneLookupDeferredRequest = pendingTimezoneSlot.take(timezoneLookupDeferredFlag);
    //Snapshot NOW, not re-read from the shared pendingTimezoneValue global at finish time below -
    //see timezoneLookupDeferredValue's own comment.
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
      //went - stops a still-pending boot-retry from redundantly re-resolving an already-correct value.
      bootTimezoneRuleResolved = true;
      checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after this confirmed change, not just on the periodic 30s check
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

  //Consumes the boot-time (or a retry of it) lookup's own result exactly once. Only reacts to a
  //completion that has NO associated HTTP request (timezoneLookupDeferredRequest == nullptr) - an
  //explicit POST /api/timezone's own outcome is fully handled by the block directly above and
  //must never be reinterpreted as a boot/retry result here.
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
    if (deviceRole == DEVICE_ROLE_EXTENSION) {
      //Read by the primary's readiness poll - see extensionDisplayBusy()'s comment.
      doc["displayBusy"] = extensionDisplayBusy();
      doc["displayWriteFailed"] = extensionDisplayWriteFailed();
    }
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
        int fwVersionMajor = -1;
        int fwVersionMinor = -1;
        int fwVersionPatch = -1;
        online = readUnitInfo(unitIndex, status, offset, phase, progress, unexpectedHomeTriggerCount, slowHomeSearchCount, fwVersionMajor, fwVersionMinor, fwVersionPatch);
        doc["units"][unitIndex]["phase"] = phase;
        doc["units"][unitIndex]["progress"] = progress;
        doc["units"][unitIndex]["unexpectedHomeTriggerCount"] = unexpectedHomeTriggerCount;
        doc["units"][unitIndex]["slowHomeSearchCount"] = slowHomeSearchCount;
        //255/255/255 means "unit firmware predates version reporting" (idle-bus read, not a real
        //version) - see readUnitInfo()'s 10-arg overload comment. Reported as null, not "v255.255.255".
        bool versionKnown = !(fwVersionMajor == 255 && fwVersionMinor == 255 && fwVersionPatch == 255) && fwVersionMajor >= 0;
        if (versionKnown) {
          doc["units"][unitIndex]["firmwareVersion"] = String(fwVersionMajor) + "." + String(fwVersionMinor) + "." + String(fwVersionPatch);
        } else {
          doc["units"][unitIndex]["firmwareVersion"] = (char*)0; //serializes as JSON null
        }
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

    //Both 4KB OTA buffers (Update's internal sector buffer, allocated inside begin(), and
    //otaChunkBuffer) go to the second heap in IRAM (mmu=4816H, code-review-V7 Punkt C) - an upload
    //is started from the open web UI, exactly when DRAM is tightest. The heap selection is global,
    //so these scopes stay yield-free and allocate nothing else. Falls back to DRAM if IRAM has no
    //room (or the build lacks mmu=4816H, where HeapSelectIram is a no-op). free()/delete[] find the
    //right heap by address, so nothing else changes.
    bool ok;
    {
      HeapSelectIram iramHeap;
      ok = Update.begin(updateSize, U_FLASH);
    }
    if (!ok && Update.getError() == UPDATE_ERROR_OOM) {
      ok = Update.begin(updateSize, U_FLASH);
    }
    if (ok) {
      otaBytesWritten = 0;
      //Allocated here, not statically - free heap on this device is chronically tight, no reason
      //to hold 4KB for a rarely-used feature the other 99% of the time.
      if (otaChunkBuffer != nullptr) {
        delete[] otaChunkBuffer; //shouldn't happen (always freed below before this runs again), don't leak if it somehow does
      }
      {
        HeapSelectIram iramHeap;
        otaChunkBuffer = new (std::nothrow) uint8_t[OTA_CHUNK_SIZE];
      }
      if (otaChunkBuffer == nullptr) {
        otaChunkBuffer = new (std::nothrow) uint8_t[OTA_CHUNK_SIZE];
      }
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
    //otaPendingPhase only ever transitions to OTA_PHASE_WRITE from the /api/firmware-chunk outer
    //handler above, which only does so once it has confirmed THIS request was the buffer's sole,
    //still-current owner.
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
    //image if called early. evenIfRemaining still has to stay true for the real end() call below,
    //though - Update.begin() rounds its internal target size UP to the next flash sector, always
    //larger than the exact byte count actually transferred, so isFinished() would never be
    //satisfied even for a genuinely complete, correctly-sized upload.
    //
    //Deliberately otaBytesWritten here, NOT Update.progress(): Updater.cpp's write() only advances
    //progress() when a full SPI_FLASH_SEC_SIZE (4096) buffer gets flushed to flash; a trailing
    //partial sector stays in Updater's internal _buffer and is only flushed inside end() - so
    //Update.progress() legitimately under-reports for any image whose size isn't an exact
    //multiple of 4096 (effectively every real image). otaBytesWritten instead counts bytes
    //actually handed to (and accepted by) Update.write() call-by-call.
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
  // Gated on otaResultReady ALONE, not also on pendingFirmwareUploadSlot.request != nullptr:
  // /api/firmware-finish's client can disconnect right after the request is accepted but before
  // this loop() tick runs (PendingRequestSlot's own onDisconnect callback already nulls the
  // slot's request pointer at that point) - reboot-flagging must still happen even then, since the
  // flash write itself doesn't care whether anyone is still listening for its result. Whether to
  // actually SEND the HTTP response is the only thing that legitimately depends on the request
  // still being there.
  if (otaResultReady) {
    if (otaResultSuccess && otaCompletedPhase == OTA_PHASE_FINISH) {
      SerialPrintln(F("Firmware update successful, rebooting..."));
      checkBootSnapshotFreshness(); //see its own comment further down - keep the snapshot fresh before every intentional reboot
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
    //Invalidate the display cache unconditionally, not just on success - pushFirmwareToUnit()'s
    //very first step (requestUnitEnterBootloader()) already interrupts the unit's running app and
    //reboots it into the bootloader regardless of whether the push that follows ultimately
    //succeeds, so the unit's actual physical position may no longer match the cache either way.
    //Same reasoning as forceUnitRecoveryReset()'s identical fix (ServiceI2CRecoveryFunctions.ino).
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
    //(ServiceUnitFirmwareFunctions.ino). This probe is NOT actually read-only:
    //requestUnitEnterBootloader() below genuinely reboots whatever answers at this address, and
    //twibootReadChipInfo() itself is a 4-byte WRITE (register-pointer + address bytes) before its
    //read. A colliding address is a real hazard here, not just "silently-wrong garbage" - also
    //checked against the separate, fixed RTC collision (isReservedI2CAddress()), independent of
    //unitsAmount.
    bool collisionRisk = (expectedBootAddress < unitsAmount) || isReservedI2CAddress(expectedBootAddress);

    if (collisionRisk) {
      doc["success"] = false;
      doc["error"] = F("Refusing to probe Unit ") + String(unitIndex) + F(": its bootloader address would collide with "
        "another unit's live app address, or is permanently reserved for the RTC module.");
    } else {
      SerialPrintln(F("Bootloader probe: requesting Unit ") + String(unitIndex) + F(" to enter bootloader..."));
      unsigned long bootloaderSearchStart = millis();
      uint8_t enterBootloaderResult = requestUnitEnterBootloader(unitIndex);
      SerialPrintln(F("  requestUnitEnterBootloader() Wire.endTransmission() = ") + String(enterBootloaderResult) + F(" (0 = success)"));
      //Invalidate the display cache - this interrupts and reboots the unit's running app
      //regardless of whether the probe below actually finds it in the bootloader afterward - see
      //the identical fix/comment on pushFirmwareToUnit()'s call site above. Not reached in the
      //collisionRisk branch above, which deliberately never sends this command at all.
      displayRenderGeneration++;

      //Same retry/backoff as pushFirmwareToUnit() - give the watchdog reset + twiboot startup
      //time to happen. Probes ONLY unitIndex's own expected address, not a full scan of the whole
      //bootloader address range - see findExpectedBootloaderAddress()'s comment
      //(ServiceUnitFirmwareFunctions.ino).
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

      //Reads twiboot's own version string while still in the bootloader - cheap (one more small
      //I2C transaction) and this is already the one place this project talks to the bootloader on
      //demand. Reports the SF-TWIBOOT vX.Y string from VERSION_STRING (see twibootReadVersion()'s
      //comment) - tells you which fork build the unit was flashed with.
      char bootloaderVersion[17];
      if (twibootReadVersion(bootAddress, bootloaderVersion, sizeof(bootloaderVersion))) {
        doc["bootloaderVersion"] = bootloaderVersion;
      } else {
        doc["bootloaderVersion"] = (char*)0; //serializes as JSON null - read failed, don't guess
      }

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

    checkBootSnapshotFreshness(); //see its own comment further down - keep the snapshot fresh before every intentional reboot
    isPendingReboot = true;
    return;
  }
#endif

  //isWifiConfigured only ever LATCHES true, once, inside initWiFi() (setup()) - it never gets
  //corrected back if the initial connect attempt genuinely failed (a brief drop after a
  //successful connect is instead handled by the SDK's own auto-reconnect). A router/AP outage
  //overlapping THIS device's boot window would otherwise leave it stuck showing "OFFLINE" until a
  //manual power cycle. Fixed in two parts: (1) render "OFFLINE" as a transient overlay via
  //showText()'s own alignment-override parameter instead of mutating the user's real settings -
  //the original deviceMode is never touched; (2) periodically retry the STA connection ourselves
  //in the background (WiFiManager's own portal has already given up for good by the time control
  //reaches loop() - see initWiFi()'s comment) and, on success, perform a single controlled reboot
  //to run the exact same successful-boot sequence a normal connect would have.
  if (!isWifiConfigured) {
    showText("OFFLINE", 0, ALIGNMENT_MODE_CENTER);
    delay(100);

    if (millisReached(lastOfflineReconnectAttempt + OFFLINE_RECONNECT_RETRY_MS)) {
      lastOfflineReconnectAttempt = millis();
      SerialPrintln(F("Still offline - retrying WiFi with the stored credentials..."));
      WiFi.mode(WIFI_STA); //in case autoConnect()'s own now-abandoned portal left AP_STA behind
      WiFi.begin(); //no args = reconnect with whatever the SDK already has persisted (WiFi.persistent(true) is the core default)
      unsigned long retryStart = millis();
      while (WiFi.status() != WL_CONNECTED && !millisReached(retryStart + 15000)) {
        delay(250);
        yield();
      }
      if (WiFi.status() == WL_CONNECTED) {
        SerialPrintln(F("WiFi recovered - rebooting to complete normal startup"));
        checkBootSnapshotFreshness(); //see its own comment further down - keep the snapshot fresh before every intentional reboot
        delay(100);
        ESP.restart();
      } else {
        SerialPrintln(F("Still offline - will retry again in ") + String(OFFLINE_RECONNECT_RETRY_MS / 60000) + F(" minutes"));
      }
    }
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
  //with a genuinely non-blocking implementation. checkTimezoneLookup() is the equivalent
  //replacement for the /api/timezone endpoint's own network lookup - see pendingTimezoneSlot's
  //processing block further down for how it's driven. Neither call below ever blocks.
  checkNtpSync();
  checkTimezoneLookup();

  //Process every second
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= 1000) {
    previousMillis = currentMillis;

#if WIFI_USE_DIRECT == false
    checkWifiScanRequest();
    checkWifiConnectAttempt();
#endif
#if MQTT_ENABLE == true
    checkMqttState();
#endif

    performOneSecondDisplayTick();

    //No-op for every role except primary (and even then, only once per
    //EXTENSION_HEALTH_CHECK_INTERVAL_MS) - see ServiceExtensionFunctions.ino's own top comment for
    //why a short-bounded blocking call here is an accepted exception to the usual "no blocking
    //network calls" rule.
    checkExtensionHealth();

    if (millisReached(lastBootSnapshotCheckAt + BOOT_SNAPSHOT_CHECK_INTERVAL_MS)) {
      lastBootSnapshotCheckAt = millis();
      checkBootSnapshotFreshness();
    }

    //Cheap (no-op unless actually empty) - see ServiceMemoryReserveFunctions.cpp's own comment.
    maintainEmergencyMemoryReserve();
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
  //though the cache's own text/alignment never changed. See displayRenderGeneration's comment.
  //Bumped unconditionally rather than only when detectedAmount differs, since a unit restarting
  //mid-scan with the SAME final count is exactly the case this needs to catch.
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

