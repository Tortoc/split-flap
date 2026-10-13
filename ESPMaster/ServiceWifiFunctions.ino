//Kept alive for the program's lifetime - the WiFiEventHandler token would otherwise be deregistered
//once a local variable inside initWiFi() goes out of scope.
WiFiEventHandler wifiDisconnectEventHandler;

//Makes WiFi disconnect reasons observable over HTTP instead of only via a live serial capture.
//wifiDisconnectCount/lastWifiDisconnectReason/lastWifiDisconnectAt (ESPMaster.ino) track
//frequency/cause over time, cheaply.

//Translates a WiFi disconnect reason into a readable string for the serial log, so "full signal but
//won't connect" can be pinned to a concrete cause instead of just "FAILED".
String wifiDisconnectReasonToString(WiFiDisconnectReason reason) {
  switch (reason) {
    case WIFI_DISCONNECT_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_DISCONNECT_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_DISCONNECT_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_DISCONNECT_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_DISCONNECT_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY (AP client limit reached)";
    case WIFI_DISCONNECT_REASON_NOT_AUTHED: return "NOT_AUTHED";
    case WIFI_DISCONNECT_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
    case WIFI_DISCONNECT_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_DISCONNECT_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_DISCONNECT_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD (11h)";
    case WIFI_DISCONNECT_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD (11h)";
    case WIFI_DISCONNECT_REASON_IE_INVALID: return "IE_INVALID (11i)";
    case WIFI_DISCONNECT_REASON_MIC_FAILURE: return "MIC_FAILURE - likely WRONG PASSWORD (11i)";
    case WIFI_DISCONNECT_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT - likely WRONG PASSWORD (11i)";
    case WIFI_DISCONNECT_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT (11i)";
    case WIFI_DISCONNECT_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS (11i)";
    case WIFI_DISCONNECT_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID - security mode mismatch (11i)";
    case WIFI_DISCONNECT_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID - security mode mismatch (11i)";
    case WIFI_DISCONNECT_REASON_AKMP_INVALID: return "AKMP_INVALID - security mode mismatch (11i)";
    case WIFI_DISCONNECT_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION (11i)";
    case WIFI_DISCONNECT_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP (11i)";
    case WIFI_DISCONNECT_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED (11i)";
    case WIFI_DISCONNECT_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED (11i)";
    case WIFI_DISCONNECT_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT (lost signal after connecting)";
    case WIFI_DISCONNECT_REASON_NO_AP_FOUND: return "NO_AP_FOUND - ESP's own scan did not see the SSID (band/channel mismatch?)";
    case WIFI_DISCONNECT_REASON_AUTH_FAIL: return "AUTH_FAIL - likely WRONG PASSWORD";
    case WIFI_DISCONNECT_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_DISCONNECT_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT - likely WRONG PASSWORD";
    default: return "UNKNOWN (" + String((int)reason) + ")";
  }
}

//Both functions below use wifiScanRequested/wifiConnectPhase/WIFI_CONNECT_* - all only declared
//under `#if WIFI_USE_DIRECT == false` in ESPMaster.ino (WiFiManager-based AP-config mode; direct
//mode has no such scan/reconnect UI), so these definitions must be guarded the same way.
#if WIFI_USE_DIRECT == false
//Starts the network scan requested via POST /api/wifi/scan - kept out of the AsyncWebServer
//callback. Empty completion callback: GET /api/wifi/scan-result polls WiFi.scanComplete() directly.
void checkWifiScanRequest() {
  if (!wifiScanRequested) {
    return;
  }
  wifiScanRequested = false;
  WiFi.scanNetworksAsync([](int numNetworksFound) {});
}

