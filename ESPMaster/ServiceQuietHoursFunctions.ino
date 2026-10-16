//Cached minutes-since-midnight for quietHoursStart/End, recomputed only when those change -
//keeps isQuietHoursActive() (called on every display update) free of String allocations.
int quietHoursStartMinutes = -1;
int quietHoursEndMinutes = -1;

void recomputeQuietHoursMinutes() {
  quietHoursStartMinutes = parseTimeOfDayToMinutes(quietHoursStart);
  quietHoursEndMinutes = parseTimeOfDayToMinutes(quietHoursEnd);
}

//True if the current local time falls inside the configured Quiet Hours window - handles a window
//spanning midnight (22:00-07:00) same as one that doesn't (13:00-15:00).
bool isQuietHoursActive() {
  if (!quietHoursEnabled || wifiSetupPortalOpen) {
    return false;
  }

  if (quietHoursStartMinutes < 0 || quietHoursEndMinutes < 0 || quietHoursStartMinutes == quietHoursEndMinutes) {
    return false; //misconfigured or a zero-length window - fail open rather than park forever
  }

  //No RTC/NTP time yet: the clock reads 1970 (01:00 with a cached Berlin rule, inside 22:00-07:00) -
  //the display went blank at boot and stayed dark without internet. Set by NTP and the RTC alike.
  if (timeStatus() == timeNotSet) {
    return false;
  }

  int nowMinutes = timezone.hour() * 60 + timezone.minute();

  if (quietHoursStartMinutes < quietHoursEndMinutes) {
    return nowMinutes >= quietHoursStartMinutes && nowMinutes < quietHoursEndMinutes;
  } else {
    //Crosses midnight
    return nowMinutes >= quietHoursStartMinutes || nowMinutes < quietHoursEndMinutes;
  }
}
