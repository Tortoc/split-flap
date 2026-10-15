//Must live in a real .cpp, not a .ino: the ESP8266 core declares this `extern "C"`, but the sketch
//preprocessor would give a .ino definition normal C++ linkage - the core's no-op stub would run
//instead, and /crash-log would stay empty.
#include <Arduino.h>
#include <user_interface.h>
#include "CrashLog.h"

extern const char* espVersion; //ESPMaster.ino

extern "C" void custom_crash_callback(struct rst_info *rstInfo, uint32_t stack, uint32_t stackEnd) {
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
  dump.uptimeMs = millis();
  strncpy(dump.firmwareVersion, espVersion, sizeof(dump.firmwareVersion) - 1);

  ESP.rtcUserMemoryWrite(CRASH_LOG_RTC_OFFSET, reinterpret_cast<uint32_t*>(&dump), sizeof(dump));
}
