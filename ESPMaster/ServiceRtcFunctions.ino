//Optional DS3231 RTC (auto-detected by probeRtc()): Clock/Date show the right time right after
//power-on and through WiFi outages. Plain Wire access, no RTC library; makeTime()/breakTime() come
//from ezTime.

//rtcPresent/rtcOscillatorStopFlag live in ESPMaster.ino - the main sketch file comes first.

uint8_t bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

uint8_t decToBcd(uint8_t dec) {
  return ((dec / 10) << 4) | (dec % 10);
}

//The hours register may still be in 12h mode from the factory; writeRtcTime() always writes 24h.
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

//Presence check (address ping only). Call once at boot.
void probeRtc() {
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  rtcPresent = (Wire.endTransmission(true) == 0); //real STOP, see readRtcTime()'s own comment for why
  SerialPrintln(rtcPresent
    ? F("RTC: DS3231 detected on the I2C bus")
    : F("RTC: no DS3231 detected - feature inactive, boot relies purely on WiFi/NTP as before"));
}

//Reads the RTC time (always stored as UTC) into outUtc. Returns false without touching outUtc if
//the RTC is missing, the bus fails or the Oscillator Stop Flag says the time can't be trusted.
bool readRtcTime(time_t &outUtc) {
  if (!rtcPresent) {
    return false;
  }

  //Real STOP + short settle delay before the read, not a repeated START - a repeated START here
  //triggers a known ESP8266 core bug (esp8266/Arduino #5718).
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(true) != 0) {
    recoverI2CBus();
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

  //makeTime() (ezTime) takes the full year and returns UTC seconds.
  outUtc = makeTime(hour, minute, second, date, month, year);
  return true;
}

//Writes utc to the RTC in 24h mode and clears the Oscillator Stop Flag explicitly (some clones
//don't clear it on a time write). No-op without RTC.
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
    //Bus NACKed: skip the status register and recover the bus for the next transaction.
    recoverI2CBus();
    return;
  }
  delayMicroseconds(500);

  //Read-modify-write: 0x0E/0x0F also hold alarm/square-wave bits. rtcOscillatorStopFlag is only
  //cleared once all three transactions succeeded.
  bool statusClearOk = false;
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission(true) == 0) {
    delayMicroseconds(500);
    if (Wire.requestFrom(RTC_I2C_ADDRESS, 1) == 1) {
      uint8_t status = Wire.read();
      Wire.beginTransmission(RTC_I2C_ADDRESS);
      Wire.write((uint8_t)0x0F);
      Wire.write((uint8_t)(status & 0x7F)); //clear bit7 (OSF), leave the rest untouched
      if (Wire.endTransmission(true) == 0) {
        statusClearOk = true;
      } else {
        recoverI2CBus();
      }
    } else {
      recoverI2CBus();
    }
  } else {
    recoverI2CBus();
  }
  if (statusClearOk) {
    rtcOscillatorStopFlag = false;
  }
  //else: keep rtcOscillatorStopFlag as last read - it may still be set.
}
