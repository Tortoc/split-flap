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
#define UNIT_BOOTLOADER_MAX_PAGE_SIZE 256 //comfortably covers the ATmega328p's actual 128-byte page size

//"Update complete" gate the bootloader checks before ever booting into the app - see Change 2 in
//Unit/bootloader/twiboot/main.c's file header comment. Must exactly match that file's
//EEPROM_UPDATE_MARKER_ADDR (E2END, i.e. the last EEPROM byte on whatever MCU twiboot is built
//for - 1023 on the atmega328p this project uses) and EEPROM_UPDATE_MARKER_INPROGRESS. Also the
//opposite end of the EEPROM from Unit.ino's own calOffset (address 0-1) - kept deliberately far
//apart. pushFirmwareToUnit() double-checks this against the unit's own reported EEPROM size
//before trusting it, in case a future MCU port ever changes it.
#define UNIT_BOOTLOADER_UPDATE_MARKER_ADDR         1023
#define UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS   0x00
#define UNIT_BOOTLOADER_UPDATE_MARKER_COMPLETE     0xFF

//Expected AVR chip signature for the ATmega328p this project targets - used by
//scanForBootloaderAddress() to tell a genuine twiboot reply apart from a live Unit.ino app's
//reply to the same read (see the comment there; found+verified live 2026-09-09, CODE_REVIEW.md
//#7). If a future hardware revision changes the MCU, this needs updating alongside it.
#define UNIT_EXPECTED_CHIP_SIGNATURE_0 0x1E
#define UNIT_EXPECTED_CHIP_SIGNATURE_1 0x95
#define UNIT_EXPECTED_CHIP_SIGNATURE_2 0x0F

//TwibootChipInfo struct is defined in Classes.h (must be a real header, see comment there)

//Tells the unit's running app (not the bootloader) to reboot into twiboot - see Unit.ino's
//receiveLetter()/CMD_ENTER_BOOTLOADER handling. Sent to the unit's normal app address.
//Returns Wire.endTransmission()'s result (0 = success) - previously discarded entirely, added
//2026-09-09 while diagnosing repeated "unit did not respond in bootloader mode" failures: if this
//write itself silently fails, the whole retry loop that follows is racing a bootloader window
//that was never opened in the first place, and the old code had no way to tell the two apart.
uint8_t requestUnitEnterBootloader(int unitIndex) {
  //Checksum byte appended, same as every other write path - this command reboots the unit, so
  //it's important it isn't triggered by a spurious/misread byte sequence.
  uint8_t sendArray[2] = {UNIT_BOOTLOADER_CMD, 0}; //speed byte - unused for this command, but the unit always reads 2 bytes
  Wire.beginTransmission(unitIndex);
  Wire.write(sendArray[0]);
  Wire.write(sendArray[1]);
  Wire.write(i2cChecksum(sendArray, sizeof sendArray / sizeof sendArray[0]));
  return Wire.endTransmission();
}

