//Non-blocking display-update state machine - showText() starts a job and returns immediately;
//updateDisplayJob() does at most one I2C transfer per loop() tick.

enum DisplayJobState {
  DISPLAY_JOB_IDLE,
  DISPLAY_JOB_WAIT_READY_BEFORE,
  DISPLAY_JOB_WRITE_LINE,
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

  //Sub-state for the "wait for all units ready" phase - one unit at a time, 90s max-wait / 15s
  //stuck-timeout, spread non-blockingly across loop() ticks (see pollDisplayJobWaitAllUnitsReady() below).
  int waitUnitIndex = 0;
  unsigned long waitUnitStart = 0;
  unsigned long waitLastProgressAt = 0;
  unsigned long waitNextPollAt = 0;
  int waitLastPhase = -1;
  int waitLastProgress = -1;
  //Counts polls since resetDisplayJobWaitSubstate() - used to only fetch phase/progress (the
  //heavy 6-byte read) every Nth poll instead of every poll, see PHASE_READ_EVERY_N_POLLS below.
  unsigned long waitPollCount = 0;
};

DisplayJob displayJob;

bool isDisplayJobBusy() {
  return displayJob.state != DISPLAY_JOB_IDLE;
}

void resetDisplayJobWaitSubstate() {
  displayJob.waitUnitIndex = 0;
  displayJob.waitUnitStart = millis();
  displayJob.waitLastProgressAt = millis();
  displayJob.waitNextPollAt = millis();
  displayJob.waitLastPhase = -1;
  displayJob.waitLastProgress = -1;
  displayJob.waitPollCount = 0;
}

