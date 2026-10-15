//Shared struct/constants for the on-device crash reporter. In its own header since
//custom_crash_callback() lives in a real .cpp file, a separate translation unit from the .ino files.
#pragma once
#include <stdint.h>

//Changed with the layout below, so a dump of an older layout is never misread.
#define CRASH_LOG_MAGIC 0xC5A5FEEEu
#define CRASH_LOG_MAX_STACK_WORDS 32
#define CRASH_LOG_MAX_ENTRIES 5
//Position in the RTC user memory, in 4-byte blocks. Blocks 0-31 (128 bytes) hold the eboot command
//of a pending OTA update (core Esp.cpp, "Layout of RTC Memory") - a crash dump there would replace it.
#define CRASH_LOG_RTC_OFFSET 32

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
  //Firmware that crashed (espVersion) and its uptime - stack addresses only decode against that
  //build's .elf.
  uint32_t uptimeMs;
  char firmwareVersion[12];
};
