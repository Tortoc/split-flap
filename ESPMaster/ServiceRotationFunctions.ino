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

  //Bug (found while fixing #117): timezone.now() doesn't return real UTC (see getRealUtcNow()'s
  //comment), but Countdown/Countup rotation items' TargetDateTimeUnix is real UTC seconds from the
  //client - see the matching comment in checkCountdown() (ServiceCountdownFunctions.ino). Only
  //used below for the item CONTENT (clock/date/countdown/countup text), not for timing the
  //item's turn - see rotationItemAcceptedSeconds' own comment for that.
  unsigned long nowSeconds = getRealUtcNow();

  //Direct array indexing - see the comment on rotationItems' declaration (ESPMaster.ino) for why
  //this is a fixed array rather than an LList.
  RotationItem &item = rotationItems[rotationCurrentIndex];
  int durationSeconds = item.DurationSeconds > 0 ? item.DurationSeconds : 5;

  //Defense-in-depth against a permanently-stuck item (CODE_REVIEW_V3.md #13, see
  //rotationItemElapsedRealSeconds' own comment) - counts every tick regardless of acceptance, so
  //it keeps climbing even while rotationItemAcceptedSeconds itself never moves. A generous grace
  //period past the item's own configured duration distinguishes "genuinely just slow to render"
  //(large flap-position change, multi-line item) from "will never succeed" without being
  //trigger-happy about ordinary rendering delays.
  rotationItemElapsedRealSeconds++;
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
  rotationCurrentAlignment = currentItem.Alignment;
  //Same "" / -1 = inherit pattern for the per-item animation override (found 2026-09-10,
  //"Animation pro Rotation-Item") - see the globals' own comment (ESPMaster.ino).
  rotationCurrentAnimationMode = currentItem.AnimationMode;
  rotationCurrentAnimationStaggerMs = currentItem.AnimationStaggerMs;

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
//Returns whether the submitted JSON was actually valid and applied - previously void, silently
//wiping the list on invalid input with no way for the caller to even know (found 2026-09-09,
//CODE_REVIEW.md #24).
bool replaceRotationItemsFromJson(String rotationItemsJson) {
  //rotationItemCount used to be zeroed unconditionally right here, before the JSON was even
  //looked at - a syntax error or a top-level value that isn't a JSON array fell through to the
  //`else` below, which only logged a message and did nothing to undo that reset. The existing,
  //previously-working rotation list was gone regardless, and writeRotationItemsToFile() at the
  //end ran unconditionally too, persisting the now-empty list - while the caller
  //(ESPMaster.ino's /api/settings POST handler) still unconditionally answered 200 OK, since
  //this function returned nothing to report failure with.
  //
  //Every individual item field below already uses ArduinoJson's `| default` fallback operator,
  //which never fails - a missing or wrong-typed field just becomes a sensible default, not an
  //error. So there's no genuine PER-ITEM validation concern to guard against here, only the
  //TOP-LEVEL one (malformed JSON, or valid JSON that isn't an array) - fixed by only touching
  //rotationItemCount/rotationItems/the persisted file once that's already confirmed to hold,
  //leaving the previous, working list completely untouched otherwise.
  if (rotationItemsJson == "") {
    return true; //nothing submitted - leave the existing list exactly as it is, don't wipe it either
  }

  JsonDocument jsonDocument;
  DeserializationError deserialisationError = deserializeJson(jsonDocument, rotationItemsJson);

  if (deserialisationError || !jsonDocument.is<JsonArray>()) {
    SerialPrintln(F("Invalid rotation items JSON - keeping the existing list, not touching it"));
    return false;
  }

  //Pre-validate BEFORE touching any existing state - same "leave the previous, working list
  //completely untouched on any validation failure" philosophy as the top-level JSON check above.
  //Found 2026-09-10, CODE_REVIEW_V3.md #13: a text-type item whose Value alone needs more lines
  //than fit (MAX_DISPLAY_LINES) passed every check this function otherwise does (the request-size
  //limit is on the whole submitted JSON, not any one item's rendered content) - but every later
  //showText() attempt for it inside checkRotation() then failed FOREVER, and since
  //rotationItemAcceptedSeconds only advances on an ACCEPTED showText() call, the rotation could
  //never progress past that one item again, permanently blocking every item behind it too. Only
  //the text type can be checked exactly here - clock/date/countdown/countup items render dynamic,
  //runtime-computed content that can't be reliably bounded upfront; checkRotation() below carries
  //its own defense-in-depth timeout for those (and for any pre-existing text item saved before
  //this check existed).
  //animationMode/animationStaggerMs validated the same way as POST /api/settings' own fields
  //(found 2026-09-10, "Animation pro Rotation-Item") - "" (mode) / -1 (stagger, absent or
  //explicitly -1) means "inherit the global default", anything else must be one of the known
  //modes / 0-ANIMATION_STAGGER_MS_MAX.
  for (JsonVariant value : jsonDocument.as<JsonArray>()) {
    String itemType = value["type"] | DEVICE_MODE_TEXT;
    if (itemType == DEVICE_MODE_TEXT && isMessageTooLongToRender(String((const char*)(value["value"] | "")))) {
      SerialPrintln(F("Rotation items JSON rejected: a text item needs more lines than fit to display - keeping the existing list, not touching it"));
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

    RotationItem &item = rotationItems[rotationItemCount];
    item.Type = value["type"] | DEVICE_MODE_TEXT;
    item.Value = String((const char*)(value["value"] | ""));
    item.Format = String((const char*)(value["format"] | ""));
    item.TargetDateTimeUnix = value["targetDateTimeUnix"] | 0;
    item.Prefix = String((const char*)(value["prefix"] | ""));
    item.Suffix = String((const char*)(value["suffix"] | ""));
    item.DurationSeconds = value["durationSeconds"] | 5;
    item.Alignment = String((const char*)(value["alignment"] | ""));
    item.AnimationMode = String((const char*)(value["animationMode"] | ""));
    item.AnimationStaggerMs = value["animationStaggerMs"] | -1;
    rotationItemCount++;
  }

  //Always start a freshly-submitted sequence from the beginning.
  rotationCurrentIndex = 0;
  rotationItemAcceptedSeconds = 0;
  rotationItemElapsedRealSeconds = 0;

  writeRotationItemsToFile();
  return true;
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
    itemObject["animationMode"] = item.AnimationMode;
    itemObject["animationStaggerMs"] = item.AnimationStaggerMs;
  }

  String rotationItemsJson;
  serializeJson(rotationItemsDocument, rotationItemsJson);
  writeFile(LittleFS, rotationItemsPath, rotationItemsJson.c_str());
}
