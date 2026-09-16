//Non-blocking display-update state machine - showText() starts a job and returns immediately;
//updateDisplayJob() does at most one I2C transfer per loop() tick.

//millis() wraps back to 0 after ~49.7 days - a direct "millis() >= target" comparison breaks right
//at that wraparound. Casting the unsigned difference to signed gives the correct answer across
//the wraparound too - the standard Arduino millis()-rollover-safe comparison idiom.
static inline bool millisReached(unsigned long target) {
  return (long)(millis() - target) >= 0;
}

enum DisplayJobState {
  DISPLAY_JOB_IDLE,
  DISPLAY_JOB_WAIT_READY_BEFORE,
  DISPLAY_JOB_WRITE_LINE,
  //Only entered when the animation mode/stagger (see computeUnitAnimationDelayMs()) means not
  //every unit's delay was already due in DISPLAY_JOB_WRITE_LINE's own tick - "simultaneous" (the
  //default) or a 0ms stagger never reach this state at all, since every unit is due at delay 0
  //and gets written in that same tick, exactly like before this feature existed.
  DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER,
  DISPLAY_JOB_WAIT_READY_AFTER,
  DISPLAY_JOB_PAUSE_BETWEEN_LINES,
  DISPLAY_JOB_PAUSE_AFTER_FINISH,
  DISPLAY_JOB_DEBUG_PAUSE //only used when UNIT_CALLS_DISABLE == true
};

struct DisplayJob {
  DisplayJobState state = DISPLAY_JOB_IDLE;
  String originalMessage;      //what showText() was actually asked to show, for lastWrittenText
  //Must stay a plain array, not LList<String> - LinkedList's destructor deletes every node, so
  //assigning a temporary LList into a longer-lived struct leaves dangling pointers once the
  //temporary is destroyed. Copy just the String elements instead.
  static const int MAX_DISPLAY_LINES = 50; //500-char max message comfortably fits under this
  String lines[MAX_DISPLAY_LINES];
  int lineCount = 0;
  bool isMultiLine = false;    //split lines are only used when there's more than one; a single
                                //(or zero) line always shows originalMessage as-is (see DISPLAY_JOB_WRITE_LINE below)
  int currentLineIndex = 0;
  int flapSpeedValue = 0;
  unsigned long finalPauseMs = 0;
  unsigned long pauseUntil = 0;
  String alignmentOverride = ""; //"" = use the global default alignment, see showText()
  //Set by DISPLAY_JOB_WRITE_LINE to whatever effective alignment it actually just used -
  //finishDisplayJob() records THIS into lastWrittenAlignment, not a fresh re-computation, since
  //the global alignment could have changed again while this job was still running.
  String lastUsedEffectiveAlignment = "";
  //Set by DISPLAY_JOB_WRITE_LINE to whatever displayRenderGeneration actually WAS at the moment
  //that line was sent, not a fresh re-read of the (possibly-since-bumped) global at completion
  //time - re-reading it would silently credit a render for a generation bump that happened AFTER
  //the last line was written but BEFORE the job finished.
  unsigned long lastUsedRenderGeneration = 0;
  //Set true by DISPLAY_JOB_WRITE_LINE if any unit's I2C write failed - finishDisplayJob() uses
  //this to force a retry on the very next showText() call for the same text.
  bool hadWriteFailure = false;

  //Animation (staggered per-unit start) support - see computeUnitAnimationDelayMs()'s own comment
  //for the delay formulas.
  //
  //animationModeOverride/animationStaggerMsOverride: what the CALLER asked for (""/-1 = "no
  //override, use the global setting") - set once in startDisplayJob(). Only Rotation ever passes
  //a non-default value here.
  String animationModeOverride = "";
  int animationStaggerMsOverride = -1;
  //animationModeValue/animationStaggerMsValue: the RESOLVED values actually in effect for the
  //line currently being written - resolved fresh in DISPLAY_JOB_WRITE_LINE, not startDisplayJob(),
  //so a no-override job always uses whatever the global default is AT RENDER TIME.
  String animationModeValue = "simultaneous";
  unsigned long animationStaggerMsValue = 0;
  //The aligned line currently being written - was a local variable in DISPLAY_JOB_WRITE_LINE
  //before this feature; promoted to a field because writing a line can now span multiple ticks
  //(DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER needs it too).
  String currentAlignedLine = "";
  //Timestamp DISPLAY_JOB_WRITE_LINE started the CURRENT line - every unit's delay (see
  //computeUnitAnimationDelayMs()) is measured relative to this, not to job start, so every line of
  //a multi-line message replays the same animation.
  unsigned long lineWriteStartMillis = 0;
  //Bit i = unit i has already been written for the current line. uint64_t comfortably covers
  //UNITS_AMOUNT_MAX (64, ESPMaster.ino) with room to spare.
  uint64_t unitsWrittenMask = 0;

