//Shows a new message on the display
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
    String messageDisplay = message == "" ? "<Blank>" : message;
    String alignmentUpdatedDisplay = alignmentUpdated ? "Yes" : "No";

    SerialPrintln("Showing new Message");
    SerialPrintln("New Message: " + messageDisplay);
    SerialPrintln("Alignment Updated: " + alignmentUpdatedDisplay);
  
    LList<String> messageLines = processSentenceToLines(message);

    if (messageLines.size() > 1) {
      SerialPrintln("Showing a split down message");
    
      //Iterate over all the message lines we've got
      for (int linesIndex = 0; linesIndex < messageLines.size(); linesIndex++) {
        String line = messageLines[linesIndex];

        SerialPrint("-- Message Line: ");
        SerialPrintln(line);

        showMessage(line, convertSpeed(flapSpeed));
    
        //If the lines index isn't the last, delay showing the next message to give time to read
        // Use non-blocking delay with yield to allow web server to process
        if (linesIndex <= messageLines.size()) {
          unsigned long delayStart = millis();
          while (millis() - delayStart < 3000) {
            yield(); // Allow web server to process requests
            //DIAGNOSTIC FIX, 2026-08-14: also service the lightweight deferred admin
            //requests here - see the comment on processLightweightDeferredRequests() in
            //ESPMaster.ino. This is a pure delay loop (no I2C transaction in flight), safe
            //to interleave.
            processLightweightDeferredRequests();
            delay(100);
          }
        }
      }  
    }
    else {
      SerialPrintln("Showing a simple message: " + message);
      
      showMessage(message, convertSpeed(flapSpeed));
    }  

    //If the device wasn't previously in text mode, delay for a short time so can read!
    // Use non-blocking delay with yield
    if (delayMillis != 0) {
      SerialPrintln("Pausing for a small duration. Delay: " + String(delayMillis));
      unsigned long delayStart = millis();
      while (millis() - delayStart < delayMillis) {
        yield(); // Allow web server to process requests
        //DIAGNOSTIC FIX, 2026-08-14: see the comment on the identical addition above.
        processLightweightDeferredRequests();
        delay(100);
      }
    }

    //Save what we last did
    lastWrittenText = message;

    //Alignment definitely has not changed now
    alignmentUpdated = false;
    
    SerialPrintln("Done showing message");
  }
}

//Pushes message to units
void showMessage(String message, int flapSpeed) {
  //Format string per alignment choice
  if (alignment == ALIGNMENT_MODE_LEFT) {
    message = leftString(message);
  } 
  else if (alignment == ALIGNMENT_MODE_RIGHT) {
    message = rightString(message);
  } 
  else if (alignment == ALIGNMENT_MODE_CENTER) {
    message = centerString(message);
  }

  SerialPrint("Showing Aligned Message: \"");
  SerialPrint(message);
  SerialPrintln("\"");

#if UNIT_CALLS_DISABLE == true
  SerialPrintln("Unit Calls are disabled for debugging. Will delay to simulate calls...");
  unsigned long delayStart = millis();
  while (millis() - delayStart < 2000) {
    yield();
    delay(100);
  }
#else
  //Wait while display is still moving - ADD YIELD TO PREVENT HANGING
  SerialPrintln("Unit calls are enabled. Will display message");
  //DIAGNOSTIC, 2026-08-13: was a fixed 30s timeout regardless of what the unit was actually
  //doing - too short for a legitimate worst-case calibration search + rotation (can add up to
  //20-30s+ by itself), which made the code give up and send the *next* letter while the unit
  //was still finishing the current one. Now waits as long as the unit keeps reporting genuine
  //progress (see waitForAllUnitsReady()), up to a generous 90s hard cap, and only gives up
  //early if there's truly been no progress for 15s.
  waitForAllUnitsReady(90000, 15000);

  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    char currentLetter = message[unitIndex];
    int currentLetterPosition = translateLettertoInt(currentLetter);
    
    SerialPrint("Unit Nr.: ");
    SerialPrint(unitIndex);
    SerialPrint(" Letter: ");
    SerialPrint(message[unitIndex]);
    SerialPrint(" Letter position: ");
    SerialPrintln(currentLetterPosition);

    //only write to unit if char exists in letter array
    if (currentLetterPosition != -1) {
      writeToUnit(unitIndex, currentLetterPosition, flapSpeed);
    }
    
    // Small yield between units to keep web server responsive
    yield();
  }

  //Wait for the display to stop moving before exit - ADD YIELD TO PREVENT HANGING
  //DIAGNOSTIC, 2026-08-13: same progress-aware wait as above, same reasoning.
  waitForAllUnitsReady(90000, 15000);
