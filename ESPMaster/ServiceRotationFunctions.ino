//Advances through rotationItems on a per-item timer, wrapping back to the start once the last
//item's duration elapses. Mirrors checkCountdown()'s "recompute inputText every second" pattern -
//called once per second from loop(), alongside checkCountdown()/checkScheduledMessages().
void checkRotation() {
  if (deviceMode != DEVICE_MODE_ROTATION || rotationItemCount == 0) {
    return;
  }

  if (rotationCurrentIndex >= rotationItemCount) {
    rotationCurrentIndex = 0; //list shrank since the last advance - fall back to the start
  }

  unsigned long nowSeconds = timezone.now();
  if (rotationItemStartedAtSeconds == 0) {
    rotationItemStartedAtSeconds = nowSeconds; //first tick after switching into rotation mode
  }

  //Direct array indexing - see the comment on rotationItems' declaration (ESPMaster.ino) for why
  //this is a fixed array rather than an LList.
  RotationItem &item = rotationItems[rotationCurrentIndex];
  int durationSeconds = item.DurationSeconds > 0 ? item.DurationSeconds : 5;

  if ((long)(nowSeconds - rotationItemStartedAtSeconds) >= durationSeconds) {
    rotationCurrentIndex = (rotationCurrentIndex + 1) % rotationItemCount;
    rotationItemStartedAtSeconds = nowSeconds;
  }

  RotationItem &currentItem = rotationItems[rotationCurrentIndex];

  //"" (empty) means "inherit the default alignment" - see showText()/DISPLAY_JOB_WRITE_LINE.
  rotationCurrentAlignment = currentItem.Alignment;

  //Recompute inputText for whichever item is current every second, not just once when it becomes
  //current - keeps a clock/date/countdown item live for as long as it stays on screen.
  if (currentItem.Type == DEVICE_MODE_CLOCK || currentItem.Type == DEVICE_MODE_DATE) {
    inputText = timezone.dateTime(convertUserDateFormatToEzTime(currentItem.Format));
  } else if (currentItem.Type == DEVICE_MODE_COUNTDOWN) {
    long differenceSeconds = currentItem.TargetDateTimeUnix - (long)nowSeconds;
    long days = differenceSeconds > 0 ? (long)ceil(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    //Per-item prefix/suffix, independent of the standalone Countdown mode's own setting - see
    //buildDayCountText() (ServiceCountdownFunctions.ino).
    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else if (currentItem.Type == DEVICE_MODE_COUNTUP) {
    long differenceSeconds = (long)nowSeconds - currentItem.TargetDateTimeUnix;
    long days = differenceSeconds > 0 ? (long)floor(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else { //DEVICE_MODE_TEXT
    inputText = currentItem.Value;
  }
}

//Rebuilds the whole rotation list from a JSON array - a full replace, the client always submits
//the complete list. Extra entries beyond ROTATION_ITEMS_MAX are dropped as a server-side backstop.
void replaceRotationItemsFromJson(String rotationItemsJson) {
  rotationItemCount = 0;

  if (rotationItemsJson != "") {
    JsonDocument jsonDocument;
    DeserializationError deserialisationError = deserializeJson(jsonDocument, rotationItemsJson);

    if (!deserialisationError && jsonDocument.is<JsonArray>()) {
      for (JsonVariant value : jsonDocument.as<JsonArray>()) {
        if (rotationItemCount >= ROTATION_ITEMS_MAX) {
          SerialPrintln(F("Rotation items JSON has more than ROTATION_ITEMS_MAX entries - dropping the rest"));
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
    } else {
      SerialPrintln(F("Invalid rotation items JSON - keeping the list empty"));
    }
  }

  //Always start a freshly-submitted sequence from the beginning.
  rotationCurrentIndex = 0;
  rotationItemStartedAtSeconds = 0;

  writeRotationItemsToFile();
}

//Convert the rotation items to a JSON document and save to file
void writeRotationItemsToFile() {
  JsonDocument rotationItemsDocument;
  JsonArray rotationItemsArray = rotationItemsDocument.to<JsonArray>();

  for (int rotationItemIndex = 0; rotationItemIndex < rotationItemCount; rotationItemIndex++) {
    RotationItem &item = rotationItems[rotationItemIndex];

    JsonObject itemObject = rotationItemsArray.add<JsonObject>();
    itemObject["type"] = item.Type;
    itemObject["value"] = item.Value;
    itemObject["format"] = item.Format;
    itemObject["targetDateTimeUnix"] = item.TargetDateTimeUnix;
    itemObject["prefix"] = item.Prefix;
    itemObject["suffix"] = item.Suffix;
    itemObject["durationSeconds"] = item.DurationSeconds;
    itemObject["alignment"] = item.Alignment;
  }

  String rotationItemsJson;
  serializeJson(rotationItemsDocument, rotationItemsJson);
  writeFile(LittleFS, rotationItemsPath, rotationItemsJson.c_str());
}
