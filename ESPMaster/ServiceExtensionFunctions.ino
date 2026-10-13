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
    int httpCode = 0;
    String failure;
    if (http.begin(client, "http://" + ext.Host + "/api/units")) {
      httpCode = http.GET();
      if (httpCode == 200) {
        String body = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, body);
        if (!error) {
          ext.UnitsAmount = doc["unitsAmount"] | 0;
          ok = ext.UnitsAmount > 0;
        } else {
          failure = String(error.c_str()) + F(", ") + String(body.length()) + F(" bytes");
        }
      }
      http.end();
    }
    ext.Reachable = ok;
    ext.LastCheckedAt = millis();
    SerialPrintln(F("Extension device \"") + ext.Host + F("\": ") + (ok ? (F("reachable, ") + String(ext.UnitsAmount) + F(" units"))
      : (F("unreachable (HTTP ") + String(httpCode) + (failure != "" ? F(", ") + failure : String()) + F(")"))));

    //combinedUnitsAmount() only reflects THIS extension's units while Reachable - a transition
    //either way, or a changed unit count while already reachable, changes the effective combined
    //width even though the requested text itself never changed. See displayRenderGeneration's own
    //comment (CODE_REVIEW_V6.md #3).
    if (ext.Reachable != wasReachable || ext.UnitsAmount != previousUnitsAmount) {
      displayRenderGeneration++;
    }
  }
}

//Signed pushes (security review 2026-10-12, OW-10/OW-02). A push used to carry the primary's admin
//password in plaintext. Now the primary signs it with HMAC-SHA256 under the secret both devices
//share since pairing, and only the signature travels. Replays: the extension hands out a random
//nonce (in every answer, and a 409 when the push used a stale one) that each push has to name,
//plus the primary's per-boot id and a counter - a seen counter is refused, and the extension
//picks a new nonce whenever a new primary boot shows up, so nothing captured earlier matches.
uint32_t linkBootId = 0; //primary side, random per boot
uint32_t linkSeq = 0;
char linkNonce[17] = ""; //extension side, 16 hex chars
bool linkPrimarySeen = false;
uint32_t linkLastBootId = 0;
uint32_t linkLastSeq = 0;

void rotateLinkNonce() {
  uint8_t randomBytes[8];
  ESP.random(randomBytes, sizeof(randomBytes));
  strlcpy(linkNonce, bytesToHex(randomBytes, sizeof(randomBytes)).c_str(), sizeof(linkNonce));
}

const char* currentLinkNonce() {
  if (linkNonce[0] == '\0') {
    rotateLinkNonce();
  }
  return linkNonce;
}

//A new pairing starts a fresh replay history.
void resetLinkReplayState() {
  linkPrimarySeen = false;
  rotateLinkNonce();
}

//Signed in this order, by both sides. Index 6-8 are the replay fields.
const char* const linkSignedFields[] = { "segment", "animationMode", "staggerMs", "globalOffset",
  "combinedWidth", "startOffsetMs", "nonce", "boot", "seq" };
#define LINK_SIGNED_FIELD_COUNT 9

//Length-prefixed, so no field value can shift its bytes into another field.
void appendLinkField(String &message, const String &value) {
  message += value.length();
  message += ':';
  message += value;
  message += '\n';
}

//Hex HMAC-SHA256 under a 64-hex-char secret, "" if the secret is malformed.
String linkSignature(const String &secretHex, const String &message) {
  uint8_t key[32];
  if (!hexToBytes(secretHex, key, sizeof(key))) {
    return "";
  }
  uint8_t mac[32];
  hmacSha256(key, sizeof(key), (const uint8_t*)message.c_str(), message.length(), mac);
  return bytesToHex(mac, sizeof(mac));
}

//Extension side: checks a POST /api/extension-display push and advances the replay state.
//Returns the HTTP status to answer with: 200, 401 (bad signature), 403 (not paired) or 409 (stale
//nonce/counter - the answer names the current nonce, the primary retries with it).
int checkExtensionLinkSignature(AsyncWebServerRequest *request) {
  if (pairedPrimaryLinkSecret == "") {
    //Unpaired: only a device without an admin password still takes unsigned pushes, as before.
    return isAdminPasswordSet() ? 403 : 200;
  }
  String values[LINK_SIGNED_FIELD_COUNT];
  String message;
  for (int i = 0; i < LINK_SIGNED_FIELD_COUNT; i++) {
    const char* field = linkSignedFields[i];
    values[i] = request->hasParam(field, true) ? request->getParam(field, true)->value() : String();
    appendLinkField(message, values[i]);
  }
  String provided = request->hasParam("sig", true) ? request->getParam("sig", true)->value() : String();
  String expected = linkSignature(pairedPrimaryLinkSecret, message);
  if (expected == "" || !constantTimeEquals(provided, expected)) {
    return 401;
  }
  if (values[6] != currentLinkNonce()) {
    return 409;
  }
  uint32_t bootId = strtoul(values[7].c_str(), nullptr, 10);
  uint32_t seq = strtoul(values[8].c_str(), nullptr, 10);
  if (linkPrimarySeen && bootId == linkLastBootId) {
    if (seq <= linkLastSeq) {
      return 409;
    }
  } else if (linkPrimarySeen) {
    rotateLinkNonce(); //the primary restarted - pushes captured before can never match again
  }
  linkPrimarySeen = true;
  linkLastBootId = bootId;
  linkLastSeq = seq;
  return 200;
}

