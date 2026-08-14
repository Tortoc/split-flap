// On-device crash reporter (added 2026-08-13).
//
// Background: diagnosing ESP crashes on this project has repeatedly been blocked by an
// unreliable USB-serial adapter dropping mid-capture (see the
// i2c-firmware-update-session-2026-08-10 notes) - by the time a live serial capture is
// running, the crash has often already happened, or the adapter drops at the critical
// moment. This makes crash diagnosis independent of any live serial connection:
//
// 1. custom_crash_callback() is called *by the ESP8266 core itself*, directly from its
//    fatal-exception handler, immediately before the automatic reset - see
//    core_esp8266_postmortem.cpp (weak symbol override, verified against the installed
//    esp8266 core 3.1.2 source). It captures the exception registers + a chunk of the
//    crashed stack into RTC memory. RTC memory is used (not LittleFS/EEPROM) because it's
//    a simple, fast register write - safe to do from inside a fatal-exception context,
//    unlike flash access which is not guaranteed to be in a safe state at that point. RTC
//    memory survives any reset that doesn't cut power (which covers the automatic
//    post-crash reset this is designed to catch).
// 2. checkAndPersistCrashLog(), called early in setup() on every boot, checks for a fresh
//    capture and - if found - copies it into a small rotating LittleFS log, so it also
//    survives a subsequent full power cycle (RTC memory does not).
// 3. GET /crash-log (registered in ESPMaster.ino) serves that LittleFS log directly, so a
//    crash can be inspected after the fact from any browser on the LAN, no serial needed.

// CRASH_LOG_PATH is defined in ESPMaster.ino (needed there too, and .ino files are
// concatenated alphabetically - ESPMaster.ino comes first).
//
// custom_crash_callback() itself lives in ServiceCrashLogFunctions.cpp, not here - see the
// long comment at the top of that file for why (Arduino's .ino auto-prototyping conflicts
// with the extern "C" linkage the core requires for this specific weak-symbol override).
#include "CrashLog.h"

// Human-readable struct rst_info.reason values. 0-6 are the ESP8266 SDK's own
// user_interface.h values; 253/254 are core_esp8266_postmortem.cpp's own software-only
// values (REASON_USER_STACK_SMASH / REASON_USER_SWEXCEPTION_RST) for abort()/panic()/
// assert() - not a real SDK reset reason, but reported the same way via custom_crash_callback.
const char* crashResetReasonText(uint32_t reason) {
  switch (reason) {
    case 0:   return "power-on";
    case 1:   return "hardware watchdog";
    case 2:   return "exception";
    case 3:   return "software watchdog";
    case 4:   return "software restart";
    case 5:   return "deep sleep wake";
    case 6:   return "external system reset";
    case 253: return "stack smashing detected";
    case 254: return "user exception (panic/abort/assert)";
    default:  return "unknown";
  }
}

// Only the handful of Xtensa exception causes most commonly seen in practice - falls back
// to an empty string (caller still has the raw number) rather than enumerating all ~40
// possible values. Only meaningful when reason==2 (REASON_EXCEPTION_RST) - for every other
// reason (including the abort()/panic() path, reason 254), rst_info.exccause is just left at
// its memset-to-0 default by the core and doesn't actually mean "IllegalInstruction".
const char* crashExceptionCauseText(uint32_t reason, uint32_t exccause) {
  if (reason != 2) return "";
  switch (exccause) {
    case 0:  return "IllegalInstruction";
    case 6:  return "IntegerDivideByZero";
    case 9:  return "LoadStoreAlignment";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "";
  }
}

// Checks RTC memory for a crash captured just before the last reset (see
// custom_crash_callback above). If found: appends it to the rotating LittleFS log and
// clears the RTC magic, so an unrelated later reboot doesn't re-report the same crash.
// Must run after LittleFS is mounted (see initialiseFileSystem() in
// ServiceFileSystemFunctions.ino).
void checkAndPersistCrashLog() {
  CrashDump dump;
  memset(&dump, 0, sizeof(dump));
  if (!ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&dump), sizeof(dump))) {
    return;
  }
  if (dump.magic != CRASH_LOG_MAGIC) {
    return; // Nothing captured since the last check - a normal boot/reset.
  }

  SerialPrintln("=== Crash detected from previous run - saving to " CRASH_LOG_PATH " ===");

  JsonDocument entryDoc;
  entryDoc["reason"] = dump.reason;
  entryDoc["reasonText"] = crashResetReasonText(dump.reason);
  entryDoc["exccause"] = dump.exccause;
  entryDoc["exccauseText"] = crashExceptionCauseText(dump.reason, dump.exccause);
  entryDoc["epc1"] = dump.epc1;
  entryDoc["epc2"] = dump.epc2;
  entryDoc["epc3"] = dump.epc3;
  entryDoc["excvaddr"] = dump.excvaddr;
  entryDoc["depc"] = dump.depc;
  JsonArray stackArray = entryDoc["stack"].to<JsonArray>();
  for (uint32_t i = 0; i < dump.stackWordCount; i++) {
    stackArray.add(dump.stackWords[i]);
  }

  String entryLine;
  serializeJson(entryDoc, entryLine);

  // Rotate: keep at most CRASH_LOG_MAX_ENTRIES lines (drop the oldest once full).
  std::vector<String> lines;
  File existing = LittleFS.open(CRASH_LOG_PATH, "r");
  if (existing) {
    while (existing.available()) {
      String line = existing.readStringUntil('\n');
      if (line.length() > 0) lines.push_back(line);
    }
    existing.close();
  }
  lines.push_back(entryLine);
  while (lines.size() > CRASH_LOG_MAX_ENTRIES) {
    lines.erase(lines.begin());
  }

  File out = LittleFS.open(CRASH_LOG_PATH, "w");
  if (out) {
    for (auto &line : lines) {
      out.println(line);
    }
    out.close();
  } else {
    SerialPrintln("- Failed to write crash log to LittleFS");
  }

  // Clear the RTC magic so a later, unrelated reboot doesn't re-report this same crash.
  memset(&dump, 0, sizeof(dump));
  ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&dump), sizeof(dump));
}
