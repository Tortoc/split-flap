//Converts RotationItem's compact RotationAlignmentOverride/RotationAnimationModeOverride enums
//(Classes.h) to/from the ALIGNMENT_MODE_*/ANIMATION_MODE_* strings used everywhere else (JSON in
//and out, showText()'s alignment/animation parameters) - see the enums' own comment for why this
//conversion exists instead of storing the String directly.
String rotationAlignmentOverrideToString(RotationAlignmentOverride value) {
  switch (value) {
    case RotationAlignmentOverride::Left: return ALIGNMENT_MODE_LEFT;
    case RotationAlignmentOverride::Center: return ALIGNMENT_MODE_CENTER;
    case RotationAlignmentOverride::Right: return ALIGNMENT_MODE_RIGHT;
    default: return ""; //Inherit
  }
}

//Callers validate against ALIGNMENT_MODE_LEFT/CENTER/RIGHT before ever reaching this (see
//replaceRotationItemsFromJson()'s pre-validation loop) - anything else, including "", falls back
//to Inherit, matching the previous ""-sentinel behavior exactly.
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

//Callers validate via isValidAnimationMode() before ever reaching this (see
//replaceRotationItemsFromJson()'s pre-validation loop) - anything else, including "", falls back
//to Inherit, matching the previous ""-sentinel behavior exactly.
RotationAnimationModeOverride stringToRotationAnimationModeOverride(const String &value) {
  if (value == ANIMATION_MODE_SIMULTANEOUS) return RotationAnimationModeOverride::Simultaneous;
  if (value == ANIMATION_MODE_LEFT_TO_RIGHT) return RotationAnimationModeOverride::LeftToRight;
  if (value == ANIMATION_MODE_RIGHT_TO_LEFT) return RotationAnimationModeOverride::RightToLeft;
  if (value == ANIMATION_MODE_OUTSIDE_IN) return RotationAnimationModeOverride::OutsideIn;
  if (value == ANIMATION_MODE_INSIDE_OUT) return RotationAnimationModeOverride::InsideOut;
  return RotationAnimationModeOverride::Inherit;
}

//Converts RotationItem's compact RotationItemType enum (Classes.h) to/from the DEVICE_MODE_*
//strings used at the JSON boundary - see the enum's own comment for why Type is narrowed but the
//global deviceMode isn't.
String rotationItemTypeToString(RotationItemType value) {
  switch (value) {
    case RotationItemType::Clock: return DEVICE_MODE_CLOCK;
    case RotationItemType::Date: return DEVICE_MODE_DATE;
    case RotationItemType::Countdown: return DEVICE_MODE_COUNTDOWN;
    case RotationItemType::Countup: return DEVICE_MODE_COUNTUP;
    default: return DEVICE_MODE_TEXT;
  }
}

//Anything unrecognized falls back to Text - matches the previous behavior exactly: an
//unrecognized type string was already stored and displayed as plain text before this enum
//existed, since checkRotation()'s dispatch chain (below) only special-cases Clock/Date/Countdown/
//Countup and treats everything else as Text in its final `else` branch.
RotationItemType stringToRotationItemType(const String &value) {
  if (value == DEVICE_MODE_CLOCK) return RotationItemType::Clock;
  if (value == DEVICE_MODE_DATE) return RotationItemType::Date;
  if (value == DEVICE_MODE_COUNTDOWN) return RotationItemType::Countdown;
  if (value == DEVICE_MODE_COUNTUP) return RotationItemType::Countup;
  return RotationItemType::Text;
}