  //Sub-state for the "wait for all units ready" phase - sweeps every still-busy unit once per
  //poll tick, rather than fully resolving one unit's OWN busy status before looking at the next
  //(waiting them out one at a time ADDED their remaining times instead of overlapping them, since
  //a later unit's own homing keeps progressing in parallel the whole time this code watches an
  //earlier one). Same 90s max-wait/15s stuck-timeout safety nets as before, applied per-unit.
  uint64_t waitUnitsReadyMask = 0; //bit i set once unit i is confirmed ready, sleeping, or given up on
  unsigned long waitPhaseStartedAt = 0; //anchors the shared 90s overall ceiling
  unsigned long waitNextPollAt = 0; //next tick allowed to do another sweep
  //Counts sweeps since resetDisplayJobWaitSubstate() - used to only fetch phase/progress (the
  //heavy 6-byte read) every Nth sweep instead of every sweep, see PHASE_READ_EVERY_N_POLLS below.
  unsigned long waitPollCount = 0;
  //Per-unit stuck-detection state, sized to the structural max (64 units). Narrowed from plain
  //`int`/`unsigned long` since real values are tiny, but readUnitInfo()'s raw wire bytes are NOT
  //bounds-checked before landing here (an idle/pulled-up I2C bus reads back as 0xFF per byte,
  //decoding to phase=255/progress=65535) - pollDisplayJobWaitAllUnitsReady() validates both
  //against UNIT_PHASE_MAX/UNIT_PROGRESS_MAX before storing, or a persistent garbage reading could
  //never be faithfully stored in a uint16_t and would keep comparing as "changed" forever.
  int8_t waitLastPhase[UNITS_AMOUNT_MAX];    //-1 = no sample yet; else 0..UNIT_PHASE_MAX
  int16_t waitLastProgress[UNITS_AMOUNT_MAX]; //-1 = no sample yet; else 0..UNIT_PROGRESS_MAX
  unsigned long waitLastProgressAt[UNITS_AMOUNT_MAX]; //already the platform's native word size - no narrowing available

  //Extension counterpart to waitUnitsReadyMask - bit i set once configured extension i is confirmed
  //ready (all its units idle) or given up on. See pollDisplayJobWaitAllExtensionsReady()
  //(CODE_REVIEW_V6.md #5): without this, a job could finish (and the next line's write, or the
  //hold timer, could start) while a remote extension's motors are still physically moving.
  uint8_t waitExtensionsReadyMask = 0; //EXTENSION_DEVICES_MAX (4) comfortably fits in one byte
  unsigned long waitExtensionsNextPollAt = 0;
};

DisplayJob displayJob;

bool isDisplayJobBusy() {
  return displayJob.state != DISPLAY_JOB_IDLE;
}

void resetDisplayJobWaitSubstate() {
  unsigned long now = millis();
  displayJob.waitUnitsReadyMask = 0;
  displayJob.waitPhaseStartedAt = now;
  displayJob.waitNextPollAt = now;
  displayJob.waitPollCount = 0;
  for (int i = 0; i < UNITS_AMOUNT_MAX; i++) {
    displayJob.waitLastProgressAt[i] = now;
    displayJob.waitLastPhase[i] = -1;
    displayJob.waitLastProgress[i] = -1;
  }
  displayJob.waitExtensionsReadyMask = 0;
  displayJob.waitExtensionsNextPollAt = now;
}

