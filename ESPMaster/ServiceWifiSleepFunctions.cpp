//Fix for the sporadic OTA hangs (BUGFIX_LOG Punkt 256, BUILD_INFO Punkt 252): Update.begin() (core
//Updater.cpp) switches the WiFi sleep type to NONE_SLEEP_T mid-session, while this firmware
//otherwise runs the SDK default (modem sleep, see ServiceWifiFunctions.ino). That runtime switch
//sporadically wedged the radio during the upload - hardware-watchdog resets, and one captured Soft
//WDT inside the SDK's power management (pm_send_nullfunc via pm_keep_active_enable) after the
//radio had stopped receiving. 2026-10-12 on .181: 4 of 17 OTAs failed with the switch, 0 of 20
//without it. Intercepted via GNU ld's --wrap (`-Wl,--wrap=wifi_set_sleep_type`, required build
//flag like the other two wraps - see firmware/README.md) so the OTA start leaves the sleep type
//alone; every other caller passes straight through.
//
//A plain .cpp file, not a .ino - see ServiceMemoryReserveFunctions.h's own comment for why.
#include <Arduino.h>
#include <user_interface.h>
#include "ServiceMemoryReserveFunctions.h"

volatile bool keepWifiSleepType = false;

extern "C" bool __real_wifi_set_sleep_type(sleep_type_t type);

extern "C" bool __wrap_wifi_set_sleep_type(sleep_type_t type) {
  if (keepWifiSleepType) {
    return true; //leave the current sleep type alone
  }
  return __real_wifi_set_sleep_type(type);
}