#endif
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

//Write letter position and speed in rpm to single unit
void writeToUnit(int address, int letter, int flapSpeed) {
  int sendArray[2] = {letter, flapSpeed}; //Array with values to send to unit

  Wire.beginTransmission(address);

  //Write values to send to slave in buffer
  for (unsigned int index = 0; index < sizeof sendArray / sizeof sendArray[0]; index++) {
    SerialPrint("sendArray: ");
    SerialPrintln(sendArray[index]);

    Wire.write(sendArray[index]);
  }
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

  Wire.beginTransmission(address);
  Wire.write(0); //letter 0 = blank, matches the position the unit ends up at after recalibrating
  Wire.write(currentSpeed);
  Wire.write(lowByte(offset));
  Wire.write(highByte(offset));
  Wire.endTransmission();
}

//Convenience overload for callers that only care about status/offset, not phase/progress.
bool readUnitInfo(int address, int &statusOut, int &offsetOut) {
  int phaseOut, progressOut;
  return readUnitInfo(address, statusOut, offsetOut, phaseOut, progressOut);
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

//DIAGNOSTIC, 2026-08-13: waits for one unit to report "not moving", but instead of betting on
//a single guessed timeout, extends the wait as long as we see genuine progress (phase/step
//count actually changing between polls - see currentPhase/currentPhaseProgress in Unit.ino).
//Only gives up if there's been no progress at all for stuckTimeoutMs (a real stuck/hung unit),
//or the hard maxTotalWaitMs safety cap is hit regardless (never wait literally forever).
//Replaces the old fixed-30s-timeout loops, which turned out to be too short for a legitimate
//worst-case calibration search + rotation, causing the code to give up and send the *next*
//letter command while the unit was still finishing the previous one.
//Returns true if the unit reported idle, false if we gave up.
bool waitForUnitReady(int unitIndex, unsigned long maxTotalWaitMs, unsigned long stuckTimeoutMs) {
  unsigned long waitStart = millis();
  unsigned long lastProgressSeenAt = waitStart;
  int lastPhase = -1;
  int lastProgress = -1;

  while (millis() - waitStart < maxTotalWaitMs) {
    int status, offset, phase, progress;
    bool ok = readUnitInfo(unitIndex, status, offset, phase, progress);

    if (ok && status != 1) {
      //0 = ready, -1 = sleeping - either way, not actively moving, nothing to wait for
      return true;
    }

    if (ok) {
      SerialPrintln("Unit " + String(unitIndex) + " busy - phase " + String(phase) + ", progress " + String(progress) +
        " (" + String((millis() - waitStart) / 1000) + "s waited)");

      if (phase != lastPhase || progress != lastProgress) {
        lastPhase = phase;
        lastProgress = progress;
        lastProgressSeenAt = millis();
      }
      //BUG FIX, 2026-08-13: the stuck-timeout check used to live inside this `if (ok)` branch,
      //so it only ever fired when reads were succeeding but showing static phase/progress. A
      //unit whose reads keep failing outright (not hanging, just erroring - a real, previously
      //observed failure mode) never hit this check at all, and would only be caught by the much
      //slower maxTotalWaitMs cap. Moved out so read failures count the same as "no progress".
    } else {
      SerialPrintln("Unit " + String(unitIndex) + " - read failed while waiting, retrying");
    }

    if (millis() - lastProgressSeenAt > stuckTimeoutMs) {
      SerialPrintln("WARNING: Unit " + String(unitIndex) + " - no progress for " + String(stuckTimeoutMs / 1000) + "s, giving up");
      return false;
    }

    yield(); // CRITICAL: Allow web server to process requests
    //DIAGNOSTIC FIX, 2026-08-14: see the comment on processLightweightDeferredRequests() in
    //ESPMaster.ino - this is right after readUnitInfo() completed for this iteration and
    //before the next one starts, a safe point with no I2C transaction in flight.
    processLightweightDeferredRequests();
    delay(1000);
  }

  SerialPrintln("WARNING: Unit " + String(unitIndex) + " - hit max wait time (" + String(maxTotalWaitMs / 1000) + "s), giving up");
  return false;
}

//Same as waitForUnitReady(), but for every currently-known unit in sequence.
bool waitForAllUnitsReady(unsigned long maxTotalWaitMs, unsigned long stuckTimeoutMs) {
  bool allReady = true;
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    if (!waitForUnitReady(unitIndex, maxTotalWaitMs, stuckTimeoutMs)) {
      allReady = false;
    }
  }
  return allReady;
}

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

