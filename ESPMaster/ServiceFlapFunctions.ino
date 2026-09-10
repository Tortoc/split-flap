//Shows a new message on the display. Non-blocking: starts a job (ServiceDisplayJobFunctions.ino)
//and returns immediately - updateDisplayJob() does the actual work incrementally per loop() tick.
//alignmentOverride: "" (default) uses the global default alignment; a non-empty value
//("left"/"center"/"right") overrides it just for this message - used by Rotation items that set
//their own alignment (see checkRotation()).
//
//Returns whether the message was actually accepted (already showing, or a job was successfully
//started) vs. genuinely dropped (no units, or the display was busy and couldn't start a job) -
//added 2026-09-09, CODE_REVIEW.md #10: callers that consume a one-shot input (scheduled messages
//dequeuing an entry, /api/text) were previously discarding this entirely and treating every call
//as successful, silently losing the message whenever the display happened to be busy.
bool showText(String message) {
  return showText(message, 0, "");
}

bool showText(String message, int delayMillis) {
  return showText(message, delayMillis, "");
}

bool showText(String message, int delayMillis, String alignmentOverride) {
  //Nothing to display to - see scanI2CBus(). Guarded here (rather than at every call site)
  //so scheduled messages, countdown, clock/date modes etc. all stay safe automatically. A real
  //failure to deliver, not a no-op - see the comment above this function.
  if (unitsAmount <= 0) {
    return false;
  }

  //Centralized here, not at each individual input path - see normalizeUmlautsForDisplay()'s own
  //comment (CODE_REVIEW.md #21) for why that matters. Must run before anything below measures
  //length/position, including the lastWrittenText cache comparison.
  message = normalizeUmlautsForDisplay(message);

  //Quiet Hours override whatever was actually requested - checked here, not at each call site, so
  //every path (mode dispatch, Rotation, scheduled messages, the /api/text push endpoint, ...)
  //parks the display uniformly without needing its own check. Deliberate, not a failure - the
  //caller asked to show something and quiet hours correctly overrode it.
  if (isQuietHoursActive()) {
    message = "";
  }

  //"" (no override, the common case) falls back to the global default alignment - same fallback
  //DISPLAY_JOB_WRITE_LINE itself uses once the job actually runs.
  String effectiveAlignment = alignmentOverride != "" ? alignmentOverride : alignment;

  //lastWrittenText/lastWrittenAlignment/lastRenderedGeneration only reflect the LAST COMPLETED
  //job (set by finishDisplayJob()) - while a job is still running, they describe stale history,
  //not what's actually on the display right now (found 2026-09-09, CODE_REVIEW_V2.md #9). A cache
  //hit against them here could therefore report success for a request that has nothing to do with
  //what's actually mid-flight - e.g. text A finished long ago (cache says A), B is now running;
  //a fresh request for A would see the stale "A" cache entry and return true without A ever being
  //shown again. While busy, only a genuine match against the job that's ACTUALLY running counts as
  //already satisfied - anything else must go through startDisplayJob() like normal, which already
  //correctly refuses (returns false) while busy, so callers retry once the display is free.
  if (isDisplayJobBusy()) {
    //Compare against the running job's own request, not resolved/global state that could have
    //moved on independently (alignmentOverride raw, not effectiveAlignment - matches exactly what
    //startDisplayJob() below would be called with). Also requires the hold time to match -
    //see the delayMillis reasoning below, same principle applies while busy: a caller explicitly
    //asking for a hold must not be silently folded into a coincidentally-matching running job that
    //was never asked to hold that long.
    bool matchesRunningJob = displayJob.originalMessage == message &&
      displayJob.alignmentOverride == alignmentOverride &&
      displayJob.finalPauseMs == (unsigned long)delayMillis;
    if (matchesRunningJob) {
      //A repeated identical request while the job is already sitting in its final hold phase
      //refreshes that hold for a full fresh duration, rather than silently succeeding against
      //whatever time happens to already be left on the original countdown - found 2026-09-10,
      //CODE_REVIEW_V3.md #8 (live-reproduced): a 5s hold started at t=5000 (due to end at
      //t=10000); an identical repeat request at t=9500 returned true here, but the message still
      //disappeared at the ORIGINAL t=10000 - only 500ms later, not a fresh 5s from the repeat.
      //Only applies once the job has actually settled into the hold (DISPLAY_JOB_PAUSE_AFTER_FINISH)
      //- while it's still WAIT_READY_BEFORE/WRITE_LINE/WAIT_READY_AFTER (genuinely still rendering
      //toward this same request for the first time), there's no hold running yet to refresh, and
      //restarting the countdown here would be premature.
      if (delayMillis > 0 && displayJob.state == DISPLAY_JOB_PAUSE_AFTER_FINISH) {
        displayJob.pauseUntil = millis() + (unsigned long)delayMillis;
      }
      return true; //already in progress toward exactly this request
    }
    return startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis, alignmentOverride); //correctly returns false: busy, try again next tick
  }

  //Redraw if the text or effective alignment actually differs from what's really on the display,
  //OR if displayRenderGeneration has moved on since the last completed render even though neither
  //of those changed - see the comment on displayRenderGeneration (found 2026-09-09,
  //CODE_REVIEW.md #14: the old check only ever compared message text plus a single global
  //alignment-changed flag, missing per-item Rotation alignment changes and any hardware-side
  //desync entirely). Also redraw whenever an explicit hold time is freshly requested (delayMillis
  //!= 0), even if the text/alignment/generation are all otherwise already correct - found
  //2026-09-09, CODE_REVIEW_V2.md #8: without this, a caller asking to hold already-visible text
  //for N ms (e.g. a temporary /api/text push repeating the current text) was silently ignored,
  //since nothing about the cache comparison itself ever changes just because a hold was requested.
  if (lastWrittenText != message || lastWrittenAlignment != effectiveAlignment || lastRenderedGeneration != displayRenderGeneration || delayMillis != 0) {
    //If a job is still running, this attempt is silently skipped - lastWrittenText hasn't updated
    //yet, so the next ~1s loop() tick naturally retries once the display is free. Propagate
    //startDisplayJob()'s own result rather than assuming success. (isDisplayJobBusy() was already
    //ruled out above, so this always actually starts here - kept as startDisplayJob()'s own return
    //value regardless, rather than assuming true, in case that ever changes.)
    return startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis, alignmentOverride);
  }
  return true; //already showing this - not a failure, nothing to do
}

