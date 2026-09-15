//Primary<->Extension communication (multiple-units.md, Phase 1) - lets a "primary" device stretch
//its display logic across its own units plus one or more configured "extension" devices' units,
//without either side needing to know about the other's internals. See multiple-units.md for the
//full architecture; this file implements it.
//
//Uses ESP8266HTTPClient (plain HTTP, no TLS - same "trusted local network, password header"
//convention as every other endpoint in this project). Verified IRAM-neutral before adding this
//dependency - see ESPMaster.ino's own include comment.
//
//DELIBERATE, DOCUMENTED DEVIATION from this project's own established "no blocking network calls"
//lesson (ServiceAsyncTimeSyncFunctions.ino's non-blocking DNS+UDP rewrite, Punkt 172 - a blocking
//NTP/timezone call colliding with WiFiManager's reconnect handling caused a real, live-reproduced
//watchdog crash): the health check and the display push below both use HTTPClient with a SHORT,
//bounded timeout (EXTENSION_HTTP_TIMEOUT_MS). Three things make this materially different from
//that earlier anti-pattern rather than a repeat of it: (1) it only ever runs on a device an admin
//has explicitly opted into this feature on (deviceRole == primary WITH a configured host) - every
//standalone/extension device, which is every device by default, never calls into this file at all;
//(2) the bound is ~10x shorter than NTP/timezone's old multi-second waits, and HTTPClient's own
//wait loop yields internally, so it can't starve the software watchdog the way that old blocking
//DNS+UDP call could; (3) the health check runs on its own independent, infrequent timer
//(EXTENSION_HEALTH_CHECK_INTERVAL_MS) separate from the actual display push, so a slow/unreachable
//extension is already known BEFORE a real display update needs to happen and that push is simply
//skipped (see pushExtensionSegments()) rather than blocking the display path on a live timeout. A
//genuinely async, from-scratch AsyncClient state machine would remove even this bounded cost, but
//is real additional complexity - noted as a Phase 2+ candidate if this bound ever proves
//insufficient in practice, not attempted here.
#define EXTENSION_HEALTH_CHECK_INTERVAL_MS (15UL * 1000UL)
#define EXTENSION_HTTP_TIMEOUT_MS 1500
unsigned long lastExtensionHealthCheckAt = 0;

//The effective display width for the Primary's own text-composition pipeline
//(processSentenceToLines()/centerString()/leftString()/rightString(), called from
//ServiceDisplayJobFunctions.ino) - own units plus every currently-reachable configured extension's
//own unitsAmount. Returns plain unitsAmount unchanged for every role other than primary (including
//the default standalone), and for a primary with no reachable extensions configured - by
//construction, this feature can never change behavior for a device that isn't deliberately opted
//in AND currently has a live extension to show for it (see multiple-units.md's "Fail-Soft" design).
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

//Polls every configured extension's existing GET /api/units (no new endpoint needed on that side -
//see multiple-units.md point 2) once per EXTENSION_HEALTH_CHECK_INTERVAL_MS. Ticked from loop()'s
//existing per-second block, like every other periodic check there.
void checkExtensionHealth() {
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionDeviceCount == 0) {
    return;
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

//Sends one already-sliced, already-aligned segment (raw display characters, no further processing
//needed on the receiving end) to one configured extension's POST /api/extension-display. Best-
//effort, no retry - a failed push here is exactly as consequential as a single failed local I2C
//write (see DisplayJob::hadWriteFailure elsewhere): this one line update on that one device is
//lost, and the next genuinely new showText() call for different content will simply try again.
//Does NOT re-check reachability itself - callers only invoke this for extensions the last
//checkExtensionHealth() sweep found reachable (see pushExtensionSegments()).
bool pushSegmentToExtension(ExtensionDevice &ext, String segment) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
  if (!http.begin(client, "http://" + ext.Host + "/api/extension-display")) {
    return false;
  }
  http.addHeader("X-OTA-Password", otaPassword);
  int httpCode = http.POST(segment);
  http.end();
  return httpCode == 200;
}

//Called once per newly-written display line (ServiceDisplayJobFunctions.ino's DISPLAY_JOB_WRITE_LINE,
//right alongside the existing local writeDueAnimatedUnits() call - NOT from the animation-stagger
//re-poll state, since Phase 1 doesn't coordinate animation timing across devices, see
//multiple-units.md point 5) - slices displayJob.currentAlignedLine (already sized to
//combinedUnitsAmount() by the caller) into one segment per configured, reachable extension, in
//configuration order, starting right after this device's own unitsAmount characters.
void pushExtensionSegments() {
  if (deviceRole != DEVICE_ROLE_PRIMARY) {
    return;
  }
  int offset = unitsAmount;
  for (int i = 0; i < extensionDeviceCount; i++) {
    ExtensionDevice &ext = extensionDevices[i];
    if (ext.Host == "" || !ext.Reachable) {
      continue; //fail-soft: skip, don't block/retry inline - see this function's own top comment
    }
    String segment = displayJob.currentAlignedLine.substring(offset, offset + ext.UnitsAmount);
    pushSegmentToExtension(ext, segment);
    offset += ext.UnitsAmount;
  }
}

//Extension-side receiver: writes a pushed segment directly to this device's own units, bypassing
//showText()/DisplayJob entirely (see multiple-units.md point 3 - an extension has no line-
//wrap/alignment logic of its own, the primary already did that against the combined width). Uses
//this device's OWN configured flapSpeed - the primary doesn't know or need to know an extension's
//speed setting, exactly like it doesn't need to know its alignment/animation preferences either.
void applyExtensionDisplaySegment(String segment) {
  int speed = convertSpeed(flapSpeed);
  int segmentLength = (int)segment.length();
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    char letterChar = unitIndex < segmentLength ? segment[unitIndex] : ' ';
    int letterPosition = translateLettertoInt(letterChar);
    if (letterPosition == -1) {
      letterPosition = translateLettertoInt('?');
    }
    writeToUnit(unitIndex, letterPosition, speed);
  }
}

//Persists a new extension host list (Primary role, System settings UI) - full replace, same
//"client always submits the complete list" convention as replaceRotationItemsFromJson(). Resets
//Reachable/UnitsAmount for every entry (a changed list means the next checkExtensionHealth() sweep
//should re-establish these from scratch, not carry over a previous entry's stale status onto
//whatever host now occupies that slot).
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