//Extension counterpart to pollDisplayJobWaitAllUnitsReady() - polls each reachable, configured
//extension's own GET /api/units (same endpoint checkExtensionHealth() already uses) once per
//second until every unit it reports is idle, so this device's own hold timer/next line doesn't
//start before a remote extension's motors have actually settled (CODE_REVIEW_V6.md #5). A no-op
//returning true immediately for every role other than primary with at least one reachable
//extension.
bool pollDisplayJobWaitAllExtensionsReady() {
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionDeviceCount == 0) {
    return true;
  }

  //Deliberately much shorter than the local 90s ceiling - a genuinely stuck UNIT is already caught
  //by that extension's own pollDisplayJobWaitAllUnitsReady() over there; this ceiling only guards
  //against an extension that stops answering entirely (network drop, reboot mid-move).
  const unsigned long maxTotalWaitMs = 30000;

  uint8_t relevantMask = 0;
  for (int i = 0; i < extensionDeviceCount; i++) {
    if (extensionDevices[i].Host != "" && extensionDevices[i].Reachable) {
      relevantMask |= (1 << i);
    }
  }
  if (relevantMask == 0) {
    return true; //nothing configured/reachable to wait on
  }
  if ((displayJob.waitExtensionsReadyMask & relevantMask) == relevantMask) {
    return true; //every relevant extension already confirmed ready or given up on
  }
  if (!millisReached(displayJob.waitExtensionsNextPollAt)) {
    return false; //not due yet this tick
  }
  displayJob.waitExtensionsNextPollAt = millis() + 1000; //same cadence as the local unit poll

  for (int i = 0; i < extensionDeviceCount; i++) {
    uint8_t bit = (uint8_t)(1 << i);
    if (!(relevantMask & bit) || (displayJob.waitExtensionsReadyMask & bit)) {
      continue;
    }
    ExtensionDevice &ext = extensionDevices[i];
    bool done = false;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(EXTENSION_HTTP_TIMEOUT_MS);
    if (http.begin(client, "http://" + ext.Host + "/api/units")) {
      if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          bool allIdle = true;
          for (JsonVariant unit : doc["units"].as<JsonArray>()) {
            if ((int)(unit["status"] | 1) == 1) {
              allIdle = false;
              break;
            }
          }
          done = allIdle;
          SerialPrintln(F("Extension \"") + ext.Host + F("\" - ") + (allIdle ? F("ready") : F("still moving")));
        }
      }
      http.end();
    }

    if (!done && millisReached(displayJob.waitPhaseStartedAt + maxTotalWaitMs)) {
      SerialPrintln(F("WARNING: Extension \"") + ext.Host + F("\" - no ready confirmation within ") + String(maxTotalWaitMs / 1000) + F("s, giving up and continuing"));
      done = true;
    }

    if (done) {
      displayJob.waitExtensionsReadyMask |= bit;
    }
  }

  return (displayJob.waitExtensionsReadyMask & relevantMask) == relevantMask;
}

//How long unit `unitIndex` (of `unitsAmount` total) should wait, relative to the line's own start,
//before DISPLAY_JOB_WRITE_LINE(_WAIT_STAGGER) writes it. `edgeDist` is each unit's distance from
//whichever physical edge is nearer - for unitsAmount=10 that's 0,1,2,3,4,4,3,2,1,0, which already
//pairs up symmetric units for outside-in/inside-out with no special-casing needed.
//simultaneous/staggerMs==0 both collapse to "every unit due at 0" - no behavior change from before
//this feature existed.
unsigned long computeUnitAnimationDelayMs(int unitIndex, int unitsAmount, String animationMode, unsigned long staggerMs) {
  if (staggerMs == 0 || unitsAmount <= 1) {
    return 0;
  }

  int edgeDist = min(unitIndex, unitsAmount - 1 - unitIndex);

  if (animationMode == ANIMATION_MODE_LEFT_TO_RIGHT) {
    return (unsigned long)unitIndex * staggerMs;
  }
  if (animationMode == ANIMATION_MODE_RIGHT_TO_LEFT) {
    return (unsigned long)(unitsAmount - 1 - unitIndex) * staggerMs;
  }
  if (animationMode == ANIMATION_MODE_OUTSIDE_IN) {
    return (unsigned long)edgeDist * staggerMs;
  }
  if (animationMode == ANIMATION_MODE_INSIDE_OUT) {
    int maxEdgeDist = (unitsAmount - 1) / 2;
    return (unsigned long)(maxEdgeDist - edgeDist) * staggerMs;
  }

  return 0; //ANIMATION_MODE_SIMULTANEOUS, or an unrecognized value - fail safe to "all at once"
}

//Whether a message needs more lines than MAX_DISPLAY_LINES to display (the same check
//startDisplayJob() does internally) - exposed separately so callers can validate renderability UP
//FRONT, at acceptance time, since this is a PERMANENT failure that retrying later never fixes.
//
//Normalizes umlauts FIRST, same as showText() does before it ever reaches startDisplayJob() - a
//2-byte UTF-8 umlaut sequence collapses to a single 1-byte placeholder once normalized, so
//counting the raw bytes instead double-counts every umlaut for line-wrapping purposes.
bool isMessageTooLongToRender(String message) {
  return processSentenceToLines(normalizeUmlautsForDisplay(message), combinedUnitsAmount()).size() > DisplayJob::MAX_DISPLAY_LINES;
}

