//Initialize LittleFS
void initialiseFileSystem() {
  if (!LittleFS.begin()) {
    SerialPrintln(F("An error has occurred while mounting LittleFS"));
  }

  SerialPrintln(F("LittleFS mounted successfully"));
}

//Load variables that are saved to the file system - tries the combined boot-snapshot cache first
//(ONE file open; see bootSnapshotPath's own comment for why), falling back to the original
//~30-individual-file path when the snapshot is missing or unparseable, which also (re)writes a
//fresh snapshot for later boots.
void loadValuesFromFileSystem() {
  if (!loadValuesFromBootSnapshot()) {
    SerialPrintln(F("Boot snapshot missing/invalid - loading individual setting files instead (slower, one-time until the snapshot is (re)written)"));
    loadValuesFromIndividualFiles();
    saveBootSnapshot();
  }

  SerialPrintln(F("Loading Scheduled Messages"));
  loadScheduledMessagesFromFile();

  //Loaded directly, not via replaceRotationItemsFromJson() - that also resets playback position
  //and re-persists the file, which shouldn't happen on a plain boot load.
  SerialPrintln(F("Loading Rotation Items"));
  loadRotationItemsFromFile();

  //Extension device list - own file, not part of the boot snapshot. Loaded regardless of
  //deviceRole, so a role switch doesn't require re-entering the list.
  String extensionDevicesJson = readFile(LittleFS, extensionDevicesPath, "[]");
  if (extensionDevicesJson != "" && extensionDevicesJson != "[]") {
    SerialPrintln(F("Loading Extension Devices"));
    JsonDocument extensionDevicesDoc;
    if (!deserializeJson(extensionDevicesDoc, extensionDevicesJson) && extensionDevicesDoc.is<JsonArray>()) {
      for (JsonVariant value : extensionDevicesDoc.as<JsonArray>()) {
        if (extensionDeviceCount >= EXTENSION_DEVICES_MAX) {
          break;
        }
        String host = String((const char*)(value | ""));
        host.trim();
        if (host == "") {
          continue;
        }
        extensionDevices[extensionDeviceCount].Host = host;
        extensionDeviceCount++;
      }
    }
  }

  SerialPrintln(F("Loaded Settings:"));
  SerialPrintln(F("   Alignment: ") + alignment);
  SerialPrintln(F("   Flap Speed: ") + flapSpeed);
  SerialPrintln(F("   Device Mode: ") + deviceMode);
  SerialPrintln(F("   Countdown to Date UNIX: ") + countdownToDateUnix);
  SerialPrintln(F("   Countup since Date UNIX: ") + countupSinceDateUnix);
  SerialPrint(F("   Scheduled Message Count: "));
  SerialPrintln(scheduledMessages.size());
  SerialPrint(F("   Rotation Item Count: "));
  SerialPrintln(rotationItemCount);

  //Baseline for the periodic dirty-check in loop() (checkBootSnapshotFreshness(), ESPMaster.ino) -
  //without this, the very first check after boot would see "changed" (comparing against an empty
  //starting signature) and do an unnecessary extra flash write moments after boot already wrote
  //one (fallback path) or confirmed one was current (snapshot path).
  lastSavedSettingsSignature = computeSettingsSignature();
}

