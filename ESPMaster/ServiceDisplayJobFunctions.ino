//Non-blocking display-update state machine - showText() starts a job and returns immediately;
//updateDisplayJob() advances it by one step per loop() tick (that step may write or poll several
//units, and on a primary make short blocking HTTP calls to its extensions).

//millis()-rollover-safe comparison (unsigned difference cast to signed).
static inline bool millisReached(unsigned long target) {
  return (long)(millis() - target) >= 0;
}

enum DisplayJobState {
  DISPLAY_JOB_IDLE,
  DISPLAY_JOB_WAIT_READY_BEFORE,
  DISPLAY_JOB_WRITE_LINE,
  //Only reached when the animation mode/stagger leaves units not yet due after
  //DISPLAY_JOB_WRITE_LINE's tick; "simultaneous" or a 0ms stagger never get here.
  DISPLAY_JOB_WRITE_LINE_WAIT_STAGGER,
  DISPLAY_JOB_WAIT_READY_AFTER,
  DISPLAY_JOB_PAUSE_BETWEEN_LINES,
  DISPLAY_JOB_PAUSE_AFTER_FINISH,
  DISPLAY_JOB_DEBUG_PAUSE //only used when UNIT_CALLS_DISABLE == true
};

struct DisplayJob {
  DisplayJobState state = DISPLAY_JOB_IDLE;
  String originalMessage;      //what showText() was actually asked to show, for lastWrittenText
  //Lines aren't stored: DISPLAY_JOB_WRITE_LINE recomputes the current one with sentenceLine()
  //against lineWidth, so the line count can't drift if the combined width changes mid-job.
  static const int MAX_DISPLAY_LINES = 50; //500-char max message comfortably fits under this
  int lineCount = 0;
  int lineWidth = 0;
  bool isMultiLine = false;    //a single line shows originalMessage as-is
  int currentLineIndex = 0;
  int flapSpeedValue = 0;
  unsigned long finalPauseMs = 0;
  unsigned long pauseUntil = 0;
  String alignmentOverride = ""; //"" = use the global default alignment, see showText()
  //Alignment DISPLAY_JOB_WRITE_LINE actually used - recorded by finishDisplayJob(), since the global
  //alignment may change while the job runs.
  String lastUsedEffectiveAlignment = "";
  //displayRenderGeneration as of the last line sent - re-reading it at completion would credit a
  //bump that happened after the last write.
  unsigned long lastUsedRenderGeneration = 0;
  //Any unit write failed - finishDisplayJob() then forces a retry on the next showText().
  bool hadWriteFailure = false;

  //Staggered animation (formulas: computeUnitAnimationDelayMs()). *Override: what the caller asked
  //for (""/-1 = global setting), set in startDisplayJob(); only rotation passes other values.
  String animationModeOverride = "";
  int animationStaggerMsOverride = -1;
  //*Value: resolved per line in DISPLAY_JOB_WRITE_LINE, so a job without override follows the
  //global setting at render time.
  String animationModeValue = "simultaneous";
  unsigned long animationStaggerMsValue = 0;
  //The aligned line being written - a field because writing a line can span several ticks.
  String currentAlignedLine = "";
  //Start of the current line; unit delays count from here, so every line replays the animation.
  unsigned long lineWriteStartMillis = 0;
  //Bit i = unit i written for the current line (UNITS_AMOUNT_MAX is 64).
  uint64_t unitsWrittenMask = 0;

  //"Wait for all units ready": every busy unit is checked in one sweep per poll tick, so waits
  //overlap instead of adding up. 90s overall ceiling and 15s stuck timeout per unit.
  uint64_t waitUnitsReadyMask = 0; //bit i set once unit i is confirmed ready, sleeping, or given up on
  unsigned long waitPhaseStartedAt = 0; //anchors the shared 90s overall ceiling
  unsigned long waitNextPollAt = 0; //next tick allowed to do another sweep
  //Sweeps since resetDisplayJobWaitSubstate(); the heavy 6-byte read only every Nth sweep.
  unsigned long waitPollCount = 0;
  //Per-unit stuck detection, sized for 64 units and narrowed. Raw I2C bytes aren't trusted (an idle
  //bus reads 0xFF -> phase 255/progress 65535): pollDisplayJobWaitAllUnitsReady() checks them
  //against UNIT_PHASE_MAX/UNIT_PROGRESS_MAX before storing.
  int8_t waitLastPhase[UNITS_AMOUNT_MAX];    //-1 = no sample yet; else 0..UNIT_PHASE_MAX
  int16_t waitLastProgress[UNITS_AMOUNT_MAX]; //-1 = no sample yet; else 0..UNIT_PROGRESS_MAX
  unsigned long waitLastProgressAt[UNITS_AMOUNT_MAX];

