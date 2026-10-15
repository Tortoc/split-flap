//Initialize LittleFS
void initialiseFileSystem() {
  if (!LittleFS.begin()) {
    SerialPrintln(F("An error has occurred while mounting LittleFS"));
    return;
  }

  SerialPrintln(F("LittleFS mounted successfully"));
}

//Loads the persisted settings - from the boot snapshot (one file open, see bootSnapshotPath) or,
//if it's missing or broken, from the ~30 individual files, which also writes a fresh snapshot.
void loadValuesFromFileSystem() {
  bool snapshotNeedsWrite = false;
  bool snapshotLoaded = loadValuesFromBootSnapshot();
  if (!snapshotLoaded) {
    SerialPrintln(F("Boot snapshot missing/invalid - loading individual setting files instead (slower, one-time until the snapshot is (re)written)"));
    loadValuesFromIndividualFiles();
    snapshotNeedsWrite = true;
  }
  //Keep the old plaintext copies until both new copies are durable. A failed migration is
  //retried on the next boot or before a password change.
  if (adminPasswordMigrated) {
    finishAdminPasswordMigration();
    snapshotNeedsWrite = false; //the migration writes the snapshot itself
  }
  if (snapshotNeedsWrite) {
    saveBootSnapshot();
  }

  SerialPrintln(F("Loading Scheduled Messages"));
  loadScheduledMessagesFromFile();

  //Not via replaceRotationItemsFromJson() - that resets playback and re-saves the file.
  SerialPrintln(F("Loading Rotation Items"));
  loadRotationItemsFromFile();

  //Extension list: own file, not in the snapshot; loaded in every role so a role switch keeps it.
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

  //Only a loaded valid snapshot establishes this baseline. Writers update it on success;
  //a failed boot write must remain dirty so the periodic freshness check retries it.
  if (snapshotLoaded && !adminPasswordMigrated) {
    lastSavedSettingsSignature = computeSettingsSignature();
  }
}

bool finishAdminPasswordMigration() {
  if (!adminPasswordMigrated) {
    return true;
  }
  if (!writeAtomicFile(LittleFS, adminPasswordHashPath, adminPasswordHash.c_str()) || !saveBootSnapshot()) {
    SerialPrintln(F("Admin password migration could not be saved - keeping the previous copies"));
    return false;
  }
  if (LittleFS.exists(legacyOtaPasswordPath) && !LittleFS.remove(legacyOtaPasswordPath)) {
    SerialPrintln(F("Admin password migration: plaintext file could not be removed - will retry"));
    return false;
  }
  adminPasswordMigrated = false;
  SerialPrintln(F("Admin password: now stored as a hash, plaintext copies removed"));
  return true;
}

