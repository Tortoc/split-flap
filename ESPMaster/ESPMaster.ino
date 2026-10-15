/*
  SplitFlap ESPMaster firmware.
  Fork of https://github.com/scientress/split-flap, based on https://github.com/JonnyBooker/split-flap,
  built on the work of David Königsmann (https://github.com/Dave19171/split-flap).
  Licensed under the GNU GPL v3, see LICENSE in the repository root.
*/

//--- Feature flags ---
#define SERIAL_ENABLE       true   //enable serial debug messages
#define UNIT_CALLS_DISABLE  false  //disable I2C calls to units (debug the ESP alone)
#define WEB_FIRMWARE_UPDATE_ENABLE true //browser-based firmware upload from the Admin page
#define UNIT_BOOTLOADER_ENABLE true //push Unit.ino updates over I2C via twiboot - needs every
                                    //unit pre-flashed with twiboot, see Unit/BOOTLOADER_SETUP.md
#define UNITS_AMOUNT_MAX    64     //matches the 6-bit address jumper range; actual amount
                                    //is auto-detected at startup via scanI2CBus()
#define SERIAL_BAUDRATE     115200
#define WIFI_USE_DIRECT     false  //false = AP config portal (WiFiManager), true = direct connect
#define USE_MULTICAST       true   //"<mdnsHostname>.local" on the LAN, unique per display
//LED_ENABLE only gates the error blink when WiFi failed at startup (end of setup()).
#define LED_ENABLE          true
#define DEBUG_ENABLE true  //startup debug page, error panel, page-load log, serial debug log -
                           //switched at runtime via POST /api/debug-mode
#define MQTT_ENABLE false  //MQTT + Home Assistant auto-discovery.
                           //Compile-time only: PubSubClient malloc()s its buffer in the
                           //constructor, so false is the only way to avoid that cost.

#define WIFI_STATIC_IP      false //try a router-assigned static IP instead of DHCP

//fae-pcb with an ESP-12F module. Debug LED on GPIO2. VCC must be 3.3V only.
#define LED_DEBUG_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

//--- Constants - only change if you know what you're doing ---
#define ANSWER_SIZE         6 //status + offset + phase + progress bytes, must match Unit.ino's requestEvent()
#define ANSWER_SIZE_LIGHT   3 //status + offset only, see readUnitInfo()'s 3-arg overload
#define ANSWER_SIZE_DIAG    8 //adds the two diagnostic counters - see readUnitInfo()'s 7-arg
                               //overload (older units read the extra bytes as 255).
#define ANSWER_SIZE_VERSION 11 //adds the 3 firmware-version bytes (major/minor/patch) - see
                               //readUnitInfo()'s 10-arg overload (same 255 caveat).
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
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

//--- Libraries ---
#if WIFI_USE_DIRECT == false
//WEBSERVER_H guard needed for WiFiManager compatibility, see
//https://github.com/me-no-dev/ESPAsyncWebServer/issues/418#issuecomment-667976368
#define WEBSERVER_H
#include <WiFiManager.h>
#endif

#if WEB_FIRMWARE_UPDATE_ENABLE == true
#include <Updater.h>
#include <bearssl/bearssl_hmac.h> //admin password hash (PBKDF2) and signed primary->extension pushes
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebSrv.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h> //primary->extension push and health check
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
#include <Schedule.h> //schedule_recurrent_function_us() - setup hotspot password display, see initWiFi()
#include <umm_malloc/umm_heap_select.h> //HeapSelectIram - OTA buffers in a second heap, if the build has one
#include <lwip/priv/tcp_priv.h> //exposes tcp_active_pcbs/etc. for /api/tcp-status
#include "Classes.h"
#include "ServiceMemoryReserveFunctions.h"
#include "LittleFS.h"

//Request method values for webServer.on()/request->method(): ESPAsyncWebSrv's own bit flags. Its
//HTTP_POST/HTTP_DELETE names are wrong in this sketch - WiFiManager pulls in ESP8266WebServer, whose
//HTTPMethod values (3/6) win, while the library still uses its bits (2/4): "HTTP_POST" matched GET.
const WebRequestMethodComposite ASYNC_HTTP_GET = 0b00000001;
const WebRequestMethodComposite ASYNC_HTTP_POST = 0b00000010;
const WebRequestMethodComposite ASYNC_HTTP_DELETE = 0b00000100;

//--- User-facing settings ---
//mDNS hostname ("<mdnsHostname>.local"), set via POST /api/wifi (Netzwerk tab).
String mdnsHostname = "split-flap";
//Exact DNS name the router gives this device (e.g. "split-flap.fritz.box"), "" = none. Only used
//to accept that Host header (RequestGuard); set via POST /api/wifi?action=set-router-hostname.
String routerHostname = "";
const char* routerHostnamePath = "/routerhostname.txt";
//Fixed setup-hotspot password, "" = a fresh random one shown on the display (default). Plaintext
//on purpose (WPA2 needs the passphrase). Set via POST /api/wifi?action=set-setup-ap-password,
//8-63 printable ASCII characters.
String setupApFixedPassword = "";
const char* setupApFixedPasswordPath = "/setupappassword.txt";

//Used if connecting via "WIFI_USE_DIRECT" of "true" - Otherwise, leave blank
const char* wifiDirectSsid = "";
const char* wifiDirectPassword = "";

//Admin password, persisted only as a salted PBKDF2-SHA256 hash (see hashAdminPassword()). Default
//"splitflap"; empty = protection off for every endpoint, firmware included, regardless of the
//toggles below.
String adminPasswordHash = "";
const char* adminPasswordHashPath = "/adminpasswordhash.txt";
//Where firmware before 5.0.0 kept the plaintext password - read once to migrate, then deleted. An
//older firmware flashed afterwards falls back to "splitflap".
const char* legacyOtaPasswordPath = "/otapassword.txt";
bool adminPasswordMigrated = false; //set while loading, persisted by loadValuesFromFileSystem()

//Primary<->extension link keys (HMAC-SHA256 signed pushes, no password on the wire):
//primaryLinkSecret is this device's key as a primary (created on first pairing),
//pairedPrimaryLinkSecret the key it accepts as an extension. 64 hex chars, "" = none.
String primaryLinkSecret = "";
const char* primaryLinkSecretPath = "/linksecret.txt";
String pairedPrimaryLinkSecret = "";
const char* pairedPrimaryLinkSecretPath = "/pairedlinksecret.txt";
//Pairing runs from loop() (blocking HTTPClient call) - POST /api/extension-devices?action=pair only
//queues it. The extension's admin password is held here just until that one request is sent.
volatile bool pendingExtensionPairing = false;
int pendingExtensionPairingIndex = -1;
String pendingExtensionPairingPassword;
//Only one delivery of the web page at a time - see GET "/"'s comment.
bool pageDeliveryActive = false;
unsigned long pageDeliveryStartedAt = 0;
//The delivery that owns pageDeliveryActive and its connection; only its own disconnect releases
//the guard (an overdue one is closed explicitly).
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

//Wrong admin-password attempts per sender IP (see checkAdminPassword()): each sender waits
//FAILED_LOGIN_PAUSE_MS after a wrong guess. Per sender, so one attacker can't lock everyone out.
#define FAILED_LOGIN_SLOTS 8
#define FAILED_LOGIN_PAUSE_MS 2000
struct FailedLogin {
  uint32_t ip = 0;
  unsigned long at = 0; //millis() of the wrong guess, 0 = free slot
};
FailedLogin failedLogins[FAILED_LOGIN_SLOTS];
//checkAdminPassword() results (ServiceAccessControlFunctions.ino).
#define PASSWORD_OK 0
#define PASSWORD_WRONG 1
#define PASSWORD_THROTTLED 2
const char* mdnsHostnamePath = "/mdnsname.txt";

//Unit 63's bootloader address (0x29 + 63 = 0x68) is RTC_I2C_ADDRESS - a chip-info read there would
//overwrite the DS3231's time registers. Checked at every bootloader-address entry point.
//Defined after the #include block: a definition before it makes arduino-cli insert its generated
//prototypes ahead of the includes, breaking the build.
bool isReservedI2CAddress(int address) {
  return address == RTC_I2C_ADDRESS;
}

//First registered handler: answers 400 to every request the handlers behind it must never see (no
//onRequest, so the library doesn't even parse the body).
//1. Multipart boundary outside RFC 2046's 1-70 characters: the library's parser loops over it with
//   a uint8_t counter, so 256+ characters hang it until the watchdog resets the device (cf.
//   CVE-2026-54571).
//2. Unknown Host header (DNS rebinding): accepted are an IPv4 address, this device's mDNS/DHCP name
//   with or without ".local", or the exact routerHostname. Anything else is refused, even names
//   starting with this device's name ("split-flap.attacker.example"). Extensions must be
//   configured under one of these names.
class RequestGuard : public AsyncWebHandler {
  public:
    bool canHandle(AsyncWebServerRequest *request) override {
      return hasInvalidMultipartBoundary(request) || !isKnownHost(request->host());
    }
    void handleRequest(AsyncWebServerRequest *request) override {
      request->send(400, "text/plain", F("Bad request (unknown host name or multipart boundary)"));
    }

  private:
    //The library keeps the boundary of the LAST multipart Content-Type header, getHeader() returns
    //the first - so more than one header is refused.
    static bool hasInvalidMultipartBoundary(AsyncWebServerRequest *request) {
      AsyncWebHeader *contentType = nullptr;
      for (size_t i = 0; i < request->headers(); i++) {
        AsyncWebHeader *header = request->getHeader(i);
        if (header->name().equalsIgnoreCase("Content-Type")) {
          if (contentType != nullptr) {
            return true;
          }
          contentType = header;
        }
      }
      if (contentType == nullptr) {
        return false;
      }
      String value = contentType->value();
      if (!value.startsWith("multipart/")) {
        return false;
      }
      String boundary = value.substring(value.indexOf('=') + 1);
      boundary.replace("\"", "");
      return value.indexOf('=') == -1 || boundary.length() == 0 || boundary.length() > 70;
    }

