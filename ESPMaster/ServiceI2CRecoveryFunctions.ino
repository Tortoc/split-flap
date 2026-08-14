// I2C bus recovery (added 2026-08-14) - a creative, targeted alternative to a full custom I2C
// reimplementation for this project's long-standing "Wire.requestFrom() can hang/fail" issue
// (see BUGFIX_LOG.md's Offene Punkte, and entry #45 for the full research writeup).
//
// Background: this project has repeatedly observed a "writes work, reads hang/fail" pattern -
// the classic signature of a slave that stops properly driving SDA during the SLA+R phase of a
// read (e.g. after an internal glitch/reset leaves its TWI hardware mid-transaction). Per the
// I2C specification (NXP AN10216) and confirmed against production implementations in ESPHome
// and ESPEasy (both researched live, see BUGFIX_LOG.md #45 for links): if SDA is stuck low, the
// master can usually free it by manually clocking SCL up to 9 times, then generating a manual
// STOP condition. Checked the installed ESP8266 core directly (core_esp8266_si2c.cpp) and
// confirmed this is NOT something Wire.begin()/Twi::init() already does - it only sets pin
// modes, no recovery logic - so this genuinely adds a missing capability rather than duplicating
// one.
//
// This does NOT replace the Wire library - it's a small, separate helper called around it: at
// the moment a read fails (see readUnitInfo() in ServiceFlapFunctions.ino) and, as a stronger
// escalation, forcing a misbehaving Unit to reboot itself via the *write* path (already known
// reliable) when a wait genuinely gives up - see the requestUnitEnterBootloader() call in
// waitForUnitReady().

// Attempts to un-stick a hung I2C bus by manually clocking SCL (releases a slave that's stuck
// holding SDA low) then generating a STOP condition. Cheap (a few ms at most) and safe to call
// speculatively - does nothing if the bus already looks idle. Returns true if the bus ends up
// looking clean (both lines high), false if recovery couldn't fix it (in which case only a
// slave-side reset - see requestUnitEnterBootloader() - can help).
//
// Uses pin *mode* switching (INPUT_PULLUP <-> OUTPUT+LOW), not digitalWrite(), matching
// ESPHome's approach - I2C is open-drain, so a real slave could be actively driving a line and
// naively digitalWrite()-ing HIGH against that would fight it rather than release it.
bool recoverI2CBus() {
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(5);

  if (digitalRead(I2C_SCL_PIN) == LOW) {
    // A slave is holding SCL itself low (clock-stretching indefinitely, or genuinely wedged) -
    // only the master normally drives SCL, so if a slave is holding it low, toggling it from
    // our side can't force anything - this needs a slave-side reset instead.
    SerialPrintln("I2C bus recovery: SCL held low by a slave, cannot recover from the master side alone");
    return false;
  }

  if (digitalRead(I2C_SDA_PIN) == HIGH) {
    // Bus already looks idle - nothing to recover.
    return true;
  }

  SerialPrintln("I2C bus recovery: SDA stuck low, clocking SCL up to 9 times to release it");

  for (int i = 0; i < 9 && digitalRead(I2C_SDA_PIN) == LOW; i++) {
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);

    // Honor clock stretching (a slave legitimately holding SCL low mid-bit) - wait, but bounded,
    // for SCL to actually go high before continuing to the next pulse.
    unsigned long stretchWaitStart = micros();
    while (digitalRead(I2C_SCL_PIN) == LOW && (micros() - stretchWaitStart) < 10000) {
      delayMicroseconds(10);
    }
    delayMicroseconds(5);
  }

  // Generate a manual STOP condition: SDA transitions LOW -> HIGH while SCL is HIGH.
  pinMode(I2C_SDA_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(5);

  bool recovered = (digitalRead(I2C_SDA_PIN) == HIGH) && (digitalRead(I2C_SCL_PIN) == HIGH);
  SerialPrintln(recovered
    ? "I2C bus recovery: SDA released, bus looks idle again"
    : "I2C bus recovery: FAILED - SDA still stuck low, needs a slave-side reset (see requestUnitEnterBootloader())");

  // We just drove the I2C pins directly, outside of Wire's own control - re-initialize it so
  // its internal state matches reality again before the next real transaction.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#if UNIT_BOOTLOADER_ENABLE == true
  Wire.setClockStretchLimit(25000);
#endif

  return recovered;
}

#if UNIT_BOOTLOADER_ENABLE == true
// Escalation for when a unit stops responding to reads even after bus recovery attempts (called
// from waitForUnitReady()'s give-up paths, see ServiceFlapFunctions.ino) - forces that specific
// unit to reboot itself via requestUnitEnterBootloader() (ServiceUnitFirmwareFunctions.ino), the
// same watchdog-reset trick used for firmware updates. Sent over the *write* path, which this
// project has consistently observed stays reliable even when reads are failing - so this can
// reach and reset a unit whose read-side TWI state is wedged, without needing anything on the
// ESP's own I2C implementation to change. Unit.ino's CMD_ENTER_BOOTLOADER handler calls
// stopMotor() before resetting, so this is safe to send even mid-rotation.
//
// Fire-and-forget: doesn't wait for or verify the reset here. twiboot auto-returns to the app on
// its own after ~1s of no further I2C activity (see Unit/BOOTLOADER_SETUP.md), so simply not
// sending anything else to this unit for a moment is enough - the *next* normal attempt at
// talking to it (by whatever caller retries after this wait gave up) should find it freshly
// booted and responsive again.
void forceUnitRecoveryReset(int unitIndex) {
  SerialPrintln("I2C recovery: unit " + String(unitIndex) + " still not responding after bus recovery - forcing it to reboot");
  requestUnitEnterBootloader(unitIndex);
}
#else
void forceUnitRecoveryReset(int unitIndex) {
  (void)unitIndex; // UNIT_BOOTLOADER_ENABLE is off - no write-path reset command available.
}
#endif