//Attempts the fast path: one combined JSON file instead of ~30 individual ones. Returns false (and
//touches nothing) if the file doesn't exist yet or fails to parse - the caller then falls back to
//loadValuesFromIndividualFiles(). Either the whole snapshot is trusted, or none of it is.
bool loadValuesFromBootSnapshot() {
  String snapshotJson = readFile(LittleFS, bootSnapshotPath, "");
  if (snapshotJson == "") {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, snapshotJson);
  if (err) {
    SerialPrintln(F("Boot snapshot: parse failed (") + String(err.c_str()) + F(") - treating as missing"));
    return false;
  }

  countdownToDateUnix = String((const char*)(doc["countdownToDateUnix"] | "0"));
  countupSinceDateUnix = String((const char*)(doc["countupSinceDateUnix"] | "0"));
  alignment = String((const char*)(doc["alignment"] | ALIGNMENT_MODE_LEFT));
  flapSpeed = String((const char*)(doc["flapSpeed"] | "80"));
  animationMode = String((const char*)(doc["animationMode"] | ANIMATION_MODE_SIMULTANEOUS));
  animationStaggerMs = doc["animationStaggerMs"] | 150;
  deviceMode = String((const char*)(doc["deviceMode"] | DEVICE_MODE_TEXT));
  deviceRole = String((const char*)(doc["deviceRole"] | DEVICE_ROLE_STANDALONE));
  extensionLinkMode = String((const char*)(doc["extensionLinkMode"] | EXTENSION_LINK_MODE_EXTEND));
  otaPassword = String((const char*)(doc["otaPassword"] | "splitflap"));
  mdnsHostname = String((const char*)(doc["mdnsHostname"] | "split-flap"));
#if MQTT_ENABLE == true
  mqttEnabled = doc["mqttEnabled"] | false;
  mqttBrokerHost = String((const char*)(doc["mqttBrokerHost"] | ""));
  mqttBrokerPort = doc["mqttBrokerPort"] | 1883;
  mqttUsername = String((const char*)(doc["mqttUsername"] | ""));
  mqttPassword = String((const char*)(doc["mqttPassword"] | ""));
#endif
  timezoneSetting = String((const char*)(doc["timezoneSetting"] | "Europe/Berlin"));
  cachedTimezonePosix = String((const char*)(doc["cachedTimezonePosix"] | ""));
  clockFormat = doc["clockFormat"] | clockFormat;
  dateFormat = doc["dateFormat"] | dateFormat;
  countdownPrefix = doc["countdownPrefix"] | countdownPrefix;
  countdownSuffix = doc["countdownSuffix"] | countdownSuffix;
  countupPrefix = doc["countupPrefix"] | countupPrefix;
  countupSuffix = doc["countupSuffix"] | countupSuffix;
  quietHoursEnabled = doc["quietHoursEnabled"] | quietHoursEnabled;
  quietHoursStart = String((const char*)(doc["quietHoursStart"] | quietHoursStart.c_str()));
  quietHoursEnd = String((const char*)(doc["quietHoursEnd"] | quietHoursEnd.c_str()));
  recomputeQuietHoursMinutes();

  accessControlTextPublic = doc["accessControlTextPublic"] | false;
  accessControlSettingsPublic = doc["accessControlSettingsPublic"] | false;
  accessControlUnitsPublic = doc["accessControlUnitsPublic"] | false;
  accessControlWifiPublic = doc["accessControlWifiPublic"] | false;
  accessControlTimezonePublic = doc["accessControlTimezonePublic"] | false;
  accessControlSystemPublic = doc["accessControlSystemPublic"] | false;

  SerialPrintln(F("Boot snapshot: loaded (fast path, one file open)"));
  return true;
}

