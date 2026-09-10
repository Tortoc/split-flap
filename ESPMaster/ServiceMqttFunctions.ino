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

//Tracks what was last published to mqttStateTopic() - checkMqttState() (below) compares this
//against inputText every second and only publishes on an actual change, same "compare to last
//known value" pattern used elsewhere in this codebase for periodic checks.
String lastPublishedMqttText = "";
unsigned long lastMqttHeapPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;
bool mqttDiscoveryPublished = false;

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

  mqttDiscoveryPublished = true;
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
    lastPublishedMqttText = "";
  } else {
    SerialPrintln(F("MQTT: connect failed, rc=") + String(mqttClient.state()));
  }

  return connected;
}

void initMqtt() {
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512); //big enough for the discovery payloads above, see their own comment
}

//Called once per second from loop(), alongside checkRotation()/checkNtpSync() etc. - publishes
//the current inputText (the same variable every device mode already treats as "what's currently
//shown/intended", see its own comment in ESPMaster.ino) only when it actually changed, and
//opportunistically republishes the heap sensor every 30s (cheap, no discovery re-announce needed).
void checkMqttState() {
  if (!mqttEnabled || !mqttClient.connected()) {
    return;
  }

  if (inputText != lastPublishedMqttText) {
    mqttClient.publish(mqttStateTopic().c_str(), inputText.c_str(), true);
    lastPublishedMqttText = inputText;
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
      lastMqttReconnectAttempt = now;
      mqttReconnect();
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
  mqttDiscoveryPublished = false;
}

#endif