//Scans UNIT_BOOTLOADER_BASE_ADDRESS..+63 for a unit sitting in twiboot, rather than assuming
//bootAddress == base+unitIndex - guards against more than one unit being in bootloader mode at
//once. Returns the found address, or -1 (none found) / -2 (more than one found).
//
//Two passes, not one repeated-start read per address: a repeated-start read against an address
//with nothing listening leaves the ESP8266's software I2C state stuck (esp8266/Arduino #5718). So:
//a cheap presence check against every address first, then a real chip-info read for every
//candidate that ACKs it.
//
//Bug found 2026-09-09: a bare address-only probe (no data bytes) never reaches twiboot's
//TWI_data_write() (see main.c's TWI_vect(), case 0x60/0xA0) - only a real data-carrying
//transaction (like twibootReadChipInfo() below) resets its boot_timeout. So this whole 64-address
//sweep races the SAME un-extended ~1000ms window from the original reset, and yield()ing after
//EVERY address (64 calls) turned out to no longer reliably fit in that window now that yield()
//has more WiFi/AsyncWebServer background work to do than when this was last verified working
//(2026-08-15) - live-reproduced as 3 consecutive "unit did not respond in bootloader mode"
//failures against a real unit. Calling yield() only every 8th address cuts that overhead ~8x
//while still keeping the WiFi stack reasonably serviced.
//
//Second bug found 2026-09-09 (CODE_REVIEW.md #7): the bootloader address range (base 0x29=41,
//so 41-104) and the application address range (0-63) structurally overlap from 41 units upward -
//a running app at address 41+ ACKs the bare presence check below just as readily as a real
//bootloader does, and the old code counted every such ACK as a hit. At 42+ installed units this
//permanently wedges every scan into the foundCount>1 "ambiguous" rejection, since the genuine
//bootloader candidate is never alone. Not reachable on this project's current 10-unit install
//(app addresses 0-9 don't reach into 41-104 yet), but a real, structural bug for any future
//scale-up - fixed now rather than left for whoever hits it. A bare ACK only proves *something*
//is listening, not that it's actually twiboot.
//
//A real chip-info read alone does NOT reliably distinguish them, contrary to the first
//assumption here - live-tested 2026-09-09 against real running app addresses and
//twibootReadChipInfo() returned SUCCESS every time, just with garbage content: the chip-info
//write's 4 bytes fail the app's own checksum validation harmlessly (see receiveLetter()), but the
//following 8-byte read then hits the app's requestEvent(), which always replies with its own,
//different fixed 6-byte status packet (currentlyrotating, calOffset lo/hi, currentPhase,
//currentPhaseProgress lo/hi) - the ESP8266 Wire master library happily accepts this as a
//"successful" 8-byte transaction, silently padding the missing 2 bytes. The result lands in
//TwibootChipInfo's fields at their normal byte offsets, so it's fully deterministic garbage, not
//random: signature[0]=currentlyrotating (always 0 or 1), signature[2]=currentPhase (always
//0/1/2 - PHASE_IDLE/CALIBRATING/ROTATING), pageSize=currentPhaseProgress's low byte (often 0),
//eepromSize=0xFFFF (the 2 phantom padding bytes). Confirmed against 4 different live units'
//real app addresses - signature[1] varied (it's each unit's own calOffset low byte - matched
//exactly), but signature[0]/[2]/pageSize were 0 every time, structurally incapable of matching a
//real AVR signature (currentlyrotating/currentPhase can never reach 0x1E/0x0F). So: verify the
//exact expected signature, not just read success.
#define UNIT_IS_EXPECTED_CHIP_SIGNATURE(info) \
  ((info).signature[0] == UNIT_EXPECTED_CHIP_SIGNATURE_0 && \
   (info).signature[1] == UNIT_EXPECTED_CHIP_SIGNATURE_1 && \
   (info).signature[2] == UNIT_EXPECTED_CHIP_SIGNATURE_2)
int scanForBootloaderAddress(TwibootChipInfo &infoOut) {
  int foundAddress = -1;
  int foundCount = 0;

  for (int address = UNIT_BOOTLOADER_BASE_ADDRESS; address < UNIT_BOOTLOADER_BASE_ADDRESS + 64; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      //Verify every candidate that ACKs, not just the (assumed-unique) survivor - see the second
      //comment above this function. Read success alone isn't enough (a live app "succeeds" too,
      //with garbage) - the signature must actually match.
      TwibootChipInfo candidateInfo;
      if (twibootReadChipInfo(address, candidateInfo) && UNIT_IS_EXPECTED_CHIP_SIGNATURE(candidateInfo)) {
        foundCount++;
        foundAddress = address;
        infoOut = candidateInfo; //avoids a redundant final read for the common single-hit case
      }
    }
    if ((address - UNIT_BOOTLOADER_BASE_ADDRESS) % 8 == 7) {
      yield();
    }
  }

  if (foundCount == 0) {
    return -1;
  }
  if (foundCount > 1) {
    return -2;
  }

  return foundAddress;
}

//Directly probes ONLY the single I2C address expected for unitIndex - does not scan the whole
//41-104 bootloader address range at all, unlike scanForBootloaderAddress() above. Used by both
//pushFirmwareToUnit() and the bootloader-probe endpoint, which both already know exactly which
//unit they're targeting before ever calling this.
//
//Found 2026-09-09, CODE_REVIEW_V2.md #15: both callers used to run the full ambiguity-checking
//scan even though the target unit was already known - if a SECOND, unrelated unit also happened
//to be sitting in the bootloader (e.g. its own earlier aborted update, now safely held there by
//the EEPROM marker), the scan permanently reported "more than one found" and refused recovery of
//EITHER unit, even though the specific address this call actually cares about can be computed
//directly and checked entirely on its own - the marker that makes multi-unit-stuck-in-bootloader
//a real, persistent (not just momentary) possibility in the first place didn't exist yet when the
//original scanning approach was written. This also makes CODE_REVIEW.md #8's later "found address
//matches the expected one" check trivially true by construction instead of a runtime check that
//could still fail - the two fixes now reinforce each other instead of overlapping.
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

