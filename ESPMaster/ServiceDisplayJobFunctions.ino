// Non-blocking display-update state machine (see BUGFIX_LOG.md #40/#81).
//
// Background: showText()/showMessage() used to block loop() synchronously for up to 90s twice
// per display update (waitForAllUnitsReady() before writing, and again after) - live-confirmed
// this makes the whole "deferred to loop()" request pattern (pendingUnitsInfoRequest etc.) starve,
// since that pattern is only ever checked at the very top of loop(), which a blocked showText()
// call never lets loop() return to. Static file serving stayed responsive throughout (confirmed
// live) because that's serviced directly by AsyncTCP/lwIP via yield(), which is a genuinely
// different mechanism from "loop() reaches its own top-level code again".
//
// A previous fix attempt called the deferred-request processing from additional yield-points
// nested inside showText()'s own wait loops - this DID keep the server responsive, but introduced
// new I2C reads that could nest inside an already-running I2C wait cycle, and was reverted after
// the user reported worse connection stability shortly after. That risk (nested/re-entrant Wire
// calls) is the reason this rewrite is a real state machine rather than "more yield points": at
// any moment, at most one I2C transfer is ever in flight, and it always happens from the very top
// of loop() (via updateDisplayJob(), called once per iteration, doing at most one I2C op per call)
// - never nested inside another call's wait loop.
//
// showText() (ServiceFlapFunctions.ino) keeps its exact old signature/dedup-guard and public
// behavior for every caller (main loop() text/date/clock display, scheduled messages, the units
// reset sequence) - it just starts a job and returns immediately instead of blocking until done.

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
  //BUG FIX, 2026-08-21: this was originally `LList<String> lines;`, assigned directly from
  //processSentenceToLines()'s return value in startDisplayJob() (`displayJob.lines =
  //processSentenceToLines(message);`). LIVE-CRASHED on the very first multi-line message tested
  //(Exception 28/LoadProhibited, garbled String content in the log - a fragment of an unrelated
  //earlier log line, the textbook signature of reading freed memory). Root cause: `LinkedList`
  //(LinkedList.h) has no custom copy constructor/operator= - its destructor unconditionally
  //`delete`s every node - so a plain member-wise copy (the compiler-generated default, taken by a
  //struct-member *assignment* from a temporary) leaves the assigned-to copy's root/last pointers
  //referring to nodes the temporary's destructor frees moments later at the end of that statement.
  //The original showText() never hit this because it used `LList<String> messageLines =
  //processSentenceToLines(message);` as a fresh *local* (copy elision applies to initialization
  //from a prvalue, not to assignment into an existing object) and read it entirely within the same
  //call - never letting a second, longer-lived copy of the LinkedList object itself exist. Fixed
  //by copying only the individual String elements (String's own copy semantics are fine) into a
  //plain fixed-size array, once, in startDisplayJob() - never storing/assigning the LList itself
  //across a struct/scope boundary again.
  static const int MAX_DISPLAY_LINES = 50; //500-char max message (#31) / a few chars per line
                                            //(unitsAmount) comfortably fits well under this
  String lines[MAX_DISPLAY_LINES];
  int lineCount = 0;
  bool isMultiLine = false;    //see getCurrentJobLineText() - matches the old code's exact
                                //behavior of only using the split lines when there's more than
                                //one; a single (or zero) line always shows originalMessage as-is,
                                //not whatever processSentenceToLines() normalized it to.
  int currentLineIndex = 0;
  int flapSpeedValue = 0;
  unsigned long finalPauseMs = 0;
  unsigned long pauseUntil = 0;

  //Sub-state for the current waitForAllUnitsReady()-equivalent phase - one unit at a time,
  //same 90s max-wait / 15s stuck-timeout semantics as the old waitForUnitReady(), just spread
  //non-blockingly across loop() ticks (see resetWaitSubstate()/pollWaitAllUnitsReadyStep() below)
  //instead of blocking in a while loop.
  int waitUnitIndex = 0;
  unsigned long waitUnitStart = 0;
  unsigned long waitLastProgressAt = 0;
  unsigned long waitNextPollAt = 0;
  int waitLastPhase = -1;
  int waitLastProgress = -1;
  //BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): counts polls since resetDisplayJobWaitSubstate(),
  //used to only fetch phase/progress (the heavy 6-byte read) every Nth poll instead of every poll -
  //see the comment on pollDisplayJobWaitAllUnitsReady() for why.
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
bool startDisplayJob(String message, int flapSpeedValue, unsigned long finalPauseMs) {
  if (displayJob.state != DISPLAY_JOB_IDLE) {
    return false;
  }

  String messageDisplay = message == "" ? "<Blank>" : message;
  SerialPrintln("Showing new Message");
  SerialPrintln("New Message: " + messageDisplay);
  SerialPrintln("Alignment Updated: " + String(alignmentUpdated ? "Yes" : "No"));

  displayJob.originalMessage = message;
  //parsedLines is a plain local here, used and discarded entirely within this function - safe.
  //See the long comment on DisplayJob::lines for why the LList itself must never be copied out
  //of a scope like this one, only its individual String elements.
  LList<String> parsedLines = processSentenceToLines(message);
  displayJob.lineCount = parsedLines.size();
  if (displayJob.lineCount > DisplayJob::MAX_DISPLAY_LINES) {
    SerialPrintln("WARNING: message split into more lines than expected, truncating");
    displayJob.lineCount = DisplayJob::MAX_DISPLAY_LINES;
  }
  for (int i = 0; i < displayJob.lineCount; i++) {
    displayJob.lines[i] = parsedLines[i];
  }
  displayJob.isMultiLine = displayJob.lineCount > 1;
  displayJob.currentLineIndex = 0;
  displayJob.flapSpeedValue = flapSpeedValue;
  displayJob.finalPauseMs = finalPauseMs;

  SerialPrintln(displayJob.isMultiLine ? "Showing a split down message" : ("Showing a simple message: " + message));

#if UNIT_CALLS_DISABLE == true
  SerialPrintln("Unit Calls are disabled for debugging. Will delay to simulate calls...");
  displayJob.pauseUntil = millis() + 2000;
  displayJob.state = DISPLAY_JOB_DEBUG_PAUSE;
#else
  resetDisplayJobWaitSubstate();
  displayJob.state = DISPLAY_JOB_WAIT_READY_BEFORE;
#endif
  return true;
}