//Advances through rotationItems on a per-item timer, wrapping back to the start once the last
//item's duration elapses. Mirrors checkCountdown()'s "recompute inputText every second" pattern -
//called once per second from loop(), alongside checkCountdown()/checkScheduledMessages().
void checkRotation() {
  if (deviceMode != DEVICE_MODE_ROTATION || rotationItemCount == 0) {
    return;
  }

  if (rotationCurrentIndex >= rotationItemCount) {
    rotationCurrentIndex = 0; //list shrank since the last advance - fall back to the start
    rotationItemAcceptedSeconds = 0;
    rotationItemElapsedRealSeconds = 0;
  }

  //timezone.now() doesn't return real UTC - see getRealUtcNow()'s comment. Only used below for
  //item CONTENT, not for timing the item's turn.
  unsigned long nowSeconds = getRealUtcNow();

  //Direct array indexing - see the comment on rotationItems' declaration (ESPMaster.ino) for why
  //this is a fixed array rather than an LList.
  RotationItem &item = rotationItems[rotationCurrentIndex];
  int durationSeconds = item.DurationSeconds > 0 ? item.DurationSeconds : 5;

  //Defense-in-depth against a permanently-stuck item - a generous grace period past the item's
  //own configured duration distinguishes "genuinely just slow to render" from "will never
  //succeed". Only counts a tick while the display is actually IDLE going into it - a job
  //legitimately still busy rendering (multi-line item, large staggered animation) is proof of
  //progress, not neglect, and shouldn't count against the budget; this also correctly covers a
  //foreign temporary message (e.g. a scheduled message) displacing the rotation.
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
  } else if (rotationItemAcceptedSeconds >= (unsigned long)durationSeconds) {
    rotationCurrentIndex = (rotationCurrentIndex + 1) % rotationItemCount;
    rotationItemAcceptedSeconds = 0;
    rotationItemElapsedRealSeconds = 0;
  }

  RotationItem &currentItem = rotationItems[rotationCurrentIndex];

  //"" (empty) means "inherit the default alignment" - see showText()/DISPLAY_JOB_WRITE_LINE.
  rotationCurrentAlignment = rotationAlignmentOverrideToString(currentItem.Alignment);
  //Same "" / -1 = inherit pattern for the per-item animation override.
  rotationCurrentAnimationMode = rotationAnimationModeOverrideToString(currentItem.AnimationMode);
  rotationCurrentAnimationStaggerMs = currentItem.AnimationStaggerMs;

  //Recompute inputText for whichever item is current every second, not just once when it becomes
  //current - keeps a clock/date/countdown item live for as long as it stays on screen.
  if (currentItem.Type == RotationItemType::Clock || currentItem.Type == RotationItemType::Date) {
    inputText = timezone.dateTime(convertUserDateFormatToEzTime(currentItem.Format));
  } else if (currentItem.Type == RotationItemType::Countdown) {
    long differenceSeconds = currentItem.TargetDateTimeUnix - (long)nowSeconds;
    long days = differenceSeconds > 0 ? (long)ceil(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    //Per-item prefix/suffix, independent of the standalone Countdown mode's own setting - see
    //buildDayCountText() (ServiceCountdownFunctions.ino).
    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else if (currentItem.Type == RotationItemType::Countup) {
    long differenceSeconds = (long)nowSeconds - currentItem.TargetDateTimeUnix;
    long days = differenceSeconds > 0 ? (long)floor(differenceSeconds / 60.0 / 60.0 / 24.0) : 0;

    inputText = buildDayCountText(days, currentItem.Prefix, currentItem.Suffix);
  } else { //DEVICE_MODE_TEXT
    inputText = currentItem.Value;
  }
}

//Rebuilds the whole rotation list from a JSON array - a full replace, the client always submits
//the complete list. Extra entries beyond ROTATION_ITEMS_MAX are dropped as a server-side backstop.
//Returns whether the submitted JSON was actually valid and applied, so the caller can report
//failure instead of always answering 200 OK.
bool replaceRotationItemsFromJson(const String &rotationItemsJson) {
  //Every individual item field below already uses ArduinoJson's `| default` fallback operator,
  //which never fails, so the only real validation concern is at the TOP level (malformed JSON, or
  //valid JSON that isn't an array) - only touch rotationItemCount/rotationItems/the persisted
  //file once that's confirmed, leaving the previous, working list untouched otherwise.
  if (rotationItemsJson == "") {
    return true; //nothing submitted - leave the existing list exactly as it is, don't wipe it either
  }

  { //scope: the parsed document is released before the items get saved below
  JsonDocument jsonDocument;
  DeserializationError deserialisationError = deserializeJson(jsonDocument, rotationItemsJson);

  if (deserialisationError || !jsonDocument.is<JsonArray>()) {
    SerialPrintln(F("Invalid rotation items JSON - keeping the existing list, not touching it"));
    return false;
  }

  //Pre-validate BEFORE touching any existing state - same "leave the previous list untouched on
  //any validation failure" philosophy as the top-level JSON check above. A text item whose Value
  //needs more lines than fit would otherwise fail forever inside checkRotation(), permanently
  //blocking the rotation from ever progressing past it. Only the text type can be checked exactly
  //here - clock/date/countdown/countup render dynamic content checkRotation() bounds at runtime
  //instead. "" (mode) / -1 (stagger) means "inherit the global default".
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

  //Couldn't be saved (e.g. out of memory): roll back to the untouched file instead of running a
  //list that would be gone after the next reboot (CODE_REVIEW_V7.md #1).
  if (!writeRotationItemsToFile()) {
    rotationItemCount = 0;
    loadRotationItemsFromFile();
    releaseUnusedRotationItems();
    return false;
  }
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

//Release any String heap buffers left over in now-unused slots - plain "=" assignment reuses
//existing capacity rather than freeing it; a fresh temporary's move-assignment does free it.
void releaseUnusedRotationItems() {
  for (int i = rotationItemCount; i < ROTATION_ITEMS_MAX; i++) {
    rotationItems[i] = RotationItem();
  }
}

//Appends the persisted rotation items (element by element, see readJsonArrayFile()) - at boot,
//and to roll back after a failed save. Callers reset rotationItemCount first. A missing or broken
//file just leaves the list empty, as before.
void loadRotationItemsFromFile() {
  readJsonArrayFile(rotationItemsPath, appendRotationItemFromJson);
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