//Reads twiboot's own 16-byte version string (e.g. "TWIBOOT v3.2" - upstream's own VERSION_STRING
//constant, unchanged by any of this project's 3 documented fork changes, see main.c's header
//comment). Same real-STOP-instead-of-repeated-START workaround as twibootReadChipInfo() above.
//versionOut must be at least 17 bytes (16 raw bytes + a null terminator this function adds, since
//twiboot's info[] array is a fixed-size byte buffer, not a null-terminated C string, and any
//trailing bytes past the actual text are whatever was in flash at compile time, not necessarily 0).
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

//Reads one EEPROM byte - used to verify the update-marker writes below. Same repeated-start ->
//STOP change as twibootReadChipInfo()/twibootReadFlash() above, same reason.
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

//Writes one EEPROM byte and reads it back to confirm - used for the update-in-progress/complete
//marker (see UNIT_BOOTLOADER_UPDATE_MARKER_ADDR's comment). Always verifies: a silently-dropped
//write of the in-progress marker would defeat the whole point of arming it before touching flash.
//Same retry pattern as writeAndVerifyPage() - a bus hiccup here is exactly as recoverable as one
//during a flash-page write.
bool twibootWriteAndVerifyEepromByte(int bootAddress, uint16_t eepromAddress, uint8_t value, String &failureReasonOut) {
  const int maxAttempts = 3;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      recoverI2CBus();
      delay(20);
    }

    //Single byte, well under Wire's 128-byte TX buffer - no need for twi_writeTo() here, unlike
    //the full-page flash write.
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
//Safety (corrected 2026-09-09 - the original version of this comment was wrong, see
//UNIT_BOOTLOADER_UPDATE_MARKER_ADDR's comment for the full story): every page except page 0 (the
//vector table) is written and verified FIRST, page 0 only after everything else is confirmed
//byte-for-byte - but on the atmega328p this project uses, the reset vector's actual jump TARGET
//already lands in page 1, not page 0, so "page 0 last" alone does NOT make an aborted update
//safe to boot from after a reset. The real safety net is the EEPROM update-marker, armed via
//twibootWriteAndVerifyEepromByte() before any page is touched and cleared only after page 0 is
//confirmed - twiboot itself refuses to boot into the app while the marker says "in progress" (see
//Unit/bootloader/twiboot/main.c's Change 2), regardless of what state the app pages are actually
//in. The bootloader itself is never touched, so a stuck unit isn't bricked - it just stays
//resident until a successful push clears the marker.
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

  //App addresses (0-63) and bootloader addresses (UNIT_BOOTLOADER_BASE_ADDRESS=41..104)
  //structurally overlap from 41 installed units upward - unitIndex's bootloader address
  //(UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex) would then be the SAME physical I2C address as
  //another unit's already-running app, a real electrical bus conflict, not just a software
  //ambiguity (found 2026-09-09, CODE_REVIEW_V2.md #16 - not reachable on this project's current
  //10-unit install, but a real structural hazard for any future scale-up). unitsAmount (the live
  //app-address scan result) is a best-effort proxy for "how many real units exist" - it can
  //undercount while some are already sitting in a bootloader (see BUGFIX_LOG #139, an existing,
  //separate, unrelated limitation), so this check is conservative in the SAFE direction (it can
  //only ever reject a push that's actually fine, never silently allow a real collision).
  int expectedBootAddressForCollisionCheck = UNIT_BOOTLOADER_BASE_ADDRESS + unitIndex;
  if (expectedBootAddressForCollisionCheck < unitsAmount) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Refusing to update Unit ") + String(unitIndex) + F(": its bootloader address (0x") +
      String(expectedBootAddressForCollisionCheck, HEX) + F(") would collide with another unit's live app address at "
        "this installation's current size. Nothing was written - see CODE_REVIEW_V2.md #16.");
    serializeJson(result, resultJsonOut);
    return false;
  }
  //Separate, fixed collision - see isReservedI2CAddress()'s own comment (ESPMaster.ino,
  //Code_REVIEW_V5.md #04). Independent of unitsAmount: reachable even without a real Unit 63
  //installed, since this check above only guards against ANOTHER UNIT's live app address.
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

  //Give the watchdog reset + twiboot startup time to happen, then retry a few times in case
  //the exact timing needs more slack than expected - tune on the bench per BOOTLOADER_SETUP.md.
  //Timed (2026-09-09, see scanForBootloaderAddress()'s comment) - this races a fixed ~1000ms
  //window from the reset, so logging exactly where the time goes matters more here than for a
  //normal, non-time-critical operation. Probes ONLY unitIndex's own expected address, not a full
  //scan of the whole bootloader address range - see findExpectedBootloaderAddress()'s comment
  //(CODE_REVIEW_V2.md #15, also folds in CODE_REVIEW.md #8's "bind to the right unit" check below,
  //now structural instead of a separate runtime comparison).
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

  //Defends against UNIT_BOOTLOADER_UPDATE_MARKER_ADDR (here) and EEPROM_UPDATE_MARKER_ADDR
  //(Unit/bootloader/twiboot/main.c) ever falling out of sync with each other or with a future
  //MCU's actual EEPROM size - chipInfo.eepromSize comes from the unit itself, not a hardcoded
  //assumption.
  if (UNIT_BOOTLOADER_UPDATE_MARKER_ADDR >= chipInfo.eepromSize) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = F("Update-marker address doesn't fit this unit's reported EEPROM size - refusing to proceed without the safety net. Nothing was written.");
    serializeJson(result, resultJsonOut);
    return false;
  }

  //Arm the "update in progress" marker BEFORE touching any flash page - see
  //UNIT_BOOTLOADER_UPDATE_MARKER_ADDR's comment. If this can't be confirmed written, don't take
  //any destructive action at all: the whole point is to never write flash without a proven-armed
  //safety net first.
  String markerFailureReason;
  if (!twibootWriteAndVerifyEepromByte(bootAddress, UNIT_BOOTLOADER_UPDATE_MARKER_ADDR, UNIT_BOOTLOADER_UPDATE_MARKER_INPROGRESS, markerFailureReason)) {
    firmwareFile.close();
    result["success"] = false;
    result["error"] = "Could not arm the update-in-progress marker (" + markerFailureReason + "). Nothing was written - old firmware should still be running normally.";
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
    //Only the LAST page (known from firmwareSize, not just "whatever read() happened to return")
    //is allowed to come back short - anything else is a genuine read error (found 2026-09-09,
    //CODE_REVIEW_V2.md #17): the old code padded ANY short read with 0xFF unconditionally, so a
    //filesystem read failure on a supposedly-FULL page would silently become a page mostly filled
    //with 0xFF instead of the real firmware bytes - and the very next verify-read would then
    //"successfully" match that same wrong content, since it's comparing against the same corrupted
    //writeBuffer, not the actual intended firmware. The marker would then get cleared and the unit
    //would boot into this corrupted image.
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

  //Only now write page 0 (the vector table) - not "the point of no return" anymore (see
  //UNIT_BOOTLOADER_UPDATE_MARKER_ADDR's comment), just the last page written before the marker
  //itself gets cleared below.
  if (allOk && !firmwareFile.seek(0)) {
    allOk = false;
    failureReason = "Failed to seek to offset 0 in the stored firmware file (page 0)";
  }
  if (allOk) {
    size_t bytesRead = firmwareFile.read(writeBuffer, pageSize);
    //Same "only the actual last page may come back short" reasoning as the loop above
    //(CODE_REVIEW_V2.md #17) - page 0 is the last page written when firmwareSize <= pageSize
    //(a one-page image), otherwise it must always come back full.
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

  //Every page (including 0) is now written and verified - clear the marker so twiboot will
  //actually boot into it. If THIS specific write fails, the firmware itself is fine, but don't
  //call twibootStartApplication() below regardless - leave the unit safely in the bootloader
  //rather than guess.
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
