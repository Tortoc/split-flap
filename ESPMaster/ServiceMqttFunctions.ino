//MQTT + Home Assistant auto-discovery - found 2026-09-10 in two independent forks
//(Jerrys-modz/split-flap, scottbez1/splitflap's own mqtt_task.cpp).
//Deliberately minimal scope compared to Jerrys-modz's full
//feature set (no dedicated MQTT device mode, no buttons, no countdown entity) - a single `text`
//entity that reuses the exact same showText()/deviceMode path POST /api/text's permanent branch
//already uses, plus one diagnostic `sensor` entity (free heap) that costs nothing extra (the
//value already exists via ESP.getFreeHeap()). Kept small on purpose given this device's own heap
//crash history (BUGFIX_LOG #41-43) - see the real, measured RAM/flash cost comparison in the chat
//that led to this file. Everything here compiles away entirely when MQTT_ENABLE is false
//(ESPMaster.ino) - see that flag's own comment for why that's the only way to truly avoid
//PubSubClient's cost (its constructor always malloc()s a buffer, a runtime "disabled" flag alone
//can't prevent that).
#if MQTT_ENABLE == true

//Unique-enough ID for this device's MQTT client/discovery topics - derived from the ESP's own
//chip ID at first use (see mqttDeviceId()) rather than hardcoded, so two displays on the same
//network/broker don't collide by default (same reasoning as mdnsHostname needing to be unique -
//but this one doesn't need the user to set anything, since MQTT topics aren't human-facing the
//way a .local hostname is).
String mqttDeviceIdCache = "";
String mqttDeviceId() {
  if (mqttDeviceIdCache == "") {
    mqttDeviceIdCache = "splitflap-" + String(ESP.getChipId(), HEX);
  }
  return mqttDeviceIdCache;
}

String mqttStateTopic() {
  return "splitflap/" + mqttDeviceId() + "/state";
}

String mqttCommandTopic() {
  return "splitflap/" + mqttDeviceId() + "/set";
}

String mqttHeapTopic() {
  return "splitflap/" + mqttDeviceId() + "/heap";
}

//Tracks what was last SUCCESSFULLY published to mqttStateTopic() - checkMqttState() (below)
//compares this against inputText every second and only attempts a publish on an actual change,
//same "compare to last known value" pattern used elsewhere in this codebase for periodic checks.
String lastPublishedMqttText = "";
//Separate from lastPublishedMqttText's VALUE, because "" is itself a real, valid displayed text
//(found 2026-09-10, CODE_REVIEW_V4.md #5) - without this, mqttReconnect() resetting the cache to
//force a fresh post-reconnect publish would stay silent whenever the CURRENT text also happened
//to be "", since inputText != lastPublishedMqttText would then be false too - leaving a stale
//retained value from before the reconnect sitting on the broker. false = "not yet confirmed
//published since the last (re)connect or the last change", regardless of what the text is.
bool mqttStatePublished = false;
unsigned long lastMqttHeapPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;

//Incoming command - mirrors POST /api/text's permanent (no duration) branch exactly: switches
//into Text mode if not already there, and calls the same showText() every other permanent text
//push uses. No length/renderability validation error path back to the broker (MQTT is fire-and-
//forget, no synchronous response to reject with) - an oversized message is simply logged and
//dropped, same as showText() would refuse it internally anyway.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  SerialPrintln(F("MQTT command received: ") + message);

  if (isMessageTooLongToRender(message)) {
    SerialPrintln(F("MQTT command ignored: needs more lines than fit to display"));
    return;
  }

  if (deviceMode != DEVICE_MODE_TEXT) {
    deviceMode = DEVICE_MODE_TEXT;
    writeFile(LittleFS, deviceModePath, deviceMode.c_str());
  }
  inputText = message;
  showText(message);
}

