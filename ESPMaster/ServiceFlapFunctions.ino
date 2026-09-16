//Shows a new message on the display. Non-blocking: starts a job (ServiceDisplayJobFunctions.ino)
//and returns immediately - updateDisplayJob() does the actual work incrementally per loop() tick.
//alignmentOverride: "" (default) uses the global default alignment; a non-empty value
//("left"/"center"/"right") overrides it just for this message - used by Rotation items that set
//their own alignment (see checkRotation()).
//
//Returns whether the message was actually accepted (already showing, or a job was successfully
//started) vs. genuinely dropped (no units, or the display was busy) - callers with a one-shot
//input (scheduled messages, /api/text) need this to know a message was actually lost.
bool showText(String message) {
  return showText(message, 0, "");
}

bool showText(String message, int delayMillis) {
  return showText(message, delayMillis, "");
}

//""/-1 = no animation override, use the global animation setting. Only Rotation items ever pass a
//non-default value here (see checkRotation()).
bool showText(String message, int delayMillis, String alignmentOverride) {
  return showText(message, delayMillis, alignmentOverride, "", -1);
}

bool showText(String message, int delayMillis, String alignmentOverride, String animationModeOverride, int animationStaggerMsOverride) {
  //Nothing to display to - guarded here so scheduled messages, countdown, clock/date modes etc.
  //all stay safe automatically. A real failure to deliver, not a no-op.
  if (unitsAmount <= 0) {
    return false;
  }

  //Centralized here, not at each individual input path - must run before anything below measures
  //length/position, including the lastWrittenText cache comparison.
  message = normalizeUmlautsForDisplay(message);

  //Quiet Hours override whatever was actually requested - checked here so every path parks the
  //display uniformly without needing its own check.
  if (isQuietHoursActive()) {
    message = "";
  }

  //"" (no override) falls back to the global default alignment.
  String effectiveAlignment = alignmentOverride != "" ? alignmentOverride : alignment;

  //lastWrittenText/lastWrittenAlignment/lastRenderedGeneration only reflect the LAST COMPLETED
  //job - while a job is still running, they describe stale history, not what's actually mid-flight.
  //While busy, only a genuine match against the job that's ACTUALLY running counts as already
  //satisfied - anything else must go through startDisplayJob() like normal.
  if (isDisplayJobBusy()) {
    //Compare against the running job's own raw request fields, not resolved/global state that
    //could have moved on independently.
    bool matchesRunningJob = displayJob.originalMessage == message &&
      displayJob.alignmentOverride == alignmentOverride &&
      displayJob.animationModeOverride == animationModeOverride &&
      displayJob.animationStaggerMsOverride == animationStaggerMsOverride &&
      displayJob.finalPauseMs == (unsigned long)delayMillis;
    if (matchesRunningJob) {
      //A repeated identical request while the job is already sitting in its final hold phase
      //refreshes that hold for a full fresh duration, rather than succeeding against whatever
      //time happens to already be left on the original countdown. Only applies once the job has
      //actually settled into the hold - while still rendering toward this request for the first
      //time, there's no hold running yet to refresh.
      if (delayMillis > 0 && displayJob.state == DISPLAY_JOB_PAUSE_AFTER_FINISH) {
        displayJob.pauseUntil = millis() + (unsigned long)delayMillis;
      }
      return true; //already in progress toward exactly this request
    }
    return startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis, alignmentOverride, animationModeOverride, animationStaggerMsOverride); //correctly returns false: busy, try again next tick
  }

  //Redraw if the text or effective alignment actually differs from what's really on the display,
  //OR if displayRenderGeneration has moved on since the last completed render (see its own
  //comment), OR whenever an explicit hold time is freshly requested (delayMillis != 0) even if
  //text/alignment/generation are otherwise already correct - e.g. a temporary /api/text push
  //repeating the current text still needs to (re)start its own hold.
  if (lastWrittenText != message || lastWrittenAlignment != effectiveAlignment || lastRenderedGeneration != displayRenderGeneration || delayMillis != 0) {
    //If a job is still running, this attempt is silently skipped - lastWrittenText hasn't updated
    //yet, so the next ~1s loop() tick naturally retries once the display is free.
    return startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis, alignmentOverride, animationModeOverride, animationStaggerMsOverride);
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
//succeeded (Wire.endTransmission() == 0) - a NACK'd write leaves that unit's flap untouched, and
//the caller needs to know rather than marking the line "shown" regardless.
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
//receiving this and ends up showing letter 0 (blank), so that letter is sent along with it to
//avoid triggering a second, separate rotation.
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