    //True for exactly "<name>" or "<name>.local" (case-insensitive).
    static bool isOwnName(const String &host, const String &name) {
      return name.length() > 0 && (host.equalsIgnoreCase(name) || host.equalsIgnoreCase(name + ".local"));
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
      return host.length() == 0 || isIpv4Address(host) || isOwnName(host, mdnsHostname) || isOwnName(host, WiFi.hostname()) ||
        (routerHostname.length() > 0 && host.equalsIgnoreCase(routerHostname));
    }
};

#if MQTT_ENABLE == true
//Declared here, not in ServiceMqttFunctions.ino: handlers in this file use mqttClient, and .ino
//files are concatenated in filename order.
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

//MQTT broker settings, persisted, changed via POST /api/mqtt. mqttEnabled is off by default -
//MQTT_ENABLE only decides whether the feature is compiled in.
bool mqttEnabled = false;
String mqttBrokerHost = "";
int mqttBrokerPort = 1883;
String mqttUsername = "";
//Same never-sent-back-over-GET convention as the admin password - see adminPasswordHash.
String mqttPassword = "";
const char* mqttEnabledPath = "/mqttenabled.txt";
const char* mqttBrokerHostPath = "/mqttbrokerhost.txt";
const char* mqttBrokerPortPath = "/mqttbrokerport.txt";
const char* mqttUsernamePath = "/mqttusername.txt";
const char* mqttPasswordPath = "/mqttpassword.txt";
#endif

//Per-category "reachable without password" toggles, only used while a password is set (see
//isRequestAllowedWithoutPassword()). Firmware/flash endpoints and access control itself have no
//toggle. All default false.
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

//POSIX TZ rule from the last successful timezoned.rop.nl lookup (e.g. "CET-1CEST,M3.5.0,M10.5.0/3"),
//applied at boot via setPosix() so the first pre-WiFi render already shows local time; the rule
//covers DST itself.
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

//How long a scheduled message stays on screen
const int scheduledMessageDisplayTimeMillis = 7500;

#if WIFI_STATIC_IP == true
//Static IP for the device - make sure it doesn't conflict with anything else on the network.
IPAddress wifiDeviceStaticIp(192, 168, 1, 50);

//Your router details
IPAddress wifiRouterGateway(192, 168, 1, 1);
IPAddress wifiSubnet(255, 255, 0, 0);

//DNS Entry. Default: Google DNS
IPAddress wifiPrimaryDns(8, 8, 8, 8);
#endif

//--- Core state - normal running of the system, be careful changing these ---
//Firmware version shown in the UI (git tags esp-vX.Y.Z) - an own version line since the fork from
//JonnyBooker's 2.3.0-Scott.
const char* espVersion = "5.0.6";

