//MQTT + Home Assistant auto-discovery - deliberately minimal scope: a single `text` entity that
//reuses the exact same showText()/deviceMode path POST /api/text uses, plus one diagnostic
//`sensor` entity (free heap). Everything here compiles away entirely when MQTT_ENABLE is false -
//the only way to truly avoid PubSubClient's cost (its constructor always malloc()s a buffer).
#if MQTT_ENABLE == true

//Unique-enough ID for this device's MQTT client/discovery topics - derived from the ESP's own
//chip ID at first use, so two displays on the same broker don't collide by default.
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

//Tracks what was last SUCCESSFULLY published to mqttStateTopic() - checkMqttState() compares this
//against inputText every second and only publishes on an actual change.
String lastPublishedMqttText = "";
//Separate from lastPublishedMqttText's VALUE, because "" is itself a real, valid displayed text -
//without this, resetting the cache on reconnect would stay silent if the CURRENT text also
//happens to be "", leaving a stale retained value on the broker.
bool mqttStatePublished = false;
unsigned long lastMqttHeapPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;

//Incoming command - mirrors POST /api/text's permanent (no duration) branch: switches into Text
//mode if not already there, and calls the same showText(). MQTT is fire-and-forget, so an
//oversized message is simply logged and dropped, no error path back to the broker.
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
    checkBootSnapshotFreshness(); //see its own comment (ESPMaster.ino) - re-sync right after this confirmed change, not just on the periodic 30s check
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
    //Force an immediate state publish - checkMqttState()'s "publish on change" comparison would
    //otherwise stay silent if inputText hasn't changed since before this connection.
    mqttStatePublished = false;
  } else {
    SerialPrintln(F("MQTT: connect failed, rc=") + String(mqttClient.state()));
  }

  return connected;
}

//mqttReconnect() calls the blocking mqttClient.connect(), which runs entirely inside loop() - the
//display state machine and every HTTP request stall for as long as it takes. Left at library
//defaults, a broker that's TCP-reachable but never answers could block for up to ~20s worst case;
//bounding both legs to a few seconds keeps a stuck broker a brief, bounded hiccup instead.
#define MQTT_TCP_CONNECT_TIMEOUT_MS 2000
#define MQTT_SOCKET_TIMEOUT_S 3

void initMqtt() {
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  mqttClient.setCallback(mqttCallback);
  //600, not 512: must fit a PUBLISH packet's header+topic ("splitflap/splitflap-<8 hex
  //chars>/state", ~35 bytes) plus up to 500 characters of payload - 512 would silently truncate
  //the small share of legitimately long texts.
  mqttClient.setBufferSize(600);
  mqttWifiClient.setTimeout(MQTT_TCP_CONNECT_TIMEOUT_MS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
}

//Called once per second from loop() - publishes the current inputText only when it actually
//changed, and opportunistically republishes the heap sensor every 30s.
void checkMqttState() {
  if (!mqttEnabled || !mqttClient.connected()) {
    return;
  }

  //!mqttStatePublished (not just a text comparison) covers the just-reconnected/still-empty-text
  //case. The cache is only updated on a CONFIRMED successful publish - leaving both untouched on
  //failure means the very next tick retries automatically.
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
      //Timestamp taken AFTER mqttReconnect() returns, not before - that call can itself block for
      //a few seconds, and measuring the 5s window from its START would retry far too eagerly.
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
