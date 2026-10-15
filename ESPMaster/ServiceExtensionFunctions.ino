//Primary<->extension link: a primary spreads its display across its own units plus the units of
//configured extension devices.
//
//Deliberate exception to the "no blocking network calls" rule (see ServiceAsyncTimeSyncFunctions.ino):
//health check, push and readiness poll use HTTPClient with a short timeout
//(EXTENSION_HTTP_TIMEOUT_MS, ESPMaster.ino). Acceptable because it's opt-in, ~10x shorter, and the
//health check runs on its own timer, so an unreachable extension is skipped before any push.
unsigned long lastExtensionHealthCheckAt = 0;

//Effective display width for the primary: own units plus every reachable extension's units. Plain
//unitsAmount in every other role and in mirror mode.
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

//Polls each extension's GET /api/units every EXTENSION_HEALTH_CHECK_INTERVAL_MS (from loop()).
void checkExtensionHealth() {
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionDeviceCount == 0) {
    return;
  }
  if (webFirmwareUpdateInProgress()) {
    return; //a blocking HTTPClient call during an OTA upload hung the device solid
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
          //Bounded like this device's own count: an implausible answer would blow up the combined
          //width (and every String built from it) on this small heap.
          int reportedUnitsAmount = doc["unitsAmount"] | 0;
          ok = reportedUnitsAmount > 0 && reportedUnitsAmount <= UNITS_AMOUNT_MAX;
          ext.UnitsAmount = ok ? reportedUnitsAmount : 0;
          if (!ok) {
            failure = String(F("unitsAmount ")) + String(reportedUnitsAmount) + F(" out of range");
          }
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

    //Reachability or unit count changes alter the combined width - see displayRenderGeneration.
    if (ext.Reachable != wasReachable || ext.UnitsAmount != previousUnitsAmount) {
      displayRenderGeneration++;
    }
  }
}

//Signed pushes: the primary signs each push with HMAC-SHA256 under the secret both devices share
//since pairing; no password travels. Replays: every push names the extension's current random nonce
//(sent in every answer, and with a 409 for a stale one) plus the primary's per-boot id and a
//counter. A seen counter is refused, and a new primary boot gets a new nonce.
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

//Extension side: checks a POST /api/extension-display push and advances the replay state. Returns
//200, 401 (bad signature), 403 (not paired) or 409 (stale nonce/counter; the answer names the
//current nonce and the primary retries with it).
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

//Sends one sliced, aligned segment to an extension's POST /api/extension-display, with the
//animation context (globalOffset/combinedWidth) so it staggers its units with the same formula.
//Best effort - a failed push is lost and the next showText() retries; only a 409 (stale nonce, e.g.
//after a restart) is retried at once with the named nonce. Callers only pass extensions the last
//health check found reachable.
bool pushSegmentToExtension(ExtensionDevice &ext, const String &segment, int globalOffset, int combinedWidth) {
  //How far this line's animation already is here when the push arrives: time since the line started
  //plus an estimated network delay, so both devices animate on one clock. Capped, since a slow TCP
  //connect would skew the round-trip estimate.
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

//Primary side, from loop(): runs a pairing queued by POST /api/extension-devices?action=pair. Sends
//this device's link secret (created on first pairing), authorized by the extension's admin password.
//Both cross the network this one time, like a login; afterwards no password is sent between them.
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

//Per newly written line: slices displayJob.currentAlignedLine into one segment per reachable
//extension (configuration order, after this device's own characters). In mirror mode every
//extension gets this device's characters instead (offset 0, own width, so mirrored units start in
//step); the receiver cuts or pads to its own unit count. One request per extension - the extension
//staggers its own writes.
void pushExtensionSegments() {
  if (deviceRole != DEVICE_ROLE_PRIMARY) {
    return;
  }
  if (webFirmwareUpdateInProgress()) {
    //OTA running: nothing is delivered this tick. Flagged like a failed push, so finishDisplayJob()
    //retries after OTA instead of caching the render as complete.
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
      continue; //skip, no inline retry
    }
    String segment = mirror
      ? displayJob.currentAlignedLine.substring(0, min(unitsAmount, ext.UnitsAmount))
      : displayJob.currentAlignedLine.substring(offset, offset + ext.UnitsAmount);
    if (!pushSegmentToExtension(ext, segment, mirror ? 0 : offset, combinedWidth)) {
      //Stale content on that extension - flag it like a failed local write, so the next showText()
      //with the same text retries.
      displayJob.hadWriteFailure = true;
    }
    offset += ext.UnitsAmount;
  }
}