//Starts a new display job. Returns false (does nothing) if one is already running - callers that
//need strict ordering against other jobs (see updateUnitsResetSequence()) check isDisplayJobBusy()
//themselves rather than relying on this to queue anything.
bool startDisplayJob(String message, int flapSpeedValue, unsigned long finalPauseMs, String alignmentOverride, String animationModeOverride, int animationStaggerMsOverride) {
  if (displayJob.state != DISPLAY_JOB_IDLE) {
    return false;
  }

  String messageDisplay = message == "" ? "<Blank>" : message;
  SerialPrintln(F("Showing new Message"));
  SerialPrintln(F("New Message: ") + messageDisplay);
  SerialPrintln(F("Effective alignment: ") + (alignmentOverride != "" ? alignmentOverride : alignment));

  //parsedLines is a plain local here, used and discarded entirely within this function - safe.
  //See the long comment on DisplayJob::lines for why the LList itself must never be copied out
  //of a scope like this one, only its individual String elements.
  LList<String> parsedLines = processSentenceToLines(message, combinedUnitsAmount());
  //MAX_DISPLAY_LINES (50) was sized for "500 chars fits comfortably" assuming a fairly large
  //unitsAmount - with fewer units (or many explicit line breaks), a within-limits message can
  //genuinely need more lines than that. Reject clearly instead of silently truncating the tail
  //while still claiming the full text was shown.
  if (parsedLines.size() > DisplayJob::MAX_DISPLAY_LINES) {
    SerialPrintln(F("Message needs more lines than MAX_DISPLAY_LINES - refusing rather than silently truncating"));
    return false;
  }

  displayJob.originalMessage = message;
  displayJob.lineCount = parsedLines.size();
  for (int i = 0; i < displayJob.lineCount; i++) {
    displayJob.lines[i] = parsedLines[i];
  }
  displayJob.isMultiLine = displayJob.lineCount > 1;
  displayJob.currentLineIndex = 0;
  displayJob.flapSpeedValue = flapSpeedValue;
  displayJob.finalPauseMs = finalPauseMs;
  displayJob.alignmentOverride = alignmentOverride;
  //Just stored here, not resolved against the global settings yet - see animationModeOverride's
  //own comment for why that resolution happens later, in DISPLAY_JOB_WRITE_LINE.
  displayJob.animationModeOverride = animationModeOverride;
  displayJob.animationStaggerMsOverride = animationStaggerMsOverride;
  displayJob.lastUsedEffectiveAlignment = ""; //defensive reset
  displayJob.lastUsedRenderGeneration = displayRenderGeneration; //fallback for DEBUG_PAUSE
  displayJob.hadWriteFailure = false; //reset once per job, not per line

  SerialPrintln(displayJob.isMultiLine ? String(F("Showing a split down message")) : (F("Showing a simple message: ") + message));

#if UNIT_CALLS_DISABLE == true
  SerialPrintln(F("Unit Calls are disabled for debugging. Will delay to simulate calls..."));
  displayJob.pauseUntil = millis() + 2000;
  displayJob.state = DISPLAY_JOB_DEBUG_PAUSE;
#else
  resetDisplayJobWaitSubstate();
  displayJob.state = DISPLAY_JOB_WAIT_READY_BEFORE;
#endif
  return true;
}

//Advances the "wait for all units ready" sub-state. Returns true once every unit has been waited
//on (ready, or given up on via forceUnitRecoveryReset()).
//
//Checks every still-busy unit in one sweep per poll tick, instead of fully resolving one before
//looking at the next - sweeping the whole not-yet-ready set every tick converges in exactly
//max(each unit's own remaining time), the theoretical best, since a later unit's own homing keeps
//progressing in parallel on its own AVR the whole time this code watches an earlier one.
//
//Only fetches the heavy 6-byte read every Nth poll, light 3-byte otherwise - reduces I2C
//interrupt load on the ATmega328p during calibration.
const int PHASE_READ_EVERY_N_POLLS = 3; //heavy 6-byte read roughly every 3s instead of every 1s

//Plausibility bounds for the raw phase/progress bytes read over I2C, checked before they're
//narrowed into DisplayJob's uint8_t/int16_t arrays. PHASE_MAX matches Unit.ino's highest
//currentPhase constant (PHASE_ROTATING=2); PROGRESS_MAX is a generous ceiling well above the real
//worst case but comfortably below the known-bad 65535 "idle bus" reading.
const int UNIT_PHASE_MAX = 2;
const int UNIT_PROGRESS_MAX = 20000;

