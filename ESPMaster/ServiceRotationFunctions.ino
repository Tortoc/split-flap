//Converts the per-item override enums (Classes.h) to/from the ALIGNMENT_MODE_*/ANIMATION_MODE_*
//strings used for JSON and showText().
String rotationAlignmentOverrideToString(RotationAlignmentOverride value) {
  switch (value) {
    case RotationAlignmentOverride::Left: return ALIGNMENT_MODE_LEFT;
    case RotationAlignmentOverride::Center: return ALIGNMENT_MODE_CENTER;
    case RotationAlignmentOverride::Right: return ALIGNMENT_MODE_RIGHT;
    default: return ""; //Inherit
  }
}

//Input is validated beforehand (replaceRotationItemsFromJson()); anything else, "" included, is
//Inherit.
RotationAlignmentOverride stringToRotationAlignmentOverride(const String &value) {
  if (value == ALIGNMENT_MODE_LEFT) return RotationAlignmentOverride::Left;
  if (value == ALIGNMENT_MODE_CENTER) return RotationAlignmentOverride::Center;
  if (value == ALIGNMENT_MODE_RIGHT) return RotationAlignmentOverride::Right;
  return RotationAlignmentOverride::Inherit;
}

String rotationAnimationModeOverrideToString(RotationAnimationModeOverride value) {
  switch (value) {
    case RotationAnimationModeOverride::Simultaneous: return ANIMATION_MODE_SIMULTANEOUS;
    case RotationAnimationModeOverride::LeftToRight: return ANIMATION_MODE_LEFT_TO_RIGHT;
    case RotationAnimationModeOverride::RightToLeft: return ANIMATION_MODE_RIGHT_TO_LEFT;
    case RotationAnimationModeOverride::OutsideIn: return ANIMATION_MODE_OUTSIDE_IN;
    case RotationAnimationModeOverride::InsideOut: return ANIMATION_MODE_INSIDE_OUT;
    default: return ""; //Inherit
  }
}

//Validated via isValidAnimationMode() beforehand; anything else, "" included, is Inherit.
RotationAnimationModeOverride stringToRotationAnimationModeOverride(const String &value) {
  if (value == ANIMATION_MODE_SIMULTANEOUS) return RotationAnimationModeOverride::Simultaneous;
  if (value == ANIMATION_MODE_LEFT_TO_RIGHT) return RotationAnimationModeOverride::LeftToRight;
  if (value == ANIMATION_MODE_RIGHT_TO_LEFT) return RotationAnimationModeOverride::RightToLeft;
  if (value == ANIMATION_MODE_OUTSIDE_IN) return RotationAnimationModeOverride::OutsideIn;
  if (value == ANIMATION_MODE_INSIDE_OUT) return RotationAnimationModeOverride::InsideOut;
  return RotationAnimationModeOverride::Inherit;
}

//RotationItemType (Classes.h) <-> DEVICE_MODE_* strings at the JSON boundary.
String rotationItemTypeToString(RotationItemType value) {
  switch (value) {
    case RotationItemType::Clock: return DEVICE_MODE_CLOCK;
    case RotationItemType::Date: return DEVICE_MODE_DATE;
    case RotationItemType::Countdown: return DEVICE_MODE_COUNTDOWN;
    case RotationItemType::Countup: return DEVICE_MODE_COUNTUP;
    default: return DEVICE_MODE_TEXT;
  }
}

//Anything unrecognized becomes Text, as checkRotation() always treated unknown types.
RotationItemType stringToRotationItemType(const String &value) {
  if (value == DEVICE_MODE_CLOCK) return RotationItemType::Clock;
  if (value == DEVICE_MODE_DATE) return RotationItemType::Date;
  if (value == DEVICE_MODE_COUNTDOWN) return RotationItemType::Countdown;
  if (value == DEVICE_MODE_COUNTUP) return RotationItemType::Countup;
  return RotationItemType::Text;
}

