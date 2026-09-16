//Primary<->Extension communication (multiple-units.md) - lets a "primary" device stretch its
//display logic across its own units plus one or more configured "extension" devices' units.
//
//DELIBERATE, DOCUMENTED DEVIATION from this project's "no blocking network calls" lesson
//(ServiceAsyncTimeSyncFunctions.ino's non-blocking DNS+UDP rewrite, Punkt 172): the health check
//and display push below both use HTTPClient with a SHORT, bounded timeout
//(EXTENSION_HTTP_TIMEOUT_MS). Materially different from that earlier anti-pattern: opt-in only
//(deviceRole==primary with a configured host), ~10x shorter bound, and the health check runs on
//its own timer separate from the display push, so an unreachable extension is already known
//before a push happens and is simply skipped.
#define EXTENSION_HEALTH_CHECK_INTERVAL_MS (15UL * 1000UL)
#define EXTENSION_HTTP_TIMEOUT_MS 1500
unsigned long lastExtensionHealthCheckAt = 0;

//The effective display width for the Primary's own text-composition pipeline - own units plus
//every currently-reachable configured extension's own unitsAmount. Returns plain unitsAmount
//unchanged for every other role.
int combinedUnitsAmount() {
  int total = unitsAmount;
  if (deviceRole != DEVICE_ROLE_PRIMARY) {
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
  if (otaChunkBuffer != nullptr || otaPendingPhase != OTA_PHASE_NONE) {
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
  }
}

//Sends one already-sliced, already-aligned segment to one configured extension's POST
///api/extension-display, along with animation context (globalOffset/combinedWidth) so the
//extension can compute its own units' stagger delay with the same formula the primary uses
//locally. Best-effort, no retry - a failed push is lost, the next showText() call simply retries.
//Does NOT re-check reachability itself - callers only invoke this for extensions the last
//checkExtensionHealth() sweep found reachable.
bool pushSegmentToExtension(ExtensionDevice &ext, String segment, int globalOffset, int combinedWidth) {
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
    "&combinedWidth=" + String(combinedWidth);
  int httpCode = http.POST(body);
  http.end();
  return httpCode == 200;
}

//Called once per newly-written display line - slices displayJob.currentAlignedLine into one
//segment per configured, reachable extension, in configuration order, starting right after this
//device's own unitsAmount characters. The push itself is a one-shot request; the receiving
//extension staggers its own writes using the animation context sent along with it.
void pushExtensionSegments() {
  if (deviceRole != DEVICE_ROLE_PRIMARY) {
    return;
  }
  if (otaChunkBuffer != nullptr || otaPendingPhase != OTA_PHASE_NONE) {
    return; //same OTA-collision guard as checkExtensionHealth() above
  }
  int combinedWidth = combinedUnitsAmount();
  int offset = unitsAmount;
  for (int i = 0; i < extensionDeviceCount; i++) {
    ExtensionDevice &ext = extensionDevices[i];
    if (ext.Host == "" || !ext.Reachable) {
      continue; //fail-soft: skip, don't block/retry inline - see this function's own top comment
    }
    String segment = displayJob.currentAlignedLine.substring(offset, offset + ext.UnitsAmount);
    pushSegmentToExtension(ext, segment, offset, combinedWidth);
    offset += ext.UnitsAmount;
  }
}

//Extension-side receiver: writes a pushed segment directly to this device's own units, bypassing
//showText()/DisplayJob entirely - an extension has no line-wrap/alignment logic of its own, the
//primary already did that against the combined width. Uses this device's OWN flapSpeed.
//
//globalOffset/combinedWidth let this device compute each unit's delay with the same
//computeUnitAnimationDelayMs() formula the primary uses, so e.g. "outside-in" reads as one
//continuous animation across devices. Applied as a simple blocking wait-until-due loop - a
//deliberate, bounded exception to the non-blocking rule (small unit count, bounded by
//ANIMATION_STAGGER_MS_MAX times a small number, opt-in only).
void applyExtensionDisplaySegment(String segment, String animationMode, unsigned long staggerMs, int globalOffset, int combinedWidth) {
  int speed = convertSpeed(flapSpeed);
  int segmentLength = (int)segment.length();
  unsigned long startTime = millis();
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    char letterChar = unitIndex < segmentLength ? segment[unitIndex] : ' ';
    int letterPosition = translateLettertoInt(letterChar);
    if (letterPosition == -1) {
      letterPosition = translateLettertoInt('?');
    }
    unsigned long delayMs = computeUnitAnimationDelayMs(globalOffset + unitIndex, combinedWidth, animationMode, staggerMs);
    while (millis() - startTime < delayMs) {
      yield(); //bounded - see this function's own top comment
    }
    writeToUnit(unitIndex, letterPosition, speed);
  }
}

//Persists a new extension host list - full replace, same convention as
//replaceRotationItemsFromJson(). Resets Reachable/UnitsAmount for every entry so a changed list
//doesn't carry over a previous entry's stale status.
bool replaceExtensionDevicesFromJson(String json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonArray>()) {
    return false;
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
  return true;
}