bool pollDisplayJobWaitAllUnitsReady() {
  const unsigned long maxTotalWaitMs = 90000;
  const unsigned long stuckTimeoutMs = 15000;

  if (unitsAmount <= 0) {
    return true; //nothing to wait on
  }

  //unitsAmount is capped at UNITS_AMOUNT_MAX (64, ESPMaster.ino) elsewhere, so this shift never
  //overflows - the ternary only guards the one edge case where unitsAmount itself equals 64,
  //where "1ULL << 64" would be undefined behavior.
  uint64_t allUnitsMask = (unitsAmount >= 64) ? ~0ULL : ((1ULL << unitsAmount) - 1);
  if ((displayJob.waitUnitsReadyMask & allUnitsMask) == allUnitsMask) {
    return true; //every unit already confirmed ready or given up on
  }

  if (!millisReached(displayJob.waitNextPollAt)) {
    return false; //not due yet this tick
  }

  bool wantPhaseRead = (displayJob.waitPollCount % PHASE_READ_EVERY_N_POLLS) == 0;
  displayJob.waitPollCount++;

  for (int address = 0; address < unitsAmount; address++) {
    if (displayJob.waitUnitsReadyMask & (1ULL << address)) {
      continue; //already confirmed ready or given up on
    }

    int status, offset;
    int phase = displayJob.waitLastPhase[address];
    int progress = displayJob.waitLastProgress[address];
    bool ok = wantPhaseRead
      ? readUnitInfo(address, status, offset, phase, progress)
      : readUnitInfo(address, status, offset);
    bool unitDone = false;

    if (ok && status != 1) {
      unitDone = true; //0 = ready, -1 = sleeping - either way, nothing to wait for
    } else if (ok) {
      if (wantPhaseRead) {
        //Reject implausible readings before they ever reach the narrowed waitLastPhase/
        //waitLastProgress arrays - treated the same as a failed read (not stored, timestamp
        //untouched), so a unit stuck returning garbage still trips the stuck-timeout.
        bool phaseReadingPlausible = phase >= 0 && phase <= UNIT_PHASE_MAX &&
          progress >= 0 && progress <= UNIT_PROGRESS_MAX;
        if (!phaseReadingPlausible) {
          SerialPrintln(F("Unit ") + String(address) + F(" - implausible phase/progress reading (") +
            String(phase) + F("/") + String(progress) + F("), ignoring this sample"));
        } else {
          SerialPrintln(F("Unit ") + String(address) + F(" busy - phase ") + String(phase) +
            F(", progress ") + String(progress) + F(" (") + String((millis() - displayJob.waitPhaseStartedAt) / 1000) + F("s total waited)"));
          if (phase != displayJob.waitLastPhase[address] || progress != displayJob.waitLastProgress[address]) {
            displayJob.waitLastPhase[address] = phase;
            displayJob.waitLastProgress[address] = progress;
            displayJob.waitLastProgressAt[address] = millis();
          }
        }
      } else {
        SerialPrintln(F("Unit ") + String(address) + F(" busy (light check, ") +
          String((millis() - displayJob.waitPhaseStartedAt) / 1000) + F("s total waited)"));
      }
    } else {
      SerialPrintln(F("Unit ") + String(address) + F(" - read failed while waiting, retrying"));
    }

    if (!unitDone && (millis() - displayJob.waitLastProgressAt[address] > stuckTimeoutMs)) {
      SerialPrintln(F("WARNING: Unit ") + String(address) + F(" - no progress for ") + String(stuckTimeoutMs / 1000) + F("s, giving up"));
      forceUnitRecoveryReset(address);
      unitDone = true;
    } else if (!unitDone && (millis() - displayJob.waitPhaseStartedAt >= maxTotalWaitMs)) {
      SerialPrintln(F("WARNING: Unit ") + String(address) + F(" - hit max wait time (") + String(maxTotalWaitMs / 1000) + F("s), giving up"));
      forceUnitRecoveryReset(address);
      unitDone = true;
    }

    if (unitDone) {
      displayJob.waitUnitsReadyMask |= (1ULL << address);
    }
  }

  //Same overall poll rate as before (was strictly per-unit; now applies to the whole sweep) -
  //preserves the original I2C-load-conscious pacing between sweeps.
  displayJob.waitNextPollAt = millis() + 1000;

  return (displayJob.waitUnitsReadyMask & allUnitsMask) == allUnitsMask;
}

