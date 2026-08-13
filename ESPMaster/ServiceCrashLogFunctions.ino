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
#define CRASH_LOG_MAGIC 0xC5A5FEEDu
#define CRASH_LOG_MAX_STACK_WORDS 32
#define CRASH_LOG_MAX_ENTRIES 5

struct CrashDump {
  uint32_t magic;
  uint32_t reason;
  uint32_t exccause;
  uint32_t epc1;
  uint32_t epc2;
  uint32_t epc3;
  uint32_t excvaddr;
  uint32_t depc;
  uint32_t stackWordCount;
  uint32_t stackWords[CRASH_LOG_MAX_STACK_WORDS];
};

// Called by the ESP8266 core directly from its fatal-exception handler, just before the
// automatic reset (weak symbol override, see file header). Must stay minimal: only RTC
// memory access, no flash/heap/Serial - the system may not be in a safe state for those
// here.
void custom_crash_callback(struct rst_info *rstInfo, uint32_t stack, uint32_t stackEnd) {
  CrashDump dump;
  memset(&dump, 0, sizeof(dump));
  dump.magic = CRASH_LOG_MAGIC;
  dump.reason = rstInfo->reason;
  dump.exccause = rstInfo->exccause;
  dump.epc1 = rstInfo->epc1;
  dump.epc2 = rstInfo->epc2;
  dump.epc3 = rstInfo->epc3;
  dump.excvaddr = rstInfo->excvaddr;
  dump.depc = rstInfo->depc;

  uint32_t count = 0;
  for (uint32_t addr = stack; addr < stackEnd && count < CRASH_LOG_MAX_STACK_WORDS; addr += 4) {
    dump.stackWords[count++] = *reinterpret_cast<uint32_t*>(addr);
  }
  dump.stackWordCount = count;

  ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&dump), sizeof(dump));
}

// Human-readable struct rst_info.reason values (from the ESP8266 SDK's user_interface.h).
const char* crashResetReasonText(uint32_t reason) {
  switch (reason) {
    case 0: return "power-on";
    case 1: return "hardware watchdog";
    case 2: return "exception";
    case 3: return "software watchdog";
    case 4: return "software restart";
    case 5: return "deep sleep wake";
    case 6: return "external system reset";
    default: return "unknown";
  }
}

// Only the handful of Xtensa exception causes most commonly seen in practice - falls back
// to an empty string (caller still has the raw number) rather than enumerating all ~40
// possible values.
const char* crashExceptionCauseText(uint32_t exccause) {
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
  entryDoc["exccauseText"] = crashExceptionCauseText(dump.exccause);
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
