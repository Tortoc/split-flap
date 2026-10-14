//Secondary safety net for the TCP connection-flood crash (primary defense:
//ServiceConnectionGateFunctions.cpp). The crash chain, per ESPAsyncTCP 1.2.4 / core 3.1.2:
//AsyncServer::_accept() allocates the AsyncClient with nothrow new, but AsyncClient's constructor
//calls std::make_shared, which uses the throwing global operator new. With exceptions disabled the
//core's operator new aborts the whole device on the first failed malloc(), so a short burst of
//connections can kill it even with ~18 KB free.
//
//ESPAsyncTCP is a globally installed library, so it isn't patched. Redefining operator new in the
//sketch fails (the core's definition is a strong symbol: "multiple definition of `_Znwj`"), so the
//linker's --wrap is used instead (`-Wl,--wrap=_Znwj`, see firmware/README.md): external calls to
//operator new(size_t) land in __wrap__Znwj, __real__Znwj stays the original. A .cpp, not a .ino -
//see ServiceMemoryReserveFunctions.h.
#include <Arduino.h>
#include "ServiceMemoryReserveFunctions.h"

extern "C" void* __real__Znwj(size_t size);

//Only has to cover the small make_shared allocation in AsyncClient's constructor, not act as a
//general cushion - more would just cost permanent heap.
#define EMERGENCY_RESERVE_SIZE 512
static void* emergencyReservePool = nullptr;

//Re-establishes the reserve when it's empty, but only with comfortable free heap (crashes started
//around 8000 bytes) - it's meant for a brief spike, not to compete with a real shortage. Called at
//the end of setup() and once per second from loop().
#define EMERGENCY_RESERVE_MIN_FREE_HEAP 10000
void maintainEmergencyMemoryReserve() {
  if (emergencyReservePool != nullptr) {
    return; //already holding the reserve
  }
  if (ESP.getFreeHeap() < EMERGENCY_RESERVE_MIN_FREE_HEAP) {
    return; //don't compete with a real low-memory situation for room to hold a spare reserve
  }
  emergencyReservePool = malloc(EMERGENCY_RESERVE_SIZE);
}

extern "C" void* __wrap__Znwj(size_t size) {
  void* result = malloc(size);
  if (result != nullptr || size == 0) {
    return result; //normal success path, byte-for-byte identical to the unwrapped original
  }
  if (emergencyReservePool != nullptr) {
    //Plain Serial.println, no String - we're on the failing allocation path.
    Serial.println(F("WARNING: operator new failed - releasing emergency memory reserve and retrying"));
    free(emergencyReservePool);
    emergencyReservePool = nullptr;
    void* retryResult = malloc(size);
    if (retryResult != nullptr) {
      return retryResult;
    }
  }
  //No reserve left or the retry failed too: a real OOM. Hand over to the original, which aborts as
  //before (and records umm_last_fail_alloc_addr/_size).
  return __real__Znwj(size);
}