void finishDisplayJob() {
  lastWrittenText = displayJob.originalMessage;
  //Record the alignment this job ACTUALLY used - lastUsedEffectiveAlignment, captured live in
  //DISPLAY_JOB_WRITE_LINE, not re-derived here from the current global alignment, which could have
  //changed again while this job was still running. Falls back to a fresh computation only for the
  //DISPLAY_JOB_DEBUG_PAUSE path, which never runs DISPLAY_JOB_WRITE_LINE at all.
  lastWrittenAlignment = displayJob.lastUsedEffectiveAlignment != "" ? displayJob.lastUsedEffectiveAlignment :
    (displayJob.alignmentOverride != "" ? displayJob.alignmentOverride : alignment);
  lastRenderedGeneration = displayJob.lastUsedRenderGeneration;
  //If any unit's write failed partway through this job, the physical display doesn't actually
  //match lastWrittenText even though everything above just claimed it does. Bump the generation
  //AFTER snapshotting lastRenderedGeneration so the very next showText() call for this same text
  //sees a mismatch and retries the whole thing.
  if (displayJob.hadWriteFailure) {
    displayRenderGeneration++;
  }

  //Release any String heap buffers this job held - a plain "=" assignment reuses existing
  //capacity rather than freeing it, so the NEXT job re-populating the same slots with SHORTER
  //text wouldn't free it on its own. Safe here: lastWrittenText already captured originalMessage.
  displayJob.originalMessage = String();
  displayJob.currentAlignedLine = String();
  for (int i = 0; i < DisplayJob::MAX_DISPLAY_LINES; i++) {
    displayJob.lines[i] = String();
  }

  SerialPrintln(F("Done showing message"));
  displayJob.state = DISPLAY_JOB_IDLE;
}

//Writes every not-yet-written unit (displayJob.unitsWrittenMask) whose animation delay (see
//computeUnitAnimationDelayMs()) has elapsed since displayJob.lineWriteStartMillis - called from
//both DISPLAY_JOB_WRITE_LINE (the first tick of a line) and DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER
//(every subsequent tick, if any units were still pending). Can write anywhere from 0 to all
//unitsAmount units in a single call - 0 while waiting between animation "waves", all of them at
//once for the simultaneous default or any unit(s) sharing the same delay (e.g. outside-in's
//symmetric pairs), matching what a single non-staggered DISPLAY_JOB_WRITE_LINE tick always did
//before this feature existed.
void writeDueAnimatedUnits() {
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    uint64_t unitBit = 1ULL << unitIndex;
    if (displayJob.unitsWrittenMask & unitBit) {
      continue; //already written for this line
    }

    //combinedUnitsAmount() instead of plain unitsAmount - a local unit's own index already equals
    //its GLOBAL position in the combined line (local units always come first, offset 0), so this
    //makes e.g. "outside-in" measure against the FULL combined display. A no-op for every role
    //other than primary with reachable extensions.
    unsigned long delayMs = computeUnitAnimationDelayMs(unitIndex, combinedUnitsAmount(), displayJob.animationModeValue, displayJob.animationStaggerMsValue);
    if (millis() - displayJob.lineWriteStartMillis < delayMs) {
      continue; //not due yet this tick
    }

    char currentLetter = displayJob.currentAlignedLine[unitIndex];
    int currentLetterPosition = translateLettertoInt(currentLetter);
    if (currentLetterPosition == -1) {
      //Unsupported character - fall back to a defined replacement instead of silently leaving
      //whatever this unit was PREVIOUSLY showing in place.
      currentLetterPosition = translateLettertoInt('?');
    }

    SerialPrint(F("Unit Nr.: "));
    SerialPrint(unitIndex);
    SerialPrint(F(" Letter: "));
    SerialPrint(currentLetter);
    SerialPrint(F(" Letter position: "));
    SerialPrintln(currentLetterPosition);

    if (currentLetterPosition != -1) {
      if (!writeToUnit(unitIndex, currentLetterPosition, displayJob.flapSpeedValue)) {
        //Don't retry inline - writeToUnit() already tried a bus recovery once. Remember it and
        //let finishDisplayJob() force a full retry on the next showText() call.
        displayJob.hadWriteFailure = true;
      }
    }

    displayJob.unitsWrittenMask |= unitBit;
  }
}

//Whether every currently-connected unit has been written for the line in progress.
bool allAnimatedUnitsWritten() {
  uint64_t fullUnitsMask = (unitsAmount >= 64) ? ~0ULL : ((1ULL << unitsAmount) - 1);
  return (displayJob.unitsWrittenMask & fullUnitsMask) == fullUnitsMask;
}