//One readFile() per individual setting. Only reached as a fallback when no valid boot snapshot
//exists yet (see loadValuesFromFileSystem()).
void loadValuesFromIndividualFiles() {
  //Load values saved in LittleFS
  countdownToDateUnix = readFile(LittleFS, countdownPath, "0");
  countupSinceDateUnix = readFile(LittleFS, countupPath, "0");
  alignment = readFile(LittleFS, alignmentPath, ALIGNMENT_MODE_LEFT);
  flapSpeed = readFile(LittleFS, flapSpeedPath, "80");
  animationMode = readFile(LittleFS, animationModePath, ANIMATION_MODE_SIMULTANEOUS);
  String animationStaggerMsRaw = readFile(LittleFS, animationStaggerMsPath, "150");
  animationStaggerMs = isNumber(animationStaggerMsRaw) ? (unsigned long)animationStaggerMsRaw.toInt() : 150;
  deviceMode = readFile(LittleFS, deviceModePath, DEVICE_MODE_TEXT);
  deviceRole = readFile(LittleFS, deviceRolePath, DEVICE_ROLE_STANDALONE);
  extensionLinkMode = readFile(LittleFS, extensionLinkModePath, EXTENSION_LINK_MODE_EXTEND);
  otaPassword = readFile(LittleFS, otaPasswordPath, "splitflap"); //persisted, not a compile-time constant
  mdnsHostname = readFile(LittleFS, mdnsHostnamePath, "split-flap"); //persisted, not a compile-time constant
#if MQTT_ENABLE == true
  mqttEnabled = readFile(LittleFS, mqttEnabledPath, "false") == "true";
  mqttBrokerHost = readFile(LittleFS, mqttBrokerHostPath, ""); //"" = not configured yet
  String mqttBrokerPortRaw = readFile(LittleFS, mqttBrokerPortPath, "1883");
  mqttBrokerPort = isNumber(mqttBrokerPortRaw) ? mqttBrokerPortRaw.toInt() : 1883;
  mqttUsername = readFile(LittleFS, mqttUsernamePath, "");
  mqttPassword = readFile(LittleFS, mqttPasswordPath, "");
#endif
  timezoneSetting = readFile(LittleFS, timezonePath, "Europe/Berlin"); //persisted, not a compile-time constant
  cachedTimezonePosix = readFile(LittleFS, timezonePosixCachePath, ""); //"" = no successful lookup yet, see its own comment
  clockFormat = readFile(LittleFS, clockFormatPath, clockFormat);
  dateFormat = readFile(LittleFS, dateFormatPath, dateFormat);
  //readFile()'s defaultValue only applies if the file doesn't exist yet - a deliberately saved ""
  //stays "" on later boots.
  countdownPrefix = readFile(LittleFS, countdownPrefixPath, countdownPrefix);
  countdownSuffix = readFile(LittleFS, countdownSuffixPath, countdownSuffix);
  countupPrefix = readFile(LittleFS, countupPrefixPath, countupPrefix);
  countupSuffix = readFile(LittleFS, countupSuffixPath, countupSuffix);
  quietHoursEnabled = readFile(LittleFS, quietHoursEnabledPath, quietHoursEnabled ? "true" : "false") == "true";
  quietHoursStart = readFile(LittleFS, quietHoursStartPath, quietHoursStart);
  quietHoursEnd = readFile(LittleFS, quietHoursEndPath, quietHoursEnd);
  recomputeQuietHoursMinutes();

  accessControlTextPublic = readFile(LittleFS, accessControlTextPublicPath, "false") == "true";
  accessControlSettingsPublic = readFile(LittleFS, accessControlSettingsPublicPath, "false") == "true";
  accessControlUnitsPublic = readFile(LittleFS, accessControlUnitsPublicPath, "false") == "true";
  accessControlWifiPublic = readFile(LittleFS, accessControlWifiPublicPath, "false") == "true";
  accessControlTimezonePublic = readFile(LittleFS, accessControlTimezonePublicPath, "false") == "true";
  accessControlSystemPublic = readFile(LittleFS, accessControlSystemPublicPath, "false") == "true";

  SerialPrintln(F("Loaded individual setting files (fallback path)"));
}

//Serializes every scalar setting loadValuesFromBootSnapshot() knows how to read back into
//bootSnapshotPath, in ONE write. scheduledMessagesPath/rotationItemsPath are NOT included -
//already their own single files, written by their own existing code paths.
//
//Returns whether the write actually succeeded - lastSavedSettingsSignature only advances on
//confirmed success, so a failed write keeps looking "dirty" and gets retried on the next check
//instead of silently going quiet.
bool saveBootSnapshot() {
  JsonDocument doc;
  doc["countdownToDateUnix"] = countdownToDateUnix;
  doc["countupSinceDateUnix"] = countupSinceDateUnix;
  doc["alignment"] = alignment;
  doc["flapSpeed"] = flapSpeed;
  doc["animationMode"] = animationMode;
  doc["animationStaggerMs"] = animationStaggerMs;
  doc["deviceMode"] = deviceMode;
  doc["deviceRole"] = deviceRole;
  doc["extensionLinkMode"] = extensionLinkMode;
  doc["otaPassword"] = otaPassword;
  doc["mdnsHostname"] = mdnsHostname;
#if MQTT_ENABLE == true
  doc["mqttEnabled"] = mqttEnabled;
  doc["mqttBrokerHost"] = mqttBrokerHost;
  doc["mqttBrokerPort"] = mqttBrokerPort;
  doc["mqttUsername"] = mqttUsername;
  doc["mqttPassword"] = mqttPassword;
#endif
  doc["timezoneSetting"] = timezoneSetting;
  doc["cachedTimezonePosix"] = cachedTimezonePosix;
  doc["clockFormat"] = clockFormat;
  doc["dateFormat"] = dateFormat;
  doc["countdownPrefix"] = countdownPrefix;
  doc["countdownSuffix"] = countdownSuffix;
  doc["countupPrefix"] = countupPrefix;
  doc["countupSuffix"] = countupSuffix;
  doc["quietHoursEnabled"] = quietHoursEnabled;
  doc["quietHoursStart"] = quietHoursStart;
  doc["quietHoursEnd"] = quietHoursEnd;
  doc["accessControlTextPublic"] = accessControlTextPublic;
  doc["accessControlSettingsPublic"] = accessControlSettingsPublic;
  doc["accessControlUnitsPublic"] = accessControlUnitsPublic;
  doc["accessControlWifiPublic"] = accessControlWifiPublic;
  doc["accessControlTimezonePublic"] = accessControlTimezonePublic;
  doc["accessControlSystemPublic"] = accessControlSystemPublic;

  //Not atomic: a missing/broken snapshot just makes the next boot take the slow individual-files
  //path, which rewrites it - but an incomplete one (overflowed) would load with silently missing
  //settings, so that is refused (CODE_REVIEW_V7.md #1).
  if (!writeJsonDocumentFile(bootSnapshotPath, doc, false)) {
    SerialPrintln(F("Boot snapshot: write FAILED - will retry on the next freshness check"));
    return false;
  }
  lastSavedSettingsSignature = computeSettingsSignature();
  SerialPrintln(F("Boot snapshot: (re)written"));
  return true;
}

