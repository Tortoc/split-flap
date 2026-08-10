//twiboot I2C client + push orchestration for updating Unit.ino firmware over I2C.
//Only used when UNIT_BOOTLOADER_ENABLE is true - see Unit/BOOTLOADER_SETUP.md for the
//one-time, physical setup this depends on (every unit must already be running twiboot).
//
//Protocol verified directly against https://github.com/orempel/twiboot main.c (2026-08-09):
//  - abort boot timeout (heartbeat): SLA+W, 0x00, STOP
//  - start application:              SLA+W, 0x01, 0x80, STOP
//  - read chip info (8 bytes):       SLA+W, 0x02, 0x00, 0x00, 0x00, SLA+R, {8 bytes}, STOP
//  - read flash:                     SLA+W, 0x02, 0x01, addrH, addrL, SLA+R, {N bytes}, STOP
//  - write one flash page:           SLA+W, 0x02, 0x01, addrH, addrL, {pageSize bytes}, STOP
//    (read vs write is NOT a different opcode - only whether a repeated-start read follows,
//    or exactly pageSize data bytes are written directly. A page only commits on STOP if
//    exactly pageSize data bytes were sent - fewer bytes are silently ignored.)
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
  Wire.beginTransmission(unitIndex);
  Wire.write(UNIT_BOOTLOADER_CMD);
  Wire.write((uint8_t)0); //speed byte - unused for this command, but the unit always reads 2 bytes
  Wire.endTransmission();
}

//Scans UNIT_BOOTLOADER_BASE_ADDRESS..+15 for a unit currently sitting in twiboot.
//Our twiboot (Unit/bootloader/twiboot/main.c - a patched fork, see BOOTLOADER_SETUP.md) reads
//the unit's own DIP switches at boot and adds them to TWI_ADDRESS_BASE, same as the app does -
//so in normal operation bootAddress == UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex already. Scanning
//instead of assuming that directly is defense in depth (e.g. a shared power-cycle could
//momentarily put more than one unit in bootloader mode at once) - it only ever proceeds if
//exactly one unit answers as a bootloader right now.
//Returns the found address, or a negative sentinel: -1 = none found, -2 = more than one found.
int scanForBootloaderAddress(TwibootChipInfo &infoOut) {
  int foundAddress = -1;
  int foundCount = 0;

  for (int address = UNIT_BOOTLOADER_BASE_ADDRESS; address < UNIT_BOOTLOADER_BASE_ADDRESS + 16; address++) {
    TwibootChipInfo candidateInfo;
    if (twibootReadChipInfo(address, candidateInfo)) {
      foundCount++;
      foundAddress = address;
      infoOut = candidateInfo;
    }
    yield();
  }

  if (foundCount == 0) {
    return -1;
  }
  if (foundCount > 1) {
    return -2;
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

//Reads chip signature, flash page size and bootloader start address (= max writable flash address)
bool twibootReadChipInfo(int bootAddress, TwibootChipInfo &infoOut) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_CHIPINFO);
  Wire.write((uint8_t)0x00); //address bytes - unused for chipinfo, but the protocol always expects 2
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) { //repeated START, not STOP - a read follows
    return false;
  }

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

//Reads `len` bytes of flash starting at `flashAddress` - used for the verify pass after every write
bool twibootReadFlash(int bootAddress, uint16_t flashAddress, uint8_t* bufferOut, uint8_t len) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)(flashAddress >> 8));
  Wire.write((uint8_t)(flashAddress & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(bootAddress, (int)len) != len) {
    return false;
  }

  for (uint8_t i = 0; i < len; i++) {
    bufferOut[i] = Wire.read();
  }
  return true;
}

//Writes one full flash page. `data` must be exactly `pageSize` bytes (pad the source image's
//final, partial page with 0xFF - the erased-flash value) - twiboot only commits the write once
//it has received exactly pageSize data bytes, a short write is silently ignored, not an error.
bool twibootWriteFlashPage(int bootAddress, uint16_t flashAddress, const uint8_t* data, uint8_t pageSize) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write(TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)(flashAddress >> 8));
  Wire.write((uint8_t)(flashAddress & 0xFF));
  for (uint8_t i = 0; i < pageSize; i++) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0; //STOP triggers the actual erase+write
}

