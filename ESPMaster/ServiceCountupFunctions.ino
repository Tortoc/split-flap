//Standalone Countup mode's prefix/suffix - see buildDayCountText() (ServiceCountdownFunctions.ino)
//for the shared logic.
String buildCountupText(long days) {
  return buildDayCountText(days, countupPrefix, countupSuffix);
}

//Check if countup is active and how many full days have elapsed since the configured date
void checkCountup() {
  if (deviceMode == DEVICE_MODE_COUNTUP) {
    long countupInSeconds = atol(countupSinceDateUnix.c_str());

    //timezone.now() doesn't return real UTC - see getRealUtcNow()'s comment.
    long currentTimeSeconds = getRealUtcNow();
    long differenceSeconds = currentTimeSeconds - countupInSeconds;
    long hours = differenceSeconds / 60 / 60;

    //Full elapsed days only - unlike Countdown's ceil(), a partial day isn't "a full day" yet.
    long days = floor(hours / 24.0);
    days = days > 0 ? days : 0; //date hasn't happened yet

    String daysText = buildCountupText(days);

    if (inputText != daysText) {
      SerialPrintln(F("Setting Countup Text to: ") + daysText);
      inputText = daysText;
    }
  }
}