//Extension side: a pushed segment is written straight to this device's units, bypassing
//showText()/DisplayJob (the primary already wrapped and aligned it), with this device's own
//flapSpeed. globalOffset/combinedWidth give each unit the same computeUnitAnimationDelayMs() delay
//as on the primary, so an animation reads as one across devices. Non-blocking per-tick state
//machine like writeDueAnimatedUnits(), writing whichever units are due.
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
  //Like displayJob.hadWriteFailure: one attempt per unit; a failure is reported via GET /api/units
  //(displayWriteFailed) so the primary re-sends the line.
  bool writeFailed = false;
};
ExtensionDisplayState extensionDisplay;

//From loop(): picks up a newly queued push, replacing anything still animating. Only an extension
//writes pushed content (a role change drops it via cancelDisplayControllers()).
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

//Role changed (POST /api/device-role, from loop()): stop both the local job and any received push,
//so nothing from the old role interleaves. displayRenderGeneration was bumped, so the new role
//redraws in full.
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

//Still writing a push - reported as displayBusy, so the primary doesn't take idle motors for done.
bool extensionDisplayBusy() {
  return pendingExtensionDisplayQueued || extensionDisplay.active;
}

bool extensionDisplayWriteFailed() {
  return extensionDisplay.writeFailed;
}

//Replaces the extension host list (full replace, like replaceRotationItemsFromJson()) and resets
//every entry's Reachable/UnitsAmount. Saved before anything changes: on EXTENSION_DEVICES_INVALID
//or _SAVE_FAILED both the active list and the file stay as they were.
int replaceExtensionDevicesFromJson(String json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonArray>()) {
    return EXTENSION_DEVICES_INVALID;
  }
  //Checked up front, so a list with one bad entry changes nothing.
  JsonDocument outDoc;
  JsonArray outArray = outDoc.to<JsonArray>();
  for (JsonVariant value : doc.as<JsonArray>()) {
    String host = String((const char*)(value | ""));
    host.trim();
    if (host == "") {
      continue;
    }
    if (!isValidExtensionHost(host)) {
      return EXTENSION_DEVICES_INVALID;
    }
    if (outArray.size() < EXTENSION_DEVICES_MAX) {
      outArray.add(host);
    }
  }
  String extensionDevicesJsonOut;
  serializeJson(outDoc, extensionDevicesJsonOut);
  if (!writeAtomicFile(LittleFS, extensionDevicesPath, extensionDevicesJsonOut.c_str())) {
    return EXTENSION_DEVICES_SAVE_FAILED;
  }

  pendingExtensionPairing = false; //its index would name a different device now
  extensionDeviceCount = 0;
  for (JsonVariant value : outArray) {
    extensionDevices[extensionDeviceCount] = ExtensionDevice(); //fresh: Reachable=false, UnitsAmount=0
    extensionDevices[extensionDeviceCount].Host = String((const char*)value);
    extensionDeviceCount++;
  }
  //Free String buffers in now-unused slots (see replaceRotationItemsFromJson()).
  for (int i = extensionDeviceCount; i < EXTENSION_DEVICES_MAX; i++) {
    extensionDevices[i] = ExtensionDevice();
  }

  //The host list is part of the combined width - see displayRenderGeneration.
  displayRenderGeneration++;
  return EXTENSION_DEVICES_OK;
}
