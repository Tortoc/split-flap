//Shows a new message. Non-blocking: starts a job (ServiceDisplayJobFunctions.ino) that
//updateDisplayJob() advances per loop() tick. alignmentOverride "" uses the global alignment;
//rotation items can pass their own. Returns whether the message was accepted (already showing or a
//job started) - callers with one-shot input (scheduled messages, /api/text) need to know a drop.
bool showText(const String &message) {
  return showText(message, 0, "");
}

bool showText(const String &message, int delayMillis) {
  return showText(message, delayMillis, "");
}

//""/-1 = use the global animation setting; only rotation items pass other values.
bool showText(const String &message, int delayMillis, const String &alignmentOverride) {
  return showText(message, delayMillis, alignmentOverride, "", -1);
}

bool showText(const String &requestedMessage, int delayMillis, const String &alignmentOverride, const String &animationModeOverride, int animationStaggerMsOverride) {
  //No units: a real delivery failure, guarded here for every caller.
  if (unitsAmount <= 0) {
    return false;
  }

  //Once, here, before any length/position measuring (incl. the lastWrittenText cache check).
  String message = normalizeUmlautsForDisplay(requestedMessage);

  //Quiet Hours override the request - checked here so every path parks the display the same way.
  if (isQuietHoursActive()) {
    message = "";
  }

  //"" (no override) falls back to the global default alignment.
  String effectiveAlignment = alignmentOverride != "" ? alignmentOverride : alignment;

  //lastWritten*/lastRenderedGeneration describe the last COMPLETED job. While busy, only a match
  //with the job actually running counts as satisfied; anything else goes through startDisplayJob().
  if (isDisplayJobBusy()) {
    //Compare the running job's raw request fields, not global state that may have moved on.
    bool matchesRunningJob = displayJob.originalMessage == message &&
      displayJob.alignmentOverride == alignmentOverride &&
      displayJob.animationModeOverride == animationModeOverride &&
      displayJob.animationStaggerMsOverride == animationStaggerMsOverride &&
      displayJob.finalPauseMs == (unsigned long)delayMillis;
    if (matchesRunningJob) {
      //A repeated request during the final hold restarts the hold for the full duration (no hold
      //exists yet while the job is still rendering).
      if (delayMillis > 0 && displayJob.state == DISPLAY_JOB_PAUSE_AFTER_FINISH) {
        displayJob.pauseUntil = millis() + (unsigned long)delayMillis;
      }
      return true; //already in progress toward exactly this request
    }
    return startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis, alignmentOverride, animationModeOverride, animationStaggerMsOverride); //false: busy, retried next tick
  }

  //Redraw if text or effective alignment differ, displayRenderGeneration moved on, or a hold time
  //is requested (a temporary /api/text with the current text still needs its own hold).
  if (lastWrittenText != message || lastWrittenAlignment != effectiveAlignment || lastRenderedGeneration != displayRenderGeneration || delayMillis != 0) {
    //Busy: skipped here, the next ~1s tick retries.
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

//Every write to a unit ends with an XOR checksum byte, verified by Unit.ino's receiveLetter() -
//I2C noise passes as a command ~1 in 256 times instead of ~1 in 6.
uint8_t i2cChecksum(uint8_t* data, int len) {
  uint8_t checksum = 0;
  for (int i = 0; i < len; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

//Writes letter position and speed to one unit. Returns whether the I2C write succeeded - a NACKed
//write leaves the flap untouched and the caller must not mark the line as shown.
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

//Writes a new calibration offset. The unit recalibrates at once and ends at letter 0 (blank), so
//that letter is sent along to avoid a second rotation.
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

//Status/offset only - 3 bytes instead of 6, so the unit's TWI interrupt fires half as often. Used
//by /api/units; the overload below adds phase/progress to tell "still working" from "stuck".
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

    // Bus recovery right at the failure, so the next read has a real chance.
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

//Adds the two diagnostic counters; separate overload because the other callers only need
//phase/progress. Used by GET /api/units?detail=1. I2C has no length prefix: a unit with older
//firmware still "sends" all requested bytes, the missing ones read as 0xFF - so these fields show
//255 there, not a real count.
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut, int &unexpectedHomeTriggerCountOut, int &slowHomeSearchCountOut) {
  Wire.requestFrom(address, ANSWER_SIZE_DIAG, 1);

  //Only the base ANSWER_SIZE (6) must be real data - see above.
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

//Adds Unit.ino's firmware version. Older units read back 255/255/255 (see above) - shown as
//"unknown" in the UI.
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut, int &unexpectedHomeTriggerCountOut, int &slowHomeSearchCountOut, int &fwVersionMajorOut, int &fwVersionMinorOut, int &fwVersionPatchOut) {
  Wire.requestFrom(address, ANSWER_SIZE_VERSION, 1);

  //Only the base ANSWER_SIZE (6) must be real data - see the 7-arg overload.
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
