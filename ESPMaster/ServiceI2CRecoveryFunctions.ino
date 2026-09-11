//I2C bus recovery for a slave stuck holding SDA low. Per the I2C spec (NXP AN10216), the master can
//free it by clocking SCL up to 9 times then forcing a STOP - the ESP8266 core doesn't do this
//itself. Called both proactively and when a read fails.

// Un-sticks a hung bus: clocks SCL to release a slave holding SDA low, then generates a STOP.
// Cheap, no-op if already idle. Uses pin *mode* switching, not digitalWrite() - I2C is open-drain,
// so writing HIGH against a slave actively driving the line would fight it instead of releasing it.
bool recoverI2CBus() {
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(5);

  if (digitalRead(I2C_SCL_PIN) == LOW) {
    // Only the master normally drives SCL - if a slave is holding it low, toggling it from our
    // side can't force anything; needs a slave-side reset instead.
    SerialPrintln(F("I2C bus recovery: SCL held low by a slave, cannot recover from the master side alone"));
    return false;
  }

  if (digitalRead(I2C_SDA_PIN) == HIGH) {
    return true; // Already idle.
  }

  SerialPrintln(F("I2C bus recovery: SDA stuck low, clocking SCL up to 9 times to release it"));

  for (int i = 0; i < 9 && digitalRead(I2C_SDA_PIN) == LOW; i++) {
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);

    // Honor clock stretching - wait (bounded) for SCL to actually go high before the next pulse.
    unsigned long stretchWaitStart = micros();
    while (digitalRead(I2C_SCL_PIN) == LOW && (micros() - stretchWaitStart) < 10000) {
      delayMicroseconds(10);
    }
    delayMicroseconds(5);
  }

  // Manual STOP: SDA transitions LOW -> HIGH while SCL is HIGH.
  pinMode(I2C_SDA_PIN, OUTPUT);
  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delayMicroseconds(5);

  bool recovered = (digitalRead(I2C_SDA_PIN) == HIGH) && (digitalRead(I2C_SCL_PIN) == HIGH);
  SerialPrintln(recovered
    ? F("I2C bus recovery: SDA released, bus looks idle again")
    : F("I2C bus recovery: FAILED - SDA still stuck low, needs a slave-side reset (see requestUnitEnterBootloader())"));

  // We drove the pins directly, outside Wire's control - re-init it before the next real transaction.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#if UNIT_BOOTLOADER_ENABLE == true
  Wire.setClockStretchLimit(25000);
#endif

  return recovered;
}

#if UNIT_BOOTLOADER_ENABLE == true
//Escalation when a unit stops responding even after bus recovery - forces a reboot via
//requestUnitEnterBootloader() (the write path stays reliable even when reads fail). Fire-and-forget:
//twiboot returns to the app after ~1s idle.
void forceUnitRecoveryReset(int unitIndex) {
  SerialPrintln(F("I2C recovery: unit ") + String(unitIndex) + F(" still not responding after bus recovery - forcing it to reboot"));
  requestUnitEnterBootloader(unitIndex);
  //Invalidate the display cache - found 2026-09-10, CODE_REVIEW_V3.md #10: this reboots the unit
  //(Unit.ino's setup()/calibrate(true) re-homes it from scratch), which can leave its actual
  //physical position no longer matching whatever the cache still believes was last successfully
  //written to it - without this, a later showText() call for the exact same text/alignment the
  //cache already has on file would hit that cache and skip re-sending, even though this
  //specific unit may no longer show the correct letter. Same "cache no longer trustworthy"
  //mechanism CODE_REVIEW.md #14 already uses for every other case where a unit's actual state
  //can't be relied on - see displayRenderGeneration's own comment.
  displayRenderGeneration++;
}
#else
void forceUnitRecoveryReset(int unitIndex) {
  (void)unitIndex; // No write-path reset available when UNIT_BOOTLOADER_ENABLE is off.
}
#endif