//Cheap (no flash access) string built from every field saveBootSnapshot() persists - lets
//checkBootSnapshotFreshness() detect "did any boot-relevant setting actually change" without a
//dedicated dirty flag at each settings-POST handler.
String computeSettingsSignature() {
  String sig;
  sig.reserve(256);
  sig += countdownToDateUnix; sig += '|';
  sig += countupSinceDateUnix; sig += '|';
  sig += alignment; sig += '|';
  sig += flapSpeed; sig += '|';
  sig += animationMode; sig += '|';
  sig += animationStaggerMs; sig += '|';
  sig += deviceMode; sig += '|';
  sig += deviceRole; sig += '|';
  sig += extensionLinkMode; sig += '|';
  sig += otaPassword; sig += '|';
  sig += mdnsHostname; sig += '|';
#if MQTT_ENABLE == true
  sig += mqttEnabled ? '1' : '0'; sig += '|';
  sig += mqttBrokerHost; sig += '|';
  sig += mqttBrokerPort; sig += '|';
  sig += mqttUsername; sig += '|';
  sig += mqttPassword; sig += '|';
#endif
  sig += timezoneSetting; sig += '|';
  sig += cachedTimezonePosix; sig += '|';
  sig += clockFormat; sig += '|';
  sig += dateFormat; sig += '|';
  sig += countdownPrefix; sig += '|';
  sig += countdownSuffix; sig += '|';
  sig += countupPrefix; sig += '|';
  sig += countupSuffix; sig += '|';
  sig += quietHoursEnabled ? '1' : '0'; sig += '|';
  sig += quietHoursStart; sig += '|';
  sig += quietHoursEnd; sig += '|';
  sig += accessControlTextPublic ? '1' : '0';
  sig += accessControlSettingsPublic ? '1' : '0';
  sig += accessControlUnitsPublic ? '1' : '0';
  sig += accessControlWifiPublic ? '1' : '0';
  sig += accessControlTimezonePublic ? '1' : '0';
  sig += accessControlSystemPublic ? '1' : '0';
  return sig;
}

//Shared tail of the two JSON writers below: drop the temp file on failure, otherwise move it into
//place (atomic) - and log how long the whole write took, since this device's flash chip is
//unusually slow at file opens (see bootSnapshotPath's comment) and rename() timing on it was
//never measured.
bool finishJsonFileWrite(const String &writtenPath, const char *path, bool ok, unsigned long startedAt) {
  if (ok && writtenPath != path && !LittleFS.rename(writtenPath.c_str(), path)) {
    SerialPrintln(F("- rename() into place failed"));
    ok = false;
  }
  if (!ok && writtenPath != path) {
    LittleFS.remove(writtenPath);
  }
  SerialPrintln(F("JSON file ") + String(path) + (ok ? F(" written") : F(" NOT written - previous file kept")) +
    F(" (") + String(millis() - startedAt) + F("ms)"));
  return ok;
}