//Publishes both Home Assistant MQTT discovery config messages - called once per successful
//(re)connect, not on every state change (HA caches discovered entities, doesn't need
//re-announcing on every publish).
void publishHomeAssistantDiscovery() {
  JsonDocument doc;
  doc["name"] = "Split Flap Text";
  doc["unique_id"] = mqttDeviceId() + "_text";
  doc["command_topic"] = mqttCommandTopic();
  doc["state_topic"] = mqttStateTopic();
  doc["icon"] = "mdi:card-text";

  JsonObject device = doc["device"].to<JsonObject>();
  JsonArray identifiers = device["identifiers"].to<JsonArray>();
  identifiers.add(mqttDeviceId());
  device["name"] = "Split Flap Display";
  device["model"] = "fae-pcb";
  device["manufacturer"] = "DIY";
  device["sw_version"] = espVersion;

  char textBuffer[512];
  size_t textLen = serializeJson(doc, textBuffer, sizeof(textBuffer));
  String textDiscoveryTopic = "homeassistant/text/" + mqttDeviceId() + "/text/config";
  mqttClient.publish(textDiscoveryTopic.c_str(), (const uint8_t*)textBuffer, textLen, true);

  JsonDocument sensorDoc;
  sensorDoc["name"] = "Split Flap Free Heap";
  sensorDoc["unique_id"] = mqttDeviceId() + "_heap";
  sensorDoc["state_topic"] = mqttHeapTopic();
  sensorDoc["unit_of_measurement"] = "B";
  sensorDoc["icon"] = "mdi:memory";
  JsonObject sensorDevice = sensorDoc["device"].to<JsonObject>();
  JsonArray sensorIdentifiers = sensorDevice["identifiers"].to<JsonArray>();
  sensorIdentifiers.add(mqttDeviceId());

  char sensorBuffer[384];
  size_t sensorLen = serializeJson(sensorDoc, sensorBuffer, sizeof(sensorBuffer));
  String sensorDiscoveryTopic = "homeassistant/sensor/" + mqttDeviceId() + "/heap/config";
  mqttClient.publish(sensorDiscoveryTopic.c_str(), (const uint8_t*)sensorBuffer, sensorLen, true);

  SerialPrintln(F("MQTT: published Home Assistant discovery"));
}

