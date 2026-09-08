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
#if UNIT_BOOTLOADER_ENABLE == true

#include <string.h>

#define TWIBOOT_CMD_WAIT              0x00
#define TWIBOOT_CMD_SWITCH_APP        0x01
#define TWIBOOT_CMD_ACCESS_MEMORY     0x02
#define TWIBOOT_BOOTTYPE_APPLICATION  0x80
#define TWIBOOT_MEMTYPE_CHIPINFO      0x00
#define TWIBOOT_MEMTYPE_FLASH         0x01
#define UNIT_BOOTLOADER_MAX_PAGE_SIZE 256 //comfortably covers the ATmega328p's actual 128-byte page size

//TwibootChipInfo struct is defined in Classes.h (must be a real header, see comment there)

//Tells the unit's running app (not the bootloader) to reboot into twiboot - see Unit.ino's
//receiveLetter()/CMD_ENTER_BOOTLOADER handling. Sent to the unit's normal app address.
void requestUnitEnterBootloader(int unitIndex) {
  //Checksum byte appended, same as every other write path - this command reboots the unit, so
  //it's important it isn't triggered by a spurious/misread byte sequence.
  uint8_t sendArray[2] = {UNIT_BOOTLOADER_CMD, 0}; //speed byte - unused for this command, but the unit always reads 2 bytes
  Wire.beginTransmission(unitIndex);
  Wire.write(sendArray[0]);
  Wire.write(sendArray[1]);
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  Wire.endTransmission();
}

//Scans UNIT_BOOTLOADER_BASE_ADDRESS..+63 for a unit sitting in twiboot, rather than assuming
//bootAddress == base+unitIndex - guards against more than one unit being in bootloader mode at
//once. Returns the found address, or -1 (none found) / -2 (more than one found).
//
//Two passes, not one repeated-start read per address: a repeated-start read against an address
//with nothing listening leaves the ESP8266's software I2C state stuck (esp8266/Arduino #5718). So:
//a cheap presence check against every address first, then one real chip-info read.
int scanForBootloaderAddress(TwibootChipInfo &infoOut) {
  int foundAddress = -1;
  int foundCount = 0;

  for (int address = UNIT_BOOTLOADER_BASE_ADDRESS; address < UNIT_BOOTLOADER_BASE_ADDRESS + 64; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      foundCount++;
      foundAddress = address;
    }
    yield();
  }

  if (foundCount == 0) {
    return -1;
  }
  if (foundCount > 1) {
    return -2;
  }

  if (!twibootReadChipInfo(foundAddress, infoOut)) {
    //Answered the cheap presence check a moment ago but not the real chip-info read now -
    //treat as not-found rather than returning a stale/empty infoOut.
    return -1;
  }
  return foundAddress;
}

//"Abort boot timeout" heartbeat - also resets twiboot's 1000ms auto-boot countdown.
//In practice every read/write call below already resets it as a side effect, this is just
//a cheap defensive extra between steps that might otherwise leave a longer gap.
bool twibootHeartbeat(int bootAddress) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_WAIT);
  return Wire.endTransmission() == 0;
}

//Reads chip signature, flash page size and bootloader start address (= max writable flash address).
//Uses a real STOP + short settle delay, not a repeated START - a repeated START here triggers an
//ESP8266 core bug (esp8266/Arduino #5718); twiboot doesn't reset its state on STOP either way.
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

//Reads `len` bytes of flash starting at `flashAddress` - used for the verify pass after every write.
//Same repeated-start -> STOP change as twibootReadChipInfo() above.
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

//Writes one full flash page. `data` must be exactly `pageSize` bytes (pad the final partial page
//with 0xFF) - a short write is silently ignored. Uses twi_writeTo() (raw I2C), not Wire - this
//transaction is 132 bytes, over Wire's 128-byte TX buffer, which would silently drop the tail.
bool twibootWriteFlashPage(int bootAddress, uint16_t flashAddress, const uint8_t* data, uint8_t pageSize) {
  uint8_t buf[4 + UNIT_BOOTLOADER_MAX_PAGE_SIZE];
  buf[0] = TWIBOOT_CMD_ACCESS_MEMORY;
  buf[1] = TWIBOOT_MEMTYPE_FLASH;
  buf[2] = (uint8_t)(flashAddress >> 8);
  buf[3] = (uint8_t)(flashAddress & 0xFF);
  memcpy(buf + 4, data, pageSize);
  return twi_writeTo(bootAddress, buf, 4 + pageSize, 1) == 0; //sendStop=1, STOP triggers the actual erase+write
}

//Jumps into the freshly written application - only call after every page (including page 0) has
//been written AND verified. twiboot deliberately NACKs this command's data byte (its internal
//state already updated before the NACK) - that's the expected result, not a failure.
bool twibootStartApplication(int bootAddress) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_SWITCH_APP);
  Wire.write(TWIBOOT_BOOTTYPE_APPLICATION);
  uint8_t result = Wire.endTransmission();
  SerialPrintln(F("DEBUG: twibootStartApplication() Wire.endTransmission() = ") + String(result));
  //A NACK on the data byte (3) is twiboot's documented, expected behavior for this specific
  //command - treat it as success. Anything else (address NACK, timeout, etc.) is a real failure.
  return result == 0 || result == 3;
}

