//Initialize WiFi
void initWiFi() {
  int wifiConnectTimeoutSeconds = 180;

  WiFi.mode(WIFI_STA);

  //BUG FIX, 2026-08-14: researched live (per user request) why WiFi kept disconnecting and
  //then failing to reconnect even with the loop()-side fix that stops giving up permanently
  //(see BUGFIX_LOG.md #46) - repeated "Reconnection attempt N failed" every ~10s for many
  //minutes straight, never recovering on its own. This is a well-documented ESP8266 pattern:
  //the default WiFi modem-sleep power-saving mode periodically sleeps the radio between beacon
  //intervals, which some routers/environments turn into missed beacons ("bcn_timeout") and
  //disconnects that the radio then recovers from poorly. WIFI_NONE_SLEEP is the standard,
  //widely-confirmed preventive fix (see esp8266/Arduino issues #5998, #5083) - addresses *why*
  //it disconnects in the first place, not just recovery after the fact. Set as early as
  //possible, before any connection attempt.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

#if WIFI_USE_DIRECT == false
  SerialPrintln("Setting up WiFi AP Setup Mode");

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
    //Sadly, if we've had to open up the WiFi manager portal to set the WiFi configuration up
    //then a soft reset is necessary. There looks to be issues with the ESP and running
    //a webserver alongside the WiFiManager. Some reading around this issue here suggests it is
    //not possible to fix:
    //https://github.com/tzapu/WiFiManager/issues/1579
    
    //Suggestion to fix is reset the device:
    //https://github.com/rancilio-pid/clevercoffee/issues/323#issuecomment-1587344185

    SerialPrintln("New WiFi configuration saved. Will need to reboot device to let webserver work...");
    isPendingReboot = true;
  });
  
  //Set the menu options
  std::vector<const char *> menu = { "wifi", "info" ,"param", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);

  SerialPrintln("Attempting to connect to WiFi... Will fallback to AP mode to allow configuring of WiFi if fails...");
  if(wifiManager.autoConnect("Split-Flap-AP")) {
    SerialPrint("Successfully Connected to WiFi. IP Address: ");
    SerialPrintln(WiFi.localIP());

    isWifiConfigured = true;

    //Re-assert this after autoConnect() - WiFiManager does its own WiFi.begin()/mode calls
    //internally, so set it again here in case any of that reset it back to a sleep mode.
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  }
  
#else
  SerialPrintln("Setting up WiFi Direct");

  if (wifiDirectSsid != "" && wifiDirectPassword != "") {
    // Disconnect any existing connection first
    WiFi.disconnect(true);
    delay(100);
    yield();
    
    // Enable auto-reconnect for better reliability
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    
    // Set WiFi power to maximum for better range (ESP01 specific)
    WiFi.setOutputPower(20.5); // Maximum power (20.5 dBm)
    
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
    
    // Start connection
    WiFi.begin(wifiDirectSsid, wifiDirectPassword);
    SerialPrint("DEBUG: Connecting");
    
    unsigned long connectionStartTime = millis();
    int maxAttemptsCount = 0;
    int lastStatus = WL_IDLE_STATUS;

    while (WiFi.status() != WL_CONNECTED && maxAttemptsCount < wifiConnectTimeoutSeconds) {
      int currentStatus = WiFi.status();
      
      // Log status changes
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

      // Use smaller delays with yield() to allow WiFi stack to process
      delay(500);
      yield();
      delay(500);
      yield();

      maxAttemptsCount++;
      
      // Check if we've exceeded timeout
      if ((millis() - connectionStartTime) > (wifiConnectTimeoutSeconds * 1000)) {
        SerialPrintln("");
        SerialPrintln("DEBUG: Connection timeout exceeded");
        break;
      }
    }

    // Verify connection
    if (WiFi.status() == WL_CONNECTED) {
      SerialPrintln("");
      SerialPrint("DEBUG: Successfully Connected to WiFi. IP Address: ");
      SerialPrintln(WiFi.localIP());
      SerialPrintln("DEBUG: Signal Strength (RSSI): " + String(WiFi.RSSI()) + " dBm");
      SerialPrintln("DEBUG: Connection took " + String(maxAttemptsCount) + " seconds");
      
      // Wait a moment for connection to stabilize
      delay(500);
      yield();
      
      // Double-check connection is still good
      if (WiFi.status() == WL_CONNECTED) {
        isWifiConfigured = true;
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
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