bool mqttReconnect() {
  if (mqttBrokerHost == "") {
    return false;
  }

  String clientId = mqttDeviceId() + "-" + String(random(0xffff), HEX);
  bool connected;
  if (mqttUsername != "") {
    connected = mqttClient.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  if (connected) {
    SerialPrintln(F("MQTT: connected to ") + mqttBrokerHost);
    mqttClient.subscribe(mqttCommandTopic().c_str());
    publishHomeAssistantDiscovery();
    //Force an immediate state publish too - checkMqttState()'s own "publish on change" comparison
    //would otherwise stay silent here if inputText hasn't changed since before this connection.
    //Clearing the flag, not lastPublishedMqttText's value (found 2026-09-10,
    //CODE_REVIEW_V4.md #5 - see mqttStatePublished's own declaration comment for why).
    mqttStatePublished = false;
  } else {
    SerialPrintln(F("MQTT: connect failed, rc=") + String(mqttClient.state()));
  }

  return connected;
}

//Both timeouts below found 2026-09-10, CODE_REVIEW_V4.md #3: mqttReconnect() calls the blocking
//mqttClient.connect(), which runs entirely inside loop() (nothing here is deferred like the OTA/
//I2C work elsewhere in this file) - the display state machine and every HTTP request this device
//serves stall for as long as that call takes. Left at their library defaults, a broker that's
//TCP-reachable but never answers (wrong port, a stuck proxy) could block for up to ~5s (WiFiClient's
//own connect() timeout) PLUS up to 15s (PubSubClient's MQTT_SOCKET_TIMEOUT, waiting for CONNACK) -
//nearly 20s worst case. A real async MQTT client would be the cleaner fix but is a much larger
//rewrite for an optional, off-by-default feature; bounding both legs to a few seconds instead
//keeps the existing simple/synchronous design while making a stuck broker merely a brief, bounded
//hiccup instead of a near-20s freeze. Not provoked against the live device (MQTT disabled there).
#define MQTT_TCP_CONNECT_TIMEOUT_MS 2000
#define MQTT_SOCKET_TIMEOUT_S 3

void initMqtt() {
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  mqttClient.setCallback(mqttCallback);
  //600, not 512 (found 2026-09-10, CODE_REVIEW_V4.md #5): must fit a PUBLISH packet's fixed
  //header (up to 5 bytes) + 2-byte topic length prefix + the state topic itself
  //("splitflap/splitflap-<8 hex chars>/state", ~35 bytes) + the payload, and POST /api/text
  //allows up to 500 characters - 512 total left as little as ~470 bytes of actual payload room,
  //silently truncating/rejecting the small share of legitimately long texts. 600 covers the real
  //worst case (500-char text + ~42 bytes of topic/header) with comfortable margin; this device
  //had ~17-18KB free heap at last check (BUGFIX_LOG #41-43's fix), so +88 bytes here is not a
  //meaningful risk. Still big enough for the discovery payloads above, see their own comment.
  mqttClient.setBufferSize(600);
  mqttWifiClient.setTimeout(MQTT_TCP_CONNECT_TIMEOUT_MS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

//Called once per second from loop(), alongside checkRotation()/checkNtpSync() etc. - publishes
//the current inputText (the same variable every device mode already treats as "what's currently
//shown/intended", see its own comment in ESPMaster.ino) only when it actually changed, and
//opportunistically republishes the heap sensor every 30s (cheap, no discovery re-announce needed).
void checkMqttState() {
  if (!mqttEnabled || !mqttClient.connected()) {
    return;
  }

  //!mqttStatePublished (not just a text comparison) covers the just-reconnected/still-empty-text
  //case - see that flag's declaration comment. The cache is only updated on a CONFIRMED successful
  //publish (found 2026-09-10, CODE_REVIEW_V4.md #5): publish() returns false whenever the message
  //- topic, header and all - doesn't fit PubSubClient's internal buffer (a real possibility here,
  //see setBufferSize()'s own comment), and the previous code marked it "published" regardless,
  //permanently skipping every future retry as long as the text stayed unchanged. Leaving both
  //untouched on failure means the very next tick (~1s later) retries automatically instead.
  if (!mqttStatePublished || inputText != lastPublishedMqttText) {
    if (mqttClient.publish(mqttStateTopic().c_str(), inputText.c_str(), true)) {
      lastPublishedMqttText = inputText;
      mqttStatePublished = true;
    }
  }

  unsigned long now = millis();
  if (now - lastMqttHeapPublish > 30000) {
    lastMqttHeapPublish = now;
    mqttClient.publish(mqttHeapTopic().c_str(), String(ESP.getFreeHeap()).c_str(), true);
  }
}

//Called at the top of loop(), like MDNS.update() - a cheap no-op whenever mqttEnabled is false or
//no broker is configured, so this costs essentially nothing while the feature is compiled in but
//not actively used.
void mqttLoop() {
  if (!mqttEnabled || mqttBrokerHost == "") {
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > 5000) {
      //Timestamp taken AFTER mqttReconnect() returns, not before (found 2026-09-10,
      //CODE_REVIEW_V4.md #3) - that call can itself block for a few seconds (see the timeout
      //comment on initMqtt() above), and measuring the 5s retry window from its START meant a
      //slow-but-ultimately-failed attempt already ate most of that window before the next one was
      //even allowed to start, retrying far more eagerly against an unresponsive broker than the
      //nominal 5s interval intends.
      mqttReconnect();
      lastMqttReconnectAttempt = millis();
    }
  } else {
    mqttClient.loop();
  }
}

//Applies a settings change from POST /api/mqtt - re-applies the broker/port to the existing
//PubSubClient rather than constructing a new one (that object is global/permanent, like
//mqttWifiClient), and drops any current connection so the next mqttLoop() tick reconnects under
//the new settings.
void applyMqttSettingsChange() {
  mqttClient.disconnect();
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  lastMqttReconnectAttempt = 0; //let the next loop() tick retry immediately instead of waiting out the 5s window
}

#endif