//Starts a new display job. Returns false (does nothing) if one is already running - callers that
//need strict ordering against other jobs (see updateUnitsResetSequence()) check isDisplayJobBusy()
//themselves rather than relying on this to queue anything.
bool startDisplayJob(String message, int flapSpeedValue, unsigned long finalPauseMs, String alignmentOverride) {
  if (displayJob.state != DISPLAY_JOB_IDLE) {
    return false;
  }

  String messageDisplay = message == "" ? "<Blank>" : message;
  SerialPrintln(F("Showing new Message"));
  SerialPrintln(F("New Message: ") + messageDisplay);
  SerialPrintln(F("Alignment Updated: ") + String(alignmentUpdated ? F("Yes") : F("No")));

  displayJob.originalMessage = message;
  //parsedLines is a plain local here, used and discarded entirely within this function - safe.
  //See the long comment on DisplayJob::lines for why the LList itself must never be copied out
  //of a scope like this one, only its individual String elements.
  LList<String> parsedLines = processSentenceToLines(message);
  displayJob.lineCount = parsedLines.size();
  if (displayJob.lineCount > DisplayJob::MAX_DISPLAY_LINES) {
    SerialPrintln(F("WARNING: message split into more lines than expected, truncating"));
    displayJob.lineCount = DisplayJob::MAX_DISPLAY_LINES;
  }
  for (int i = 0; i < displayJob.lineCount; i++) {
    displayJob.lines[i] = parsedLines[i];
  }
  displayJob.isMultiLine = displayJob.lineCount > 1;
  displayJob.currentLineIndex = 0;
  displayJob.flapSpeedValue = flapSpeedValue;
  displayJob.finalPauseMs = finalPauseMs;
  displayJob.alignmentOverride = alignmentOverride;

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

//Advances the "wait for all units ready" sub-state by at most one I2C read per call. Returns true
//once every unit has been waited on (ready, or given up on via forceUnitRecoveryReset()).
//
//Only fetches the heavy 6-byte read every Nth poll, light 3-byte otherwise - reduces I2C interrupt
//load on the ATmega328p during calibration.
const int PHASE_READ_EVERY_N_POLLS = 3; //heavy 6-byte read roughly every 3s instead of every 1s

bool pollDisplayJobWaitAllUnitsReady() {
  const unsigned long maxTotalWaitMs = 90000;
  const unsigned long stuckTimeoutMs = 15000;

  if (displayJob.waitUnitIndex >= unitsAmount) {
    return true; //nothing to wait on, e.g. unitsAmount == 0
  }

  if (millis() < displayJob.waitNextPollAt) {
    return false; //not due yet this tick
  }

  bool wantPhaseRead = (displayJob.waitPollCount % PHASE_READ_EVERY_N_POLLS) == 0;
  displayJob.waitPollCount++;

  int status, offset;
  int phase = displayJob.waitLastPhase;
  int progress = displayJob.waitLastProgress;
  bool ok = wantPhaseRead
    ? readUnitInfo(displayJob.waitUnitIndex, status, offset, phase, progress)
    : readUnitInfo(displayJob.waitUnitIndex, status, offset);
  bool unitDone = false;

  if (ok && status != 1) {
    unitDone = true; //0 = ready, -1 = sleeping - either way, nothing to wait for
  } else if (ok) {
    if (wantPhaseRead) {
      SerialPrintln(F("Unit ") + String(displayJob.waitUnitIndex) + F(" busy - phase ") + String(phase) +
        F(", progress ") + String(progress) + F(" (") + String((millis() - displayJob.waitUnitStart) / 1000) + F("s waited)"));
      if (phase != displayJob.waitLastPhase || progress != displayJob.waitLastProgress) {
        displayJob.waitLastPhase = phase;
        displayJob.waitLastProgress = progress;
        displayJob.waitLastProgressAt = millis();
      }
    } else {
      SerialPrintln(F("Unit ") + String(displayJob.waitUnitIndex) + F(" busy (light check, ") +
        String((millis() - displayJob.waitUnitStart) / 1000) + F("s waited)"));
    }
  } else {
    SerialPrintln(F("Unit ") + String(displayJob.waitUnitIndex) + F(" - read failed while waiting, retrying"));
  }

  if (!unitDone && (millis() - displayJob.waitLastProgressAt > stuckTimeoutMs)) {
    SerialPrintln(F("WARNING: Unit ") + String(displayJob.waitUnitIndex) + F(" - no progress for ") + String(stuckTimeoutMs / 1000) + F("s, giving up"));
    forceUnitRecoveryReset(displayJob.waitUnitIndex);
    unitDone = true;
  } else if (!unitDone && (millis() - displayJob.waitUnitStart >= maxTotalWaitMs)) {
    SerialPrintln(F("WARNING: Unit ") + String(displayJob.waitUnitIndex) + F(" - hit max wait time (") + String(maxTotalWaitMs / 1000) + F("s), giving up"));
    forceUnitRecoveryReset(displayJob.waitUnitIndex);
    unitDone = true;
  }

  if (unitDone) {
    displayJob.waitUnitIndex++;
    displayJob.waitUnitStart = millis();
    displayJob.waitLastProgressAt = millis();
    displayJob.waitNextPollAt = millis(); //move to the next unit right away, don't wait another 1s
    displayJob.waitLastPhase = -1;
    displayJob.waitLastProgress = -1;
    displayJob.waitPollCount = 0;
  } else {
    displayJob.waitNextPollAt = millis() + 1000; //match the old 1s poll interval
  }

  return displayJob.waitUnitIndex >= unitsAmount;
}

void finishDisplayJob() {
  lastWrittenText = displayJob.originalMessage;
  alignmentUpdated = false;
  SerialPrintln(F("Done showing message"));
  displayJob.state = DISPLAY_JOB_IDLE;
}

//Call exactly once per loop() iteration, unconditionally - a no-op (single switch + return) while
//idle. Does at most one I2C read/write per call, so a single call is always fast regardless of
//display job state.
void updateDisplayJob() {
  switch (displayJob.state) {
    case DISPLAY_JOB_IDLE:
      return;

    case DISPLAY_JOB_DEBUG_PAUSE:
      if (millis() >= displayJob.pauseUntil) {
        finishDisplayJob();
      }
      return;

    case DISPLAY_JOB_WAIT_READY_BEFORE:
      if (pollDisplayJobWaitAllUnitsReady()) {
        displayJob.state = DISPLAY_JOB_WRITE_LINE;
      }
      return;

    case DISPLAY_JOB_WRITE_LINE: {
      //Matches the old code exactly: only pull from the split lines when there's more than one -
      //a single line always uses originalMessage verbatim, not processSentenceToLines()'s output.
      String line = displayJob.isMultiLine ? displayJob.lines[displayJob.currentLineIndex] : displayJob.originalMessage;
      if (displayJob.isMultiLine) {
        SerialPrint(F("-- Message Line: "));
        SerialPrintln(line);
      }

      //"" (no override, the common case) falls back to the global default alignment - see
      //showText()/startDisplayJob(). Only Rotation items ever set a non-empty override.
      String effectiveAlignment = displayJob.alignmentOverride != "" ? displayJob.alignmentOverride : alignment;

      String aligned = line;
      if (effectiveAlignment == ALIGNMENT_MODE_LEFT) {
        aligned = leftString(aligned);
      } else if (effectiveAlignment == ALIGNMENT_MODE_RIGHT) {
        aligned = rightString(aligned);
      } else if (effectiveAlignment == ALIGNMENT_MODE_CENTER) {
        aligned = centerString(aligned);
      }

      SerialPrint(F("Showing Aligned Message: \""));
      SerialPrint(aligned);
      SerialPrintln(F("\""));

      SerialPrintln(F("Unit calls are enabled. Will display message"));
      for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
        char currentLetter = aligned[unitIndex];
        int currentLetterPosition = translateLettertoInt(currentLetter);

        SerialPrint(F("Unit Nr.: "));
        SerialPrint(unitIndex);
        SerialPrint(F(" Letter: "));
        SerialPrint(currentLetter);
        SerialPrint(F(" Letter position: "));
        SerialPrintln(currentLetterPosition);

        if (currentLetterPosition != -1) {
          writeToUnit(unitIndex, currentLetterPosition, displayJob.flapSpeedValue);
        }
      }

      resetDisplayJobWaitSubstate();
      displayJob.state = DISPLAY_JOB_WAIT_READY_AFTER;
      return;
    }

    case DISPLAY_JOB_WAIT_READY_AFTER:
      if (pollDisplayJobWaitAllUnitsReady()) {
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

    case DISPLAY_JOB_PAUSE_BETWEEN_LINES:
      if (millis() >= displayJob.pauseUntil) {
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
      if (millis() >= displayJob.pauseUntil) {
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
      if (startDisplayJob(createRepeatingString('-'), convertSpeed(flapSpeed), 0, "")) {
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
      if (millis() >= unitsResetSequencePauseUntil) {
        if (startDisplayJob(createRepeatingString('.'), convertSpeed(flapSpeed), 0, "")) {
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
