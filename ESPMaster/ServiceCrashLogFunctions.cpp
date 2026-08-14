// custom_crash_callback() lives in a real .cpp file, not a .ino, for a specific reason found
// the hard way (2026-08-13): Arduino's sketch preprocessor auto-generates a forward
// declaration for every top-level function it finds in .ino files, with plain C++ linkage.
// The ESP8266 core declares custom_crash_callback() inside an `extern "C" { ... }` block
// (core_esp8266_postmortem.cpp:37-373), so a definition here needs matching `extern "C"`
// linkage to actually override the core's weak no-op stub - but doing that in a .ino
// produces a hard compile error ("conflicting declaration ... with 'C' linkage" vs the
// auto-generated 'C++' linkage prototype). Worse, the *silent* failure mode (before this was
// caught) was deploying it anyway without extern "C": it compiles and links, but under a
// C++-mangled symbol name that nothing references, so the linker just keeps the core's
// original no-op - verified via `nm` on the .elf, where custom_crash_callback and the core's
// own __custom_crash_callback resolved to the *same* address. A real crash was captured live
// this session and /crash-log stayed empty even after that fix attempt, which is what led to
// writing a deliberate test-crash endpoint (/debug/trigger-crash) and finally finding this.
// Plain .cpp files aren't run through the sketch preprocessor at all, so this problem doesn't
// exist here.
#include <Arduino.h>
#include <user_interface.h>
#include "CrashLog.h"

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

  ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&dump), sizeof(dump));
}
