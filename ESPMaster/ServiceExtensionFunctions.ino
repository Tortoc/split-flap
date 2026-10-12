//Primary<->Extension communication (multiple-units.md) - lets a "primary" device stretch its
//display logic across its own units plus one or more configured "extension" devices' units.
//
//DELIBERATE, DOCUMENTED DEVIATION from this project's "no blocking network calls" lesson
//(ServiceAsyncTimeSyncFunctions.ino's non-blocking DNS+UDP rewrite, Punkt 172): the health check,
//display push, and extension-readiness poll below all use HTTPClient with a SHORT, bounded timeout
//(EXTENSION_HTTP_TIMEOUT_MS, declared in ESPMaster.ino - see its own comment for why). Materially
//different from that earlier anti-pattern: opt-in only (deviceRole==primary with a configured
//host), ~10x shorter bound, and the health check runs on its own timer separate from the display
//push, so an unreachable extension is already known before a push happens and is simply skipped.
unsigned long lastExtensionHealthCheckAt = 0;

//The effective display width for the Primary's own text-composition pipeline - own units plus
//every currently-reachable configured extension's own unitsAmount. Returns plain unitsAmount
//unchanged for every other role, and in mirror link mode (mirrors don't widen the display).
int combinedUnitsAmount() {
  int total = unitsAmount;
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionLinkMode == EXTENSION_LINK_MODE_MIRROR) {
    return total;
  }
  for (int i = 0; i < extensionDeviceCount; i++) {
    if (extensionDevices[i].Reachable) {
      total += extensionDevices[i].UnitsAmount;
    }
  }
  return total;
}

//Polls every configured extension's existing GET /api/units once per
//EXTENSION_HEALTH_CHECK_INTERVAL_MS. Ticked from loop()'s per-second block.
void checkExtensionHealth() {
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionDeviceCount == 0) {
    return;
  }
  if (webFirmwareUpdateInProgress()) {
    return; //don't contend with an active OTA update - a blocking HTTPClient call here hung the
    //device solid (no crash-log entry, needed a power cycle) when it collided with an incoming
    //firmware-chunk upload
  }
  if (!millisReached(lastExtensionHealthCheckAt + EXTENSION_HEALTH_CHECK_INTERVAL_MS)) {
    return;
  }
  lastExtensionHealthCheckAt = millis();

  for (int i = 0; i < extensionDeviceCount; i++) {
    ExtensionDevice &ext = extensionDevices[i];
    if (ext.Host == "") {
      continue;
    }
    bool wasReachable = ext.Reachable;
    int previousUnitsAmount = ext.UnitsAmount;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
    bool ok = false;
    if (http.begin(client, "http://" + ext.Host + "/api/units")) {
      if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          ext.UnitsAmount = doc["unitsAmount"] | 0;
          ok = ext.UnitsAmount > 0;
        }
      }
      http.end();
    }
    ext.Reachable = ok;
    ext.LastCheckedAt = millis();
    SerialPrintln(F("Extension device \"") + ext.Host + F("\": ") + (ok ? (F("reachable, ") + String(ext.UnitsAmount) + F(" units")) : F("unreachable")));

    //combinedUnitsAmount() only reflects THIS extension's units while Reachable - a transition
    //either way, or a changed unit count while already reachable, changes the effective combined
    //width even though the requested text itself never changed. See displayRenderGeneration's own
    //comment (CODE_REVIEW_V6.md #3).
    if (ext.Reachable != wasReachable || ext.UnitsAmount != previousUnitsAmount) {
      displayRenderGeneration++;
    }
  }
}

//Sends one already-sliced, already-aligned segment to one configured extension's POST
///api/extension-display, along with animation context (globalOffset/combinedWidth) so the
//extension can compute its own units' stagger delay with the same formula the primary uses
//locally. Best-effort, no retry - a failed push is lost, the next showText() call simply retries.
//Does NOT re-check reachability itself - callers only invoke this for extensions the last
//checkExtensionHealth() sweep found reachable.
bool pushSegmentToExtension(ExtensionDevice &ext, const String &segment, int globalOffset, int combinedWidth) {
  //How far this line's animation already is on this device when the push arrives over there:
  //time since the line started here (local unit writes, pushes to earlier extensions) plus an
  //estimate of the network delay - so the extension's units start on the same shared clock instead
  //of whenever the push happened to arrive (CODE_REVIEW_V7.md, common start time). Capped, since a
  //round trip that included a slow TCP connect would otherwise skew the estimate.
  unsigned long startOffsetMs = (millis() - displayJob.lineWriteStartMillis) + min(ext.LastPushRoundTripMs / 2, 250UL);
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
  if (!http.begin(client, "http://" + ext.Host + "/api/extension-display")) {
    return false;
  }
  http.addHeader("X-OTA-Password", otaPassword);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "segment=" + urlEncodeForFormBody(segment) +
    "&animationMode=" + urlEncodeForFormBody(displayJob.animationModeValue) +
    "&staggerMs=" + String(displayJob.animationStaggerMsValue) +
    "&globalOffset=" + String(globalOffset) +
    "&combinedWidth=" + String(combinedWidth) +
    "&startOffsetMs=" + String(startOffsetMs);
  unsigned long sentAt = millis();
  int httpCode = http.POST(body);
  http.end();
  if (httpCode == 200) {
    ext.LastPushRoundTripMs = millis() - sentAt;
  }
  return httpCode == 200;
}

