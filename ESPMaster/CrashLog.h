//Shared struct/constants for the on-device crash reporter. In its own header since
//custom_crash_callback() lives in a real .cpp file, a separate translation unit from the .ino files.
#pragma once
#include <stdint.h>

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
