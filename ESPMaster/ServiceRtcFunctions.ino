//Optional DS3231 RTC module (added 2026-09-11 on user request, following the boot-speed
//discussion that removed the LED blink codes) - lets Clock/Date modes show a correct time almost
//immediately after power-on instead of waiting for WiFi to connect and NTP to sync, and keeps
//the clock correct through extended WiFi outages instead of sitting at ezTime's UTC/no-rule
//default. Entirely optional and auto-detected (probeRtc()) - a build/device without one attached
//behaves exactly as it did before this feature existed.
//
//Plain direct I2C via Wire, no external RTC library - the DS3231 is a simple register-addressed
//peripheral (7 contiguous BCD time registers at 0x00-0x06, one status register at 0x0F carrying
//the Oscillator Stop Flag), not worth a dependency given how little code it takes. makeTime()/
//breakTime()/tmElements_t come from ezTime itself (already `using namespace ezt;`-exposed
//unqualified, see ezTime.h) - no separate Time library needed either.

//rtcPresent/rtcOscillatorStopFlag are declared in ESPMaster.ino itself, not here - found while
//compiling: Arduino concatenates the main sketch file first, before every other .ino file in
//alphabetical order, so a global used by ESPMaster.ino's own setup()/GET /api/diag handler (both
//textually "earlier" in that concatenated order) must live there, same reasoning already hit for
//MQTT's mqttClient and Animation's ANIMATION_MODE_* defines this session - functions get
//auto-prototyped regardless of file order, but globals don't.

uint8_t bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

uint8_t decToBcd(uint8_t dec) {
  return ((dec / 10) << 4) | (dec % 10);
}

//The DS3231 hours register can be factory/previously configured in 12h mode (bit6=1, bit5=AM/PM)
//or 24h mode (bit6=0) - handle both on read, regardless of how the module happens to arrive.
//writeRtcTime() below always writes in 24h mode going forward, so this only matters for however
//the very first read (before this firmware ever wrote to it) finds it.
uint8_t decodeRtcHours(uint8_t reg) {
  if (reg & 0x40) { //12-hour mode
    bool pm = (reg & 0x20) != 0;
    uint8_t hour = bcdToDec(reg & 0x1F); //1-12
    if (hour == 12) hour = 0; //12 AM -> 0
    if (pm) hour += 12;
    return hour;
  }
  return bcdToDec(reg & 0x3F); //24-hour mode
}

//Cheap presence check - a bare address ping, no register read. Call once at boot before relying
//on readRtcTime()/writeRtcTime() anywhere else.
void probeRtc() {
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  rtcPresent = (Wire.endTransmission(true) == 0); //real STOP, see readRtcTime()'s own comment for why
  SerialPrintln(rtcPresent
    ? F("RTC: DS3231 detected on the I2C bus")
    : F("RTC: no DS3231 detected - feature inactive, boot relies purely on WiFi/NTP as before"));
}

