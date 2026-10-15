//twiboot I2C client + push orchestration for updating Unit.ino firmware over I2C. Only used when
//UNIT_BOOTLOADER_ENABLE is true - every unit must already run twiboot, see Unit/BOOTLOADER_SETUP.md.
//
//Protocol (https://github.com/orempel/twiboot):
//  - abort boot timeout: SLA+W, 0x00, STOP
//  - start application:  SLA+W, 0x01, 0x80, STOP
//  - read chip info:     SLA+W, 0x02, 0x00, 0x00, 0x00, SLA+R, {8 bytes}, STOP
//  - read flash:         SLA+W, 0x02, 0x01, addrH, addrL, SLA+R, {N bytes}, STOP
//  - write flash page:   SLA+W, 0x02, 0x01, addrH, addrL, {pageSize bytes}, STOP
//    (read vs write differ only in whether a repeated-start read follows; a page only commits if
//    exactly pageSize bytes were sent)
//  - read eeprom byte:   SLA+W, 0x02, 0x02, addrH, addrL, SLA+R, {N bytes}, STOP
//  - write eeprom byte:  SLA+W, 0x02, 0x02, addrH, addrL, {N bytes}, STOP
#if UNIT_BOOTLOADER_ENABLE == true

#include <string.h>

#define TWIBOOT_CMD_WAIT              0x00
#define TWIBOOT_CMD_SWITCH_APP        0x01
#define TWIBOOT_CMD_ACCESS_MEMORY     0x02
#define TWIBOOT_BOOTTYPE_APPLICATION  0x80
#define TWIBOOT_MEMTYPE_CHIPINFO      0x00
#define TWIBOOT_MEMTYPE_FLASH         0x01
#define TWIBOOT_MEMTYPE_EEPROM        0x02
#define UNIT_BOOTLOADER_MAX_PAGE_SIZE 256 //ATmega328p pages are 128 bytes

//"Update complete" gate twiboot checks before booting the app (Change 2 in twiboot's main.c). Must
//match EEPROM_UPDATE_MARKER_ADDR/INPROGRESS there; opposite end of the EEPROM from calOffset.
#define UNIT_BOOTLOADER_UPDATE_MARKER_ADDR         1023
#define UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS   0x00
#define UNIT_BOOTLOADER_UPDATE_MARKER_COMPLETE     0xFF

//ATmega328p signature - tells a real twiboot reply apart from a running app answering the same read.
#define UNIT_EXPECTED_CHIP_SIGNATURE_0 0x1E
#define UNIT_EXPECTED_CHIP_SIGNATURE_1 0x95
#define UNIT_EXPECTED_CHIP_SIGNATURE_2 0x0F

//TwibootChipInfo is defined in Classes.h

//Tells the unit's running app to reboot into twiboot (Unit.ino's CMD_ENTER_BOOTLOADER), sent to the
//normal app address. Returns Wire.endTransmission()'s result (0 = success).
uint8_t requestUnitEnterBootloader(int unitIndex) {
  //With checksum like every write - a misread byte sequence must not reboot the unit.
  uint8_t sendArray[2] = {UNIT_BOOTLOADER_CMD, 0}; //speed byte - unused for this command, but the unit always reads 2 bytes
  Wire.beginTransmission(unitIndex);
  Wire.write(sendArray[0]);
  Wire.write(sendArray[1]);
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  return Wire.endTransmission();
}

//From 42 units upward the bootloader range (41-104) overlaps the app range (0-63): a running app
//ACKs the presence check and "succeeds" the chip-info read with garbage, so the exact signature is
//verified, not just read success.
#define UNIT_IS_EXPECTED_CHIP_SIGNATURE(info) \
  ((info).signature[0] == UNIT_EXPECTED_CHIP_SIGNATURE_0 && \
   (info).signature[1] == UNIT_EXPECTED_CHIP_SIGNATURE_1 && \
   (info).signature[2] == UNIT_EXPECTED_CHIP_SIGNATURE_2)

