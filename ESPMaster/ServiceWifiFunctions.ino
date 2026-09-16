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

//Advances the "connect to a new WiFi network from the web UI" state machine started by
//POST /api/wifi/connect - call every loop() iteration (a cheap no-op while WIFI_CONNECT_NONE).
void checkWifiConnectAttempt() {
  if (wifiConnectPhase == WIFI_CONNECT_PENDING) {
    SerialPrintln(F("Attempting to connect to new WiFi network: \"") + wifiConnectNewSsid + F("\""));
    WiFi.begin(wifiConnectNewSsid.c_str(), wifiConnectNewPassword.c_str());
    wifiConnectPhaseChangedAt = millis();
    wifiConnectPhase = WIFI_CONNECT_TESTING;
  } else if (wifiConnectPhase == WIFI_CONNECT_TESTING) {
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln(F("New WiFi network connected successfully: \"") + WiFi.SSID() + F("\", IP: ") + WiFi.localIP().toString());
      wifiConnectPhase = WIFI_CONNECT_SUCCESS;
    } else if (millis() - wifiConnectPhaseChangedAt > WIFI_CONNECT_TIMEOUT_MS) {
      SerialPrintln(F("New WiFi network failed to connect within ") + String(WIFI_CONNECT_TIMEOUT_MS / 1000) +
        F("s - reverting to previous network: \"") + wifiConnectOldSsid + F("\""));
      WiFi.begin(wifiConnectOldSsid.c_str(), wifiConnectOldPassword.c_str());
      wifiConnectPhase = WIFI_CONNECT_FAILED_REVERTED;
    }
  }
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

  SerialPrintln(F("Attempting to connect to WiFi... Will fallback to AP mode to allow configuring of WiFi if fails..."));
  if(wifiManager.autoConnect("Split-Flap-AP")) {
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