//Pushes the firmware currently stored at unitFirmwarePath to the given unit over I2C.
//
//Safety: every page except page 0 (the vector table) is written and verified FIRST - page 0 only
//after everything else is confirmed byte-for-byte, so a failed push never leaves a half-written
//app. The bootloader itself is never touched, so a stuck unit isn't bricked.
//
//writeAndVerifyPage() retries with a bus-recovery pass in between - a repeated-start read can
//occasionally corrupt just the read-back (not the actual write), and a retry clears that.
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
      //Dump the actual bytes on both sides (once, not every retry) - lets a stale-read mismatch
      //(read-back matches old firmware) be told apart from a real write corruption (scrambled/
      //random) without a logic analyzer.
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
    result["error"] = "Could not open stored firmware file. Upload a .bin first.";
    serializeJson(result, resultJsonOut);
    return false;
  }

  size_t firmwareSize = firmwareFile.size();
  if (firmwareSize == 0) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "Stored firmware file is empty.";
    serializeJson(result, resultJsonOut);
    return false;
  }

  SerialPrintln(F("Requesting Unit ") + String(unitIndex) + F(" to enter bootloader mode..."));
  requestUnitEnterBootloader(unitIndex);

  //Give the watchdog reset + twiboot startup time to happen, then retry a few times in case
  //the exact timing needs more slack than expected - tune on the bench per BOOTLOADER_SETUP.md.
  TwibootChipInfo chipInfo;
  int bootAddress = -1;
  for (int attempt = 0; attempt < 5 && bootAddress == -1; attempt++) {
    delay(attempt == 0 ? 300 : 150);
    yield();
    bootAddress = scanForBootloaderAddress(chipInfo);
  }

  if (bootAddress == -2) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "More than one unit is answering as a bootloader right now - refusing to guess which one is the target. "
      "Nothing was written. Make sure only this one update was triggered and no other unit was power-cycled at the same time, then retry.";
    serializeJson(result, resultJsonOut);
    return false;
  }

  if (bootAddress < 0) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "Unit did not respond in bootloader mode. Nothing was written - old firmware should still be running normally.";
    serializeJson(result, resultJsonOut);
    return false;
  }

  SerialPrintln(F("Unit ") + String(unitIndex) + F(" found in bootloader mode at I2C address 0x") + String(bootAddress, HEX));
  result["bootAddress"] = "0x" + String(bootAddress, HEX);
  twibootHeartbeat(bootAddress);

  uint8_t pageSize = chipInfo.pageSize;
  uint16_t maxFlashAddress = chipInfo.bootloaderStart;

  SerialPrintln(F("Unit ") + String(unitIndex) + F(" bootloader responded. Page size: ") + String(pageSize) +
    F(", max flash address: ") + String(maxFlashAddress));

  //pageSize is a uint8_t (max 255), always comfortably within UNIT_BOOTLOADER_MAX_PAGE_SIZE (256)
  //by construction - no separate upper-bound check needed there.
  if (pageSize == 0 || firmwareSize > maxFlashAddress) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "Firmware image (" + String(firmwareSize) + " bytes) doesn't fit, or the unit reported an implausible page size (" +
      String(pageSize) + "). Nothing was written.";
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

    //Check seek()'s return value - if it fails, read() would silently continue from the current
    //file cursor instead of flashAddress, writing wrong bytes while still verifying clean
    //(verify-read would just match the same wrong data). Bail out instead of writing bad data.
    if (!firmwareFile.seek(flashAddress)) {
      allOk = false;
      failureReason = "Failed to seek to offset " + String(flashAddress) + " in the stored firmware file (page " + String(page) + ")";
      break;
    }
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    for (size_t i = bytesRead; i < pageSize; i++) {
      writeBuffer[i] = 0xFF; //pad the final, partial page with the erased-flash value
    }

    if (!writeAndVerifyPage(bootAddress, flashAddress, writeBuffer, verifyBuffer, pageSize, failureReason)) {
      allOk = false;
      failureReason += " (page " + String(page) + " of " + String(pageCount - 1) +
        " - page 0 was never touched, the old firmware's vector table is intact.)";
    } else {
      pagesWritten++;
    }

    yield();
  }

  //Only now write page 0 (the vector table) - this is the point of no return for the old firmware
  if (allOk && !firmwareFile.seek(0)) {
    allOk = false;
    failureReason = "Failed to seek to offset 0 in the stored firmware file (page 0)";
  }
  if (allOk) {
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    for (size_t i = bytesRead; i < pageSize; i++) {
      writeBuffer[i] = 0xFF;
    }

    if (!writeAndVerifyPage(bootAddress, 0, writeBuffer, verifyBuffer, pageSize, failureReason)) {
      allOk = false;
      failureReason += " (page 0, vector table)";
    } else {
      pagesWritten++;
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