//Probes only the address expected for unitIndex instead of scanning the whole bootloader range -
//used by pushFirmwareToUnit() and the probe endpoint, which already know their unit. A full scan
//refused to recover any unit as soon as a second one sat in the bootloader (e.g. parked there by
//the EEPROM marker after an aborted update).
bool findExpectedBootloaderAddress(int unitIndex, TwibootChipInfo &infoOut, unsigned long searchStartMs) {
  int expectedBootAddress = UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex;
  for (int attempt = 0; attempt < 5; attempt++) {
    delay(attempt == 0 ? 300 : 150);
    yield();
    unsigned long probeStart = millis();
    bool found = twibootReadChipInfo(expectedBootAddress, infoOut) && UNIT_IS_EXPECTED_CHIP_SIGNATURE(infoOut);
    SerialPrintln(F("Bootloader probe attempt ") + String(attempt) + F(": ") + String(millis() - probeStart) +
      F("ms for this probe, ") + String(millis() - searchStartMs) + F("ms total since requesting bootloader mode, found=") +
      (found ? F("yes") : F("no")));
    if (found) {
      return true;
    }
  }
  return false;
}

//"Abort boot timeout" heartbeat, resets twiboot's 1000ms auto-boot countdown. Every read/write does
//that too; this is a cheap extra between steps.
bool twibootHeartbeat(int bootAddress) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_WAIT);
  return Wire.endTransmission() == 0;
}

//Reads signature, page size and bootloader start (= max writable flash address). Real STOP + short
//settle delay instead of a repeated START (esp8266/Arduino #5718); twiboot keeps its state on STOP.
bool twibootReadChipInfo(int bootAddress, TwibootChipInfo &infoOut) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_CHIPINFO);
  Wire.write((uint8_t)0x00); //address bytes - unused for chipinfo, but the protocol always expects 2
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(true) != 0) { //real STOP - see the comment above this function
    return false;
  }
  delayMicroseconds(500);

  if (Wire.requestFrom(bootAddress, 8) != 8) {
    return false;
  }

  infoOut.signature[0] = Wire.read();
  infoOut.signature[1] = Wire.read();
  infoOut.signature[2] = Wire.read();
  infoOut.pageSize = Wire.read();
  infoOut.bootloaderStart = ((uint16_t)Wire.read()) << 8;
  infoOut.bootloaderStart |= Wire.read();
  infoOut.eepromSize = ((uint16_t)Wire.read()) << 8;
  infoOut.eepromSize |= Wire.read();
  return true;
}

//Reads twiboot's 16-byte version string (real STOP, see above). versionOut needs 17 bytes - the
//null terminator is added here.
bool twibootReadVersion(int bootAddress, char* versionOut, size_t bufSize) {
  if (bufSize < 17) {
    return false;
  }
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_SWITCH_APP); //same opcode as CMD_READ_VERSION (0x01) - see main.c's protocol comment
  if (Wire.endTransmission(true) != 0) { //real STOP - see the comment above this function
    return false;
  }
  delayMicroseconds(500);

  if (Wire.requestFrom(bootAddress, 16) != 16) {
    return false;
  }

  for (int i = 0; i < 16; i++) {
    versionOut[i] = (char)Wire.read();
  }
  versionOut[16] = '\0';
  return true;
}

//Reads `len` flash bytes at `flashAddress` for the verify pass (real STOP, see above).
bool twibootReadFlash(int bootAddress, uint16_t flashAddress, uint8_t* bufferOut, uint8_t len) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)(flashAddress >> 8));
  Wire.write((uint8_t)(flashAddress & 0xFF));
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  delayMicroseconds(500);

  if (Wire.requestFrom(bootAddress, (int)len) != len) {
    return false;
  }

  for (uint8_t i = 0; i < len; i++) {
    bufferOut[i] = Wire.read();
  }
  return true;
}

//Reads one EEPROM byte to verify the marker writes (real STOP, see above).
bool twibootReadEepromByte(int bootAddress, uint16_t eepromAddress, uint8_t* byteOut) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_EEPROM);
  Wire.write((uint8_t)(eepromAddress >> 8));
  Wire.write((uint8_t)(eepromAddress & 0xFF));
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  delayMicroseconds(500);

  if (Wire.requestFrom(bootAddress, 1) != 1) {
    return false;
  }
  *byteOut = Wire.read();
  return true;
}

