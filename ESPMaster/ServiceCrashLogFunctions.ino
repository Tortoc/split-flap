//On-device crash reporter, independent of a serial connection: custom_crash_callback() (called by
//the core right before its reset) stores exception + stack in RTC memory, which survives a reset;
//checkAndPersistCrashLog() copies it into a rotating LittleFS log at boot, served by
//GET /api/crash-log.

//custom_crash_callback() itself lives in ServiceCrashLogFunctions.cpp - see that file.
#include "CrashLog.h"

//Human-readable struct rst_info.reason values. 0-6 are the SDK's own; 253/254 are
//core_esp8266_postmortem.cpp's software-only values for abort()/panic()/assert().
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

//Only meaningful when reason==2 - for every other reason (including 254), exccause is left at 0
//and doesn't mean "IllegalInstruction".
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

//Checks RTC memory for a crash captured just before the last reset. If found: appends it to the
//rotating LittleFS log and clears the RTC magic. Must run after LittleFS is mounted.
void checkAndPersistCrashLog() {
  CrashDump dump;
  memset(&dump, 0, sizeof(dump));
  if (!ESP.rtcUserMemoryRead(CRASH_LOG_RTC_OFFSET, reinterpret_cast<uint32_t*>(&dump), sizeof(dump))) {
    return;
  }
  if (dump.magic != CRASH_LOG_MAGIC) {
    return; //nothing captured since the last check
  }

  SerialPrintln(F("=== Crash detected from previous run - saving to " CRASH_LOG_PATH " ==="));

  JsonDocument entryDoc;
  dump.firmwareVersion[sizeof(dump.firmwareVersion) - 1] = '\0'; //never trust RTC memory to be terminated
  entryDoc["firmware"] = dump.firmwareVersion;
  entryDoc["uptimeSec"] = dump.uptimeMs / 1000;
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

  //Rotate: keep at most CRASH_LOG_MAX_ENTRIES lines.
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
    SerialPrintln(F("- Failed to write crash log to LittleFS"));
  }

  //Clear the RTC magic so a later, unrelated reboot doesn't re-report this same crash.
  memset(&dump, 0, sizeof(dump));
  ESP.rtcUserMemoryWrite(CRASH_LOG_RTC_OFFSET, reinterpret_cast<uint32_t*>(&dump), sizeof(dump));
}