//Persists one JSON document. Refuses when ArduinoJson ran out of memory while building it -
//overflowed() means fields were silently dropped, and the result would still serialize as valid
//but incomplete JSON (CODE_REVIEW_V7.md #1). Streams straight into the file instead of building
//the whole JSON text as a String first. atomic: write "<path>.tmp" and rename() it over `path`
//only once every byte made it, so a failure or power loss mid-write never truncates the existing
//file - costs one extra rename(). Non-atomic is meant for derived caches that rebuild themselves.
bool writeJsonDocumentFile(const char *path, JsonDocument &doc, bool atomic) {
  unsigned long startedAt = millis();
  if (doc.overflowed()) {
    SerialPrintln(F("JSON for ") + String(path) + F(" incomplete (out of memory) - not writing it"));
    return false;
  }
  String writtenPath = atomic ? String(path) + F(".tmp") : String(path);
  File file = LittleFS.open(writtenPath, "w");
  if (!file) {
    SerialPrintln(F("- Failed to open ") + writtenPath + F(" for writing"));
    return false;
  }
  BufferedFileWriter out(file);
  size_t expected = measureJson(doc);
  serializeJson(doc, out);
  out.flush();
  file.close();
  return finishJsonFileWrite(writtenPath, path, out.accepted() == expected, startedAt);
}

//Persists a JSON array one element at a time - only one small element document is ever alive,
//instead of a document holding the whole list plus the whole serialized String (the two together
//could exceed the free heap with 20 long scheduled messages, CODE_REVIEW_V7.md #1). Always atomic,
//for the same reason as writeJsonDocumentFile(): these files hold user data that nothing else can
//rebuild. fillElement() fills one element; an element that overflowed aborts the whole write.
bool writeJsonArrayFile(const char *path, int count, void (*fillElement)(int index, JsonDocument &element)) {
  unsigned long startedAt = millis();
  String writtenPath = String(path) + F(".tmp");
  File file = LittleFS.open(writtenPath, "w");
  if (!file) {
    SerialPrintln(F("- Failed to open ") + writtenPath + F(" for writing"));
    return false;
  }
  BufferedFileWriter out(file);
  size_t expected = 2; //'[' and ']'
  bool ok = true;
  out.write('[');
  for (int i = 0; i < count; i++) {
    JsonDocument element;
    fillElement(i, element);
    if (element.overflowed()) {
      SerialPrintln(F("JSON element ") + String(i) + F(" for ") + String(path) + F(" incomplete (out of memory)"));
      ok = false;
      break;
    }
    if (i > 0) {
      out.write(',');
      expected++;
    }
    expected += measureJson(element);
    serializeJson(element, out);
  }
  out.write(']');
  out.flush();
  file.close();
  return finishJsonFileWrite(writtenPath, path, ok && out.accepted() == expected, startedAt);
}

