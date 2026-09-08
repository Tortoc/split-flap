//Initialize LittleFS
void initialiseFileSystem() {
  if (!LittleFS.begin()) {
    SerialPrintln(F("An error has occurred while mounting LittleFS"));
  }

  SerialPrintln(F("LittleFS mounted successfully"));
}

//Load variables that are saved to the file system
void loadValuesFromFileSystem() {
  //Load values saved in LittleFS
  countdownToDateUnix = readFile(LittleFS, countdownPath, "0");
  countupSinceDateUnix = readFile(LittleFS, countupPath, "0");
  alignment = readFile(LittleFS, alignmentPath, ALIGNMENT_MODE_LEFT);
  flapSpeed = readFile(LittleFS, flapSpeedPath, "80");
  deviceMode = readFile(LittleFS, deviceModePath, DEVICE_MODE_TEXT);
  otaPassword = readFile(LittleFS, otaPasswordPath, "splitflap"); //persisted, not a compile-time constant
  timezoneSetting = readFile(LittleFS, timezonePath, "Europe/Berlin"); //persisted, not a compile-time constant
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

  String scheduledMessagesJson = readFile(LittleFS, scheduledMessagesPath, "");
  if (scheduledMessagesJson != "") {
    SerialPrintln(F("Loading Scheduled Messages"));
    readScheduledMessagesFromJson(scheduledMessagesJson);
  }

  //Loaded directly, not via replaceRotationItemsFromJson() - that also resets playback position
  //and re-persists the file, which shouldn't happen on a plain boot load.
  String rotationItemsJson = readFile(LittleFS, rotationItemsPath, "[]");
  if (rotationItemsJson != "" && rotationItemsJson != "[]") {
    SerialPrintln(F("Loading Rotation Items"));
    JsonDocument rotationJsonDocument;
    DeserializationError rotationDeserialisationError = deserializeJson(rotationJsonDocument, rotationItemsJson);
    if (!rotationDeserialisationError && rotationJsonDocument.is<JsonArray>()) {
      for (JsonVariant value : rotationJsonDocument.as<JsonArray>()) {
        if (rotationItemCount >= ROTATION_ITEMS_MAX) {
          break;
        }
        RotationItem &item = rotationItems[rotationItemCount];
        item.Type = value["type"] | DEVICE_MODE_TEXT;
        item.Value = String((const char*)(value["value"] | ""));
        item.Format = String((const char*)(value["format"] | ""));
        item.TargetDateTimeUnix = value["targetDateTimeUnix"] | 0;
        item.Prefix = String((const char*)(value["prefix"] | ""));
        item.Suffix = String((const char*)(value["suffix"] | ""));
        item.DurationSeconds = value["durationSeconds"] | 5;
        item.Alignment = String((const char*)(value["alignment"] | ""));
        rotationItemCount++;
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
}

//Read File from LittleFS
String readFile(fs::FS &fs, const char * path, String defaultValue) {
  SerialPrintf("Reading file: %s\r\n", path);

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

//Write file to LittleFS
void writeFile(fs::FS &fs, const char * path, const char * message) {
  SerialPrintf("Writing file: %s\r\n", path);

  File file = fs.open(path, "w");
  if (!file) {
    SerialPrintln(F("- Failed to open file for writing"));
    return;
  }

  if (file.print(message)) {
    SerialPrintln(F("- File written"));
  }
  else {
    SerialPrintln(F("- Write failed"));
  }
}