//Jumps into the freshly written application - only ever call this after every page (including
//page 0) has been written AND read back and verified.
bool twibootStartApplication(int bootAddress) {
  Wire.beginTransmission(bootAddress);
  Wire.write(TWIBOOT_CMD_SWITCH_APP);
  Wire.write(TWIBOOT_BOOTTYPE_APPLICATION);
  return Wire.endTransmission() == 0;
}

//Pushes the firmware currently stored at unitFirmwarePath to the given unit over I2C.
//
//Safety property this relies on: every page except page 0 (the interrupt vector table) is
//written and verified FIRST. Page 0 - the point where a partially-updated app would become
//unbootable - is only ever written once everything else has already been verified byte-for-byte.
//So a failed/interrupted push always leaves the unit in one of two safe states: either the old,
//fully-intact application (page 0 untouched), or the fully verified new one. It can never leave
//a half-written app straddling both. If it stops responding as an app at all, it's not bricked -
//the bootloader itself is never touched by this protocol (twiboot refuses to write anything at
//or above its own start address) and a fresh push attempt on next reset can recover it.
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

  SerialPrintln("Requesting Unit " + String(unitIndex) + " to enter bootloader mode...");
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

  SerialPrintln("Unit " + String(unitIndex) + " found in bootloader mode at I2C address 0x" + String(bootAddress, HEX));
  result["bootAddress"] = "0x" + String(bootAddress, HEX);
  twibootHeartbeat(bootAddress);

  uint8_t pageSize = chipInfo.pageSize;
  uint16_t maxFlashAddress = chipInfo.bootloaderStart;

  SerialPrintln("Unit " + String(unitIndex) + " bootloader responded. Page size: " + String(pageSize) +
    ", max flash address: " + String(maxFlashAddress));

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

    firmwareFile.seek(flashAddress);
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    for (size_t i = bytesRead; i < pageSize; i++) {
      writeBuffer[i] = 0xFF; //pad the final, partial page with the erased-flash value
    }

    if (!twibootWriteFlashPage(bootAddress, flashAddress, writeBuffer, pageSize)) {
      allOk = false;
      failureReason = "I2C write failed on page " + String(page) + " of " + String(pageCount - 1);
    } else if (!twibootReadFlash(bootAddress, flashAddress, verifyBuffer, pageSize)) {
      allOk = false;
      failureReason = "I2C verify-read failed on page " + String(page) + " of " + String(pageCount - 1);
    } else if (memcmp(writeBuffer, verifyBuffer, pageSize) != 0) {
      allOk = false;
      failureReason = "Verification mismatch on page " + String(page) + " of " + String(pageCount - 1) +
        " - page 0 was never touched, the old firmware's vector table is intact.";
    } else {
      pagesWritten++;
    }

    yield();
  }

  //Only now write page 0 (the vector table) - this is the point of no return for the old firmware
  if (allOk) {
    firmwareFile.seek(0);
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    for (size_t i = bytesRead; i < pageSize; i++) {
      writeBuffer[i] = 0xFF;
    }

    if (!twibootWriteFlashPage(bootAddress, 0, writeBuffer, pageSize)) {
      allOk = false;
      failureReason = "I2C write failed on page 0 (vector table)";
    } else if (!twibootReadFlash(bootAddress, 0, verifyBuffer, pageSize)) {
      allOk = false;
      failureReason = "I2C verify-read failed on page 0 (vector table)";
    } else if (memcmp(writeBuffer, verifyBuffer, pageSize) != 0) {
      allOk = false;
      failureReason = "Verification mismatch on page 0 (vector table)";
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
    SerialPrintln("Unit " + String(unitIndex) + " firmware update successful (" + String(pagesWritten) + " pages).");
  } else {
    result["success"] = false;
    result["error"] = failureReason;
    SerialPrintln("Unit " + String(unitIndex) + " firmware update FAILED: " + failureReason);
  }

  serializeJson(result, resultJsonOut);
  return allOk;
}

#endif /* UNIT_BOOTLOADER_ENABLE */
