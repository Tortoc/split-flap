//Standalone Countup mode's prefix/suffix - see buildDayCountText() (ServiceCountdownFunctions.ino)
//for the shared logic.
String buildCountupText(long days) {
  return buildDayCountText(days, countupPrefix, countupSuffix);
}

//Check if countup is active and how many full days have elapsed since the configured date
void checkCountup() {
  if (deviceMode == DEVICE_MODE_COUNTUP) {
    long countupInSeconds = atol(countupSinceDateUnix.c_str());

    long currentTimeSeconds = timezone.now();
    long differenceSeconds = currentTimeSeconds - countupInSeconds;
    long hours = differenceSeconds / 60 / 60;

    //Full elapsed days only - unlike Countdown's ceil() (which rounds a partial day up so "still
    //today" reads as "1 day left"), a partial day since the date hasn't been "a full day" yet.
    long days = floor(hours / 24.0);

    //Make sure we don't go negative (date hasn't happened yet)
    days = days > 0 ? days : 0;

    String daysText = buildCountupText(days);

    if (inputText != daysText) {
      SerialPrintln(F("Setting Countup Text to: ") + daysText);
      inputText = daysText;
    }
  }
}