//Fast path: one combined JSON file instead of ~30. Returns false and touches nothing if it's
//missing or doesn't parse - all of the snapshot or none of it.
bool loadValuesFromBootSnapshot() {
  //A legacy file can be newer than this snapshot (downgrade), or await migration cleanup.
  if (LittleFS.exists(legacyOtaPasswordPath)) {
    return false;
  }
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
  //Only stored in Text mode, see saveBootSnapshot().
  inputText = deviceMode == DEVICE_MODE_TEXT ? String((const char*)(doc["inputText"] | "")) : String();
  //"otaPassword" is only written by firmware before 5.0.0 (plaintext) - it wins over an older hash
  //and gets migrated.
  if (doc["adminPasswordHash"].is<const char*>() && !doc["otaPassword"].is<const char*>()) {
    adminPasswordHash = doc["adminPasswordHash"].as<String>();
  } else {
    adminPasswordHash = hashAdminPassword(String((const char*)(doc["otaPassword"] | "splitflap")));
    adminPasswordMigrated = true;
  }
  primaryLinkSecret = String((const char*)(doc["primaryLinkSecret"] | ""));
  pairedPrimaryLinkSecret = String((const char*)(doc["pairedPrimaryLinkSecret"] | ""));
  mdnsHostname = String((const char*)(doc["mdnsHostname"] | "split-flap"));
  routerHostname = String((const char*)(doc["routerHostname"] | ""));
  setupApFixedPassword = String((const char*)(doc["setupApFixedPassword"] | ""));
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

//One readFile() per setting - fallback when there is no valid boot snapshot.
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
  inputText = deviceMode == DEVICE_MODE_TEXT ? readFile(LittleFS, inputTextPath, "") : String();
  //Same precedence as the snapshot path: a plaintext file can only come from a firmware before
  //5.0.0, so it's newer than any hash file.
  bool legacyPasswordExists = LittleFS.exists(legacyOtaPasswordPath);
  if (legacyPasswordExists || !LittleFS.exists(adminPasswordHashPath)) {
    adminPasswordHash = hashAdminPassword(legacyPasswordExists ? readFile(LittleFS, legacyOtaPasswordPath, "") : String(F("splitflap")));
    adminPasswordMigrated = true;
  } else {
    adminPasswordHash = readFile(LittleFS, adminPasswordHashPath, "");
  }
  primaryLinkSecret = readFile(LittleFS, primaryLinkSecretPath, "");
  pairedPrimaryLinkSecret = readFile(LittleFS, pairedPrimaryLinkSecretPath, "");
  mdnsHostname = readFile(LittleFS, mdnsHostnamePath, "split-flap");
  routerHostname = readFile(LittleFS, routerHostnamePath, "");
  setupApFixedPassword = readFile(LittleFS, setupApFixedPasswordPath, "");
#if MQTT_ENABLE == true
  mqttEnabled = readFile(LittleFS, mqttEnabledPath, "false") == "true";
  mqttBrokerHost = readFile(LittleFS, mqttBrokerHostPath, ""); //"" = not configured yet
  String mqttBrokerPortRaw = readFile(LittleFS, mqttBrokerPortPath, "1883");
  mqttBrokerPort = isNumber(mqttBrokerPortRaw) ? mqttBrokerPortRaw.toInt() : 1883;
  mqttUsername = readFile(LittleFS, mqttUsernamePath, "");
  mqttPassword = readFile(LittleFS, mqttPasswordPath, "");
#endif
  timezoneSetting = readFile(LittleFS, timezonePath, "Europe/Berlin");
  cachedTimezonePosix = readFile(LittleFS, timezonePosixCachePath, ""); //"" = no successful lookup yet
  clockFormat = readFile(LittleFS, clockFormatPath, clockFormat);
  dateFormat = readFile(LittleFS, dateFormatPath, dateFormat);
  //The default only applies while the file doesn't exist - a saved "" stays "".
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

//Writes every scalar setting loadValuesFromBootSnapshot() reads back into bootSnapshotPath, in
//one write (scheduled messages and rotation items have their own files). Returns whether it
//succeeded; lastSavedSettingsSignature only advances on success, so a failure is retried.
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
  //Text mode's text only - in every other mode inputText changes each second.
  if (deviceMode == DEVICE_MODE_TEXT) {
    doc["inputText"] = inputText;
  }
  doc["adminPasswordHash"] = adminPasswordHash;
  doc["primaryLinkSecret"] = primaryLinkSecret;
  doc["pairedPrimaryLinkSecret"] = pairedPrimaryLinkSecret;
  doc["mdnsHostname"] = mdnsHostname;
  doc["routerHostname"] = routerHostname;
  doc["setupApFixedPassword"] = setupApFixedPassword;
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

  //The snapshot also holds the password hash: a failed write must preserve the previous copy.
  if (!writeJsonDocumentFile(bootSnapshotPath, doc, true)) {
    SerialPrintln(F("Boot snapshot: write FAILED - will retry on the next freshness check"));
    return false;
  }
  lastSavedSettingsSignature = computeSettingsSignature();
  SerialPrintln(F("Boot snapshot: (re)written"));
  return true;
}

//Cheap (no flash access) string of every field saveBootSnapshot() persists - lets
//checkBootSnapshotFreshness() detect changes without a dirty flag in every settings handler.
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
  //A checksum instead of the text: lastSavedSettingsSignature stays in RAM for good.
  if (deviceMode == DEVICE_MODE_TEXT) {
    sig += String(extendFileChecksum(FILE_CHECKSUM_SEED, (const uint8_t*)inputText.c_str(), inputText.length()), HEX);
  }
  sig += '|';
  sig += adminPasswordHash; sig += '|';
  sig += primaryLinkSecret; sig += '|';
  sig += pairedPrimaryLinkSecret; sig += '|';
  sig += mdnsHostname; sig += '|';
  sig += routerHostname; sig += '|';
  sig += setupApFixedPassword; sig += '|';
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

//Text mode's text survives a reboot: own file plus the boot snapshot. In the file a real line break
//becomes the two characters \n, which sentenceLine() treats the same way - readFile() stops at the
//first real one.
void persistTextModeText() {
  String fileText = inputText;
  fileText.replace("\n", "\\n");
  writeFile(LittleFS, inputTextPath, fileText.c_str());
}