//The letters on the flaps, in drum order - must match the physical drums.
const char letters[] = {' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '&', '#', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '.', '-', '?', '!'};
unsigned long previousMillis = 0;
int unitsAmount = 0; //auto-detected via I2C scan, sequential from 0 - see scanI2CBus()

//Adaptive wait for unit readiness after boot (instead of a fixed 30s) - see its block in loop().
//showText() is a no-op while unitsAmount <= 0, so this gates the first render after boot.
enum UnitReadinessPhase { UNIT_READINESS_SCAN, UNIT_READINESS_POLLING, UNIT_READINESS_DONE };
UnitReadinessPhase unitReadinessPhase = UNIT_READINESS_SCAN;
unsigned long unitReadinessStartedAt = 0; //set in setup(); millisReached() below is overflow-safe
unsigned long unitReadinessLastPollAt = 0;
#define UNIT_READINESS_POLL_INTERVAL_MS 200
//Ceiling equal to the old fixed wait, so the worst case is never slower than before.
#define UNIT_READINESS_MAX_WAIT_MS 30000

//Wire calls from an AsyncWebServer callback crash the ESP (too little stack for clock stretching),
//so handlers only stash the request and loop() does the I2C work and responds. One in flight;
//another gets 503. loop() frees the slot as soon as it takes a request, and the library deletes a
//request on disconnect - so each disconnect is scoped to its own request generation
//(accept()/take()/stillConnected()), never to whichever request holds the slot now.
struct PendingRequestSlot {
  AsyncWebServerRequest* request = nullptr;
  uint32_t generation = 0;             //bumped every time a new request is accepted into this slot

  //Each accept() gets its own disconnect flag (shared_ptr, held by the onDisconnect closure and by
  //take()'s caller). A single "last disconnected generation" field let a newer request's disconnect
  //hide an older one's, and a deleted request got used.
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

  //From loop(): returns the request and frees the slot right away. Keep flagOut (shared_ptr) for
  //the stillConnected() check.
  AsyncWebServerRequest* take(std::shared_ptr<std::atomic<bool>> &flagOut) {
    AsyncWebServerRequest* result = request;
    flagOut = disconnectedFlag;
    request = nullptr;
    return result;
  }

  //Right before using the request: false means its connection is gone. Pass the flag take() gave
  //you, not the slot's current one.
  static bool stillConnected(const std::shared_ptr<std::atomic<bool>> &flag) {
    return flag && !flag->load();
  }
};

PendingRequestSlot pendingRescanSlot;
PendingRequestSlot pendingUnitsInfoSlot;
PendingRequestSlot pendingLegacyI2CScanSlot;
bool pendingUnitsInfoRequestDetail = false; //captured from the "detail" query param at request time

//Offset writes are deferred to loop() like every other I2C access.
PendingRequestSlot pendingUnitOffsetSlot;
int pendingUnitOffsetUnitId = -1;
uint16_t pendingUnitOffsetValue = 0;

//Timezone changes are deferred to loop() too (no network I/O in the callback).
PendingRequestSlot pendingTimezoneSlot;
String pendingTimezoneValue = "";
//The lookup (checkTimezoneLookup()) spans several loop() ticks; these hold the request meanwhile.
AsyncWebServerRequest* timezoneLookupDeferredRequest = nullptr;
std::shared_ptr<std::atomic<bool>> timezoneLookupDeferredFlag;
//Snapshot of pendingTimezoneValue at kickoff, so a second request can't overwrite it mid-lookup.
String timezoneLookupDeferredValue = "";
//Declared here, not in ServiceAsyncTimeSyncFunctions.ino: loop() uses them, and globals aren't
//forward-declared across .ino files.
bool tzLookupDone = false;
bool tzLookupResult = false;

//Retry for the one-shot timezone lookup at boot - a transient failure would otherwise leave the
//zone rule unresolved (the periodic NTP sync only refreshes the clock). bootTimezoneLookupPending
//makes the retry logic react to each completion once.
bool bootTimezoneRuleResolved = false;
bool bootTimezoneLookupPending = false;
unsigned long nextBootTimezoneRetryAt = 0; //0 = no retry currently scheduled
#define BOOT_TIMEZONE_RETRY_MS 60000UL //not urgent - just needs to eventually succeed

//Same reasoning, for the twiboot/bootloader endpoints (UNIT_BOOTLOADER_ENABLE).
PendingRequestSlot pendingFirmwarePushSlot;
int pendingFirmwarePushUnitIndex = -1;
PendingRequestSlot pendingBootloaderProbeSlot;
int pendingBootloaderProbeUnitIndex = -1;

//Used by ServiceDisplayJobFunctions.ino and ServiceExtensionFunctions.ino - declared here because
//.ino files are concatenated alphabetically (the display-job file comes first).
#define EXTENSION_HEALTH_CHECK_INTERVAL_MS (15UL * 1000UL)
#define EXTENSION_HTTP_TIMEOUT_MS 1500

//POST /api/extension-display only validates and queues; tickExtensionDisplay() writes from loop()
//(no I2C in callbacks). The handler answers at once - a staggered animation can outlast the
//primary's EXTENSION_HTTP_TIMEOUT_MS, which would count as failed and loop the re-render. The
//primary sees the real state via GET /api/units (displayBusy/displayWriteFailed). A newer push
//replaces a queued older one.
volatile bool pendingExtensionDisplayQueued = false;
//Set by POST /api/device-role on a real change; loop() then cancels both display controllers
//(the handler may run while loop() yields mid-job).
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

//Multi-device roles ("Primärgerät"/"Erweiterungsgerät", no Master/Slave): standalone (default);
//primary drives its own units plus reachable extensionDevices[]; extension has no display logic of
//its own, its units follow POST /api/extension-display pushes - its web UI/API stay usable.
const char* DEVICE_ROLE_STANDALONE = "standalone";
const char* DEVICE_ROLE_PRIMARY = "primary";
const char* DEVICE_ROLE_EXTENSION = "extension";

//How a primary uses all its extensions: extend (default) appends their units after its own;
//mirror sends each extension the primary's own characters (cut/padded to its width) and keeps the
//primary's width.
const char* EXTENSION_LINK_MODE_EXTEND = "extend";
const char* EXTENSION_LINK_MODE_MIRROR = "mirror";

//Alignment options
const char* ALIGNMENT_MODE_LEFT = "left";
const char* ALIGNMENT_MODE_CENTER = "center";
const char* ALIGNMENT_MODE_RIGHT = "right";

//Animation options (see computeUnitAnimationDelayMs()). Defined here because this file uses them
//too and must see them first.
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
#define ANIMATION_STAGGER_MS_MAX 1000 //10 units * 1000ms is already a ~9s spread

//Shared validation for POST /api/settings and rotation items. Functions are auto-prototyped, so
//this works from any file.
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
//Own file (a JSON array of hosts), not part of the boot snapshot.
const char* extensionDevicesPath = "/extensiondevices.json";
//Boot snapshot: this flash chip takes ~800ms per file open, and loading used ~30 files. This file
//mirrors those scalar settings in one JSON blob, so boot needs one open. The individual files stay
//the source of truth; this is a derived cache.
const char* bootSnapshotPath = "/boot-snapshot.json";
//Kept current by checkBootSnapshotFreshness() from loop() (signature compare, no dirty flags).
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
String animationMode = ANIMATION_MODE_SIMULTANEOUS; //default: all units animate at once
unsigned long animationStaggerMs = 150; //ms between consecutive units' start, see ANIMATION_MODES
String inputText = "";
String deviceMode = "";
String deviceRole = "standalone"; //standalone|primary|extension
String extensionLinkMode = "extend"; //extend|mirror
//Fixed small array like rotationItems (no per-entry heap, no LinkedList operator[]). The struct is
//in Classes.h (function parameter type, see TwibootChipInfo there).
#define EXTENSION_DEVICES_MAX 4
ExtensionDevice extensionDevices[EXTENSION_DEVICES_MAX];
int extensionDeviceCount = 0;
String countdownToDateUnix = "";
String countupSinceDateUnix = "";
String lastWrittenText = "";
String lastReceivedMessageDateTime = "";
//With lastWrittenText and displayRenderGeneration: what showText() compares a request against. The
//alignment actually used, so rotation items with equal text but different alignment still redraw.
String lastWrittenAlignment = "";
//Bumped whenever the physical display may diverge from lastWrittenText/lastWrittenAlignment
//without the request changing (global alignment change, rescan, calibration offset change, ...).
//showText() redraws whenever it differs from lastRenderedGeneration.
unsigned long displayRenderGeneration = 0;
unsigned long lastRenderedGeneration = 0;
bool isPendingReboot = false;
bool isPendingUnitsReset = false;
//True while WiFiManager's setup hotspot is open - its password is on the display then, and Quiet
//Hours must not blank it (see initWiFi(), ServiceWifiFunctions.ino).
bool wifiSetupPortalOpen = false;
bool isWifiConfigured = false;
//Background offline-recovery retry - see loop()'s `if (!isWifiConfigured)` block.
unsigned long lastOfflineReconnectAttempt = 0;
#define OFFLINE_RECONNECT_RETRY_MS (5UL * 60UL * 1000UL) //5 minutes
//WiFi disconnect statistics for GET /api/diag - declared here because this file is concatenated
//before ServiceWifiFunctions.ino.
unsigned long wifiDisconnectCount = 0;
String lastWifiDisconnectReason = "";
unsigned long lastWifiDisconnectAt = 0;
//Optional DS3231 RTC (ServiceRtcFunctions.ino) - declared here since setup() and GET /api/diag in
//this file need them.
bool rtcPresent = false;
bool rtcOscillatorStopFlag = false;
bool showDebugPage = false; //defaults off - the startup debug page can render blank on fresh boots
volatile bool webRequestActive = false; //skip display updates while a web request is in flight
LList<ScheduledMessage> scheduledMessages;
//False while an out-of-memory read left scheduledMessages incomplete - saving that partial list
//would overwrite every entry never read. writeScheduledMessagesToFile()
//refuses while it's false; ensureScheduledMessagesComplete() retries the load first.
bool scheduledMessagesComplete = true;
//Bumped on every change to scheduledMessages / rotationItems - a chunked GET response aborts if
//its list changes between two chunks instead of mixing old and new entries.
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

//Cap on scheduled messages, like ROTATION_ITEMS_MAX: each text is capped at 500 bytes, but the
//count wasn't - unbounded heap pressure through normal API use. Generous for a handful of upcoming
//messages.
#define SCHEDULED_MESSAGES_MAX 20
//Total characters of all scheduled message texts (and separately of all rotation texts) kept in
//RAM. 16-20 messages of 440 characters left only ~9-11 KB free (a hang while saving, a failed OTA);
//4000 keeps ~16 KB free after boot. Entries that would exceed it are rejected; a bigger list
//already on flash still loads in full.
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
//Seconds the current rotation item was actually accepted by showText(), not wall-clock seconds -
//a busy display swallows showText() calls, and a wall-clock timer skipped slow or blocked items.
//Reset when the item changes or rotation restarts.
unsigned long rotationItemAcceptedSeconds = 0;
//Every tick the current item has been current, accepted or not - only used to detect a stuck item
//(too many lines, or dynamic content grown too long) that would otherwise block the rotation
//forever. Reset with rotationItemAcceptedSeconds.
unsigned long rotationItemElapsedRealSeconds = 0;
//rotationItemStarted: a render of the current item was accepted since it became current (or since
//a temporary message displaced it). rotationItemSettled: that render finished - from then on every
//second counts, even while a clock/date item keeps refreshing. Reset with
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

//timezone.now() and timezone.tzTime(TIME_NOW, UTC_TIME) both return LOCAL time despite the name -
//real UTC needs the offset reversed by hand. Needed wherever client Unix timestamps are compared
//(scheduled messages, countdown/countup, rotation).
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
//Request that owns otaChunkBuffer/otaChunkLen while its body streams in - bodies of two requests
//can interleave on the event loop. Claimed at index==0, released by its outer handler, disconnect
//or the abandoned-upload watchdog.
AsyncWebServerRequest* otaChunkOwner = nullptr;
volatile size_t otaChunkLen = 0;
enum OtaPhase { OTA_PHASE_NONE, OTA_PHASE_BEGIN, OTA_PHASE_WRITE, OTA_PHASE_FINISH };
volatile OtaPhase otaPendingPhase = OTA_PHASE_NONE; //what loop() should do next
volatile OtaPhase otaCompletedPhase = OTA_PHASE_NONE;
volatile uint32_t otaBeginSize = 0;
//Bytes actually handed to Update.write() so far, tracked ourselves rather than trusting
//Update.progress() at finish time (see the finish-phase comparison below for why).
volatile size_t otaBytesWritten = 0;
//No shared "reject this chunk" flag: a losing concurrent request's body never touches shared
//state, and the outer handler derives its answer from "was I still the owner" plus re-checked facts
//(password, contentLength()).
volatile bool otaResultReady = false;
volatile bool otaResultSuccess = false;
String otaResultMessage = "";
volatile unsigned long otaLastActivityAt = 0; //watchdog for an abandoned upload (client vanished)
//90s: a client retrying a dropped chunk over flaky WiFi can genuinely take a while even though the
//upload itself is fine.
#define OTA_ABANDONED_TIMEOUT_MS 90000
PendingRequestSlot pendingFirmwareUploadSlot; //reused in turn by all three /api/firmware-* steps
#endif

//A browser firmware upload is running - the multi-device health check/push stay out of its way.
//Always false when web OTA is compiled out.
bool webFirmwareUpdateInProgress() {
#if WEB_FIRMWARE_UPDATE_ENABLE == true
  return otaChunkBuffer != nullptr || otaPendingPhase != OTA_PHASE_NONE;
#else
  return false;
#endif
}
#if UNIT_BOOTLOADER_ENABLE == true
//Upload couldn't be written or renamed into place. Only set by the current owner and only read by
//that owner's outer handler; reset when a new request claims ownership.
bool unitFirmwareUploadWriteFailed = false;
//Request that owns the shared unitFirmwareFile/temp-file state while its body streams in - two
//authenticated uploads would otherwise interleave their fragments into one file. Claimed at
//index==0 (after the password check), released by its outer handler or onDisconnect. A request
//rejected before claiming never touches shared state; the outer handler's answer comes from
//re-checked facts or wasOwner, not from a flag another request could write.
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
#endif

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  Wire.setClock(400000); //400kHz - units run at 16MHz with 4.7k pull-ups, comfortably support this

#if UNIT_BOOTLOADER_ENABLE == true
  //twiboot holds SCL low during a flash page write - give it more time than the worst-case
  //erase+write duration so it never gets misread as a bus error.
  Wire.setClockStretchLimit(25000);
#endif

  //Proactive bus recovery, not just reactive after a failed read - cheap, no-op if already idle.
  recoverI2CBus();

  //Optional DS3231: seeds the clock here, before initWiFi() and regardless of its outcome
  //(ntpSyncApplyResult()'s offset math). Shows UTC until the timezone rule is known, like the NTP
  //path.
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

  //Anchors the 30s readiness ceiling at boot, not at the end of the scan.
  unitReadinessStartedAt = millis();

  //The scan/readiness wait runs synchronously further down, before initWiFi() - loop() can't run
  //before setup() returns, and initWiFi()'s autoConnect() blocks.
  SerialPrintln(F(""));
  SerialPrintln(F("#######################################################"));
  SerialPrintln(F("..............Split Flap Display Starting.............."));
  SerialPrintln(F("#######################################################"));
  debugStatus = "Starting";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);

  //Config load and crash-log persistence first (no WiFi dependency), so the configured mode can
  //render as soon as units are ready, regardless of WiFi.
  debugStatus = "File System Init";
  SerialPrintln(F("DEBUG: Status = ") + debugStatus);
  initialiseFileSystem();
  loadValuesFromFileSystem();
  checkAndPersistCrashLog(); // see ServiceCrashLogFunctions.ino - must run after LittleFS mounts

  //Show the configured mode BEFORE WiFi: loop() can't run until setup() returns, and initWiFi()'s
  //autoConnect() blocks. WiFiManager's connect logic stays untouched (a custom rewrite made things
  //worse), so the readiness wait and one display update are spun synchronously here, with the
  //ceiling from UNIT_READINESS_MAX_WAIT_MS.
  //Apply the cached POSIX rule first (see cachedTimezonePosix); without one, Clock/Date show UTC
  //until the network lookup resolves.
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

    //ezTime's own sync is off (setInterval(0)); checkNtpSync() in loop() replaces it without
    //blocking.
    setInterval(0);

    //No setDebug(INFO): ezTime logs very often around WiFi hiccups.
    setDebug(NONE);

    // Try to sync with timeout (non-blocking)
    unsigned long ntpStartTime = millis();
    unsigned long ntpTimeout = 30000; // 30 second timeout

    SerialPrintln(F("DEBUG: Starting NTP sync (non-blocking, 30s timeout)"));

    // Wait with timeout; startNtpSync()/checkNtpSync() never block, only this delay(100) does.
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

    //After loadValuesFromFileSystem() (needs timezoneSetting). Fire-and-forget instead of a blocking
    //setLocation(): checkTimezoneLookup() applies it from loop() once the network is up. With a
    //cached rule the first render was already local time; only a device that never resolved its
    //timezone shows UTC until then.
    startTimezoneLookup(timezoneSetting);
    bootTimezoneLookupPending = true; //lets loop() retry on failure
    SerialPrintln(F("DEBUG: Timezone lookup started (non-blocking) for: ") + timezoneSetting);
    
    //Debug mode was already loaded at the very top of setup() - just log the already-known value.