//Called once per newly-written display line - slices displayJob.currentAlignedLine into one
//segment per configured, reachable extension, in configuration order, starting right after this
//device's own unitsAmount characters. In mirror link mode every extension instead gets this
//device's own characters (offset 0, own width as the animation width, so each mirror unit starts
//exactly when the primary's unit at the same index does) - applyExtensionDisplaySegment() on the
//receiving side already cuts off or blank-pads a segment that doesn't match its unit count. The
//push itself is a one-shot request; the receiving extension staggers its own writes using the
//animation context sent along with it.
void pushExtensionSegments() {
  if (deviceRole != DEVICE_ROLE_PRIMARY) {
    return;
  }
  if (webFirmwareUpdateInProgress()) {
    //Same OTA-collision guard as checkExtensionHealth() above - any reachable extension is left
    //undelivered this tick. Flag it the same as a failed push below so finishDisplayJob() forces a
    //retry once OTA is done, instead of silently caching this render as complete
    //(CODE_REVIEW_V6.md #4).
    for (int i = 0; i < extensionDeviceCount; i++) {
      if (extensionDevices[i].Host != "" && extensionDevices[i].Reachable) {
        displayJob.hadWriteFailure = true;
        break;
      }
    }
    return;
  }
  bool mirror = extensionLinkMode == EXTENSION_LINK_MODE_MIRROR;
  int combinedWidth = combinedUnitsAmount();
  int offset = unitsAmount;
  for (int i = 0; i < extensionDeviceCount; i++) {
    ExtensionDevice &ext = extensionDevices[i];
    if (ext.Host == "" || !ext.Reachable) {
      continue; //fail-soft: skip, don't block/retry inline - see this function's own top comment
    }
    String segment = mirror
      ? displayJob.currentAlignedLine.substring(0, min(unitsAmount, ext.UnitsAmount))
      : displayJob.currentAlignedLine.substring(offset, offset + ext.UnitsAmount);
    if (!pushSegmentToExtension(ext, segment, mirror ? 0 : offset, combinedWidth)) {
      //Leaves this extension showing stale content - mark the same write-failure flag
      //writeDueAnimatedUnits() uses for a failed local I2C write, so finishDisplayJob() forces a
      //retry on the next showText() call for the same text instead of caching this as delivered
      //(CODE_REVIEW_V6.md #4).
      displayJob.hadWriteFailure = true;
    }
    offset += ext.UnitsAmount;
  }
}

//Extension-side receiver state: a pushed segment is written to this device's own units, bypassing
//showText()/DisplayJob entirely - an extension has no line-wrap/alignment logic of its own, the
//primary already did that against the combined width. Uses this device's OWN flapSpeed.
//
//globalOffset/combinedWidth let this device compute each unit's delay with the same
//computeUnitAnimationDelayMs() formula the primary uses, so e.g. "outside-in" reads as one
//continuous animation across devices. Run as a non-blocking per-tick state machine, exactly like
//the primary's own writeDueAnimatedUnits(): every pass writes whichever units are due, in any
//order - a plain "wait for unit 0, then unit 1, ..." loop wrote right-to-left/outside-in/inside-out
//in the wrong order and blocked loop() for the whole animation (CODE_REVIEW_V7.md #2/#3).
struct ExtensionDisplayState {
  bool active = false;
  String segment;
  String animationMode;
  unsigned long staggerMs = 0;
  int globalOffset = 0;
  int combinedWidth = 0;
  int speed = 0;
  //Set back by the push's startOffsetMs, so delays count from the primary's line start.
  unsigned long startedAt = 0;
  uint64_t writtenMask = 0;
  //Same semantics as the primary's displayJob.hadWriteFailure: one attempt per unit, a failed
  //write is reported (GET /api/units' displayWriteFailed) so the primary re-sends the whole line
  //instead of counting it as shown (CODE_REVIEW_V7.md #4).
  bool writeFailed = false;
};
ExtensionDisplayState extensionDisplay;

