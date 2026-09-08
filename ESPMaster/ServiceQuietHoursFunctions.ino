//Cached minutes-since-midnight for quietHoursStart/End, recomputed only when those change (boot
//load, POST /api/quiet-hours) via recomputeQuietHoursMinutes() - keeps isQuietHoursActive() (called
//from showText() on every display update, i.e. roughly once a second) free of String allocations,
//since it runs during exactly the same windows an OTA upload's own buffers put heap under pressure.
int quietHoursStartMinutes = -1;
int quietHoursEndMinutes = -1;

void recomputeQuietHoursMinutes() {
  quietHoursStartMinutes = parseTimeOfDayToMinutes(quietHoursStart);
  quietHoursEndMinutes = parseTimeOfDayToMinutes(quietHoursEnd);
}

//Returns true if the current local time falls inside the configured Quiet Hours window - handles
//a window that spans midnight (e.g. 22:00-07:00) the same as one that doesn't (e.g. 13:00-15:00).
//showText() (ServiceFlapFunctions.ino) checks this to park the display; checkScheduledMessages()
//checks it separately to defer a due message instead of silently consuming it while parked.
bool isQuietHoursActive() {
  if (!quietHoursEnabled) {
    return false;
  }

  if (quietHoursStartMinutes < 0 || quietHoursEndMinutes < 0 || quietHoursStartMinutes == quietHoursEndMinutes) {
    return false; //misconfigured or a zero-length window - fail open rather than park forever
  }

  int nowMinutes = timezone.hour() * 60 + timezone.minute();

  if (quietHoursStartMinutes < quietHoursEndMinutes) {
    return nowMinutes >= quietHoursStartMinutes && nowMinutes < quietHoursEndMinutes;
  } else {
    //Crosses midnight
    return nowMinutes >= quietHoursStartMinutes || nowMinutes < quietHoursEndMinutes;
  }
}