//Advances through rotationItems on a per-item timer, wrapping at the end. Called once per second
//from loop(), like checkCountdown().
void checkRotation() {
  //Retry an incomplete load (rotationItemsComplete) first - a failed load leaves rotationItemCount
  //at 0. At most every 10s.
  if (!rotationItemsComplete) {
    static unsigned long lastReloadAttemptAt = 0;
    if (millis() - lastReloadAttemptAt >= 10000) {
      lastReloadAttemptAt = millis();
      ensureRotationItemsComplete();
    }
  }

  if (deviceMode != DEVICE_MODE_ROTATION || rotationItemCount == 0) {
    return;
  }

  if (rotationCurrentIndex >= rotationItemCount) {
    rotationCurrentIndex = 0; //list shrank since the last advance - fall back to the start
    rotationItemAcceptedSeconds = 0;
    rotationItemElapsedRealSeconds = 0;
    rotationItemStarted = false;
    rotationItemSettled = false;
  }

  //timezone.now() isn't real UTC (see getRealUtcNow()); only used for item content, not timing.
  unsigned long nowSeconds = getRealUtcNow();

  //Fixed array, not an LList - see rotationItems' declaration.
  RotationItem &item = rotationItems[rotationCurrentIndex];
  int durationSeconds = item.DurationSeconds > 0 ? item.DurationSeconds : 5;

  //Safety net for an item that never renders: a grace period past its own duration. Only ticks
  //while the display is idle - a busy display (long item, staggered animation, a scheduled message
  //in between) is progress, not a stuck item.
  if (!isDisplayJobBusy()) {
    rotationItemElapsedRealSeconds++;
  }
  #define ROTATION_STUCK_ITEM_GRACE_SECONDS 30
  if (rotationItemElapsedRealSeconds > (unsigned long)durationSeconds + ROTATION_STUCK_ITEM_GRACE_SECONDS) {
    SerialPrintln(F("Rotation item ") + String(rotationCurrentIndex) + F(" stuck (no accepted render within ") +
      String(ROTATION_STUCK_ITEM_GRACE_SECONDS) + F("s past its own duration) - skipping to the next item"));
    rotationCurrentIndex = (rotationCurrentIndex + 1) % rotationItemCount;
    rotationItemAcceptedSeconds = 0;
    rotationItemElapsedRealSeconds = 0;
    rotationItemStarted = false;
    rotationItemSettled = false;
  } else if (rotationItemAcceptedSeconds >= (unsigned long)durationSeconds) {
    rotationCurrentIndex = (rotationCurrentIndex + 1) % rotationItemCount;
    rotationItemAcceptedSeconds = 0;
    rotationItemElapsedRealSeconds = 0;
    rotationItemStarted = false;
    rotationItemSettled = false;
  }

  RotationItem &currentItem = rotationItems[rotationCurrentIndex];

  //"" (empty) means "inherit the default alignment" - see showText()/DISPLAY_JOB_WRITE_LINE.
  rotationCurrentAlignment = rotationAlignmentOverrideToString(currentItem.Alignment);
  //Same "" / -1 = inherit pattern for the per-item animation override.
  rotationCurrentAnimationMode = rotationAnimationModeOverrideToString(currentItem.AnimationMode);
  rotationCurrentAnimationStaggerMs = currentItem.AnimationStaggerMs;

  //Recomputed every second, so a clock/date/countdown item stays live while shown.
  if (currentItem.Type == RotationItemType::Clock || currentItem.Type == RotationItemType::Date) {
    inputText = timezone.dateTime(convertUserDateFormatToEzTime(currentItem.Format));
  } else if (currentItem.Type == RotationItemType::Countdown) {
    long differenceSeconds = currentItem.TargetDateTimeUnix - (long)nowSeconds;
    long days = differenceSeconds > 0 ? (long)ceil(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    //Per-item prefix/suffix, independent of standalone Countdown - see buildDayCountText().
    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else if (currentItem.Type == RotationItemType::Countup) {
    long differenceSeconds = (long)nowSeconds - currentItem.TargetDateTimeUnix;
    long days = differenceSeconds > 0 ? (long)floor(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else { //DEVICE_MODE_TEXT
    inputText = currentItem.Value;
  }
}

//Replaces the whole rotation list from a JSON array (the client always sends the full list); entries
//beyond ROTATION_ITEMS_MAX are dropped. Returns whether it was valid and applied.
bool replaceRotationItemsFromJson(const char *rotationItemsJson) {
  //Item fields use ArduinoJson's `| default` and can't fail, so only the top level is checked
  //(malformed JSON, not an array); the previous list stays untouched until that passes.
  if (rotationItemsJson == nullptr || rotationItemsJson[0] == '\0') {
    return true; //nothing submitted - leave the existing list exactly as it is, don't wipe it either
  }

  { //scope: the parsed document is released before the items get saved below
  JsonDocument jsonDocument;
  DeserializationError deserialisationError = deserializeJson(jsonDocument, rotationItemsJson);

  if (deserialisationError || !jsonDocument.is<JsonArray>()) {
    SerialPrintln(F("Invalid rotation items JSON - keeping the existing list, not touching it"));
    return false;
  }

  //Validate before touching any state. A text item with more lines than fit would block the
  //rotation forever; only text can be checked exactly here, dynamic content is bounded at runtime.
  //"" (mode) / -1 (stagger) inherit the global default.
  unsigned int totalTextLength = 0;
  for (JsonVariant value : jsonDocument.as<JsonArray>()) {
    //Every text field stays in RAM for as long as the list exists - see STORED_TEXT_BUDGET.
    totalTextLength += strlen(value["value"] | "") + strlen(value["format"] | "") +
      strlen(value["prefix"] | "") + strlen(value["suffix"] | "");
    if (totalTextLength > STORED_TEXT_BUDGET) {
      SerialPrintln(F("Rotation items JSON rejected: texts exceed STORED_TEXT_BUDGET - keeping the existing list, not touching it"));
      return false;
    }

    String itemType = value["type"] | DEVICE_MODE_TEXT;
    if (itemType == DEVICE_MODE_TEXT && isMessageTooLongToRender(String((const char*)(value["value"] | "")))) {
      SerialPrintln(F("Rotation items JSON rejected: a text item needs more lines than fit to display - keeping the existing list, not touching it"));
      return false;
    }

    String itemAlignment = String((const char*)(value["alignment"] | ""));
    if (itemAlignment != "" && itemAlignment != ALIGNMENT_MODE_LEFT && itemAlignment != ALIGNMENT_MODE_CENTER && itemAlignment != ALIGNMENT_MODE_RIGHT) {
      SerialPrintln(F("Rotation items JSON rejected: invalid alignment - keeping the existing list, not touching it"));
      return false;
    }

    String itemAnimationMode = String((const char*)(value["animationMode"] | ""));
    if (itemAnimationMode != "" && !isValidAnimationMode(itemAnimationMode)) {
      SerialPrintln(F("Rotation items JSON rejected: invalid animationMode - keeping the existing list, not touching it"));
      return false;
    }
    int itemAnimationStaggerMs = value["animationStaggerMs"] | -1;
    if (itemAnimationStaggerMs != -1 && (itemAnimationStaggerMs < 0 || itemAnimationStaggerMs > ANIMATION_STAGGER_MS_MAX)) {
      SerialPrintln(F("Rotation items JSON rejected: animationStaggerMs out of range - keeping the existing list, not touching it"));
      return false;
    }
  }

  rotationItemCount = 0;
  rotationItemsGeneration++; //see its own comment - an in-flight GET must not mix both lists
  for (JsonVariant value : jsonDocument.as<JsonArray>()) {
    if (rotationItemCount >= ROTATION_ITEMS_MAX) {
      SerialPrintln(F("Rotation items JSON has more than ROTATION_ITEMS_MAX entries - dropping the rest"));
      break;
    }
    appendRotationItemFromJson(value);
  }
  } //jsonDocument is gone here - only the items themselves stay in memory while saving below

  releaseUnusedRotationItems();

  //Always start a freshly-submitted sequence from the beginning.
  rotationCurrentIndex = 0;
  rotationItemAcceptedSeconds = 0;
  rotationItemElapsedRealSeconds = 0;
  rotationItemStarted = false;
  rotationItemSettled = false;

  //Saving failed (e.g. out of memory): roll back to the untouched file instead of running a list
  //that's gone after the next reboot.
  if (!writeRotationItemsToFile()) {
    rotationItemCount = 0;
    rotationItemsGeneration++;
    loadRotationItemsFromFile();
    releaseUnusedRotationItems();
    return false;
  }
  rotationItemsComplete = true; //memory and file hold the same, complete list again
  return true;
}

//Appends one JSON element as the next rotation item - shared by a submitted list and the boot load.
void appendRotationItemFromJson(JsonVariant value) {
  if (rotationItemCount >= ROTATION_ITEMS_MAX) {
    return;
  }
  RotationItem &item = rotationItems[rotationItemCount];
  item.Type = stringToRotationItemType(String((const char*)(value["type"] | DEVICE_MODE_TEXT)));
  item.Value = String((const char*)(value["value"] | ""));
  item.Format = String((const char*)(value["format"] | ""));
  item.TargetDateTimeUnix = value["targetDateTimeUnix"] | 0;
  item.Prefix = String((const char*)(value["prefix"] | ""));
  item.Suffix = String((const char*)(value["suffix"] | ""));
  item.DurationSeconds = value["durationSeconds"] | 5;
  item.Alignment = stringToRotationAlignmentOverride(String((const char*)(value["alignment"] | "")));
  item.AnimationMode = stringToRotationAnimationModeOverride(String((const char*)(value["animationMode"] | "")));
  item.AnimationStaggerMs = value["animationStaggerMs"] | -1;
  rotationItemCount++;
}

//Free String buffers in now-unused slots - "=" keeps the capacity, a moved-in temporary frees it.
void releaseUnusedRotationItems() {
  for (int i = rotationItemCount; i < ROTATION_ITEMS_MAX; i++) {
    rotationItems[i] = RotationItem();
  }
}

//Appends the persisted rotation items element by element (readJsonArrayFile()) - at boot and to
//roll back after a failed save; callers reset rotationItemCount first. Missing/broken file = empty
//list. Out of memory marks the list incomplete (rotationItemsComplete).
void loadRotationItemsFromFile() {
  rotationItemsComplete = (readJsonArrayFile(rotationItemsPath, appendRotationItemFromJson) != JSON_ARRAY_FILE_NO_MEMORY);
  if (!rotationItemsComplete) {
    SerialPrintln(F("Out of memory while loading rotation items - list incomplete until a reload succeeds"));
  }
}

//Retries an incomplete load (see rotationItemsComplete). True once memory matches the file.
bool ensureRotationItemsComplete() {
  if (!rotationItemsComplete) {
    rotationItemCount = 0;
    rotationItemsGeneration++;
    loadRotationItemsFromFile();
    releaseUnusedRotationItems();
  }
  return rotationItemsComplete;
}

void fillRotationItemJson(int index, JsonDocument &element) {
  RotationItem &item = rotationItems[index];
  element["type"] = rotationItemTypeToString(item.Type);
  element["value"] = item.Value;
  element["format"] = item.Format;
  element["targetDateTimeUnix"] = item.TargetDateTimeUnix;
  element["prefix"] = item.Prefix;
  element["suffix"] = item.Suffix;
  element["durationSeconds"] = item.DurationSeconds;
  element["alignment"] = rotationAlignmentOverrideToString(item.Alignment);
  element["animationMode"] = rotationAnimationModeOverrideToString(item.AnimationMode);
  element["animationStaggerMs"] = item.AnimationStaggerMs;
}

//Save the rotation items to file - see writeJsonArrayFile() for why element by element and via a
//temp file. Returns false if they couldn't be saved; the previous file is then unchanged.
bool writeRotationItemsToFile() {
  return writeJsonArrayFile(rotationItemsPath, rotationItemCount, fillRotationItemJson);
}
