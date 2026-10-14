//Declarations for ServiceMemoryReserveFunctions.cpp / ServiceConnectionGateFunctions.cpp. Both
//are plain .cpp files because Arduino's prototype generation for .ino files drops `extern "C"`,
//and the generated prototype then conflicts with the real extern "C" __wrap_* definition.
#ifndef SERVICE_MEMORY_RESERVE_FUNCTIONS_H
#define SERVICE_MEMORY_RESERVE_FUNCTIONS_H

void maintainEmergencyMemoryReserve();

extern unsigned long connectionGateRejectedCount;

#endif