#if DEBUG_ENABLE == true
    SerialPrintln(F("DEBUG: Debug mode: ") + String(showDebugPage ? F("enabled") : F("disabled")) + F(" (controls startup page, error status, page load log, and serial log)"));
#endif

    //Web Server Endpoint configuration
    debugStatus = "Web Server Setup";
    SerialPrintln(F("DEBUG: Status = ") + debugStatus);

    //No filesystem-wide static route: the only static asset is index.html(.gz), served below (the
    //favicon is embedded), and a catch-all exposed every config file in LittleFS without a password.

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
        //Both reads need a login - the header lets the session cookie count.
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
        html += F("    var entered = prompt('Admin password (leave empty if none is set):', '');");
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
        html += F("      alert('Wrong admin password.');");
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
      //Heap guard before LittleFSImpl::open() - a low-heap open() is a known abort() site.
      if (ESP.getFreeHeap() < 6000) {
        request->send(503, "text/plain", "Low memory, try again shortly");
        return;
      }

      //One page delivery at a time: two AsyncFileResponses streaming index.html.gz at once hung the
      //device (watchdog reset) or corrupted LittleFS's open-file list. A second visitor gets a tiny
      //page that reloads after a second. Released when the delivering connection ends; one still
      //running after 30s is closed, and the next visitor still waits for its actual disconnect.
      if (pageDeliveryActive) {
        if (millis() - pageDeliveryStartedAt >= 30000 && pageDeliveryClient != nullptr) {
          pageDeliveryClient->close(); //deferred to its next poll; its onDisconnect then releases the guard and the file
        }
        AsyncWebServerResponse *busyResponse = request->beginResponse(503, "text/html",
          String(F("<!DOCTYPE html><meta http-equiv=\"refresh\" content=\"1\"><p>...</p>")));
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
      //Reading needs the password too, unless this category is public.
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
      //inputText is shared - every mode writes its rendered content into it. The client only uses it
      //in Text mode, so sending it always is harmless.
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
      minimalDoc["routerHostname"] = routerHostname; //"set-router-hostname" action, same endpoint
      minimalDoc["setupApPasswordFixed"] = setupApFixedPassword != ""; //never the password itself
      minimalDoc["deviceRole"] = deviceRole; //user-editable, see POST /api/device-role
      minimalDoc["extensionLinkMode"] = extensionLinkMode; //same endpoint, optional 'linkMode' param
#if MQTT_ENABLE == true
      //Folded in here as well (one fetch for the System modal); never the password itself.
      minimalDoc["mqttEnabled"] = mqttEnabled;
      minimalDoc["mqttBrokerHost"] = mqttBrokerHost;
      minimalDoc["mqttBrokerPort"] = mqttBrokerPort;
      minimalDoc["mqttUsername"] = mqttUsername;
      minimalDoc["mqttPasswordSet"] = mqttPassword != "";
      minimalDoc["mqttConnected"] = mqttEnabled && mqttClient.connected();
#endif
      //One fetch for the admin view (the device takes only 5 TCP connections). Never the password,
      //only whether one is set plus the toggle states.
      minimalDoc["otaPasswordSet"] = isAdminPasswordSet();
      minimalDoc["linkPaired"] = pairedPrimaryLinkSecret != ""; //as an extension - never the secret itself
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

      //Scheduled messages have their own handler (GET /api/scheduled-messages) - building them here
      //crashed the device. Only the count.
      minimalDoc["scheduledMessageCount"] = scheduledMessages.size();

      //Rotation items likewise (stack overflow here) - the client loads them via
      //GET /api/rotation-items when the Rotation tab opens.
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

    //Separate from the GET handler on purpose: merged into one HTTP_GET|HTTP_POST registration, real
    //POSTs got the GET response.
    webServer.on("/api/settings", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      SerialPrintln(F("DEBUG: Request Post of Form Received"));

      bool submissionError = false;
      //Invalid rotation JSON; unlike submissionError the rest of the request is still processed,
      //only the response wording changes.
      bool rotationItemsUpdateRejected = false;
      //A scheduled message that can never be shown (too many lines) or is past due is rejected.
      bool scheduledMessageRejected = false;
      //Reason text for scheduledMessageRejected (too long, or past due).
      String scheduledMessageRejectionReason = "";
      //A direct text that needs too many lines (exclusive with scheduledMessageRejected).
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
      //"" is a valid value here (bare day count), not "not submitted".
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

          //HTTP POST Animation mode value
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

          //HTTP POST Countdown prefix/suffix - "" is a valid value (bare day count), so tracked via
          //hasNew* instead of the newX != "" pattern.
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

          //HTTP POST Countup prefix/suffix - same as Countdown.
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

          //HTTP POST Rotation Items (JSON array, always the whole list), bounded as a heap safeguard.
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
          displayRenderGeneration++; //see the comment on this variable

          writeFile(LittleFS, alignmentPath, alignment.c_str());
          SerialPrintln(F("Alignment Updated: ") + alignment);
        }

        //Only if a new flap speed value
        if (newFlapSpeedValue != "" && flapSpeed != newFlapSpeedValue) {
          flapSpeed = newFlapSpeedValue;

          writeFile(LittleFS, flapSpeedPath, flapSpeed.c_str());
          SerialPrintln(F("Flap Speed Updated: ") + flapSpeed);
        }

        //No displayRenderGeneration bump, like flapSpeed: only affects the timing of future writes.
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

        //Only from the countdown tab's own submission - its hidden field is form-wide, and another
        //tab would resubmit a stale "0" and wipe the stored date.
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

        //Rotation items: always the full list, and only from the rotation tab's own submission
        //(another tab would resubmit a stale "[]" and wipe the list).
        if (newDeviceModeValue == DEVICE_MODE_ROTATION && hasNewRotationItems) {
          //Invalid JSON keeps the old list, and the response says so; the rest of the request is
          //still processed. An EMPTY value is a failure too: on a fragmented heap the web server can
          //lose a form value of a few KB entirely. The web UI uses POST /api/rotation-items now.
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
              //Past due (a near deadline can pass before the request is processed) or
              //SCHEDULED_MESSAGES_MAX reached.
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
          //The mode after this request, submitted or not - the too-long check applies whenever
          //Text mode is (or stays) active.
          String effectiveDeviceMode = (newDeviceModeValue != "") ? newDeviceModeValue : deviceMode;

          //Validated BEFORE deviceMode is assigned/persisted, so a rejected text leaves the running
          //mode untouched. Too many lines must be rejected here too, or startDisplayJob() would
          //refuse it forever.
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

        //Re-sync the boot snapshot now, not only on the 30s check, so a power cut can't lose a
        //confirmed change. No-op if nothing changed.
        checkBootSnapshotFreshness();

        //scheduledMessageRejected and directTextRejected are mutually exclusive.
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

    //Own small handler (own stack frame), fetched when the Rotation tab opens. GET: the list,
    //element by element (sendJsonArrayChunked()). POST: replaces the list from a raw JSON body,
    //collected into one buffer of its final size - a form field gets grown and copied twice by the
    //library, and lists of a few KB got lost that way.
    webServer.on("/api/rotation-items", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;
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
        //A client dropping mid-body never reaches the handler - release the buffer here (a no-op
        //after a normal response).
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

    //Former /api/health, /api/heap, /api/tcp-status and /api/test in one route (?type=) - every
    //registered handler costs permanent heap.
    webServer.on("/api/diag", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      String type = request->hasParam("type") ? request->getParam("type")->value() : "";

      if (type == "health") {
        request->send(200, "text/plain", "Healthy");
        return;
      }
      //Everything but the health check needs the password - chip IDs, versions, network state.
      if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;

      if (type == "heap") {
        char buf[440]; //worst-case length checked by hand incl. the longest wifiDisconnectReason string, +20 for iramFree
        //flashReal = the chip's own SPI ID, flashConfigured = what the eesz layout assumes (a
        //mismatch means a wrong build). rtc/rtcOsf show the optional DS3231's state. resetReason is
        //the SDK's record of the last reset - unlike the crash reporter it also covers clean resets
        //(external, brownout, hardware watchdog). uptimeSec reveals any reboot between two polls.
        //The WiFi fields make AUTH_EXPIRE/ASSOC_EXPIRE disconnects visible without a serial capture.
        const char* lastDisconnectReasonForDiag = wifiDisconnectCount > 0 ? lastWifiDisconnectReason.c_str() : "none";
        long lastDisconnectSecAgo = wifiDisconnectCount > 0 ? (long)((millis() - lastWifiDisconnectAt) / 1000) : -1;
        //iramFree: second heap in IRAM (OTA buffers), 0 if the build has none (mmu=3232 since 5.0.1).
        uint32_t iramFree = 0;
#ifdef UMM_HEAP_IRAM
        {
          HeapSelectIram iramHeap;
          iramFree = ESP.getFreeHeap();
        }
#endif
        snprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(F("free=%u frag=%u%% maxBlock=%u flashReal=%u flashConfigured=%u flashChipId=0x%08X espChipId=0x%08X sdk=%s core=%s rtc=%s rtcOsf=%u resetReason=%s uptimeSec=%lu wifiDisconnects=%lu lastWifiDisconnectReason=%s lastWifiDisconnectSecAgo=%ld iramFree=%u")),
          ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize(),
          ESP.getFlashChipRealSize(), ESP.getFlashChipSize(), ESP.getFlashChipId(), ESP.getChipId(),
          ESP.getSdkVersion(), ESP.getCoreVersion().c_str(),
          rtcPresent ? "present" : "absent", rtcOscillatorStopFlag ? 1 : 0,
          crashResetReasonText(ESP.getResetInfoPtr()->reason), millis() / 1000,
          wifiDisconnectCount, lastDisconnectReasonForDiag, lastDisconnectSecAgo, iramFree);
        request->send(200, "text/plain", buf);
        return;
      }

      if (type == "tcp") {
        //MEMP_NUM_TCP_PCB=5 is no hard cap: with MEMP_MEM_MALLOC=1 the PCBs come from malloc(), so
        //more than 5 connections can be in flight (a 15-connection burst got well past 5). The real
        //defense is the connection gate (connectionGateRejectedCount).
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

    // Crash log (ServiceCrashLogFunctions.ino), one JSON object per line, oldest first. GET and
    // DELETE in one registration; clearing needs DELETE, not GET (CSRF).
    webServer.on("/api/crash-log", ASYNC_HTTP_GET | ASYNC_HTTP_DELETE, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSystemPublic)) return;
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
      if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return; //drives the I2C bus
      SerialPrintln(F("DEBUG: I2C scan requested"));
      //Deferred to loop().
      if (pendingLegacyI2CScanSlot.request != nullptr) {
        request->send(503, "text/plain", "Another I2C scan is already in progress, try again shortly");
        return;
      }
      pendingLegacyI2CScanSlot.accept(request);
    });

#if UNIT_BOOTLOADER_ENABLE == true
    // Registered before "/api/units": canHandle() also matches "<uri>/..." and the first matching
    // handler wins, so "/api/units" would swallow "/api/units/firmware*".

    // Admin: upload a compiled Unit.ino firmware image (raw AVR .bin, NOT ESPMaster's own) to be
    // pushed to a unit via /api/units/firmware-push. Requires X-OTA-Password, same as /api/firmware.
    webServer.on("/api/units/firmware", ASYNC_HTTP_POST,
      [](AsyncWebServerRequest * request) {
        //Ownership captured before releasing it - the request-scoped way to know whether THIS
        //request's body owned the shared file state.
        bool wasOwner = (unitFirmwareUploadOwner == request);
        if (wasOwner) {
          unitFirmwareUploadOwner = nullptr;
        }
        //Password re-checked here statelessly, never via a shared flag.
        if (isAdminPasswordSet()) {
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
          //Password first, before touching any upload state - an unauthenticated request must not
          //disturb an upload in progress. On failure return without touching shared state.
          if (isAdminPasswordSet()) {
            if (!requestIsAuthenticated(request)) { //password header or web UI login session
              SerialPrintln(F("Unit firmware upload rejected: invalid password"));
              return;
            }
          }

          //A second authenticated upload while another owns the state: just return.
          if (unitFirmwareUploadOwner != nullptr && unitFirmwareUploadOwner != request) {
            SerialPrintln(F("Unit firmware upload rejected: another upload is already in progress"));
            return;
          }
          unitFirmwareUploadOwner = request;
          //New ownership: clear a failure left by a previous owner that vanished before its outer
          //handler ran.
          unitFirmwareUploadWriteFailed = false;
          //Release the claim if the connection drops before the outer handler runs, or every later
          //upload would be rejected as busy.
          request->onDisconnect([request]() {
            if (unitFirmwareUploadOwner == request) {
              unitFirmwareUploadOwner = nullptr;
            }
          });

          //Credentials confirmed and ownership claimed - now touch the shared state. Close a handle a
          //vanished previous upload left open (it costs heap).
          if (unitFirmwareFile) {
            unitFirmwareFile.close();
          }
          //Also remove its leftover temp file.
          if (LittleFS.exists(unitFirmwareTmpPath)) {
            LittleFS.remove(unitFirmwareTmpPath);
          }

          SerialPrintln(F("DEBUG: Unit firmware upload starting: ") + filename);
          //Write to a temp path and rename() it into place only when complete - a failed upload
          //never damages the stored firmware.
          unitFirmwareFile = LittleFS.open(unitFirmwareTmpPath, "w");
          if (!unitFirmwareFile) {
            SerialPrintln(F("Unit firmware upload: failed to open ") + String(unitFirmwareTmpPath) + F(" for writing"));
            unitFirmwareUploadWriteFailed = true;
          }
        }

        //Per-fragment ownership check: a rejected request's later fragments would otherwise be
        //written into the winning request's file.
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
          //Re-authorized before replacing the stored file - a password change or revoked session
          //during the transfer must stop it.
          if (!unitFirmwareUploadWriteFailed && isAdminPasswordSet() && !requestIsAuthenticated(request)) {
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

    //Pushes the stored unit firmware to one unit over I2C (twiboot), or only probes its address -
    //both deferred to loop(), one registration dispatched by 'action'. Separate from
    //"/api/units/firmware" (a multipart upload route with a different callback signature).
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

      //Bounded by UNITS_AMOUNT_MAX, not unitsAmount: this endpoint exists to reach a unit stuck in
      //the bootloader, which drops out of unitsAmount. The bootloader probe is the real safety net.
      int unitId = idValue.toInt();
      if (unitId < 0 || unitId >= UNITS_AMOUNT_MAX) {
        request->send(400, "text/plain", "Unit id out of range. Valid range: 0 to " + String(UNITS_AMOUNT_MAX - 1));
        return;
      }

      if (action == "push") {
        if (isAdminPasswordSet()) {
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
        //Always needs the password, not accessControlUnitsPublic: "probe" reboots the unit's app
        //just like "push".
        if (isAdminPasswordSet()) {
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

    // GET: live status + calibration offset of every unit (deferred to loop()). POST ?action=
    // rescan/offset/reset. One registration; both branches are small (unlike /api/settings).
    webServer.on("/api/units", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        //Readable without password: a primary polls its extensions' unit count here without
        //credentials.
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
        if (rejectIfPasswordRequired(request, accessControlUnitsPublic)) return; //drives the I2C bus
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
        //The reset sequence bypasses showText()/lastWrittenText, so invalidate the cache.
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
        //Ownership captured before releasing it, as for /api/units/firmware.
        bool wasOwner = (webFileUploadOwner == request);
        if (wasOwner) {
          webFileUploadOwner = nullptr;
        }
        //Password re-checked here statelessly, never via a shared flag.
        if (isAdminPasswordSet()) {
          if (!requestIsAuthenticated(request)) { //password header or web UI login session
            request->send(401, "text/plain", "Invalid password");
            return;
          }
        }
        if (!wasOwner) {
          //Tell "own filename invalid" from "lost the ownership race" from this request's own file
          //param, not a shared flag - avoids a double response (400 and 503).
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
          //Password first, as for /api/units/firmware: an unauthenticated request must not disturb an
          //upload in progress. On failure just return.
          if (isAdminPasswordSet()) {
            if (!requestIsAuthenticated(request)) { //password header or web UI login session
              SerialPrintln(F("Web file upload rejected: invalid password"));
              return;
            }
          }

          //A second authenticated upload while another owns the state: just return.
          if (webFileUploadOwner != nullptr && webFileUploadOwner != request) {
            SerialPrintln(F("Web file upload rejected: another upload is already in progress"));
            return;
          }
          webFileUploadOwner = request;
          //New ownership: clear a failure left by a vanished previous owner.
          webFileUploadWriteFailed = false;
          //Release the claim if the connection drops before the outer handler runs.
          request->onDisconnect([request]() {
            if (webFileUploadOwner == request) {
              webFileUploadOwner = nullptr;
            }
          });

          //Credentials confirmed and ownership claimed - now touch the shared state, closing anything a
          //vanished previous upload left open.
          if (webUploadFile) {
            webUploadFile.close();
          }
          if (webUploadTmpPath != "" && LittleFS.exists(webUploadTmpPath)) {
            LittleFS.remove(webUploadTmpPath);
          }

          //Only "index.html.gz" is valid (the whole UI, served with Content-Encoding: gzip). An
          //invalid name gives up ownership at once, so every later fragment fails the ownership
          //check too. No response here - the outer handler sends the one reply.
          if (filename != "index.html.gz") {
            SerialPrintln(F("Web file upload rejected: filename not in allowlist: ") + filename);
            webFileUploadOwner = nullptr;
            return;
          }

          SerialPrintln(F("DEBUG: Web file upload starting: ") + filename);
          //Temp file + atomic rename(), as for /api/units/firmware - a failed upload never truncates
          //the live UI file.
          webUploadTmpPath = "/" + filename + ".tmp";
          webUploadFile = LittleFS.open(webUploadTmpPath, "w");
          if (!webUploadFile) {
            SerialPrintln(F("Web file upload: failed to open ") + webUploadTmpPath + F(" for writing"));
            webFileUploadWriteFailed = true;
          }
        }

        //Per-fragment ownership check, as for /api/units/firmware.
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
          //Re-authorized right before the swap, as for /api/units/firmware.
          if (!webFileUploadWriteFailed && isAdminPasswordSet() && !requestIsAuthenticated(request)) {
            SerialPrintln(F("Web file upload: no longer authorized at the end - discarding it"));
            webFileUploadWriteFailed = true;
          }
          if (webFileUploadWriteFailed) {
            LittleFS.remove(webUploadTmpPath); //don't leave a corrupt temp file lying around
            SerialPrintln(F("DEBUG: Web file upload failed, previous file (if any) left untouched"));
          } else {
            //Rename the new file into place first and only then remove the old "/index.html" - the
            //other order could leave no UI file at all after a power cut.
            if (LittleFS.rename(webUploadTmpPath, "/" + filename)) {
              SerialPrintln(F("DEBUG: Web file upload finished: ") + filename + F(", total bytes: ") + String(index + len));
              //A leftover "/index.html" would take precedence over "/index.html.gz".
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
      if (!isAdminPasswordSet()) {
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
      otaChunkOwner = nullptr;
      otaChunkLen = 0;
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
        //Ownership captured before releasing it (see otaChunkOwner).
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
          //This body never claimed the buffer: either Content-Length exceeded OTA_CHUNK_SIZE or
          //another chunk was in flight - told apart via contentLength(), not a shared flag.
          if (request->contentLength() > OTA_CHUNK_SIZE) {
            request->send(400, "text/plain", "Chunk too large");
          } else {
            request->send(503, "text/plain", "Another chunk upload is already in progress, try again shortly");
          }
          return;
        }
        //An overlapping request is rejected, not allowed to overwrite the pending one.
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
          //Auth checked here too: this body handler writes into the shared otaChunkBuffer as data
          //arrives, before the request handler runs. On failure just return - this request never
          //becomes otaChunkOwner.
          if (!checkOtaPassword(request)) {
            return;
          }
          //Reject an overlapping chunk before it touches otaChunkLen/otaChunkBuffer.
          if (otaPendingPhase != OTA_PHASE_NONE) {
            return;
          }
          //Also reject a second body still mid-transfer - otaPendingPhase only changes once a body is
          //complete, so two interleaved bodies would pass both checks (see otaChunkOwner).
          if (otaChunkOwner != nullptr && otaChunkOwner != request) {
            return;
          }
          if (otaChunkBuffer == nullptr || total > OTA_CHUNK_SIZE) {
            //Invalid chunk (no upload started, or too large): never claim ownership; the outer
            //handler derives the reason from contentLength().
            return;
          }
          otaChunkOwner = request;
          otaChunkLen = 0;
          request->onDisconnect([request]() {
            if (otaChunkOwner == request) {
              otaChunkOwner = nullptr;
              otaChunkLen = 0; //discard an incomplete body; completed chunks already released ownership
            }
          });
        }
        //Per-fragment ownership check: a request that never claimed (or lost) ownership is dropped.
        if (otaChunkOwner != request) {
          return;
        }
        //Refreshed on every fragment, so the abandoned-upload watchdog can't free otaChunkBuffer
        //between two fragments of a slow but active body.
        otaLastActivityAt = millis();
        if (otaChunkBuffer == nullptr || otaChunkLen + len > OTA_CHUNK_SIZE) {
          //Never overflow the buffer: give up ownership, the outer handler then answers 503.
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
      //A chunk body may still own the buffer before its request handler queues the write.
      if (otaPendingPhase != OTA_PHASE_NONE || otaChunkOwner != nullptr) {
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
    
    //POST, not GET (also reset-units/ota/reset-wifi): a GET link can be triggered cross-site.
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
      checkBootSnapshotFreshness(); //keep the snapshot fresh before rebooting
      isPendingReboot = true;
    });

    //Like GET /api/rotation-items: own small JsonDocument, no yield() in the loop (it overflowed the
    //callback's stack). Separate from DELETE below.
    webServer.on("/api/scheduled-messages", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;
      //An incomplete list isn't served as the whole schedule; loop() keeps retrying the load.
      if (!scheduledMessagesComplete) {
        request->send(503, "text/plain", "Scheduled messages not fully loaded (low memory), try again shortly");
        return;
      }
      //Element by element (sendJsonArrayChunked()).
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

    //Lightweight text push for automations (webhooks, Home Assistant, shortcuts): a message and an
    //optional hold time instead of the full /api/settings form.
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

      //Checked up front: the permanent branch ignores showText()'s result, so a too-long text would
      //get "OK" and be retried forever.
      if (isMessageTooLongToRender(newText)) {
        request->send(400, "text/plain", "text needs more lines than fit to display - shorten it or add fewer line breaks");
        return;
      }

      if (durationSeconds > 0) {
        //Temporary: deviceMode/inputText stay untouched (like a non-indefinite scheduled message);
        //after the hold the normal mode loop takes over. Nothing retries this call, so a busy display
        //means the message is lost - reported as such.
        if (!showText(newText, (int)(durationSeconds * 1000))) {
          request->send(503, "text/plain", "Nothing was displayed - the display is busy right now. Try again shortly.");
          return;
        }
      } else {
        //Permanent: switches to Text mode like the main form. A busy display isn't a loss - the
        //mode loop keeps retrying.
        if (deviceMode != DEVICE_MODE_TEXT) {
          deviceMode = DEVICE_MODE_TEXT;
          writeFile(LittleFS, deviceModePath, deviceMode.c_str());
        }
        inputText = newText;
        showText(newText);
        checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
      }

      request->send(200, "text/plain", "OK");
    });

    //Extension side: receives a primary's sliced, aligned segment plus animation context (see
    //ServiceExtensionFunctions.ino). Plain form-urlencoded POST.
    webServer.on("/api/extension-display", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      //Pairing request from a primary (runPendingExtensionPairing()) - same route to save heap. Needs
      //this device's admin password; accepted in any role, so pairing can precede the role switch.
      if (request->hasParam("pairSecret", true)) {
        if (isAdminPasswordSet()) {
          String providedPassword = request->hasHeader("X-OTA-Password") ? request->getHeader("X-OTA-Password")->value() : "";
          int passwordCheck = checkAdminPassword(request, providedPassword);
          if (passwordCheck != PASSWORD_OK) {
            request->send(passwordCheck == PASSWORD_THROTTLED ? 429 : 401, "text/plain", "Invalid password");
            return;
          }
        }
        String secret = request->getParam("pairSecret", true)->value();
        uint8_t secretBytes[32];
        if (!hexToBytes(secret, secretBytes, sizeof(secretBytes))) {
          request->send(400, "text/plain", F("'pairSecret' must be 64 hex characters"));
          return;
        }
        pairedPrimaryLinkSecret = secret;
        writeFile(LittleFS, pairedPrimaryLinkSecretPath, pairedPrimaryLinkSecret.c_str());
        resetLinkReplayState();
        checkBootSnapshotFreshness();
        SerialPrintln(F("Paired with a primary device"));
        request->send(200, "text/plain", F("Paired"));
        return;
      }
      //Display push: signed with the pairing secret, never the admin password.
      int linkCheck = checkExtensionLinkSignature(request);
      if (linkCheck == 409) {
        request->send(409, "text/plain", currentLinkNonce());
        return;
      }
      if (linkCheck != 200) {
        request->send(linkCheck, "text/plain", linkCheck == 403 ? F("Not paired with a primary device") : F("Invalid signature"));
        return;
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
      //Optional (older primaries don't send it): how far the primary's line already is.
      String startOffsetMsRaw = request->hasParam("startOffsetMs", true) ? request->getParam("startOffsetMs", true)->value() : "0";
      //Same bounds as on the primary - unchecked values could schedule a start hours ahead.
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
      //Clamped, not rejected: a late push is still valid, every unit is simply due.
      startOffsetMs = min(startOffsetMs, staggerMs * combinedWidth);

      //Queued for loop() and acknowledged right away - see pendingExtensionDisplayQueued's comment.
      pendingExtensionDisplaySegment = segment;
      pendingExtensionDisplayAnimationMode = animationMode;
      pendingExtensionDisplayStaggerMs = staggerMs;
      pendingExtensionDisplayGlobalOffset = globalOffset;
      pendingExtensionDisplayCombinedWidth = combinedWidth;
      pendingExtensionDisplayStartOffsetMs = startOffsetMs;
      pendingExtensionDisplayQueued = true;
      request->send(200, "text/plain", currentLinkNonce()); //the nonce the next push has to name
    });

    //Primary-side extension host list: GET returns hosts plus last health-check status, POST
    //replaces the whole list.
    webServer.on("/api/extension-devices", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      //Changing the list always needs the password: pairing sends the link secret to a listed host,
      //and an added host would receive the display content.
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
          //Pairing outcome and whether the last push was refused as unsigned/unknown (not paired).
          switch (extensionDevices[i].PairState) {
            case EXTENSION_PAIR_PENDING: entry["pairing"] = F("pending"); break;
            case EXTENSION_PAIR_OK: entry["pairing"] = F("ok"); break;
            case EXTENSION_PAIR_WRONG_PASSWORD: entry["pairing"] = F("wrongPassword"); break;
            case EXTENSION_PAIR_THROTTLED: entry["pairing"] = F("throttled"); break;
            case EXTENSION_PAIR_FAILED: entry["pairing"] = F("failed"); break;
            default: entry["pairing"] = ""; break;
          }
          entry["linkRejected"] = extensionDevices[i].LinkRejected;
        }
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // HTTP_POST
      if (request->hasParam("action") && request->getParam("action")->value() == "pair") {
        //Queued for loop() (runPendingExtensionPairing()). 'password' is the extension's admin
        //password, kept in RAM only until that request is sent; empty if it has none.
        String indexRaw = request->hasParam("index", true) ? request->getParam("index", true)->value() : "";
        int index = isNumber(indexRaw) ? indexRaw.toInt() : -1;
        if (index < 0 || index >= extensionDeviceCount) {
          request->send(400, "text/plain", F("'index' must name a configured extension device"));
          return;
        }
        if (pendingExtensionPairing) {
          request->send(409, "text/plain", F("A pairing is already in progress"));
          return;
        }
        pendingExtensionPairingIndex = index;
        pendingExtensionPairingPassword = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
        extensionDevices[index].PairState = EXTENSION_PAIR_PENDING;
        pendingExtensionPairing = true;
        request->send(202, "text/plain", F("Pairing started"));
        return;
      }
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

    //Device role - own small endpoint instead of growing /api/settings.
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
        //Changes the combined width and what extensions get - see displayRenderGeneration.
        displayRenderGeneration++;
      }
      request->send(200, "text/plain", "OK");
    });

#if WIFI_USE_DIRECT == false
    //All five former WiFi endpoints in one route: GET by ?type=, POST by ?action=. One registration
    //for both methods - separate GET/POST registrations on this path routed GETs to the POST handler.
    webServer.on("/api/wifi", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        //Gated like the POST actions - scan results and the current SSID/IP aren't for anyone on the
        //LAN; the session that started the scan already has the password.
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

            //Dedupe by SSID (one entry per BSSID/channel/repeater), keeping the strongest.
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
        //Only sets a flag; loop() starts WiFi.scanNetworksAsync().
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
        //Connects without rebooting - the current connection drops at once;
        //checkWifiConnectAttempt() reverts on failure.
        if (wifiConnectPhase != WIFI_CONNECT_NONE) {
          request->send(409, "text/plain", "A connection attempt is already in progress");
          return;
        }
        if (!request->hasParam("ssid", true) || request->getParam("ssid", true)->value() == "") {
          request->send(400, "text/plain", "'ssid' is required");
          return;
        }

        //Captured now - by the time loop() calls WiFi.begin(), SSID()/psk() already show the new ones.
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
        //Same registration to save heap, gated by accessControlWifiPublic like the other actions.
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
        //MDNS.begin() only runs once in setup(); calling it again without end() keeps the old name.
        MDNS.end();
        if (MDNS.begin(mdnsHostname.c_str())) {
          SerialPrintln(F("mDNS hostname changed - now reachable as ") + mdnsHostname + F(".local"));
        } else {
          SerialPrintln(F("mDNS hostname changed but responder failed to restart"));
        }
#endif

        checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
        request->send(200, "text/plain", "Hostname updated");
        return;
      }

      if (action == "set-router-hostname") {
        //The router's full DNS name for this device (RequestGuard accepts it in the Host header).
        //Empty clears it. A full hostname without port.
        String newRouterHostname = request->hasParam("hostname", true) ? request->getParam("hostname", true)->value() : "";
        newRouterHostname.trim();
        if (newRouterHostname != "" && (newRouterHostname.indexOf(':') != -1 || !isValidExtensionHost(newRouterHostname))) {
          request->send(400, "text/plain", F("Invalid hostname - a full DNS name like split-flap.fritz.box, or empty to clear"));
          return;
        }
        routerHostname = newRouterHostname;
        writeFile(LittleFS, routerHostnamePath, routerHostname.c_str());
        checkBootSnapshotFreshness();
        request->send(200, "text/plain", "Router hostname updated");
        return;
      }

      if (action == "set-setup-ap-password") {
        //Fixed setup hotspot password (see setupApFixedPassword). Empty switches back to a random
        //one on the display. Checked here because WiFiManager refuses to open the hotspot at all
        //with a passphrase outside WPA2's 8-63 characters.
        String newPassword = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
        bool valid = newPassword == "" || (newPassword.length() >= 8 && newPassword.length() <= 63);
        for (unsigned int i = 0; valid && i < newPassword.length(); i++) {
          valid = newPassword[i] >= 0x20 && newPassword[i] <= 0x7E;
        }
        if (!valid) {
          request->send(400, "text/plain", F("Invalid password - 8-63 printable ASCII characters, or empty for a random one on the display"));
          return;
        }
        setupApFixedPassword = newPassword;
        writeFile(LittleFS, setupApFixedPasswordPath, setupApFixedPassword.c_str());
        checkBootSnapshotFreshness();
        request->send(200, "text/plain", F("Setup hotspot password updated"));
        return;
      }

      request->send(400, "text/plain", F("Unknown 'action' - use reset, scan, connect, set-hostname, set-router-hostname, or set-setup-ap-password"));
    });
#endif

    //No GET - a password is never returned (the toggle states come with GET /api/settings). POST
    //changes the password and/or toggles and always needs the CURRENT password - this endpoint is
    //never toggleable-public, or "settings public" could reopen every category, firmware included.
    //Web UI login (see ServiceAccessControlFunctions.ino): GET /api/session is public; it only says
    //whether the page should show its login dialog.
    webServer.on("/api/session", ASYNC_HTTP_GET, [](AsyncWebServerRequest * request) {
      String json = String(F("{\"loginRequired\":")) + (isLoginRequired() ? F("true") : F("false")) +
        F(",\"loggedIn\":") + (isAdminPasswordSet() && requestIsAuthenticated(request) ? F("true") : F("false")) + F("}");
      request->send(200, "application/json", json);
    });

    webServer.on("/api/login", ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      //Same CSRF reasoning as requestIsAuthenticated(): only the web UI itself logs in this way.
      if (!request->hasHeader("X-Requested-With") || request->getHeader("X-Requested-With")->value() != "splitflap") {
        request->send(403, "text/plain", "Missing X-Requested-With header");
        return;
      }
      if (!isAdminPasswordSet()) {
        request->send(200, "text/plain", "No password set");
        return;
      }
      //Slows down guessing - a wrong attempt blocks that sender's further attempts for 2 seconds.
      String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
      int passwordCheck = checkAdminPassword(request, password);
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
      //Saved before answering (see /api/ota-password); the cookie is cleared either way.
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
      int passwordCheck = checkAdminPassword(request, currentPasswordValue);
      if (passwordCheck == PASSWORD_THROTTLED) {
        request->send(429, "text/plain", "Too many attempts, wait a moment");
        return;
      }
      if (passwordCheck == PASSWORD_WRONG) {
        SerialPrintln(F("OTA password/access-control change rejected: current password incorrect"));
        request->send(401, "text/plain", "Current password is incorrect");
        return;
      }

      //newPassword is optional: empty switches protection off completely, absent leaves the password
      //unchanged (so toggle-only updates needn't resend it).
      if (request->hasParam("newPassword", true)) {
        //Empty switches protection off; anything else needs 8+ characters. Checked before any
        //session is revoked.
        size_t newPasswordLength = request->getParam("newPassword", true)->value().length();
        if (newPasswordLength > 0 && newPasswordLength < 8) {
          request->send(400, "text/plain", F("New password must be at least 8 characters (or empty to switch protection off)"));
          return;
        }
        //Sessions are revoked and saved BEFORE the new password, so a power cut can't leave old
        //sessions on flash that a reboot would accept.
        endAllLoginSessions();
        if (!saveLoginSessionsNow()) {
          request->send(500, "text/plain", "Could not revoke existing sessions - password not changed");
          return;
        }
        if (!setAdminPassword(request->getParam("newPassword", true)->value())) {
          request->send(500, "text/plain", "Could not save the new password - password not changed (existing sessions were ended)");
          return;
        }
        SerialPrintln(!isAdminPasswordSet() ? F("OTA password cleared - protection now globally OFF") : F("OTA password changed"));
      }

      //Toggles follow checkbox semantics: an absent param means false, not "unchanged". The
      //frontend always sends the full set.
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

      //A power cut right after a password change must not bring the old password back.
      checkBootSnapshotFreshness();
      request->send(200, "text/plain", "Security settings updated");
    });

    webServer.on("/api/timezone", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlTimezonePublic)) return;
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

      //isTimezoneLookupBusy() covers a previous request still in its async lookup;
      //timezoneLookupDeferredRequest covers the gap until loop() has saved and answered it - so a new
      //request can't overwrite pendingTimezoneValue before the first result is saved.
      if (pendingTimezoneSlot.request != nullptr || isTimezoneLookupBusy() || timezoneLookupDeferredRequest != nullptr) {
        request->send(503, "text/plain", "Another timezone change is already in progress, try again shortly");
        return;
      }

      //Deferred to loop() (non-blocking lookup, ServiceAsyncTimeSyncFunctions.ino).
      pendingTimezoneValue = newTimezoneValue;
      pendingTimezoneSlot.accept(request);
    });

