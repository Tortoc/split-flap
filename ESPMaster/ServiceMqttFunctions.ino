//MQTT + Home Assistant auto-discovery, deliberately minimal: one `text` entity using the same
//showText() path as POST /api/text, plus a free-heap `sensor`. Compiled out entirely with
//MQTT_ENABLE false - the only way to avoid PubSubClient's buffer malloc().
#if MQTT_ENABLE == true

//Client/topic ID derived from the chip ID, so two displays on one broker don't collide.
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

//Last text successfully published; checkMqttState() only publishes on change.
String lastPublishedMqttText = "";
//Separate flag because "" is a valid text: without it, a reconnect with an empty current text
//would never publish and leave a stale retained value.
bool mqttStatePublished = false;
unsigned long lastMqttHeapPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;

//Incoming command, like POST /api/text without duration: switch to Text mode and showText().
//Oversized messages are only logged - MQTT has no error path back.
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

  applyTextModeText(message);
  showText(message);
}

//Publishes both discovery configs - once per (re)connect; HA caches discovered entities.
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
    //Publish now - the on-change check would stay silent if inputText hasn't changed.
    mqttStatePublished = false;
  } else {
    SerialPrintln(F("MQTT: connect failed, rc=") + String(mqttClient.state()));
  }

  return connected;
}

//mqttClient.connect() blocks loop(); with library defaults a broker that accepts TCP but never
//answers could stall everything for ~20s, so both timeouts are kept to a few seconds.
#define MQTT_TCP_CONNECT_TIMEOUT_MS 2000
#define MQTT_SOCKET_TIMEOUT_S 3

void initMqtt() {
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  mqttClient.setCallback(mqttCallback);
  //600, not 512: PUBLISH header + topic (~35 bytes) + up to 500 characters of payload.
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

  //!mqttStatePublished also covers a fresh reconnect. The cache is only updated on success, so a
  //failure is retried next tick.
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

//Called at the top of loop(); a cheap no-op when MQTT is disabled or unconfigured.
void mqttLoop() {
  if (!mqttEnabled || mqttBrokerHost == "") {
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > 5000) {
      //Timestamp after the (possibly blocking) reconnect, so it doesn't eat the 5s retry window.
      mqttReconnect();
      lastMqttReconnectAttempt = millis();
    }
  } else {
    mqttClient.loop();
  }
}

//Applies new settings from POST /api/mqtt to the existing global client and drops the connection,
//so the next mqttLoop() reconnects with them.
void applyMqttSettingsChange() {
  mqttClient.disconnect();
  if (mqttBrokerHost != "") {
    mqttClient.setServer(mqttBrokerHost.c_str(), mqttBrokerPort);
  }
  lastMqttReconnectAttempt = 0; //retry on the next tick
}

#endif