//Translates char to letter position
int translateLettertoInt(char letterchar) {
  for (int flapIndex = 0; flapIndex < FLAP_AMOUNT; flapIndex++) {
    if (letterchar == letters[flapIndex]) {
      return flapIndex;
    }
  }

  return -1;
}

//Every write to a Unit ends with one extra XOR-checksum byte. Unit.ino's receiveLetter() verifies
//it and ignores the message on a mismatch - defends against I2C noise being misread as a command
//(~1-in-256 chance of a stray sequence passing, vs. ~1-in-6 of just landing in the letter range).
uint8_t i2cChecksum(uint8_t* data, int len) {
  uint8_t checksum = 0;
  for (int i = 0; i < len; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

//Write letter position and speed in rpm to single unit. Returns true if the I2C write actually
//succeeded (Wire.endTransmission() == 0) - previously discarded entirely (found 2026-09-09,
//CODE_REVIEW.md #15): a NACK'd write left that one unit's flap untouched, but the caller
//(DISPLAY_JOB_WRITE_LINE) had no way to know and went on to mark the whole line "shown" once every
//unit merely reported "not moving" - true for a unit that never received a new letter in the first
//place, not just one that finished moving to it.
bool writeToUnit(int address, int letter, int flapSpeed) {
  uint8_t sendArray[2] = {(uint8_t)letter, (uint8_t)flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(address);

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint(F("sendArray: "));
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  uint8_t result = Wire.endTransmission(); //send values to unit
  if (result != 0) {
    SerialPrintln(F("ERROR: Unit ") + String(address) + F(" - write failed (Wire.endTransmission()=") + String(result) + F(")"));
    recoverI2CBus(); //same "try recovery right at the point of failure" pattern as readUnitInfo()
  }
  return result == 0;
}

//Write a new calibration offset to a single unit. The unit recalibrates immediately upon
//receiving this and ends up showing letter 0 (blank), so that letter is sent along with it
//to avoid triggering a second, separate rotation. Returns true on a successful I2C write - see
//writeToUnit()'s comment (CODE_REVIEW.md #15); the caller (/api/units?action=offset) already
//tells the admin recalibration is in progress rather than instantaneous, so on failure it just
//logs here rather than retrying - a genuinely stuck offset is already visible via the unit's
//own reported offset not matching what was requested.
bool writeOffsetToUnit(int address, uint16_t offset) {
  int currentSpeed = convertSpeed(flapSpeed);

  SerialPrint(F("Writing offset "));
  SerialPrint(offset);
  SerialPrint(F(" to unit "));
  SerialPrintln(address);

  //letter 0 = blank, matches the position the unit ends up at after recalibrating
  uint8_t sendArray[4] = {0, (uint8_t)currentSpeed, lowByte(offset), highByte(offset)};

  Wire.beginTransmission(address);
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    Wire.write(sendArray[index]);
  }
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  uint8_t result = Wire.endTransmission();
  if (result != 0) {
    SerialPrintln(F("ERROR: Unit ") + String(address) + F(" - offset write failed (Wire.endTransmission()=") + String(result) + F(")"));
    recoverI2CBus();
  }
  return result == 0;
}

//Lightweight version for callers that only need status/offset - 3 bytes instead of 6, so the
//Unit's TWI interrupt fires half as often. Used by /api/units; the 4-out-param version below is
//for callers that genuinely need phase/progress to tell "still working" from "actually stuck".
bool readUnitInfo(int address, int &statusOut, int &offsetOut) {
  Wire.requestFrom(address, ANSWER_SIZE_LIGHT, 1);

  if (Wire.available() < ANSWER_SIZE_LIGHT) {
    SerialPrint(F("ERROR: Unit "));
    SerialPrint(address);
    SerialPrintln(F(" - No/incomplete response reading status+offset (light)"));
    recoverI2CBus();
    return false;
  }

  statusOut = Wire.read();
  int offsetLow = Wire.read();
  int offsetHigh = Wire.read();
  offsetOut = offsetLow + (offsetHigh << 8);

  return true;
}

//Reads status, calibration offset, and phase/progress from a single unit in one I2C transaction.
//Returns true if the unit responded (out-params unchanged on failure).
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut) {
  Wire.requestFrom(address, ANSWER_SIZE, 1);

  if (Wire.available() < ANSWER_SIZE) {
    SerialPrint(F("ERROR: Unit "));
    SerialPrint(address);
    SerialPrintln(F(" - No/incomplete response reading status+offset"));

    // Try a bus recovery right at the point of failure (cheap) - gives the next read attempt a
    // real chance of succeeding instead of hitting the same stuck bus again.
    recoverI2CBus();

    return false;
  }

  statusOut = Wire.read();
  int offsetLow = Wire.read();
  int offsetHigh = Wire.read();
  offsetOut = offsetLow + (offsetHigh << 8);
  phaseOut = Wire.read();
  int progressLow = Wire.read();
  int progressHigh = Wire.read();
  progressOut = progressLow + (progressHigh << 8);

  return true;
}

//The old blocking waitForUnitReady()/waitForAllUnitsReady()/showMessage() wait loop now lives as
//the non-blocking pollDisplayJobWaitAllUnitsReady() state machine in ServiceDisplayJobFunctions.ino.

//Checks if unit in display is currently moving
bool isDisplayMoving() {
  //Request all units moving state and write to array
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    displayState[unitIndex] = checkIfMoving(unitIndex);
    if (displayState[unitIndex] == 1) {
      SerialPrint(F("Unit "));
      SerialPrint(unitIndex);
      SerialPrintln(F(" is busy (moving)"));
      return true;
    }
    //If unit is not available through i2c
    else if (displayState[unitIndex] == -1) {
      SerialPrint(F("Unit "));
      SerialPrint(unitIndex);
      SerialPrintln(F(" is sleeping/not responding"));
      return true;
    }
  }

  SerialPrintln(F("Display is standing still"));
  return false;
}

//Checks if single unit is moving
int checkIfMoving(int address) {
  int active;
  int offsetUnused;

  if (!readUnitInfo(address, active, offsetUnused)) {
    // Check I2C error
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    SerialPrint(F("  I2C error code: "));
    SerialPrintln(error);

    if (error == 2) {
      SerialPrintln(F("  -> Address received NACK (device not found)"));
    } else if (error == 3) {
      SerialPrintln(F("  -> Data received NACK"));
    } else if (error == 4) {
      SerialPrintln(F("  -> Unknown I2C error"));
    } else if (error == 5) {
      SerialPrintln(F("  -> Timeout"));
    }

    return -1;
  }

  SerialPrint(F("Unit "));
  SerialPrint(address);
  SerialPrint(F(" status: "));
  SerialPrintln(active);

  if (active == -1) {
    SerialPrint(F("WARNING: Unit "));
    SerialPrint(address);
    SerialPrintln(F(" returned -1 (sleeping), attempting wake-up..."));
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error != 0) {
      SerialPrint(F("  Wake-up failed, I2C error: "));
      SerialPrintln(error);
    }
  }
  
  return active;
}