//Advances the waitForAllUnitsReady()-equivalent sub-state by at most one I2C read (one unit at a
//time, one poll per ~1s), matching the old waitForUnitReady()'s per-unit stuck/timeout handling
//exactly. Returns true once every currently-known unit has been waited on (ready, or given up on
//via forceUnitRecoveryReset() - the old code didn't stop the whole sequence on one unit's failure
//either, see waitForAllUnitsReady()'s plain for-loop).
//BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #82): this used to call the heavy 6-byte readUnitInfo()
//(status+offset+phase+progress) on every ~1s poll while waiting. The ATmega328p's TWI hardware
//fires one interrupt PER BYTE transferred (not once per transaction), so a 6-byte read means 6 ISR
//firings landing on the Unit right in the middle of whatever it's doing - including calibrate()'s
//search loop (Unit.ino), which paces the stepper purely by micros() with zero feedback on whether
//the rotor actually kept up. Diffed Unit.ino against upstream (scientress/split-flap) on user
//request: requestEvent() grew from 1 byte (just currentlyrotating) to 6 bytes (+ calOffset lo/hi,
//+ currentPhase, + currentPhaseProgress lo/hi) in the same timeframe the diagnostic phase/progress
//fields were added - and the user independently confirmed the "unit spins very slowly" symptom
//first appeared around that same twiboot-related work, after "hundreds of hours" of prior
//trouble-free operation. A plausible, code-grounded mechanism (accumulated per-poll ISR jitter
//against a busy-wait timing loop with no position feedback) that doesn't require a power-supply
//theory. Mitigation: only fetch phase/progress every PHASE_READ_EVERY_N_POLLS-th poll (a light
//3-byte status+offset read the rest of the time) - still frequent enough for the 15s stuck-timeout
//to mean something, but cuts ISR firings during a wait by roughly 2/3.
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
      SerialPrintln("Unit " + String(displayJob.waitUnitIndex) + " busy - phase " + String(phase) +
        ", progress " + String(progress) + " (" + String((millis() - displayJob.waitUnitStart) / 1000) + "s waited)");
      if (phase != displayJob.waitLastPhase || progress != displayJob.waitLastProgress) {
        displayJob.waitLastPhase = phase;
        displayJob.waitLastProgress = progress;
        displayJob.waitLastProgressAt = millis();
      }
    } else {
      SerialPrintln("Unit " + String(displayJob.waitUnitIndex) + " busy (light check, " +
        String((millis() - displayJob.waitUnitStart) / 1000) + "s waited)");
    }
  } else {
    SerialPrintln("Unit " + String(displayJob.waitUnitIndex) + " - read failed while waiting, retrying");
  }

  if (!unitDone && (millis() - displayJob.waitLastProgressAt > stuckTimeoutMs)) {
    SerialPrintln("WARNING: Unit " + String(displayJob.waitUnitIndex) + " - no progress for " + String(stuckTimeoutMs / 1000) + "s, giving up");
    forceUnitRecoveryReset(displayJob.waitUnitIndex);
    unitDone = true;
  } else if (!unitDone && (millis() - displayJob.waitUnitStart >= maxTotalWaitMs)) {
    SerialPrintln("WARNING: Unit " + String(displayJob.waitUnitIndex) + " - hit max wait time (" + String(maxTotalWaitMs / 1000) + "s), giving up");
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
  SerialPrintln("Done showing message");
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
        SerialPrint("-- Message Line: ");
        SerialPrintln(line);
      }

      String aligned = line;
      if (alignment == ALIGNMENT_MODE_LEFT) {
        aligned = leftString(aligned);
      } else if (alignment == ALIGNMENT_MODE_RIGHT) {
        aligned = rightString(aligned);
      } else if (alignment == ALIGNMENT_MODE_CENTER) {
        aligned = centerString(aligned);
      }

      SerialPrint("Showing Aligned Message: \"");
      SerialPrint(aligned);
      SerialPrintln("\"");

      SerialPrintln("Unit calls are enabled. Will display message");
      for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
        char currentLetter = aligned[unitIndex];
        int currentLetterPosition = translateLettertoInt(currentLetter);

        SerialPrint("Unit Nr.: ");
        SerialPrint(unitIndex);
        SerialPrint(" Letter: ");
        SerialPrint(currentLetter);
        SerialPrint(" Letter position: ");
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