//Same as the 5-arg overload above, plus the two diagnostic counters. Deliberately a separate
//overload rather than extending the 5-arg one in place - its two existing call sites only need
//phase/progress. Used only where the diagnostics are actually surfaced (GET /api/units?detail=1).
//
//IMPORTANT: I2C has no short-response/length-prefix mechanism - once Wire.requestFrom() succeeds
//at all, the master gets ALL requested bytes regardless of how many the slave actually prepared;
//a not-yet-updated unit's "extra" bytes come back as 0xFF (idle bus), not a detectable "nothing
//sent" marker. So these two fields read 255, not -1, for a not-yet-updated unit - not a real
//count, not a distinguishable "unsupported" sentinel either.
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut, int &unexpectedHomeTriggerCountOut, int &slowHomeSearchCountOut) {
  Wire.requestFrom(address, ANSWER_SIZE_DIAG, 1);

  //Only the base ANSWER_SIZE (6) is required for a successful read - see the comment above for why
  //a not-yet-updated unit's response is still exactly ANSWER_SIZE_DIAG (8) bytes from this
  //function's point of view, just with the last 2 not being real data.
  if (Wire.available() < ANSWER_SIZE) {
    SerialPrint(F("ERROR: Unit "));
    SerialPrint(address);
    SerialPrintln(F(" - No/incomplete response reading status+offset+diagnostics"));

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
  unexpectedHomeTriggerCountOut = Wire.available() >= 2 ? Wire.read() : -1;
  slowHomeSearchCountOut = Wire.available() >= 1 ? Wire.read() : -1;

  return true;
}

//Same as the 7-arg overload above, plus Unit.ino's own firmware version (major/minor/patch).
//Same caveat: a unit running firmware from before this version byte existed still answers without
//error - the last 3 bytes read back as 255/255/255, not a distinguishable "unsupported" marker.
//Report that triple as "unknown" in the UI, not as a literal "v255.255.255".
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut, int &unexpectedHomeTriggerCountOut, int &slowHomeSearchCountOut, int &fwVersionMajorOut, int &fwVersionMinorOut, int &fwVersionPatchOut) {
  Wire.requestFrom(address, ANSWER_SIZE_VERSION, 1);

  //Only the base ANSWER_SIZE (6) is required for a successful read - see the 7-arg overload's own
  //comment for why a not-yet-updated unit's response is still exactly ANSWER_SIZE_VERSION (11)
  //bytes from this function's point of view, just with the last 5 not being real data.
  if (Wire.available() < ANSWER_SIZE) {
    SerialPrint(F("ERROR: Unit "));
    SerialPrint(address);
    SerialPrintln(F(" - No/incomplete response reading status+offset+diagnostics+version"));

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
  unexpectedHomeTriggerCountOut = Wire.available() >= 5 ? Wire.read() : -1;
  slowHomeSearchCountOut = Wire.available() >= 4 ? Wire.read() : -1;
  fwVersionMajorOut = Wire.available() >= 3 ? Wire.read() : -1;
  fwVersionMinorOut = Wire.available() >= 2 ? Wire.read() : -1;
  fwVersionPatchOut = Wire.available() >= 1 ? Wire.read() : -1;

  return true;
}

//The old blocking waitForUnitReady()/waitForAllUnitsReady()/showMessage() wait loop now lives as
//the non-blocking pollDisplayJobWaitAllUnitsReady() state machine in ServiceDisplayJobFunctions.ino.