//Ticked from loop() on every pass. Picks up a newly queued push (POST /api/extension-display),
//replacing whatever was still animating - only the latest content matters. Only an extension
//writes pushed content (cancelDisplayControllers() also drops it on a role change).
void tickExtensionDisplay() {
  if (deviceRole != DEVICE_ROLE_EXTENSION) {
    return;
  }
  if (pendingExtensionDisplayQueued) {
    pendingExtensionDisplayQueued = false;
    extensionDisplay.active = true;
    extensionDisplay.segment = pendingExtensionDisplaySegment;
    extensionDisplay.animationMode = pendingExtensionDisplayAnimationMode;
    extensionDisplay.staggerMs = pendingExtensionDisplayStaggerMs;
    extensionDisplay.globalOffset = pendingExtensionDisplayGlobalOffset;
    extensionDisplay.combinedWidth = pendingExtensionDisplayCombinedWidth;
    extensionDisplay.speed = convertSpeed(flapSpeed);
    extensionDisplay.startedAt = millis() - pendingExtensionDisplayStartOffsetMs;
    extensionDisplay.writtenMask = 0;
    extensionDisplay.writeFailed = false;
  }
  if (!extensionDisplay.active) {
    return;
  }

  int segmentLength = (int)extensionDisplay.segment.length();
  bool allWritten = true;
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    uint64_t unitBit = 1ULL << unitIndex;
    if (extensionDisplay.writtenMask & unitBit) {
      continue;
    }
    unsigned long delayMs = computeUnitAnimationDelayMs(extensionDisplay.globalOffset + unitIndex, extensionDisplay.combinedWidth,
      extensionDisplay.animationMode, extensionDisplay.staggerMs);
    if (millis() - extensionDisplay.startedAt < delayMs) {
      allWritten = false;
      continue; //not due yet this pass
    }
    char letterChar = unitIndex < segmentLength ? extensionDisplay.segment[unitIndex] : ' ';
    int letterPosition = translateLettertoInt(letterChar);
    if (letterPosition == -1) {
      letterPosition = translateLettertoInt('?');
    }
    if (!writeToUnit(unitIndex, letterPosition, extensionDisplay.speed)) {
      extensionDisplay.writeFailed = true;
    }
    extensionDisplay.writtenMask |= unitBit;
  }
  if (allWritten) {
    extensionDisplay.active = false;
    extensionDisplay.segment = String(); //release the buffer until the next push
  }
}

//The device role changed (POST /api/device-role, run from loop()): neither the local display job
//nor a push received as an extension may keep writing for the old role - a staggered push from the
//old primary or a local multi-line job would interleave with the new role's writes
//(CODE_REVIEW_V8.md #6). The role handler bumped displayRenderGeneration, so the new role redraws
//in full instead of trusting lastWrittenText.
void cancelDisplayControllers() {
  if (isDisplayJobBusy()) {
    SerialPrintln(F("Cancelling the running display job (role change or WiFi setup)"));
    displayJob.state = DISPLAY_JOB_IDLE;
    displayJob.originalMessage = String();
    displayJob.currentAlignedLine = String();
  }
  pendingExtensionDisplayQueued = false;
  pendingExtensionDisplaySegment = String();
  extensionDisplay.active = false;
  extensionDisplay.segment = String();
  extensionDisplay.writeFailed = false;
  displayRenderGeneration++;
}

//Still has letters to send (or a push waiting to start) - GET /api/units reports this as
//displayBusy, so the primary's readiness poll doesn't take idle motors for "done" between a push
//and the moment the last staggered unit actually got its letter.
bool extensionDisplayBusy() {
  return pendingExtensionDisplayQueued || extensionDisplay.active;
}

bool extensionDisplayWriteFailed() {
  return extensionDisplay.writeFailed;
}

//Persists a new extension host list - full replace, same convention as
//replaceRotationItemsFromJson(). Resets Reachable/UnitsAmount for every entry so a changed list
//doesn't carry over a previous entry's stale status.
bool replaceExtensionDevicesFromJson(String json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonArray>()) {
    return false;
  }
  //Checked up front, so a list with one bad entry changes nothing.
  for (JsonVariant value : doc.as<JsonArray>()) {
    String host = String((const char*)(value | ""));
    host.trim();
    if (host != "" && !isValidExtensionHost(host)) {
      return false;
    }
  }

  extensionDeviceCount = 0;
  for (JsonVariant value : doc.as<JsonArray>()) {
    if (extensionDeviceCount >= EXTENSION_DEVICES_MAX) {
      break;
    }
    String host = String((const char*)(value | ""));
    host.trim();
    if (host == "") {
      continue;
    }
    extensionDevices[extensionDeviceCount] = ExtensionDevice(); //fresh: Reachable=false, UnitsAmount=0
    extensionDevices[extensionDeviceCount].Host = host;
    extensionDeviceCount++;
  }
  //Release any leftover String heap buffers in now-unused slots - same reasoning as
  //replaceRotationItemsFromJson()'s own identical cleanup loop.
  for (int i = extensionDeviceCount; i < EXTENSION_DEVICES_MAX; i++) {
    extensionDevices[i] = ExtensionDevice();
  }

  JsonDocument outDoc;
  JsonArray outArray = outDoc.to<JsonArray>();
  for (int i = 0; i < extensionDeviceCount; i++) {
    outArray.add(extensionDevices[i].Host);
  }
  String extensionDevicesJsonOut;
  serializeJson(outDoc, extensionDevicesJsonOut);
  writeFile(LittleFS, extensionDevicesPath, extensionDevicesJsonOut.c_str());

  //The configured host list is part of the effective combined width/topology - see
  //displayRenderGeneration's own comment (CODE_REVIEW_V6.md #3).
  displayRenderGeneration++;
  return true;
}
