//Kept alive for the program's lifetime - the WiFiEventHandler token would otherwise be deregistered
//once a local variable inside initWiFi() goes out of scope.
WiFiEventHandler wifiDisconnectEventHandler;

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
    SerialPrintln("Attempting to connect to new WiFi network: \"" + wifiConnectNewSsid + "\"");
    WiFi.begin(wifiConnectNewSsid.c_str(), wifiConnectNewPassword.c_str());
    wifiConnectPhaseChangedAt = millis();
    wifiConnectPhase = WIFI_CONNECT_TESTING;
  } else if (wifiConnectPhase == WIFI_CONNECT_TESTING) {
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln("New WiFi network connected successfully: \"" + WiFi.SSID() + "\", IP: " + WiFi.localIP().toString());
      wifiConnectPhase = WIFI_CONNECT_SUCCESS;
    } else if (millis() - wifiConnectPhaseChangedAt > WIFI_CONNECT_TIMEOUT_MS) {
      SerialPrintln("New WiFi network failed to connect within " + String(WIFI_CONNECT_TIMEOUT_MS / 1000) +
        "s - reverting to previous network: \"" + wifiConnectOldSsid + "\"");
      WiFi.begin(wifiConnectOldSsid.c_str(), wifiConnectOldPassword.c_str());
      wifiConnectPhase = WIFI_CONNECT_FAILED_REVERTED;
    }
  }
}

//Initialize WiFi
void initWiFi() {
  int wifiConnectTimeoutSeconds = 180;

  WiFi.mode(WIFI_STA);

  //Logs the real 802.11 disconnect reason so a "full signal but won't connect" report can be
  //pinned to a concrete cause instead of WiFiManager's generic "AutoConnect: FAILED".
  wifiDisconnectEventHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &event) {
    SerialPrintln("DEBUG: WiFi Disconnected - SSID: \"" + event.ssid + "\", Reason: " + wifiDisconnectReasonToString(event.reason));
  });

  //No WiFi.setSleepMode(WIFI_NONE_SLEEP) here deliberately - forcing the radio always-on was a
  //plausible power-instability contributor.

#if WIFI_USE_DIRECT == false
  SerialPrintln("Setting up WiFi AP Setup Mode");

  //Bumped from the default so WiFiManager's own connect attempts/results show up in the log.
  wifiManager.setDebugOutput(true, WM_DEBUG_VERBOSE);

  //No WiFi.setOutputPower(20.5) here - max TX power is a known ESP8266 instability risk.

#if WIFI_STATIC_IP == true
  wifiManager.setSTAStaticIPConfig(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns);
  SerialPrintln("WiFi Static IP Configured");
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
    SerialPrintln("New WiFi configuration saved. Will need to reboot device to let webserver work...");
    isPendingReboot = true;
  });

  std::vector<const char *> menu = { "wifi", "info" ,"param", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);

  SerialPrintln("Attempting to connect to WiFi... Will fallback to AP mode to allow configuring of WiFi if fails...");
  if(wifiManager.autoConnect("Split-Flap-AP")) {
    SerialPrint("Successfully Connected to WiFi. IP Address: ");
    SerialPrintln(WiFi.localIP());

    isWifiConfigured = true;
  }

#else
  SerialPrintln("Setting up WiFi Direct");

  if (wifiDirectSsid != "" && wifiDirectPassword != "") {
    WiFi.disconnect(true);
    delay(100);
    yield();

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.setOutputPower(20.5); //max power for range (ESP01)

    SerialPrintln("DEBUG: WiFi disconnected, starting fresh connection");
    SerialPrintln("DEBUG: SSID: " + String(wifiDirectSsid));
    SerialPrintln("DEBUG: Attempting connection (timeout: " + String(wifiConnectTimeoutSeconds) + "s)");

#if WIFI_STATIC_IP == true
    SerialPrintln("DEBUG: Configuring static IP...");
    if (WiFi.config(wifiDeviceStaticIp, wifiRouterGateway, wifiSubnet, wifiPrimaryDns)) {
      SerialPrintln("DEBUG: WiFi Static IP Configuration Success");
      delay(100);
      yield();
    }
    else {
      SerialPrintln("DEBUG: WiFi Static IP Configuration could not take place");
    }
#endif

    WiFi.begin(wifiDirectSsid, wifiDirectPassword);
    SerialPrint("DEBUG: Connecting");

    unsigned long connectionStartTime = millis();
    int maxAttemptsCount = 0;
    int lastStatus = WL_IDLE_STATUS;

    while (WiFi.status() != WL_CONNECTED && maxAttemptsCount < wifiConnectTimeoutSeconds) {
      int currentStatus = WiFi.status();

      if (currentStatus != lastStatus) {
        SerialPrintln("");
        SerialPrint("DEBUG: WiFi status changed: ");
        switch(currentStatus) {
          case WL_IDLE_STATUS: SerialPrintln("IDLE"); break;
          case WL_NO_SSID_AVAIL: SerialPrintln("NO_SSID_AVAIL"); break;
          case WL_SCAN_COMPLETED: SerialPrintln("SCAN_COMPLETED"); break;
          case WL_CONNECTED: SerialPrintln("CONNECTED"); break;
          case WL_CONNECT_FAILED: SerialPrintln("CONNECT_FAILED"); break;
          case WL_CONNECTION_LOST: SerialPrintln("CONNECTION_LOST"); break;
          case WL_DISCONNECTED: SerialPrintln("DISCONNECTED"); break;
          default: SerialPrintln("UNKNOWN (" + String(currentStatus) + ")"); break;
        }
        lastStatus = currentStatus;
      }

      if (maxAttemptsCount % 10 == 0 && maxAttemptsCount > 0) {
        SerialPrint('\n');
        SerialPrint("DEBUG: Still connecting... (" + String(maxAttemptsCount) + "s elapsed)");
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
        SerialPrintln("");
        SerialPrintln("DEBUG: Connection timeout exceeded");
        break;
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln("");
      SerialPrint("DEBUG: Successfully Connected to WiFi. IP Address: ");
      SerialPrintln(WiFi.localIP());
      SerialPrintln("DEBUG: Signal Strength (RSSI): " + String(WiFi.RSSI()) + " dBm");
      SerialPrintln("DEBUG: Connection took " + String(maxAttemptsCount) + " seconds");

      delay(500);
      yield();

      if (WiFi.status() == WL_CONNECTED) {
        isWifiConfigured = true;
      } else {
        SerialPrintln("DEBUG: WARNING - Connection lost immediately after connect!");
        isWifiConfigured = false;
      }
    } else {
      SerialPrintln("");
      SerialPrintln("DEBUG: ERROR - Failed to connect to WiFi after " + String(maxAttemptsCount) + " seconds");
      SerialPrintln("DEBUG: Final WiFi status: " + String(WiFi.status()));
      SerialPrintln("DEBUG: Possible causes:");
      SerialPrintln("DEBUG:   - Incorrect SSID or password");
      SerialPrintln("DEBUG:   - Router not in range");
      SerialPrintln("DEBUG:   - Router not allowing new connections");
      SerialPrintln("DEBUG:   - Power supply issues (ESP01 needs stable 3.3V)");
      isWifiConfigured = false;
    }
  } else {
    SerialPrintln("DEBUG: ERROR - WiFi SSID or password not configured!");
    isWifiConfigured = false;
  }

#endif
}
