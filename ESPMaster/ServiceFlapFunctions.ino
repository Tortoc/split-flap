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
  unsigned long waitStart = millis();
  unsigned long waitTimeout = 30000; // 30 second timeout to prevent infinite hang
  //DIAGNOSTIC, 2026-08-13: poll interval widened from 500ms to 1000ms (30 -> ~30 I2C reads
  //over the same timeout, half as many as before). Wire.requestFrom() has a rare but real
  //chance of hanging outright on this hardware with no way for us to
  //time it out from the caller side - every extra read here is extra exposure to that for
  //no benefit, since the unit can't plausibly finish a rotation and report it faster than
  //once a second anyway.
  while (isDisplayMoving() && (millis() - waitStart < waitTimeout)) {
    SerialPrintln("Waiting for display to stop");
    yield(); // CRITICAL: Allow web server to process requests
    delay(1000);
  }
  
  if (millis() - waitStart >= waitTimeout) {
    SerialPrintln("WARNING: Display wait timeout - continuing anyway");
  }

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
  waitStart = millis();
  waitTimeout = 30000; // 30 second timeout
  //DIAGNOSTIC, 2026-08-13: was delay(100) - up to 300 I2C reads over the full timeout, by
  //far the single biggest source of exposure to the rare Wire.requestFrom() hang (see the
  //comment on the wait loop above). Widened to 1000ms, same reasoning.
  while (isDisplayMoving() && (millis() - waitStart < waitTimeout)) {
    SerialPrintln("Waiting for display to stop now message is display");
    yield(); // CRITICAL: Allow web server to process requests
    delay(1000);
  }
  
  if (millis() - waitStart >= waitTimeout) {
    SerialPrintln("WARNING: Display wait timeout - continuing anyway");
  }
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

//Reads status and calibration offset from a single unit in one I2C transaction.
//Returns true if the unit responded, false otherwise (statusOut/offsetOut are left unchanged on failure).
bool readUnitInfo(int address, int &statusOut, int &offsetOut) {
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

  return true;
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