//WiFi.begin() with the credentials also saved to flash as the network to join after a reboot.
//WiFiManager::autoConnect() switches WiFi.persistent() off at boot and - when it connects with
//saved credentials - never switches it back on, so a plain WiFi.begin() here only changed the
//network until the next reboot (OTA, crash, power cut), after which the device silently rejoined
//whatever network was still in flash. Persistence is switched back off right after, matching what
//WiFiManager expects for the rest of the runtime.
void beginWifiAndPersist(const String &ssid, const String &password) {
  WiFi.persistent(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  WiFi.persistent(false);
}

//Advances the "connect to a new WiFi network from the web UI" state machine started by
//POST /api/wifi/connect - call every loop() iteration (a cheap no-op while WIFI_CONNECT_NONE).
//Both the new network and, on failure, the reverted previous one are saved to flash (see
//beginWifiAndPersist()), so flash always holds the network the device is actually using.
void checkWifiConnectAttempt() {
  if (wifiConnectPhase == WIFI_CONNECT_PENDING) {
    SerialPrintln(F("Attempting to connect to new WiFi network: \"") + wifiConnectNewSsid + F("\""));
    beginWifiAndPersist(wifiConnectNewSsid, wifiConnectNewPassword);
    wifiConnectPhaseChangedAt = millis();
    wifiConnectPhase = WIFI_CONNECT_TESTING;
  } else if (wifiConnectPhase == WIFI_CONNECT_TESTING) {
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln(F("New WiFi network connected successfully: \"") + WiFi.SSID() + F("\", IP: ") + WiFi.localIP().toString());
      wifiConnectPhase = WIFI_CONNECT_SUCCESS;
    } else if (millis() - wifiConnectPhaseChangedAt > WIFI_CONNECT_TIMEOUT_MS) {
      SerialPrintln(F("New WiFi network failed to connect within ") + String(WIFI_CONNECT_TIMEOUT_MS / 1000) +
        F("s - reverting to previous network: \"") + wifiConnectOldSsid + F("\""));
      beginWifiAndPersist(wifiConnectOldSsid, wifiConnectOldPassword);
      wifiConnectPhase = WIFI_CONNECT_FAILED_REVERTED;
    }
  }
}
#endif

#if WIFI_USE_DIRECT == false
//Setup hotspot password on the display (security review OW-06, code review V9-03/04). A display
//with 8+ units shows it whole and keeps it. A narrower one cycles through it for as long as the
//portal is open: frame 0 is all dashes (marks the start, the password has no '-'), frames 1..n
//are consecutive parts of the password, each held for SETUP_AP_PASSWORD_HOLD_MS.
//Holds the user's fixed password instead when one is set (setupApFixedPassword) - that one is
//never shown, so the frame functions below only ever see the random 8-character one.
char setupApPassword[64] = "";
int setupApPasswordFrame = 0;
bool setupApPasswordFrameStarted = false;
unsigned long setupApPasswordFrameDoneAt = 0; //0 = current frame not finished yet
#define SETUP_AP_PASSWORD_HOLD_MS 4000

int setupApPasswordFrameCount() {
  return unitsAmount >= 8 ? 1 : 1 + (8 + unitsAmount - 1) / unitsAmount;
}

String setupApPasswordFrameText(int frame) {
  if (unitsAmount >= 8) {
    return String(setupApPassword);
  }
  if (frame == 0) {
    String dashes;
    for (int i = 0; i < unitsAmount; i++) {
      dashes += '-';
    }
    return dashes;
  }
  //Exactly one display line per part - a longer text would be hyphenated and paged.
  return String(setupApPassword).substring((frame - 1) * unitsAmount, min(frame * unitsAmount, 8));
}

//Recurrent function (schedule_recurrent_function_us) - runs after every yield() until the portal
//closes. Must not call yield()/delay() itself.
bool tickSetupApPasswordDisplay() {
  if (!wifiSetupPortalOpen) {
    return false; //portal closed - unschedule; loop() takes the display over again
  }
  if (isDisplayJobBusy()) {
    updateDisplayJob();
    return true;
  }
  if (!setupApPasswordFrameStarted) {
    bool wide = unitsAmount >= 8;
    //Simultaneous animation: every character lands at once, whatever animation the user set.
    setupApPasswordFrameStarted = showText(setupApPasswordFrameText(setupApPasswordFrame), 0,
      wide ? ALIGNMENT_MODE_CENTER : ALIGNMENT_MODE_LEFT, ANIMATION_MODE_SIMULTANEOUS, 0);
    return true;
  }
  if (setupApPasswordFrameCount() == 1) {
    return true; //whole password is on the display - leave it there
  }
  if (setupApPasswordFrameDoneAt == 0) {
    setupApPasswordFrameDoneAt = millis();
  } else if (millis() - setupApPasswordFrameDoneAt >= SETUP_AP_PASSWORD_HOLD_MS) {
    setupApPasswordFrame = (setupApPasswordFrame + 1) % setupApPasswordFrameCount();
    setupApPasswordFrameStarted = false;
    setupApPasswordFrameDoneAt = 0;
  }
  return true;
}
#endif

//Initialize WiFi
void initWiFi() {
  int wifiConnectTimeoutSeconds = 180;

  WiFi.mode(WIFI_STA);

  //Logs the real 802.11 disconnect reason so a "full signal but won't connect" report can be
  //pinned to a concrete cause instead of WiFiManager's generic "AutoConnect: FAILED".
  wifiDisconnectEventHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &event) {
    SerialPrintln(F("DEBUG: WiFi Disconnected - SSID: \"") + event.ssid + F("\", Reason: ") + wifiDisconnectReasonToString(event.reason));
    wifiDisconnectCount++;
    lastWifiDisconnectReason = wifiDisconnectReasonToString(event.reason);
    lastWifiDisconnectAt = millis();
    diagFreeHeapAtLastWifiDisconnect = ESP.getFreeHeap(); //TEMPORARY DIAGNOSTIC, see diagMinFreeHeap
  });

  //No WiFi.setSleepMode(WIFI_NONE_SLEEP) here deliberately - forcing the radio always-on was a
  //plausible power-instability contributor.

