//Builds "<prefix> <days> <suffix>", any empty part omitted. Falls back to the bare number if the
//full text doesn't fit. Shared by standalone Countdown/Countup mode (via buildCountdownText()/
//buildCountupText() below) and Rotation's countdown/countup items (called directly with the
//item's own Prefix/Suffix).
String buildDayCountText(long days, String prefix, String suffix) {
  String daysText = String(days);
  if (prefix != "") {
    daysText = prefix + " " + daysText;
  }
  if (suffix != "") {
    daysText = daysText + " " + suffix;
  }

  if ((int)daysText.length() > unitsAmount) {
    SerialPrintln(F("Day count text was too long, cutting down to just the number..."));
    //Must be String(days), not `"" + days` - "" is a const char*, so `+ days` would do pointer
    //arithmetic instead of concatenation (undefined behavior, readily a crash).
    daysText = String(days);
  }
  return daysText;
}

//Standalone Countdown mode's prefix/suffix - see buildDayCountText() for the shared logic.
String buildCountdownText(long days) {
  return buildDayCountText(days, countdownPrefix, countdownSuffix);
}

//Check if countdown is required and check how long left
void checkCountdown() {
  //This will check if a day has passed since the last time the countdown was updated
  if (deviceMode == DEVICE_MODE_COUNTDOWN) {
    long countdownInSeconds = atol(countdownToDateUnix.c_str());

    //Work out how long left
    //Bug (found while fixing #117): timezone.now() doesn't return real UTC (see getRealUtcNow()'s
    //comment), but countdownToDateUnix is real UTC seconds from the client - comparing them
    //directly shifted the hours/days computation below by the timezone offset, occasionally
    //flipping which day a near-midnight countdown rounds to.
    long currentTimeSeconds = getRealUtcNow();
    long differenceSeconds = countdownInSeconds - currentTimeSeconds;

    //If there is any remainder, we want to actually still display the full day remaining - must
    //compute directly from seconds (matching checkRotation()'s countdown branch below), not via
    //an intermediate truncated-to-hours value. `differenceSeconds / 60 / 60` is integer division
    //and silently discards any partial-hour remainder BEFORE ceil() ever sees it, so ceil() can
    //only ever round up whole discarded hours, never the sub-hour remainder that's the whole
    //point of this comment (found 2026-09-09, CODE_REVIEW.md #22, reproduced: 30 minutes
    //remaining rounded down to 0 days instead of up to 1; 24h30m rounded to 1 day instead of 2).
    //Note: checkCountup()'s equivalent uses floor(), not ceil() - that one's mathematically
    //unaffected (floor(floor(x/3600)/24) == floor(x/86400) for x>=0), so it wasn't touched.
    long days = ceil(differenceSeconds / 60.0 / 60.0 / 24.0);

    //Make sure we don't go negative
    days = days > 0 ? days : 0;

    String daysText = buildCountdownText(days);

    if (inputText != daysText) {
      SerialPrintln(F("Setting Countdown Text to: ") + daysText);
      inputText = daysText;
    }
  }
}