//Call exactly once per loop() iteration, unconditionally - a no-op (single switch + return) while
//idle. Does at most one I2C read/write per call, so a single call is always fast regardless of
//display job state.
void updateDisplayJob() {
  //Quiet Hours became active mid-job: abort cleanly to IDLE rather than letting an already-running
  //job keep writing its remaining lines. Deliberately does NOT call finishDisplayJob() - that
  //would mark originalMessage as "lastWrittenText" even though only part of it was shown.
  //Bumping displayRenderGeneration forces a real redraw regardless of what lastWrittenText
  //already says, since a coincidentally-matching earlier "" completion could otherwise make a
  //fresh blank-out request look like a no-op.
  //
  //originalMessage != "" is required here: without it, the blank-out job itself (started by
  //showText("")) would get caught by this exact same check and re-aborted before ever reaching
  //WRITE_LINE, forever, since Quiet Hours stays active the whole time by definition.
  if (displayJob.state != DISPLAY_JOB_IDLE && isQuietHoursActive() && displayJob.originalMessage != "") {
    SerialPrintln(F("Quiet Hours became active mid-job - aborting remaining lines"));
    displayJob.state = DISPLAY_JOB_IDLE;
    displayRenderGeneration++;
    return;
  }

  switch (displayJob.state) {
    case DISPLAY_JOB_IDLE:
      return;

    case DISPLAY_JOB_DEBUG_PAUSE:
      if (millisReached(displayJob.pauseUntil)) {
        finishDisplayJob();
      }
      return;

    case DISPLAY_JOB_WAIT_READY_BEFORE: {
      //Both polled unconditionally every tick (not short-circuited) so neither's own ~1s poll
      //cadence gets needlessly delayed by the other still being busy.
      bool localReady = pollDisplayJobWaitAllUnitsReady();
      bool extensionsReady = pollDisplayJobWaitAllExtensionsReady();
      if (localReady && extensionsReady) {
        displayJob.state = DISPLAY_JOB_WRITE_LINE;
      }
      return;
    }

    case DISPLAY_JOB_WRITE_LINE: {
      //Only pull from the split lines when there's more than one - a single line always uses
      //originalMessage verbatim, not processSentenceToLines()'s output. Leading/repeated spaces
      //are a deliberate way to manually position text on this device - collapsing/trimming them
      //for a single-line message would be a regression, not a fix; only the multi-line word-wrap
      //path needs its own whitespace normalization for line-breaking purposes.
      String line = displayJob.isMultiLine ? displayJob.lines[displayJob.currentLineIndex] : displayJob.originalMessage;
      if (displayJob.isMultiLine) {
        SerialPrint(F("-- Message Line: "));
        SerialPrintln(line);
      }

      //"" (no override, the common case) falls back to the global default alignment - see
      //showText()/startDisplayJob(). Only Rotation items ever set a non-empty override.
      String effectiveAlignment = displayJob.alignmentOverride != "" ? displayJob.alignmentOverride : alignment;
      displayJob.lastUsedEffectiveAlignment = effectiveAlignment; //see the field's own comment
      displayJob.lastUsedRenderGeneration = displayRenderGeneration; //see the field's own comment

      //combinedUnitsAmount() - own unitsAmount unless this is a primary device with reachable
      //extensions.
      int effectiveWidth = combinedUnitsAmount();
      String aligned = line;
      if (effectiveAlignment == ALIGNMENT_MODE_LEFT) {
        aligned = leftString(aligned, effectiveWidth);
      } else if (effectiveAlignment == ALIGNMENT_MODE_RIGHT) {
        aligned = rightString(aligned, effectiveWidth);
      } else if (effectiveAlignment == ALIGNMENT_MODE_CENTER) {
        aligned = centerString(aligned, effectiveWidth);
      }

      SerialPrint(F("Showing Aligned Message: \""));
      SerialPrint(aligned);
      SerialPrintln(F("\""));

      //Staggered per-unit start - see computeUnitAnimationDelayMs()'s own comment.
      //currentAlignedLine/lineWriteStartMillis/unitsWrittenMask are DisplayJob fields (not
      //locals) because writing a line can now span multiple loop() ticks.
      //
      //Resolve the per-line override vs. the global setting HERE, not in startDisplayJob() - see
      //animationModeOverride's own comment for why.
      displayJob.animationModeValue = displayJob.animationModeOverride != "" ? displayJob.animationModeOverride : animationMode;
      displayJob.animationStaggerMsValue = displayJob.animationStaggerMsOverride >= 0 ? (unsigned long)displayJob.animationStaggerMsOverride : animationStaggerMs;
      displayJob.currentAlignedLine = aligned;
      displayJob.lineWriteStartMillis = millis();
      displayJob.unitsWrittenMask = 0;

      SerialPrintln(F("Unit calls are enabled. Will display message"));
      writeDueAnimatedUnits();
      //Once per new line, not from the WAIT_STAGGER re-poll below - a no-op unless this is a
      //primary device with reachable extensions configured.
      pushExtensionSegments();

      if (allAnimatedUnitsWritten()) {
        //The common case (animationModeValue == simultaneous, or staggerMs == 0): every unit's
        //delay was 0 and writeDueAnimatedUnits() just wrote all of them above, in this exact same
        //tick - identical to this function's behavior before this feature existed, no new state
        //ever visited, no behavior change for the existing default.
        resetDisplayJobWaitSubstate();
        displayJob.state = DISPLAY_JOB_WAIT_READY_AFTER;
      } else {
        displayJob.state = DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER;
      }
      return;
    }

    case DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER:
      writeDueAnimatedUnits();
      if (allAnimatedUnitsWritten()) {
        resetDisplayJobWaitSubstate();
        displayJob.state = DISPLAY_JOB_WAIT_READY_AFTER;
      }
      return;

    case DISPLAY_JOB_WAIT_READY_AFTER: {
      bool localReady = pollDisplayJobWaitAllUnitsReady();
      bool extensionsReady = pollDisplayJobWaitAllExtensionsReady();
      if (localReady && extensionsReady) {
        if (displayJob.isMultiLine) {
          //Matches the old code's loop condition exactly (`linesIndex <= messageLines.size()`,
          //always true given linesIndex's range) - the 3s "give time to read" pause fires after
          //every line, including the last one, before the job is considered finished.
          displayJob.pauseUntil = millis() + 3000;
          displayJob.state = DISPLAY_JOB_PAUSE_BETWEEN_LINES;
        } else if (displayJob.finalPauseMs != 0) {
          displayJob.pauseUntil = millis() + displayJob.finalPauseMs;
          displayJob.state = DISPLAY_JOB_PAUSE_AFTER_FINISH;
        } else {
          finishDisplayJob();
        }
      }
      return;
    }

    case DISPLAY_JOB_PAUSE_BETWEEN_LINES:
      if (millisReached(displayJob.pauseUntil)) {
        displayJob.currentLineIndex++;
        if (displayJob.currentLineIndex < displayJob.lineCount) {
          resetDisplayJobWaitSubstate();
          displayJob.state = DISPLAY_JOB_WAIT_READY_BEFORE;
        } else if (displayJob.finalPauseMs != 0) {
          displayJob.pauseUntil = millis() + displayJob.finalPauseMs;
          displayJob.state = DISPLAY_JOB_PAUSE_AFTER_FINISH;
        } else {
          finishDisplayJob();
        }
      }
      return;

    case DISPLAY_JOB_PAUSE_AFTER_FINISH:
      if (millisReached(displayJob.pauseUntil)) {
        finishDisplayJob();
      }
      return;
  }
}