#if WIFI_USE_DIRECT == false
  SerialPrintln(F("Setting up WiFi AP Setup Mode"));

  //Bumped from the default so WiFiManager's own connect attempts/results show up in the log.
  wifiManager.setDebugOutput(true, WM_DEBUG_VERBOSE);

  //No WiFi.setOutputPower(20.5) here - max TX power is a known ESP8266 instability risk.

#if WIFI_STATIC_IP == true
  wifiManager.setSTAStaticIPConfig(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns);
  SerialPrintln(F("WiFi Static IP Configured"));
#endif

  wifiManager.setTitle("Split-Flap Setup");
  wifiManager.setHostname("Split-Flap");
  wifiManager.setDarkMode(true);
  wifiManager.setShowInfoUpdate(false);
  wifiManager.setConfigPortalBlocking(true);
  wifiManager.setConfigPortalTimeout(wifiConnectTimeoutSeconds);
  wifiManager.setConnectTimeout(120);
  wifiManager.setWiFiAutoReconnect(true);
  wifiManager.setSaveConfigCallback([]() {
    //Running the config portal and our own webserver together doesn't work reliably - reboot
    //after saving new credentials is the accepted workaround.
    SerialPrintln(F("New WiFi configuration saved. Will need to reboot device to let webserver work..."));
    checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - keep the snapshot fresh before every intentional reboot
    isPendingReboot = true;
  });

  std::vector<const char *> menu = { "wifi", "info" ,"param", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);

  //The setup hotspot used to be open - anyone in range could point the device at their own network
  //(security review 2026-10-12, OW-06). It now gets a fresh random WPA2 password every time,
  //shown on the flaps once the hotspot is up: only someone standing in front of the display can
  //use it. Uppercase letters and digits only (the flaps have no lowercase), without the
  //look-alikes 0/O, 1/I, 2/Z, 5/S and 8/B.
  //No two equal characters in a row - on a one-unit display they'd look like a single one.
  //A fixed password set by the user replaces it; the display then stays quiet. The serial log
  //below is the way back in if that password is forgotten.
  if (setupApFixedPassword != "") {
    strlcpy(setupApPassword, setupApFixedPassword.c_str(), sizeof(setupApPassword));
  } else {
    static const char apPasswordAlphabet[] = "ACDEFGHJKLMNPQRTUVWXY34679";
    for (int i = 0; i < 8; i++) {
      do {
        setupApPassword[i] = apPasswordAlphabet[ESP.random() % (sizeof(apPasswordAlphabet) - 1)];
      } while (i > 0 && setupApPassword[i] == setupApPassword[i - 1]);
    }
    setupApPassword[8] = '\0';
  }
  wifiManager.setAPCallback([](WiFiManager*) {
    SerialPrintln(F("Setup hotspot \"Split-Flap-AP\" open - password: ") + String(setupApPassword));
    wifiSetupPortalOpen = true;
    //The setup takes precedence over whatever setup()'s pre-WiFi display is still writing (code
    //review V9-03) - a busy display would refuse the password.
    cancelDisplayControllers();
    if (setupApFixedPassword != "") {
      return; //fixed password - nothing to show
    }
    setupApPasswordFrame = unitsAmount >= 8 ? 0 : 1; //narrow display: start with the first part
    setupApPasswordFrameStarted = false;
    setupApPasswordFrameDoneAt = 0;
    //WiFiManager's portal loop offers no hook of its own, but it yield()s on every pass, and the
    //core runs recurrent functions after each yield() - this keeps the display job going for the
    //whole portal instead of only while this callback runs.
    if (unitsAmount > 0) {
      schedule_recurrent_function_us(tickSetupApPasswordDisplay, 10000);
    }
  });

  SerialPrintln(F("Attempting to connect to WiFi... Will fallback to AP mode to allow configuring of WiFi if fails..."));
  bool wifiConnected = wifiManager.autoConnect("Split-Flap-AP", setupApPassword);
  wifiSetupPortalOpen = false;
  if(wifiConnected) {
    SerialPrint(F("Successfully Connected to WiFi. IP Address: "));
    SerialPrintln(WiFi.localIP());

    isWifiConfigured = true;
  }