#if MQTT_ENABLE == true
    //Own registration for this settings category (each costs a little permanent heap).
    webServer.on("/api/mqtt", ASYNC_HTTP_GET | ASYNC_HTTP_POST, [](AsyncWebServerRequest * request) {
      if (request->method() == ASYNC_HTTP_GET) {
        if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;
        JsonDocument doc;
        doc["mqttEnabled"] = mqttEnabled;
        doc["mqttBrokerHost"] = mqttBrokerHost;
        doc["mqttBrokerPort"] = mqttBrokerPort;
        doc["mqttUsername"] = mqttUsername;
        //The password is never sent back, only whether one is set.
        doc["mqttPasswordSet"] = mqttPassword != "";
        doc["mqttConnected"] = mqttEnabled && mqttClient.connected();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
        return;
      }

      // request->method() == HTTP_POST
      if (rejectIfPasswordRequired(request, accessControlSettingsPublic)) return;

      //Validate everything into temporaries first: a partial update (host applied, port rejected)
      //would skip applyMqttSettingsChange() and leave PubSubClient pointing into mqttBrokerHost's old,
      //possibly reallocated buffer. All or nothing, then one applyMqttSettingsChange().
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
      //Present = set, absent = unchanged, empty = clear (like newPassword).
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
      checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
      request->send(200, "text/plain", "MQTT settings updated");
    });
