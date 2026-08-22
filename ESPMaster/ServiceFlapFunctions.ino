//Shows a new message on the display.
//BUG FIX, 2026-08-21 (see BUGFIX_LOG.md #40/#81): this used to block synchronously for up to 90s
//twice per update (via waitForAllUnitsReady()) - live-confirmed this starves the "deferred to
//loop()" web request pattern, since that's only ever checked at the very top of loop(), which a
//blocked showText() call never lets loop() return to. Now just starts a non-blocking job (see
//ServiceDisplayJobFunctions.ino) and returns immediately; updateDisplayJob(), called once per
//loop() iteration, does the actual work incrementally. Signature/dedup-guard/caller-visible
//behavior are unchanged - every existing call site (main text/date/clock display, scheduled
//messages, the units reset sequence) keeps working the same way from the outside.
void showText(String message) {
  showText(message, 0);
}

void showText(String message, int delayMillis) {
  //Nothing to display to - see scanI2CBus(). Guarded here (rather than at every call site)
  //so scheduled messages, countdown, clock/date modes etc. all stay safe automatically.
  if (unitsAmount <= 0) {
    return;
  }

  if (lastWrittenText != message || alignmentUpdated) {
    //If a previous job is still running (e.g. a still-in-progress calibration search), this
    //attempt is silently skipped - lastWrittenText hasn't been updated yet, so the very next
    //loop() tick (showText() is called every ~1s from the main mode-display logic) naturally
    //retries once the current job finishes. Nothing is lost, just delayed until the display is
    //actually free.
    startDisplayJob(message, convertSpeed(flapSpeed), (unsigned long)delayMillis);
  }
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

//HARDENING, 2026-08-22 (see BUGFIX_LOG.md #85): every write to a Unit now ends with one extra
//XOR-checksum byte over the payload that preceded it. Unit.ino's receiveLetter() verifies it and
//silently ignores the whole message on a mismatch, instead of possibly acting on it. This is the
//defense against I2C bus noise/glitches (e.g. right after an ESPMaster reset/reflash, see #84's
//Offene Punkte) being misread as a real command - a stray/garbage byte sequence has only a
//~1-in-256 chance of passing a correct checksum by pure chance, versus the ~1-in-6 chance it
//would already fall in the valid 0-44 letter range on its own (range-checking alone is a much
//weaker defense here). Small helper shared by every write path below.
uint8_t i2cChecksum(uint8_t* data, int len) {
  uint8_t checksum = 0;
  for (int i = 0; i < len; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

//Write letter position and speed in rpm to single unit
void writeToUnit(int address, int letter, int flapSpeed) {
  uint8_t sendArray[2] = {(uint8_t)letter, (uint8_t)flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(address);

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint("sendArray: ");
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  Wire.endTransmission(); //send values to unit
}

//Write a new calibration offset to a single unit. The unit recalibrates immediately upon
//receiving this and ends up showing letter 0 (blank), so that letter is sent along with it
//to avoid triggering a second, separate rotation.
void writeOffsetToUnit(int address, uint16_t offset) {
  int currentSpeed = convertSpeed(flapSpeed);

  SerialPrint("Writing offset ");
  SerialPrint(offset);
  SerialPrint(" to unit ");
  SerialPrintln(address);

  //letter 0 = blank, matches the position the unit ends up at after recalibrating
  uint8_t sendArray[4] = {0, (uint8_t)currentSpeed, lowByte(offset), highByte(offset)};

  Wire.beginTransmission(address);
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    Wire.write(sendArray[index]);
  }
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  Wire.endTransmission();
}

//Lightweight version for callers that only care about status/offset, not phase/progress -
//requests only ANSWER_SIZE_LIGHT (3) bytes instead of the full ANSWER_SIZE (6), so the Unit's
//TWI interrupt only fires 3 times per read instead of 6 (see the comment on ANSWER_SIZE_LIGHT
//in ESPMaster.ino). Used by /api/units (the Administration page's unit list, which never
//displayed phase/progress anyway) - waitForUnitReady() below still uses the full 4-out-param
//version, since it genuinely needs phase/progress to tell "still working" from "actually stuck".
bool readUnitInfo(int address, int &statusOut, int &offsetOut) {
  Wire.requestFrom(address, ANSWER_SIZE_LIGHT, 1);

  if (Wire.available() < ANSWER_SIZE_LIGHT) {
    SerialPrint("ERROR: Unit ");
    SerialPrint(address);
    SerialPrintln(" - No/incomplete response reading status+offset (light)");
    recoverI2CBus();
    return false;
  }

  statusOut = Wire.read();
  int offsetLow = Wire.read();
  int offsetHigh = Wire.read();
  offsetOut = offsetLow + (offsetHigh << 8);

  return true;
}

//Reads status, calibration offset, and (DIAGNOSTIC, 2026-08-13) phase/progress from a single
//unit in one I2C transaction. phaseOut/progressOut let a caller tell "still legitimately busy,
//making progress" from "actually stuck" - see the comment on currentPhase in Unit.ino.
//Returns true if the unit responded, false otherwise (out-params are left unchanged on failure).
bool readUnitInfo(int address, int &statusOut, int &offsetOut, int &phaseOut, int &progressOut) {
  Wire.requestFrom(address, ANSWER_SIZE, 1);

  if (Wire.available() < ANSWER_SIZE) {
    SerialPrint("ERROR: Unit ");
    SerialPrint(address);
    SerialPrintln(" - No/incomplete response reading status+offset");

    // CREATIVE FIX, 2026-08-14: right at the point of failure, try an I2C bus recovery (see
    // recoverI2CBus() in ServiceI2CRecoveryFunctions.ino) - cheap, and gives the *next* read
    // attempt (by whichever caller retries) a real chance of succeeding instead of hitting the
    // same stuck bus again. See BUGFIX_LOG.md #45 for the research behind this approach.
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

//REMOVED, 2026-08-21 (see BUGFIX_LOG.md #40/#81): waitForUnitReady()/waitForAllUnitsReady()/
//showMessage() used to live here - a blocking, up-to-90s-per-call wait loop plus its caller.
//Replaced by the non-blocking pollDisplayJobWaitAllUnitsReady() state-machine step in
//ServiceDisplayJobFunctions.ino, which does the exact same per-unit stuck/timeout logic (same
//90s/15s constants, same forceUnitRecoveryReset() escalation) but advances at most one unit-poll
//per loop() iteration instead of blocking loop() itself until the whole wait is done. No other
//caller used these functions (verified by grep before removing them) - showText() above was the
//only one.

//Checks if unit in display is currently moving
bool isDisplayMoving() {
  //Request all units moving state and write to array
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    displayState[unitIndex] = checkIfMoving(unitIndex);
    if (displayState[unitIndex] == 1) {
      SerialPrint("Unit ");
      SerialPrint(unitIndex);
      SerialPrintln(" is busy (moving)");
      return true;
    } 
    //If unit is not available through i2c
    else if (displayState[unitIndex] == -1) {
      SerialPrint("Unit ");
      SerialPrint(unitIndex);
      SerialPrintln(" is sleeping/not responding");
      return true;
    }
  }

  SerialPrintln("Display is standing still");
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
    SerialPrint("  I2C error code: ");
    SerialPrintln(error);

    if (error == 2) {
      SerialPrintln("  -> Address received NACK (device not found)");
    } else if (error == 3) {
      SerialPrintln("  -> Data received NACK");
    } else if (error == 4) {
      SerialPrintln("  -> Unknown I2C error");
    } else if (error == 5) {
      SerialPrintln("  -> Timeout");
    }

    return -1;
  }

  SerialPrint("Unit ");
  SerialPrint(address);
  SerialPrint(" status: ");
  SerialPrintln(active);

  if (active == -1) {
    SerialPrint("WARNING: Unit ");
    SerialPrint(address);
    SerialPrintln(" returned -1 (sleeping), attempting wake-up...");
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    if (error != 0) {
      SerialPrint("  Wake-up failed, I2C error: ");
      SerialPrintln(error);
    }
  }
  
  return active;
}