#else
  SerialPrintln(F("Setting up WiFi Direct"));

  if (wifiDirectSsid != "" && wifiDirectPassword != "") {
    WiFi.disconnect(true);
    delay(100);
    yield();

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.setOutputPower(20.5); //max power for range (ESP01)

    SerialPrintln(F("DEBUG: WiFi disconnected, starting fresh connection"));
    SerialPrintln(F("DEBUG: SSID: ") + String(wifiDirectSsid));
    SerialPrintln(F("DEBUG: Attempting connection (timeout: ") + String(wifiConnectTimeoutSeconds) + F("s)"));

#if WIFI_STATIC_IP == true
    SerialPrintln(F("DEBUG: Configuring static IP..."));
    if (WiFi.config(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns)) {
      SerialPrintln(F("DEBUG: WiFi Static IP Configuration Success"));
      delay(100);
      yield();
    }
    else {
      SerialPrintln(F("DEBUG: WiFi Static IP Configuration could not take place"));
    }
#endif

    WiFi.begin(wifiDirectSsid, wifiDirectPassword);
    SerialPrint(F("DEBUG: Connecting"));

    unsigned long connectionStartTime = millis();
    int maxAttemptsCount = 0;
    int lastStatus = WL_IDLE_STATUS;

    while (WiFi.status() != WL_CONNECTED && maxAttemptsCount < wifiConnectTimeoutSeconds) {
      int currentStatus = WiFi.status();

      if (currentStatus != lastStatus) {
        SerialPrintln(F(""));
        SerialPrint(F("DEBUG: WiFi status changed: "));
        switch(currentStatus) {
          case WL_IDLE_STATUS: SerialPrintln(F("IDLE")); break;
          case WL_NO_SSID_AVAIL: SerialPrintln(F("NO_SSID_AVAIL")); break;
          case WL_SCAN_COMPLETED: SerialPrintln(F("SCAN_COMPLETED")); break;
          case WL_CONNECTED: SerialPrintln(F("CONNECTED")); break;
          case WL_CONNECT_FAILED: SerialPrintln(F("CONNECT_FAILED")); break;
          case WL_CONNECTION_LOST: SerialPrintln(F("CONNECTION_LOST")); break;
          case WL_DISCONNECTED: SerialPrintln(F("DISCONNECTED")); break;
          default: SerialPrintln(F("UNKNOWN (") + String(currentStatus) + F(")")); break;
        }
        lastStatus = currentStatus;
      }

      if (maxAttemptsCount % 10 == 0 && maxAttemptsCount > 0) {
        SerialPrint('\n');
        SerialPrint(F("DEBUG: Still connecting... (") + String(maxAttemptsCount) + F("s elapsed)"));
      }
      else {
        SerialPrint('.');
      }

      delay(500);
      yield();
      delay(500);
      yield();

      maxAttemptsCount++;

      if ((millis() - connectionStartTime) > (wifiConnectTimeoutSeconds * 1000)) {
        SerialPrintln(F(""));
        SerialPrintln(F("DEBUG: Connection timeout exceeded"));
        break;
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln(F(""));
      SerialPrint(F("DEBUG: Successfully Connected to WiFi. IP Address: "));
      SerialPrintln(WiFi.localIP());
      SerialPrintln(F("DEBUG: Signal Strength (RSSI): ") + String(WiFi.RSSI()) + F(" dBm"));
      SerialPrintln(F("DEBUG: Connection took ") + String(maxAttemptsCount) + F(" seconds"));

      delay(500);
      yield();

      if (WiFi.status() == WL_CONNECTED) {
        isWifiConfigured = true;
      } else {
        SerialPrintln(F("DEBUG: WARNING - Connection lost immediately after connect!"));
        isWifiConfigured = false;
      }
    } else {
      SerialPrintln(F(""));
      SerialPrintln(F("DEBUG: ERROR - Failed to connect to WiFi after ") + String(maxAttemptsCount) + F(" seconds"));
      SerialPrintln(F("DEBUG: Final WiFi status: ") + String(WiFi.status()));
      SerialPrintln(F("DEBUG: Possible causes:"));
      SerialPrintln(F("DEBUG:   - Incorrect SSID or password"));
      SerialPrintln(F("DEBUG:   - Router not in range"));
      SerialPrintln(F("DEBUG:   - Router not allowing new connections"));
      SerialPrintln(F("DEBUG:   - Power supply issues (ESP01 needs stable 3.3V)"));
      isWifiConfigured = false;
    }
  } else {
    SerialPrintln(F("DEBUG: ERROR - WiFi SSID or password not configured!"));
    isWifiConfigured = false;
  }

#endif
}