//Reads the DS3231's current time into outUtc (real UTC, exactly as stored - this project always
//writes UTC to the RTC, never local time, avoiding any DST-ambiguity on read). Returns false (and
//leaves outUtc untouched) if the RTC isn't present, a bus error occurs, or the Oscillator Stop
//Flag is set (time not trustworthy) - callers must not seed the clock in that case.
bool readRtcTime(time_t &outUtc) {
  if (!rtcPresent) {
    return false;
  }

  //Real STOP + short settle delay before the read, not a repeated START - a repeated START here
  //triggers a known ESP8266 core bug (esp8266/Arduino #5718), already worked around the exact
  //same way for twiboot communication (see twibootReadChipInfo()'s own comment,
  //ServiceUnitFirmwareFunctions.ino).
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(true) != 0) {
    recoverI2CBus(); //keep the shared bus from staying wedged for whatever I2C traffic comes next - see this file's own comment on why every other I2C caller in this project already does this
    return false;
  }
  delayMicroseconds(500);

  if (Wire.requestFrom(RTC_I2C_ADDRESS, 7) != 7) {
    recoverI2CBus();
    return false;
  }
  uint8_t regSeconds = Wire.read();
  uint8_t regMinutes = Wire.read();
  uint8_t regHours = Wire.read();
  Wire.read(); //day-of-week register (0x03) - not needed, makeTime() derives it itself
  uint8_t regDate = Wire.read();
  uint8_t regMonth = Wire.read();
  uint8_t regYear = Wire.read();

  //Status register (0x0F) isn't contiguous with 0x00-0x06, so it's a separate transaction.
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission(true) != 0) {
    recoverI2CBus();
    return false;
  }
  delayMicroseconds(500);
  if (Wire.requestFrom(RTC_I2C_ADDRESS, 1) != 1) {
    recoverI2CBus();
    return false;
  }
  uint8_t status = Wire.read();
  rtcOscillatorStopFlag = (status & 0x80) != 0;
  if (rtcOscillatorStopFlag) {
    SerialPrintln(F("RTC: Oscillator Stop Flag set - time not trustworthy, not seeding clock from it"));
    return false;
  }

  uint8_t second = bcdToDec(regSeconds & 0x7F);
  uint8_t minute = bcdToDec(regMinutes & 0x7F);
  uint8_t hour = decodeRtcHours(regHours);
  uint8_t date = bcdToDec(regDate & 0x3F);
  uint8_t month = bcdToDec(regMonth & 0x1F); //bit7 is the century flag, ignored - valid until 2099
  uint16_t year = 2000 + bcdToDec(regYear);

  //makeTime() (ezTime, unqualified via its own `using namespace ezt;`) accepts a full calendar
  //year directly and returns real UTC seconds - exactly what this project already stores in the
  //RTC (see writeRtcTime() below).
  outUtc = makeTime(hour, minute, second, date, month, year);
  return true;
}

//Writes utc (real UTC, same convention as readRtcTime()) to the DS3231, always in 24h mode, and
//explicitly clears the Oscillator Stop Flag afterward - found while implementing this: some
//DS3231 clones don't clear it automatically just because valid time was written, and a caller
//that immediately read it back (unlikely today, but a future one might) should see it correctly
//cleared. No-op if the RTC isn't present.
void writeRtcTime(time_t utc) {
  if (!rtcPresent) {
    return;
  }

  tmElements_t tm;
  breakTime(utc, tm);
  //tm.Year is offset-from-1970 (ezTime convention); the RTC's year register is offset-from-2000.
  uint8_t rawYear = (uint8_t)(tm.Year - 30);

  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  Wire.write(decToBcd(tm.Second));
  Wire.write(decToBcd(tm.Minute));
  Wire.write(decToBcd(tm.Hour)); //0-23, bit6 stays 0 -> 24h mode
  Wire.write(decToBcd(tm.Wday));
  Wire.write(decToBcd(tm.Day));
  Wire.write(decToBcd(tm.Month));
  Wire.write(decToBcd(rawYear));
  if (Wire.endTransmission(true) != 0) {
    //Time write itself failed - don't even attempt the status-register clear below against a bus
    //that just NACKed, and give the next caller (Unit I2C traffic, or the next writeRtcTime()) a
    //clean bus to start from instead of whatever state this transaction left it in.
    recoverI2CBus();
    return;
  }
  delayMicroseconds(500);

  //Read-modify-write the status register rather than blindly writing 0x00 - registers 0x0E/0x0F
  //also carry alarm-interrupt-enable/square-wave-frequency bits this project doesn't use but
  //shouldn't silently reset either.
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission(true) == 0) {
    delayMicroseconds(500);
    if (Wire.requestFrom(RTC_I2C_ADDRESS, 1) == 1) {
      uint8_t status = Wire.read();
      Wire.beginTransmission(RTC_I2C_ADDRESS);
      Wire.write((uint8_t)0x0F);
      Wire.write((uint8_t)(status & 0x7F)); //clear bit7 (OSF), leave the rest untouched
      Wire.endTransmission(true);
    } else {
      recoverI2CBus();
    }
  } else {
    recoverI2CBus();
  }
  rtcOscillatorStopFlag = false;
}