//Answers a request with a JSON array generated one element at a time as a chunked response - the
//HTTP counterpart of writeJsonArrayFile(), using the same fillElement() functions. Building the
//whole array as a JsonDocument plus an AsyncResponseStream buffer needed the full content in RAM
//twice on top of the list itself; with 20 long scheduled messages that aborted the device on every
//page load (operator new in AsyncResponseStream's cbuf). Only one serialized element is held at a
//time now.
//
//The response must never look like a complete list when it isn't one - a client (the rotation
//editor) submits what it got back as the full replacement (CODE_REVIEW_V8.md #4/#5). So instead of
//skipping an element that can't be built (out of memory), or mixing old and new entries when the
//list changes between two chunks (*generation moved on), the response just ends there without the
//closing "]": invalid JSON the client rejects.
void sendJsonArrayChunked(AsyncWebServerRequest *request, int count, void (*fillElement)(int index, JsonDocument &element), uint32_t *generation) {
  struct ChunkState {
    int nextElement = 0;
    bool opened = false;
    bool closed = false;
    bool aborted = false;
    uint32_t generation = 0;
    String pending;
    size_t pendingOffset = 0;
  };
  std::shared_ptr<ChunkState> state = std::make_shared<ChunkState>();
  state->generation = *generation;
  AsyncWebServerResponse *response = request->beginChunkedResponse("application/json",
    [state, count, fillElement, generation](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (state->pendingOffset >= state->pending.length()) {
          //Produce the next piece: "[", then each element (comma-separated), then "]".
          state->pending = String();
          state->pendingOffset = 0;
          if (state->aborted) {
            break;
          } else if (!state->opened) {
            state->opened = true;
            state->pending = "[";
          } else if (state->nextElement < count) {
            if (*generation != state->generation) {
              SerialPrintln(F("List changed while streaming it - ending the response incomplete"));
              state->aborted = true;
              break;
            }
            JsonDocument element;
            fillElement(state->nextElement, element);
            if (element.overflowed()) {
              SerialPrintln(F("List element ") + String(state->nextElement) + F(" incomplete (out of memory) - ending the response incomplete"));
              state->aborted = true;
              break;
            }
            //serializeJson() into a String replaces its content, it doesn't append - the
            //separator is added afterwards. Every earlier element was emitted (an unbuildable one
            //aborts above), so the index decides the comma.
            serializeJson(element, state->pending);
            if (state->nextElement > 0) {
              state->pending = "," + state->pending;
            }
            state->nextElement++;
          } else if (!state->closed) {
            state->closed = true;
            state->pending = "]";
          } else {
            break; //everything sent
          }
        }
        size_t take = min(maxLen - written, state->pending.length() - state->pendingOffset);
        memcpy(buffer + written, state->pending.c_str() + state->pendingOffset, take);
        state->pendingOffset += take;
        written += take;
      }
      return written;
    });
  request->send(response);
}

//Reads a JSON array file one element at a time, handing each to handleElement() - the read-side
//counterpart of writeJsonArrayFile(): neither the whole file as a String nor a document holding
//every element is ever in memory. Elements already handed over before an error stay handed over.
//NO_MEMORY is kept apart from INVALID so a caller never throws away a perfectly good file just
//because this one read attempt ran out of heap.
int readJsonArrayFile(const char *path, void (*handleElement)(JsonVariant element)) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    return JSON_ARRAY_FILE_MISSING;
  }
  //Stream::find()/readBytes() otherwise wait out their 1s default timeout at end of file.
  file.setTimeout(0);
  if (!file.find("[")) {
    return JSON_ARRAY_FILE_INVALID;
  }
  if (file.peek() == ']') {
    return JSON_ARRAY_FILE_OK; //empty array
  }
  do {
    JsonDocument element;
    DeserializationError error = deserializeJson(element, file);
    if (error == DeserializationError::NoMemory) {
      return JSON_ARRAY_FILE_NO_MEMORY;
    }
    if (error) {
      return JSON_ARRAY_FILE_INVALID;
    }
    handleElement(element.as<JsonVariant>());
  } while (file.findUntil(",", "]"));
  return JSON_ARRAY_FILE_OK;
}

//Read File from LittleFS
String readFile(fs::FS &fs, const char * path, String defaultValue) {
  SerialPrintf(F("Reading file: %s\r\n"), path);

  File file = fs.open(path, "r");
  if (!file || file.isDirectory()) {
    SerialPrintln(F("- Failed to open file for reading"));

    if (defaultValue.length() > 0) {
      SerialPrintln(F("- Writing a default value as one has been specified. Default: ") + defaultValue);
      writeFile(LittleFS, path, defaultValue.c_str());
      
      return defaultValue;
    }
    
    return String();
  }

  String fileContent;
  while (file.available()) {
    fileContent = file.readStringUntil('\n');
    break;
  }
  return fileContent;
}

//Write file to LittleFS. Returns whether the FULL message was actually written - a short/partial
//write (flash full, unplugged mid-write) is not success.
bool writeFile(fs::FS &fs, const char * path, const char * message) {
  SerialPrintf(F("Writing file: %s\r\n"), path);

  File file = fs.open(path, "w");
  if (!file) {
    SerialPrintln(F("- Failed to open file for writing"));
    return false;
  }

  size_t expectedLen = strlen(message);
  size_t written = file.print(message);
  file.close();

  if (written == expectedLen) {
    SerialPrintln(F("- File written"));
    return true;
  }
  SerialPrintln(F("- Write failed (wrote ") + String(written) + F(" of ") + String(expectedLen) + F(" expected bytes)"));
  return false;
}