  //Bit i = extension i ready (all units idle) or given up on, so a job doesn't finish while a
  //remote extension's motors still move (see pollDisplayJobWaitAllExtensionsReady()).
  uint8_t waitExtensionsReadyMask = 0; //EXTENSION_DEVICES_MAX (4) comfortably fits in one byte
  unsigned long waitExtensionsNextPollAt = 0;
};

DisplayJob displayJob;

bool isDisplayJobBusy() {
  return displayJob.state != DISPLAY_JOB_IDLE;
}

//A temporary message (/api/text with a duration, a scheduled message) is being shown or held -
//only those jobs carry a hold time. Lets Rotation tell its own refresh jobs from a message
//displacing it.
bool isTemporaryMessageJobBusy() {
  return isDisplayJobBusy() && displayJob.finalPauseMs > 0;
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

//Extension counterpart of pollDisplayJobWaitAllUnitsReady(): polls each reachable extension's
//GET /api/units once per second until all its units are idle. Returns true at once unless this is
//a primary with reachable extensions.
bool pollDisplayJobWaitAllExtensionsReady() {
  if (deviceRole != DEVICE_ROLE_PRIMARY || extensionDeviceCount == 0) {
    return true;
  }

  //Much shorter than the local 90s ceiling - stuck units are caught on the extension itself; this
  //only covers an extension that stops answering.
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
        //No units array, or still sending letters (displayBusy), isn't done. An offline unit over
        //there can't become ready, so it doesn't hold the wait; its failed write is reported via
        //displayWriteFailed.
        if (!deserializeJson(doc, http.getString()) && doc["units"].is<JsonArray>() && !(doc["displayBusy"] | false)) {
          bool allIdle = true;
          for (JsonVariant unit : doc["units"].as<JsonArray>()) {
            if ((int)(unit["status"] | 1) == 1) {
              allIdle = false;
              break;
            }
          }
          done = allIdle;
          //Only after this job's own push: before it, the flag still describes the previous push,
          //whose failure already caused this redraw.
          if (done && displayJob.state == DISPLAY_JOB_WAIT_READY_AFTER && (doc["displayWriteFailed"] | false)) {
            //Like a failed local write: finishDisplayJob() forces a re-send of this text.
            displayJob.hadWriteFailure = true;
          }
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

//Delay of unit `unitIndex` relative to the line start. `edgeDist` is the distance to the nearer
//edge (10 units: 0,1,2,3,4,4,3,2,1,0), which pairs symmetric units for outside-in/inside-out.
//simultaneous or staggerMs 0 means every unit at 0.
unsigned long computeUnitAnimationDelayMs(int unitIndex, int unitsAmount, const String &animationMode, unsigned long staggerMs) {
  if (staggerMs == 0 || unitsAmount <= 1) {
    return 0;
  }
  //A mirror with more units than its primary indexes past the width - clamp, or the unsigned delay
  //wraps to a huge value.
  unitIndex = constrain(unitIndex, 0, unitsAmount - 1);

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

//Whether a message needs more than MAX_DISPLAY_LINES lines - a permanent failure, so callers
//validate up front. Normalizes umlauts first like showText() (raw UTF-8 would count them twice).
bool isMessageTooLongToRender(const String &message) {
  return countSentenceLines(normalizeUmlautsForDisplay(message), combinedUnitsAmount()) > DisplayJob::MAX_DISPLAY_LINES;
}

//Starts a display job. Returns false if one is running - nothing is queued; callers that need
//ordering check isDisplayJobBusy() themselves.
bool startDisplayJob(const String &message, int flapSpeedValue, unsigned long finalPauseMs, const String &alignmentOverride, const String &animationModeOverride, int animationStaggerMsOverride) {
  if (displayJob.state != DISPLAY_JOB_IDLE) {
    return false;
  }

  String messageDisplay = message == "" ? "<Blank>" : message;
  SerialPrintln(F("Showing new Message"));
  SerialPrintln(F("New Message: ") + messageDisplay);
  SerialPrintln(F("Effective alignment: ") + (alignmentOverride != "" ? alignmentOverride : alignment));

  int lineWidth = combinedUnitsAmount();
  int lineCount = countSentenceLines(message, lineWidth);
  //With few units or many line breaks even an allowed message can exceed MAX_DISPLAY_LINES - reject
  //instead of silently cutting the tail.
  if (lineCount > DisplayJob::MAX_DISPLAY_LINES) {
    SerialPrintln(F("Message needs more lines than MAX_DISPLAY_LINES - refusing rather than silently truncating"));
    return false;
  }

  displayJob.originalMessage = message;
  displayJob.lineCount = lineCount;
  displayJob.lineWidth = lineWidth;
  displayJob.isMultiLine = displayJob.lineCount > 1;
  displayJob.currentLineIndex = 0;
  displayJob.flapSpeedValue = flapSpeedValue;
  displayJob.finalPauseMs = finalPauseMs;
  displayJob.alignmentOverride = alignmentOverride;
  //Only stored here; resolved per line in DISPLAY_JOB_WRITE_LINE (see animationModeOverride).
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

//Advances "wait for all units ready". Returns true once every unit is ready or given up on
//(forceUnitRecoveryReset()). Sweeps all busy units per poll tick, so the wait takes as long as the
//slowest unit, not the sum. The heavy 6-byte read only every Nth poll keeps the ATmega's I2C
//interrupt load low during calibration.
const int PHASE_READ_EVERY_N_POLLS = 3; //heavy 6-byte read roughly every 3s instead of every 1s

//Plausibility bounds for raw phase/progress bytes: PHASE_MAX is Unit.ino's highest phase
//(PHASE_ROTATING=2); PROGRESS_MAX is well above the real worst case but below the idle-bus 65535.
const int UNIT_PHASE_MAX = 2;
const int UNIT_PROGRESS_MAX = 20000;

bool pollDisplayJobWaitAllUnitsReady() {
  const unsigned long maxTotalWaitMs = 90000;
  const unsigned long stuckTimeoutMs = 15000;

  if (unitsAmount <= 0) {
    return true; //nothing to wait on
  }

  //unitsAmount <= 64; the ternary avoids the undefined "1ULL << 64".
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
      unitDone = true; //0 = ready - nothing to wait for
    } else if (ok) {
      if (wantPhaseRead) {
        //Implausible readings count as a failed read (not stored, timestamp untouched), so garbage
        //still trips the stuck timeout.
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

  //Same poll rate as before, now per sweep.
  displayJob.waitNextPollAt = millis() + 1000;

  return (displayJob.waitUnitsReadyMask & allUnitsMask) == allUnitsMask;
}

void finishDisplayJob() {
  lastWrittenText = displayJob.originalMessage;
  //Record the alignment the job actually used (the global one may have changed meanwhile). Only the
  //DEBUG_PAUSE path, which never runs WRITE_LINE, computes it here.
  lastWrittenAlignment = displayJob.lastUsedEffectiveAlignment != "" ? displayJob.lastUsedEffectiveAlignment :
    (displayJob.alignmentOverride != "" ? displayJob.alignmentOverride : alignment);
  lastRenderedGeneration = displayJob.lastUsedRenderGeneration;
  //A failed unit write means the display doesn't match lastWrittenText - bump the generation after
  //the snapshot so the next showText() with this text retries.
  if (displayJob.hadWriteFailure) {
    displayRenderGeneration++;
  }

  //Free this job's String buffers ("=" keeps capacity); lastWrittenText already has the message.
  displayJob.originalMessage = String();
  displayJob.currentAlignedLine = String();

  SerialPrintln(F("Done showing message"));
  displayJob.state = DISPLAY_JOB_IDLE;
}

//Writes every unit not yet written for this line whose animation delay has elapsed since
//lineWriteStartMillis - from DISPLAY_JOB_WRITE_LINE and later _WAIT_STAGGER ticks. Anywhere from 0
//to all units per call (all at once for simultaneous, or units sharing a delay).
void writeDueAnimatedUnits() {
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    uint64_t unitBit = 1ULL << unitIndex;
    if (displayJob.unitsWrittenMask & unitBit) {
      continue; //already written for this line
    }

    //Local units come first in the combined line, so their index is already the global position,
    //measured against the full combined width.
    unsigned long delayMs = computeUnitAnimationDelayMs(unitIndex, combinedUnitsAmount(), displayJob.animationModeValue, displayJob.animationStaggerMsValue);
    if (millis() - displayJob.lineWriteStartMillis < delayMs) {
      continue; //not due yet this tick
    }

    char currentLetter = displayJob.currentAlignedLine[unitIndex];
    int currentLetterPosition = translateLettertoInt(currentLetter);
    if (currentLetterPosition == -1) {
      //Unsupported character: write a defined replacement instead of leaving the old letter.
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
        //No inline retry (writeToUnit() already tried a bus recovery); finishDisplayJob() retries.
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

//Call once per loop() iteration; a cheap no-op while idle, one state step per call.
void updateDisplayJob() {
  //Quiet Hours started mid-job: abort to IDLE without finishDisplayJob() (only part was shown), and
  //bump displayRenderGeneration so the blank-out redraws even if an earlier "" looks current. The
  //blank-out job itself (originalMessage "") is exempt, or it would abort itself forever.
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
      //Both polled every tick, not short-circuited, so neither's ~1s cadence waits on the other.
      bool localReady = pollDisplayJobWaitAllUnitsReady();
      bool extensionsReady = pollDisplayJobWaitAllExtensionsReady();
      if (localReady && extensionsReady) {
        displayJob.state = DISPLAY_JOB_WRITE_LINE;
      }
      return;
    }

    case DISPLAY_JOB_WRITE_LINE: {
      //A single line shows originalMessage verbatim - leading/repeated spaces are a deliberate way to
      //position text - but only if it fits and has no line break. Otherwise the line sentenceLine()
      //counted (trimmed, spaces collapsed): verbatim, "HALLO\n" showed "HALLO?N" and a text that
      //only fits once its double space is collapsed lost its last character.
      const String &message = displayJob.originalMessage;
      bool showVerbatim = !displayJob.isMultiLine && message.indexOf('\n') == -1 &&
        message.indexOf("\\n") == -1 && (int)message.length() <= displayJob.lineWidth;
      int ignoredLineCount;
      String line = showVerbatim
        ? message
        : sentenceLine(message, displayJob.lineWidth, displayJob.currentLineIndex, ignoredLineCount);
      if (displayJob.isMultiLine) {
        SerialPrint(F("-- Message Line: "));
        SerialPrintln(line);
      }

      //"" (the common case) uses the global alignment; only rotation items override it.
      String effectiveAlignment = displayJob.alignmentOverride != "" ? displayJob.alignmentOverride : alignment;
      displayJob.lastUsedEffectiveAlignment = effectiveAlignment;
      displayJob.lastUsedRenderGeneration = displayRenderGeneration;

      //Own unitsAmount unless this is a primary with reachable extensions.
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

      //Staggered start (computeUnitAnimationDelayMs()). The override is resolved against the global
      //setting here, per line - see animationModeOverride.
      displayJob.animationModeValue = displayJob.animationModeOverride != "" ? displayJob.animationModeOverride : animationMode;
      displayJob.animationStaggerMsValue = displayJob.animationStaggerMsOverride >= 0 ? (unsigned long)displayJob.animationStaggerMsOverride : animationStaggerMs;
      displayJob.currentAlignedLine = aligned;
      displayJob.lineWriteStartMillis = millis();
      displayJob.unitsWrittenMask = 0;

      SerialPrintln(F("Unit calls are enabled. Will display message"));
      writeDueAnimatedUnits();
      //Once per new line; a no-op unless this is a primary with reachable extensions.
      pushExtensionSegments();

      if (allAnimatedUnitsWritten()) {
        //Common case (simultaneous or stagger 0): writeDueAnimatedUnits() already wrote every unit.
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
          //The 3s reading pause follows every line, the last one included.
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

//Non-blocking "blank1, pause, blank2" sequence for isPendingUnitsReset; call every loop() while
//it's true.
enum UnitsResetSequenceState { UNITS_RESET_SEQ_IDLE, UNITS_RESET_SEQ_BLANK1, UNITS_RESET_SEQ_PAUSE, UNITS_RESET_SEQ_BLANK2 };
UnitsResetSequenceState unitsResetSequenceState = UNITS_RESET_SEQ_IDLE;
unsigned long unitsResetSequencePauseUntil = 0;

void updateUnitsResetSequence() {
  switch (unitsResetSequenceState) {
    case UNITS_RESET_SEQ_IDLE:
      //Only once the display is free; otherwise retry quietly next tick.
      if (startDisplayJob(createRepeatingString('-'), convertSpeed(flapSpeed), 0, "", "", -1)) {
        SerialPrintln(F("Resetting units now..."));
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
