//Mitigation for BUGFIX_LOG Punkt 207: a burst of simultaneous incoming TCP connections reliably
//crashed this device (reason:254 "user exception"/abort) - decoded via addr2line, the stack has
//NO I2C involvement at all (ruling out the older, long-suspected Wire.requestFrom() hang theory).
//Root cause chain, verified against the actually-installed ESPAsyncTCP 1.2.4 and ESP8266 Core
//3.1.2 sources:
//  lwIP accepts a new TCP connection -> AsyncServer::_accept() (ESPAsyncTCP.cpp) constructs
//  `new (std::nothrow) AsyncClient(pcb)` (that outer allocation IS nothrow-safe) -> but
//  AsyncClient's OWN constructor separately does `std::make_shared<ACErrorTracker>(...)`, which
//  uses the plain, THROWING global `operator new` internally, not the nothrow form. This core
//  builds with C++ exceptions disabled, so its own abi.cpp defines `operator new(size_t)` to call
//  `__unhandled_exception()` (= abort) the moment malloc() returns NULL - there is no exception
//  path left for a failed make_shared to be recovered through. A burst of near-simultaneous new
//  connections, each hitting this same small internal allocation within the same short window, is
//  exactly the kind of transient spike that can turn an otherwise perfectly healthy free-heap
//  number (this device measured ~18KB free at the time) into a single failed malloc() - and that
//  single failure aborts the whole device instead of just failing the one connection.
//
//ESPAsyncTCP itself is a globally-installed library (~/Arduino/libraries/ESPAsyncTCP), not
//tracked in this repo - patching it in place wouldn't survive a reinstall/update (same reasoning
//as never patching WiFiManager for the unrelated AUTH_EXPIRE crash). A direct sketch-local
//`operator new` override was tried first and rejected: the core's own abi.cpp definition is a
//normal STRONG symbol despite its "overwrite weak operators new" comment, so redefining it causes
//a "multiple definition of `_Znwj`" link error.
//
//Fix instead uses the GNU linker's --wrap mechanism (`-Wl,--wrap=_Znwj`, added via
//`compiler.c.elf.extra_flags` - see firmware/README.md for the required build command; this core
//ALREADY uses the identical mechanism for two other symbols, `-Wl,-wrap,system_restart_local
//-Wl,-wrap,spi_flash_read`, see platform.txt's `compiler.c.elf.flags`) - this redirects every
//EXTERNAL reference to `operator new(size_t)` (mangled `_Znwj` on this 32-bit target) to
//__wrap__Znwj below, while __real__Znwj remains a direct alias for abi.cpp's untouched original.
//No symbol collision, since nothing here re-defines `_Znwj` itself.
//
//This alone only rescues ONE failed allocation via a small emergency reserve - see
//maintainEmergencyMemoryReserve()'s own comment for why the connection-rate/heap gate in
//ServiceConnectionGateFunctions.cpp is the PRIMARY defense and this is a secondary safety net.
//
//A plain .cpp file, not a .ino - see ServiceMemoryReserveFunctions.h's own comment for why.
#include <Arduino.h>
#include "ServiceMemoryReserveFunctions.h"

extern "C" void* __real__Znwj(size_t size);

//Deliberately small - this only needs to cover ACErrorTracker's own small make_shared control
//block+object (a handful of small member fields, well under 100 bytes in practice), not a general
//low-memory cushion. Bigger doesn't help this specific mitigation and just costs permanent heap.
#define EMERGENCY_RESERVE_SIZE 512
static void* emergencyReservePool = nullptr;

//(Re-)establishes the reserve whenever it's currently empty (never allocated yet, or consumed by
//a rescue below). Only while free heap is comfortably above this project's own established
//"danger zone" (BUGFIX_LOG #41-43/#51 - this device has historically started genuinely crashing
//from low heap around 8000 bytes free) - deliberately never competes with a real, sustained
//low-memory situation for room to hold a reserve that only helps a brief spike anyway. Called once
//at the end of setup() and once per second from loop() (ESPMaster.ino).
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
    //Plain Serial.println (no String concatenation) deliberately - this runs on the same
    //failure path we're trying to rescue FROM; building a String here could itself need an
    //allocation that fails for the same reason.
    Serial.println(F("WARNING: operator new failed - releasing emergency memory reserve and retrying (Punkt 207)"));
    free(emergencyReservePool);
    emergencyReservePool = nullptr;
    void* retryResult = malloc(size);
    if (retryResult != nullptr) {
      return retryResult;
    }
  }
  //Reserve unavailable or the retry still failed - a genuine, unrescued out-of-memory condition.
  //Delegate to the untouched original implementation (abi.cpp): it re-attempts malloc(size) once
  //more (same deterministic failure) and then calls __unhandled_exception() exactly as it always
  //did - unchanged behavior for a real, sustained OOM condition. Also updates
  //umm_last_fail_alloc_addr/_size itself, so no need to duplicate that here.
  return __real__Znwj(size);
}