//Non-blocking replacement for the old "showText(blank1); delay(2000); showText(blank2);"
//sequence in ESPMaster.ino's loop() (isPendingUnitsReset handling) - that relied on showText()
//blocking until fully done to guarantee ordering, which no longer holds now that showText()
//returns immediately. Call every loop() iteration while isPendingUnitsReset is true.
enum UnitsResetSequenceState { UNITS_RESET_SEQ_IDLE, UNITS_RESET_SEQ_BLANK1, UNITS_RESET_SEQ_PAUSE, UNITS_RESET_SEQ_BLANK2 };
UnitsResetSequenceState unitsResetSequenceState = UNITS_RESET_SEQ_IDLE;
unsigned long unitsResetSequencePauseUntil = 0;

void updateUnitsResetSequence() {
  switch (unitsResetSequenceState) {
    case UNITS_RESET_SEQ_IDLE:
      //Only actually logs/transitions once the display is free - if busy, silently retries next
      //tick instead of spamming this line every loop() iteration.
      if (startDisplayJob(createRepeatingString('-'), convertSpeed(flapSpeed), 0)) {
        SerialPrintln("Reseting Units now...");
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
        if (startDisplayJob(createRepeatingString('.'), convertSpeed(flapSpeed), 0)) {
          unitsResetSequenceState = UNITS_RESET_SEQ_BLANK2;
        }
      }
      return;

    case UNITS_RESET_SEQ_BLANK2:
      if (!isDisplayJobBusy()) {
        isPendingUnitsReset = false;
        unitsResetSequenceState = UNITS_RESET_SEQ_IDLE;
        SerialPrintln("Done Units Reset!");
      }
      return;
  }
}