//Non-blocking "show blank1, pause, show blank2" sequence for isPendingUnitsReset (ESPMaster.ino's
//loop()) - needed since showText() no longer blocks until done. Call every loop() iteration while
//isPendingUnitsReset is true.
enum UnitsResetSequenceState { UNITS_RESET_SEQ_IDLE, UNITS_RESET_SEQ_BLANK1, UNITS_RESET_SEQ_PAUSE, UNITS_RESET_SEQ_BLANK2 };
UnitsResetSequenceState unitsResetSequenceState = UNITS_RESET_SEQ_IDLE;
unsigned long unitsResetSequencePauseUntil = 0;

void updateUnitsResetSequence() {
  switch (unitsResetSequenceState) {
    case UNITS_RESET_SEQ_IDLE:
      //Only actually logs/transitions once the display is free - if busy, silently retries next
      //tick instead of spamming this line every loop() iteration.
      if (startDisplayJob(createRepeatingString('-'), convertSpeed(flapSpeed), 0, "", "", -1)) {
        SerialPrintln(F("Reseting Units now..."));
        unitsResetSequenceState = UNITS_RESET_SEQ_BLANK1;
      }
      return;

    case UNITS_RESET_SEQ_BLANK1:
      if (!isDisplayJobBusy()) {
        unitsResetSequencePauseUntil = millis() + 2000;
        unitsResetSequenceState = UNITS_RESET_SEQ_PAUSE;
      }
      return;

    case UNITS_RESET_SEQ_PAUSE:
      if (millisReached(unitsResetSequencePauseUntil)) {
        if (startDisplayJob(createRepeatingString('.'), convertSpeed(flapSpeed), 0, "", "", -1)) {
          unitsResetSequenceState = UNITS_RESET_SEQ_BLANK2;
        }
      }
      return;

    case UNITS_RESET_SEQ_BLANK2:
      if (!isDisplayJobBusy()) {
        isPendingUnitsReset = false;
        unitsResetSequenceState = UNITS_RESET_SEQ_IDLE;
        SerialPrintln(F("Done Units Reset!"));
      }
      return;
  }
}