//Text mode with `text`, from POST /api/text, MQTT and an indefinite scheduled message: switches the
//mode if needed, persists mode and text and re-syncs the boot snapshot. The caller shows it.
void applyTextModeText(const String &text) {
  bool modeChanged = deviceMode != DEVICE_MODE_TEXT;
  if (modeChanged) {
    deviceMode = DEVICE_MODE_TEXT;
    writeFile(LittleFS, deviceModePath, deviceMode.c_str());
  }
  if (modeChanged || inputText != text) {
    inputText = text;
    persistTextModeText();
  }
  checkBootSnapshotFreshness(); //re-sync now, not only on the 30s check
}

//Shared tail of the JSON writers: drop the temp file on failure, otherwise rename it into place
//(atomic), and log how long the write took (this flash chip is slow at file opens).
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

//Persists one JSON document, streamed into the file. Refuses if ArduinoJson ran out of memory while
//building it (overflowed() = fields silently dropped). atomic: write "<path>.tmp" and rename() it
//over `path` only once complete, so a failure never truncates the existing file. Non-atomic is for
//caches that rebuild themselves.
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
  bool ok = out.accepted() == expected;
  if (ok && atomic) {
    ok = verifyFileContents(LittleFS, writtenPath.c_str(), expected, out.checksum());
  }
  return finishJsonFileWrite(writtenPath, path, ok, startedAt);
}

//Persists a JSON array one element at a time, so only one small element document is alive (a whole
//list document plus its serialized String could exceed the free heap). Always atomic: these files
//hold user data. An element that overflowed aborts the whole write.
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
  ok = ok && out.accepted() == expected;
  if (ok) {
    ok = verifyFileContents(LittleFS, writtenPath.c_str(), expected, out.checksum());
  }
  return finishJsonFileWrite(writtenPath, path, ok, startedAt);
}

//Sends a JSON array as a chunked response, one element at a time - the HTTP counterpart of
//writeJsonArrayFile(). Building it as a whole needed the content in RAM twice and aborted the device
//with 20 long scheduled messages.
//
//The response must never look complete when it isn't - the rotation editor submits what it got as
//the full replacement. So if an element can't be built or the list changes mid-response
//(*generation moved on), it just ends without the closing "]": invalid JSON the client rejects.
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
            //serializeJson() into a String replaces its content; the comma is added afterwards.
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

//Reads a JSON array file element by element into handleElement() - the counterpart of
//writeJsonArrayFile(). NO_MEMORY is kept apart from INVALID, so a good file is never thrown away
//just because one read ran out of heap.
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

//File.close() cannot report LittleFS sync errors. Reopen the closed file and check the committed
//bytes before promoting it; a complete write count alone only proves data reached the cache.
bool verifyFileContents(fs::FS &fs, const char *path, size_t expected, uint32_t expectedChecksum) {
  File file = fs.open(path, "r");
  if (!file || file.size() != expected) {
    return false;
  }
  uint8_t block[64];
  size_t received = 0;
  uint32_t checksum = FILE_CHECKSUM_SEED;
  while (file.available()) {
    size_t length = file.read(block, sizeof(block));
    if (length == 0) {
      return false;
    }
    received += length;
    checksum = extendFileChecksum(checksum, block, length);
  }
  return received == expected && checksum == expectedChecksum;
}

//Atomic replacement for security-sensitive values. A failed write or rename leaves the old file
//intact. Only the temporary path is allocated; the message is streamed by writeFile().
bool writeAtomicFile(fs::FS &fs, const char *path, const char *message) {
  String temporaryPath = String(path) + F(".tmp");
  if (!writeFile(fs, temporaryPath.c_str(), message)) {
    fs.remove(temporaryPath.c_str());
    return false;
  }
  size_t expected = strlen(message);
  uint32_t checksum = extendFileChecksum(FILE_CHECKSUM_SEED, reinterpret_cast<const uint8_t*>(message), expected);
  if (!verifyFileContents(fs, temporaryPath.c_str(), expected, checksum)) {
    fs.remove(temporaryPath.c_str());
    SerialPrintln(F("- Atomic file verification failed"));
    return false;
  }
  if (!fs.rename(temporaryPath.c_str(), path)) {
    fs.remove(temporaryPath.c_str());
    SerialPrintln(F("- Atomic file rename failed"));
    return false;
  }
  return true;
}

//Write file to LittleFS. Returns whether the FULL message was written (a short write is a failure).
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