//Writes one EEPROM byte and reads it back - used for the update marker. Always verified: a dropped
//"in progress" write would defeat arming it before touching flash. Retries like writeAndVerifyPage().
bool twibootWriteAndVerifyEepromByte(int bootAddress, uint16_t eepromAddress, uint8_t value, String &failureReasonOut) {
  const int maxAttempts = 3;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      recoverI2CBus();
      delay(20);
    }

    //One byte fits Wire's 128-byte TX buffer - no twi_writeTo() needed here.
    Wire.beginTransmission(bootAddress);
    Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
    Wire.write(TWIBOOT_MEMTYPE_EEPROM);
    Wire.write((uint8_t)(eepromAddress >> 8));
    Wire.write((uint8_t)(eepromAddress & 0xFF));
    Wire.write(value);
    if (Wire.endTransmission(true) != 0) {
      failureReasonOut = "I2C write failed on EEPROM byte at address " + String(eepromAddress);
      continue;
    }

    uint8_t readBack = 0;
    if (!twibootReadEepromByte(bootAddress, eepromAddress, &readBack)) {
      failureReasonOut = "I2C verify-read failed on EEPROM byte at address " + String(eepromAddress);
      continue;
    }
    if (readBack != value) {
      failureReasonOut = "Verification mismatch on EEPROM byte at address " + String(eepromAddress);
      continue;
    }
    return true;
  }
  return false;
}

//Writes one full flash page; `data` must be exactly `pageSize` bytes (pad the last page with 0xFF),
//a short write is ignored. Raw twi_writeTo(): 132 bytes exceed Wire's 128-byte TX buffer.
bool twibootWriteFlashPage(int bootAddress, uint16_t flashAddress, const uint8_t* data, uint8_t pageSize) {
  uint8_t buf[4 + UNIT_BOOTLOADER_MAX_PAGE_SIZE];
  buf[0] = TWIBOOT_CMD_ACCESS_MEMORY;
  buf[1] = TWIBOOT_MEMTYPE_FLASH;
  buf[2] = (uint8_t)(flashAddress >> 8);
  buf[3] = (uint8_t)(flashAddress & 0xFF);
  memcpy(buf + 4, data, pageSize);
  return twi_writeTo(bootAddress, buf, 4 + pageSize, 1) == 0; //sendStop=1, STOP triggers the actual erase+write
}

//Starts the new application - only after every page (page 0 too) is written and verified. twiboot
//NACKs this command's data byte by design; that's success.
bool twibootStartApplication(int bootAddress) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_SWITCH_APP);
  Wire.write(TWIBOOT_BOOTTYPE_APPLICATION);
  uint8_t result = Wire.endTransmission();
  SerialPrintln(F("DEBUG: twibootStartApplication() Wire.endTransmission() = ") + String(result));
  //A data-byte NACK (3) is expected here; anything else is a real failure.
  return result == 0 || result == 3;
}

//For a push that stops before any flash page was written: disarms the update marker if this push may
//have armed it (a failed read-back doesn't mean the write failed), then sends the unit back to its
//app. Without this a bootloader without the rolling idle timeout (before v3.5) waits for a power
//cycle, and an armed marker keeps any bootloader there. A marker that was already armed (or couldn't
//be read) before this push stays untouched - the flash may hold a half-written earlier update.
//Returns a sentence for the error message.
String returnUnitToAppAfterAbort(int bootAddress, bool markerFreeBefore, bool armingAttempted) {
  if (!markerFreeBefore) {
    return F(" The unit still holds an unfinished earlier update and stays in its bootloader until an update completes.");
  }
  if (armingAttempted) {
    String ignoredReason;
    if (!twibootWriteAndVerifyEepromByte(bootAddress, UNIT_BOOTLOADER_UPDATE_MARKER_ADDR, UNIT_BOOTLOADER_UPDATE_MARKER_COMPLETE, ignoredReason)) {
      return F(" The update marker could not be cleared - the unit may stay in its bootloader; run the update again (Units tab, \"Other unit number...\").");
    }
  }
  if (!twibootStartApplication(bootAddress)) {
    return F(" The unit didn't confirm the return to its app - check the Units page, or power-cycle the unit if it doesn't come back.");
  }
  return F(" The unit was sent back to its app.");
}