//Sends one already-sliced, already-aligned segment to one configured extension's POST
///api/extension-display, along with animation context (globalOffset/combinedWidth) so the
//extension can compute its own units' stagger delay with the same formula the primary uses
//locally. Best-effort - a failed push is lost, the next showText() call simply retries. The one
//exception is a 409 (stale nonce, e.g. right after either device restarted): retried once at once
//with the nonce that answer named.
//Does NOT re-check reachability itself - callers only invoke this for extensions the last
//checkExtensionHealth() sweep found reachable.
bool pushSegmentToExtension(ExtensionDevice &ext, const String &segment, int globalOffset, int combinedWidth) {
  //How far this line's animation already is on this device when the push arrives over there:
  //time since the line started here (local unit writes, pushes to earlier extensions) plus an
  //estimate of the network delay - so the extension's units start on the same shared clock instead
  //of whenever the push happened to arrive (CODE_REVIEW_V7.md, common start time). Capped, since a
  //round trip that included a slow TCP connect would otherwise skew the estimate.
  unsigned long startOffsetMs = (millis() - displayJob.lineWriteStartMillis) + min(ext.LastPushRoundTripMs / 2, 250UL);
  while (linkBootId == 0) {
    linkBootId = ESP.random();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    String fields[LINK_SIGNED_FIELD_COUNT] = { segment, displayJob.animationModeValue, String(displayJob.animationStaggerMsValue),
      String(globalOffset), String(combinedWidth), String(startOffsetMs), ext.LinkNonce, String(linkBootId), String(++linkSeq) };
    String body;
    String message;
    for (int i = 0; i < LINK_SIGNED_FIELD_COUNT; i++) {
      body += i == 0 ? "" : "&";
      body += linkSignedFields[i];
      body += '=';
      body += urlEncodeForFormBody(fields[i]);
      appendLinkField(message, fields[i]);
    }
    if (primaryLinkSecret != "") {
      body += F("&sig=");
      body += linkSignature(primaryLinkSecret, message);
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
    if (!http.begin(client, "http://" + ext.Host + "/api/extension-display")) {
      return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    unsigned long sentAt = millis();
    int httpCode = http.POST(body);
    //200 and 409 both answer with the extension's current nonce (16 hex chars).
    String answer = (httpCode == 200 || httpCode == 409) ? http.getString() : String();
    http.end();
    ext.LinkRejected = httpCode == 401 || httpCode == 403;
    if (answer.length() == 16) {
      ext.LinkNonce = answer;
    }
    if (httpCode == 200) {
      ext.LastPushRoundTripMs = millis() - sentAt;
      return true;
    }
    if (httpCode != 409) {
      return false;
    }
  }
  return false;
}

//Primary side, ticked from loop(): runs a pairing queued by POST /api/extension-devices?action=pair.
//Sends this device's link secret (created here on the first pairing) to the extension, authorized
//by the extension's own admin password the user typed in. Both travel over the network this one
//time, like any login; afterwards neither password is ever sent between the devices again.
void runPendingExtensionPairing() {
  if (!pendingExtensionPairing || webFirmwareUpdateInProgress()) {
    return;
  }
  int index = pendingExtensionPairingIndex;
  String password = pendingExtensionPairingPassword;
  pendingExtensionPairingPassword = String();
  pendingExtensionPairing = false;
  if (index < 0 || index >= extensionDeviceCount) {
    return;
  }
  ExtensionDevice &ext = extensionDevices[index];
  if (primaryLinkSecret == "") {
    uint8_t secret[32];
    ESP.random(secret, sizeof(secret));
    primaryLinkSecret = bytesToHex(secret, sizeof(secret));
    writeFile(LittleFS, primaryLinkSecretPath, primaryLinkSecret.c_str());
    checkBootSnapshotFreshness();
  }

  int httpCode = -1;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
  if (http.begin(client, "http://" + ext.Host + "/api/extension-display")) {
    if (password != "") {
      http.addHeader("X-OTA-Password", password);
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    httpCode = http.POST(String(F("pairSecret=")) + primaryLinkSecret);
    http.end();
  }
  password = String();
  ext.PairState = httpCode == 200 ? EXTENSION_PAIR_OK
    : httpCode == 401 ? EXTENSION_PAIR_WRONG_PASSWORD
    : httpCode == 429 ? EXTENSION_PAIR_THROTTLED
    : EXTENSION_PAIR_FAILED;
  ext.LinkNonce = "";
  ext.LinkRejected = false;
  SerialPrintln(F("Pairing with extension \"") + ext.Host + F("\": HTTP ") + String(httpCode));
  if (httpCode == 200) {
    displayRenderGeneration++; //re-send the current line - the extension may have refused it before
  }
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

  pendingExtensionPairing = false; //its index would name a different device now
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
