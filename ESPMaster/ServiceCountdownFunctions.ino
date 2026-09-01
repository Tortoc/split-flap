//Builds "<prefix> <days> <suffix>", any empty part omitted. Shared by standalone Countdown mode and
//Rotation's countdown items. Falls back to the bare number if the full text doesn't fit.
String buildCountdownText(long days) {
  String daysText = String(days);
  if (countdownPrefix != "") {
    daysText = countdownPrefix + " " + daysText;
  }
  if (countdownSuffix != "") {
    daysText = daysText + " " + countdownSuffix;
  }

  if ((int)daysText.length() > unitsAmount) {
    SerialPrintln("Countdown text was too long, cutting down to just the number...");
    //Must be String(days), not `"" + days` - "" is a const char*, so `+ days` would do pointer
    //arithmetic instead of concatenation (undefined behavior, readily a crash).
    daysText = String(days);
  }
  return daysText;
}

//Check if countdown is required and check how long left
void checkCountdown() {
  //This will check if a day has passed since the last time the countdown was updated
  if (deviceMode == DEVICE_MODE_COUNTDOWN) {
    long countdownInSeconds = atol(countdownToDateUnix.c_str());

    //Work out how long left
    long currentTimeSeconds = timezone.now();
    long differenceSeconds = countdownInSeconds - currentTimeSeconds;
    long hours = differenceSeconds / 60 / 60;

    //If there is any remainder, we want to actually still display the full day remaining 
    long days = ceil(hours / 24.0);

    //Make sure we don't go negative
    days = days > 0 ? days : 0;
    
    String daysText = buildCountdownText(days);

    if (inputText != daysText) {
      SerialPrintln("Setting Countdown Text to: " + daysText);
      inputText = daysText;
    }
  }
}