//Pushes the firmware at unitFirmwarePath to the unit over I2C.
//
//Safety: the EEPROM update marker is armed before any page is touched and cleared only after page 0
//is verified; twiboot won't boot the app while it says "in progress" (Change 2 in main.c). Writing
//page 0 last isn't enough on its own - the reset vector's jump target lands in page 1. The
//bootloader itself is never touched, so a stuck unit isn't bricked. writeAndVerifyPage() retries
//with a bus recovery in between (a repeated-start read occasionally corrupts just the read-back).
bool writeAndVerifyPage(int bootAddress, uint16_t flashAddress, uint8_t* writeBuffer, uint8_t* verifyBuffer,
                         uint8_t pageSize, String &failureReasonOut) {
  const int maxAttempts = 3;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      SerialPrintln(F("Retrying page write+verify at flash address ") + String(flashAddress) + F(" (attempt ") + String(attempt + 1) + F(")"));
      recoverI2CBus();
      delay(20);
    }

    if (!twibootWriteFlashPage(bootAddress, flashAddress, writeBuffer, pageSize)) {
      failureReasonOut = "I2C write failed on page at flash address " + String(flashAddress);
      continue;
    }
    if (!twibootReadFlash(bootAddress, flashAddress, verifyBuffer, pageSize)) {
      failureReasonOut = "I2C verify-read failed on page at flash address " + String(flashAddress);
      continue;
    }
    if (memcmp(writeBuffer, verifyBuffer, pageSize) != 0) {
      failureReasonOut = "Verification mismatch on page at flash address " + String(flashAddress);
      //Dump both sides once, to tell a stale read (old firmware) from real write corruption.
      if (attempt == 0) {
        int diffCount = 0;
        int firstDiffIndex = -1;
        for (int i = 0; i < pageSize; i++) {
          if (writeBuffer[i] != verifyBuffer[i]) {
            diffCount++;
            if (firstDiffIndex == -1) firstDiffIndex = i;
          }
        }
        SerialPrintln(F("Mismatch detail: ") + String(diffCount) + F(" of ") + String(pageSize) +
          F(" bytes differ, first difference at offset ") + String(firstDiffIndex));
        String writeHex = "";
        String verifyHex = "";
        for (int i = 0; i < pageSize; i++) {
          if (writeBuffer[i] < 0x10) writeHex += "0";
          writeHex += String(writeBuffer[i], HEX) + " ";
          if (verifyBuffer[i] < 0x10) verifyHex += "0";
          verifyHex += String(verifyBuffer[i], HEX) + " ";
        }
        SerialPrintln(F("Intended (write) bytes: ") + writeHex);
        SerialPrintln(F("Actual (read-back) bytes: ") + verifyHex);
      }
      continue;
    }
    return true;
  }
  return false;
}