#endif

    //No GET: the quiet-hours fields come with GET /api/settings. A second route here broke
    ///api/settings on this heap-constrained device (every handler costs permanent heap).
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
      checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
      request->send(200, "text/plain", "Quiet Hours updated");
    });

    //Without this, ESPAsyncWebSrv's catch-all returns 500 instead of 404. Same low-heap guard as
    //GET "/".
    webServer.onNotFound([](AsyncWebServerRequest * request) {
      if (ESP.getFreeHeap() < 6000) {
        request->send(503, "text/plain", "Low memory, try again shortly");
        return;
      }
      request->send(404, "text/plain", "Not Found");
    });

    delay(250);
    //Before the server accepts connections - see ServiceMemoryReserveFunctions.cpp.
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
    //Config load and crash-log persistence already ran before initWiFi().

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

//Adaptive unit-readiness wait instead of a fixed 30s. Called from setup() (synchronously, right
//before initWiFi(), whose autoConnect() blocks - loop() can't run before setup() returns) and from
//loop() (a no-op once DONE), so the first display doesn't wait for WiFi.
void tickUnitReadiness() {
  if (unitReadinessPhase == UNIT_READINESS_SCAN) {
    //One scan to learn which addresses exist, then cheap per-unit polling instead of repeated
    //64-address sweeps.
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
      //Nothing found - nothing to wait for (showText() is a no-op with 0 units).
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
        //No response counts as not ready - a unit just starting or in calibrate() can miss a poll.
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

//Throttled check (BOOT_SNAPSHOT_CHECK_INTERVAL_MS): rewrites the boot snapshot only if something
//changed, bounding staleness after a crash or power loss without wearing the slow flash. Also
//called right before every intentional reboot.
unsigned long lastBootSnapshotCheckAt = 0;
void checkBootSnapshotFreshness() {
  String currentSignature = computeSettingsSignature();
  if (currentSignature != lastSavedSettingsSignature) {
    saveBootSnapshot(); //also updates lastSavedSettingsSignature
  }
}

//One tick of the mode-driven display update (scheduled messages, countdown/countup, rotation, then
//showText() for the active mode). Also run once from setup() before initWiFi() - see
//tickUnitReadiness(). WiFi scan/connect and MQTT checks stay in loop().
void performOneSecondDisplayTick() {
  //An extension's content comes only from POST /api/extension-display - skip the content loop. Its
  //web UI/API stay fully usable.
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
      //rotationCurrentAlignment ("" = inherit) is set by checkRotation(). A second counts toward the
      //item's duration only once its render has SETTLED - showText() returns true when a job merely
      //starts, and flap movement isn't reading time. A temporary message (finalPauseMs > 0) displaces
      //the item; it then has to render and settle again.
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
  //At most ~1000 passes per second. Unthrottled, loop() ran 20000+ times a second through ~30 mostly idle
  //calls and kept evicting the network and file system code from the flash cache: depending on where
  //the linker happened to place it, page delivery got up to 10x slower.
  delay(1);

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

  //I2C requests deferred by the web handlers (see PendingRequestSlot) - served first.
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
    //Recalibration re-homes the unit - invalidate the display cache.
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
  runPendingExtensionPairing();

  //Login sessions created/renewed/ended by web requests - persisted here, not in the callbacks.
  saveLoginSessionsIfDirty();

  //Finishes a POST /api/timezone: the non-blocking lookup (checkTimezoneLookup(), already run this
  //tick) spans several ticks, so the request is held in timezoneLookupDeferredRequest meanwhile.
  if (pendingTimezoneSlot.request != nullptr && timezoneLookupDeferredRequest == nullptr && !isTimezoneLookupBusy()) {
    timezoneLookupDeferredRequest = pendingTimezoneSlot.take(timezoneLookupDeferredFlag);
    //Snapshot now (see timezoneLookupDeferredValue).
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
      //The rule is resolved now - cancel any pending boot retry.
      bootTimezoneRuleResolved = true;
      checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
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

  //Consumes the boot lookup's (or its retry's) result once - only a completion without an HTTP
  //request; a POST /api/timezone result is handled above.
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

  //Fires the scheduled retry when due, but steps aside while a real /api/timezone request is
  //pending.
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
      //Light 3-byte read by default; ?detail=1 adds phase/progress and the diagnostic counters.
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
        //255/255/255 = firmware without version reporting (idle-bus read) - reported as null.
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

    //Both 4KB OTA buffers (Update's sector buffer and otaChunkBuffer) go to a second IRAM heap if
    //the build has one - uploads start from the open web UI, when DRAM is tightest. The heap
    //selection is global, so these scopes stay yield-free and allocate nothing else. With mmu=3232
    //(since 5.0.1) there is no IRAM heap and HeapSelectIram is a no-op; free()/delete[] find the
    //right heap by address.
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
      //Allocated only while uploading - free heap is tight.
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
        Update.end(false); //release Update's "running" lock (see below)
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
    //OTA_PHASE_WRITE is only set once the chunk's request was confirmed as the buffer's owner.
    bool ok = (Update.write(otaChunkBuffer, otaChunkLen) == otaChunkLen);
    if (!ok) {
      SerialPrintln(F("Firmware upload: chunk write failed"));
      Update.printError(Serial);
      //end() is still needed (result ignored): begin() refuses while the previous update is marked
      //"running", and only end() clears that.
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
    //Verify every declared byte arrived: end(true)'s evenIfRemaining skips the library's
    //isFinished() check, so an early finish would activate a truncated image. evenIfRemaining stays
    //true because begin() rounds the target up to a full sector. Counted with otaBytesWritten, not
    //Update.progress() - that only advances per flushed 4096-byte sector, and the last partial
    //sector is only flushed inside end().
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
    //A body that claimed ownership and then vanished would block otaChunkOwner forever - released
    //by the same watchdog.
    otaChunkOwner = nullptr;
  }

  // Separate from the block above: flash result and deferred request can be ready in either
  // order. Gated on otaResultReady alone - the reboot must happen even if the client already
  // disconnected; only sending the response depends on the request.
  if (otaResultReady) {
    if (otaResultSuccess && otaCompletedPhase == OTA_PHASE_FINISH) {
      SerialPrintln(F("Firmware update successful, rebooting..."));
      checkBootSnapshotFreshness(); //keep the snapshot fresh before rebooting
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
    //Runs to completion even if the client left (see the onDisconnect handler).
    bool success = pushFirmwareToUnit(unitIndex, resultJson);
    //Invalidate the display cache either way: the push already rebooted the unit into the
    //bootloader, success or not.
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

    //Same collision guard as pushFirmwareToUnit(): this probe isn't read-only -
    //requestUnitEnterBootloader() reboots whatever answers there and twibootReadChipInfo() writes
    //first. Also checked against the fixed RTC collision (isReservedI2CAddress()).
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
      //Invalidate the display cache - the unit's app is rebooted either way (not reached for a
      //collision, which never sends the command).
      displayRenderGeneration++;

      //Same retry/backoff as pushFirmwareToUnit(); probes only unitIndex's own address.
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

      //Reads twiboot's version string (SF-TWIBOOT vX.Y) while in the bootloader - shows which build
      //the unit was flashed with.
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

      //Explicitly return to the app - twiboot's ~1000ms idle timeout wasn't reliable (a unit once
      //stayed 44+s in the bootloader).
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

  //Reboot from loop() - rebooting inside a request handler would never send the response
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

    checkBootSnapshotFreshness(); //keep the snapshot fresh before rebooting
    isPendingReboot = true;
    return;
  }
#endif

  //isWifiConfigured only latches true once in initWiFi(); if the first connect failed (e.g. router
  //down during boot), the device would show "OFFLINE" until a power cycle. So "OFFLINE" is a
  //transient overlay (deviceMode untouched), and the STA connection is retried in the background
  //(WiFiManager's portal has given up by now); on success the device reboots once to run the
  //normal boot sequence.
  if (!isWifiConfigured) {
    showText("OFFLINE", 0, ALIGNMENT_MODE_CENTER);
    delay(100);

    if (millisReached(lastOfflineReconnectAttempt + OFFLINE_RECONNECT_RETRY_MS)) {
      lastOfflineReconnectAttempt = millis();
      SerialPrintln(F("Still offline - retrying WiFi with the stored credentials..."));
      WiFi.mode(WIFI_STA); //in case autoConnect()'s own now-abandoned portal left AP_STA behind
      WiFi.begin(); //no args = reconnect with the credentials persisted by the SDK
      unsigned long retryStart = millis();
      while (WiFi.status() != WL_CONNECTED && !millisReached(retryStart + 15000)) {
        delay(250);
        yield();
      }
      if (WiFi.status() == WL_CONNECTED) {
        SerialPrintln(F("WiFi recovered - rebooting to complete normal startup"));
        checkBootSnapshotFreshness(); //keep the snapshot fresh before rebooting
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
  
  //ezTime's events() isn't called - its sync is off (setInterval(0)); checkNtpSync() and
  //checkTimezoneLookup() replace its network calls without blocking.
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

    //No-op except on a primary (once per EXTENSION_HEALTH_CHECK_INTERVAL_MS); a short blocking
    //call, see ServiceExtensionFunctions.ino.
    checkExtensionHealth();

    if (millisReached(lastBootSnapshotCheckAt + BOOT_SNAPSHOT_CHECK_INTERVAL_MS)) {
      lastBootSnapshotCheckAt = millis();
      checkBootSnapshotFreshness();
    }

    //Cheap no-op unless the reserve is empty (ServiceMemoryReserveFunctions.cpp).
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
  //A rescan can mean a unit restarted and lost its letter, even with the same count - always bump
  //displayRenderGeneration.
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
    SerialPrintln(F("  1. Address jumpers on J4 (address 0 must be assigned to a unit)"));
    SerialPrintln(F("  2. I2C wiring (SDA/SCL connections)"));
    SerialPrintln(F("  3. Power connections"));
    SerialPrintln(F("  Use the Units page's \"Rescan Units\" button once fixed, no reboot required."));
  } else {
    SerialPrintln(F("✓ ") + String(unitsAmount) + F(" unit(s) detected and ready."));
  }

  SerialPrintln(F("========================================"));
  return unitsAmount;
}