bool pushFirmwareToUnit(int unitIndex, String &resultJsonOut) {
  JsonDocument result;
  result["unitIndex"] = unitIndex;

  File firmwareFile = LittleFS.open(unitFirmwarePath, "r");
  if (!firmwareFile) {
    result["success"] = false;
    result["error"] = F("Could not open stored firmware file. Upload a .bin first.");
    serializeJson(result, resultJsonOut);
    return false;
  }

  size_t firmwareSize = firmwareFile.size();
  if (firmwareSize == 0) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Stored firmware file is empty.");
    serializeJson(result, resultJsonOut);
    return false;
  }

  //From 42 units upward unitIndex's bootloader address equals another unit's app address - a real
  //bus conflict. Errs on the safe side: may reject a fine push, never allows a collision.
  int expectedBootAddressForCollisionCheck = UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex;
  if (expectedBootAddressForCollisionCheck < unitsAmount) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Refusing to update Unit ") + String(unitIndex) + F(": its bootloader address (0x") +
      String(expectedBootAddressForCollisionCheck, HEX) + F(") would collide with another unit's live app address at "
        "this installation's current size. Nothing was written.");
    serializeJson(result, resultJsonOut);
    return false;
  }
  //Fixed collision with the RTC (isReservedI2CAddress()), independent of unitsAmount.
  if (isReservedI2CAddress(expectedBootAddressForCollisionCheck)) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Refusing to update Unit ") + String(unitIndex) + F(": its bootloader address (0x") +
      String(expectedBootAddressForCollisionCheck, HEX) + F(") is permanently reserved for the RTC module. Nothing was written.");
    serializeJson(result, resultJsonOut);
    return false;
  }

  SerialPrintln(F("Requesting Unit ") + String(unitIndex) + F(" to enter bootloader mode..."));
  unsigned long bootloaderSearchStart = millis();
  uint8_t enterBootloaderResult = requestUnitEnterBootloader(unitIndex);
  SerialPrintln(F("  requestUnitEnterBootloader() Wire.endTransmission() = ") + String(enterBootloaderResult) + F(" (0 = success)"));

  //Wait for the watchdog reset and twiboot startup, then probe a few times - this races twiboot's
  //~1000ms window, so the timing is logged. Probes only unitIndex's own address.
  TwibootChipInfo chipInfo;
  int expectedBootAddress = UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex;
  int bootAddress = findExpectedBootloaderAddress(unitIndex, chipInfo, bootloaderSearchStart) ? expectedBootAddress : -1;

  if (bootAddress < 0) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Unit did not respond in bootloader mode. Nothing was written - old firmware should still be running normally.");
    serializeJson(result, resultJsonOut);
    return false;
  }

  SerialPrintln(F("Unit ") + String(unitIndex) + F(" found in bootloader mode at I2C address 0x") + String(bootAddress, HEX));
  result["bootAddress"] = "0x" + String(bootAddress, HEX);
  twibootHeartbeat(bootAddress);

  //Whether the update marker was clear before this push - only then may an early abort disarm it
  //again and send the unit to its app (see returnUnitToAppAfterAbort()).
  uint8_t markerBefore = UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS;
  bool markerFreeBefore = twibootReadEepromByte(bootAddress, UNIT_BOOTLOADER_UPDATE_MARKER_ADDR, &markerBefore) &&
    markerBefore != UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS;

  uint8_t pageSize = chipInfo.pageSize;
  uint16_t maxFlashAddress = chipInfo.bootloaderStart;

  SerialPrintln(F("Unit ") + String(unitIndex) + F(" bootloader responded. Page size: ") + String(pageSize) +
    F(", max flash address: ") + String(maxFlashAddress));

  //pageSize is a uint8_t, so it always fits UNIT_BOOTLOADER_MAX_PAGE_SIZE (256).
  if (pageSize == 0 || firmwareSize > maxFlashAddress) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "Firmware image (" + String(firmwareSize) + " bytes) doesn't fit, or the unit reported an implausible page size (" +
      String(pageSize) + "). Nothing was written." + returnUnitToAppAfterAbort(bootAddress, markerFreeBefore, false);
    serializeJson(result, resultJsonOut);
    return false;
  }

  //Guards against the marker address falling out of sync with twiboot or the MCU's EEPROM size
  //(eepromSize comes from the unit itself).
  if (UNIT_BOOTLOADER_UPDATE_MARKER_ADDR >= chipInfo.eepromSize) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = String(F("Update-marker address doesn't fit this unit's reported EEPROM size - refusing to proceed without the safety net. Nothing was written.")) +
      returnUnitToAppAfterAbort(bootAddress, markerFreeBefore, false);
    serializeJson(result, resultJsonOut);
    return false;
  }

  //Only this fork's twiboot (SF-TWIBOOT, v3.5 and later) keeps the unit in the bootloader while the
  //marker is armed. An older build would accept the marker write but boot a half-written image after
  //an aborted update, so it gets nothing written.
  char bootloaderVersion[17];
  if (!twibootReadVersion(bootAddress, bootloaderVersion, sizeof(bootloaderVersion)) || strncmp(bootloaderVersion, "SF-TWIBOOT", 10) != 0) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = String(F("The unit's bootloader doesn't report SF-TWIBOOT (v3.5 or later), so an aborted update could start a "
      "half-written firmware. Flash the current twiboot over ISP first (see Unit/BOOTLOADER_SETUP.md). Nothing was written.")) +
      returnUnitToAppAfterAbort(bootAddress, markerFreeBefore, false);
    serializeJson(result, resultJsonOut);
    return false;
  }
  result["bootloaderVersion"] = String(bootloaderVersion);

  //Arm the "in progress" marker before touching flash; if it can't be confirmed, write nothing.
  String markerFailureReason;
  if (!twibootWriteAndVerifyEepromByte(bootAddress, UNIT_BOOTLOADER_UPDATE_MARKER_ADDR, UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS, markerFailureReason)) {
    firmwareFile.close();
    result["success"] = false;
    //The marker write may have landed even though its read-back failed - disarm it again.
    result["error"] = "Could not arm the update-in-progress marker (" + markerFailureReason + "). No flash page was written." +
      returnUnitToAppAfterAbort(bootAddress, markerFreeBefore, true);
    serializeJson(result, resultJsonOut);
    return false;
  }

  int pageCount = (firmwareSize + pageSize - 1) / pageSize;
  int pagesWritten = 0;
  bool allOk = true;
  String failureReason = "";

  uint8_t writeBuffer[UNIT_BOOTLOADER_MAX_PAGE_SIZE];
  uint8_t verifyBuffer[UNIT_BOOTLOADER_MAX_PAGE_SIZE];

  //Write every page except page 0 first
  for (int page = 1; page < pageCount && allOk; page++) {
    uint16_t flashAddress = page * pageSize;

    //A failed seek() would make read() continue from the old cursor - wrong bytes that still verify.
    if (!firmwareFile.seek(flashAddress)) {
      allOk = false;
      failureReason = "Failed to seek to offset " + String(flashAddress) + " in the stored firmware file (page " + String(page) + ")";
      break;
    }
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    //Only the last page may come back short; padding a short middle page would boot a corrupt image.
    size_t expectedBytesThisPage = (size_t)pageSize < (firmwareSize - flashAddress) ? (size_t)pageSize : (firmwareSize - flashAddress);
    if (bytesRead != expectedBytesThisPage) {
      allOk = false;
      failureReason = "Read " + String(bytesRead) + " of " + String(expectedBytesThisPage) +
        " expected bytes from the stored firmware file at offset " + String(flashAddress) + " (page " + String(page) +
        ") - the update-in-progress marker is still armed, so the unit will stay in the bootloader.";
      break;
    }
    for (size_t i = bytesRead; i < pageSize; i++) {
      writeBuffer[i] = 0xFF; //pad the final, partial page (only reachable here) with the erased-flash value
    }

    if (!writeAndVerifyPage(bootAddress, flashAddress, writeBuffer, verifyBuffer, pageSize, failureReason)) {
      allOk = false;
      failureReason += " (page " + String(page) + " of " + String(pageCount - 1) +
        " - the update-in-progress marker is still armed, so the unit will stay in the bootloader instead of trying to run this partial update.)";
    } else {
      pagesWritten++;
    }

    yield();
  }

  //Page 0 (vector table) last, right before the marker is cleared.
  if (allOk && !firmwareFile.seek(0)) {
    allOk = false;
    failureReason = "Failed to seek to offset 0 in the stored firmware file (page 0)";
  }
  if (allOk) {
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    //Same "only the actual last page may come back short" reasoning as the loop above.
    size_t expectedBytesThisPage = (size_t)pageSize < firmwareSize ? (size_t)pageSize : firmwareSize;
    if (bytesRead != expectedBytesThisPage) {
      allOk = false;
      failureReason = "Read " + String(bytesRead) + " of " + String(expectedBytesThisPage) +
        " expected bytes from the stored firmware file at offset 0 (page 0) - the update-in-progress marker is still armed, "
        "so the unit will stay in the bootloader.";
    }
    for (size_t i = bytesRead; allOk && i < pageSize; i++) {
      writeBuffer[i] = 0xFF;
    }

    if (allOk) {
      if (!writeAndVerifyPage(bootAddress, 0, writeBuffer, verifyBuffer, pageSize, failureReason)) {
        allOk = false;
        failureReason += " (page 0, vector table - the update-in-progress marker is still armed, so the unit will stay in the bootloader.)";
      } else {
        pagesWritten++;
      }
    }
  }

  //All pages verified - clear the marker so twiboot boots the app. If that write fails, the firmware
  //is fine, but stay in the bootloader rather than start the app.
  if (allOk) {
    String markerClearFailureReason;
    if (!twibootWriteAndVerifyEepromByte(bootAddress, UNIT_BOOTLOADER_UPDATE_MARKER_ADDR, UNIT_BOOTLOADER_UPDATE_MARKER_COMPLETE, markerClearFailureReason)) {
      allOk = false;
      failureReason = "Firmware written and verified successfully, but clearing the update-in-progress marker failed (" + markerClearFailureReason +
        "). The unit will stay in the bootloader - a retry should be fast, since every page will already verify-match.";
    }
  }

  firmwareFile.close();

  result["pageSize"] = pageSize;
  result["totalPages"] = pageCount;
  result["pagesWritten"] = pagesWritten;

  if (allOk) {
    bool started = twibootStartApplication(bootAddress);
    result["success"] = true;
    result["started"] = started;
    result["message"] = started
      ? "Firmware written and verified. Unit is starting the new application now."
      : "Firmware written and verified, but the unit didn't confirm the start-application request. "
        "It will boot into the new firmware automatically within about a second regardless.";
    SerialPrintln(F("Unit ") + String(unitIndex) + F(" firmware update successful (") + String(pagesWritten) + F(" pages)."));
  } else {
    result["success"] = false;
    result["error"] = failureReason;
    SerialPrintln(F("Unit ") + String(unitIndex) + F(" firmware update FAILED: ") + failureReason);
  }

  serializeJson(result, resultJsonOut);
  return allOk;
}

#endif /* UNIT_BOOTLOADER_ENABLE */
