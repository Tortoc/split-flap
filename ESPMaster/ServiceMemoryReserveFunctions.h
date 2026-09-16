//Declarations for ServiceMemoryReserveFunctions.cpp / ServiceConnectionGateFunctions.cpp - kept
//in a real header, not a .ino, deliberately: the actual __wrap_*/extern "C" definitions live in
//plain .cpp files (NOT .ino) because Arduino's auto-prototype-generation (ctags-based, runs only
//over .ino files) generates a prototype for every top-level function it sees, but strips the
//`extern "C"` qualifier while doing so - the resulting mismatched-linkage prototype conflicts
//with the real `extern "C"` definition later in the same file ("conflicting declaration ... with
//'C' linkage" vs. "'C++' linkage"). A plain .cpp file is compiled as an ordinary translation unit,
//with no prototype generation at all, so this conflict can't arise. See both .cpp files' own
//top comments for the full story.
#ifndef SERVICE_MEMORY_RESERVE_FUNCTIONS_H
#define SERVICE_MEMORY_RESERVE_FUNCTIONS_H

void maintainEmergencyMemoryReserve();

extern unsigned long connectionGateRejectedCount;

#endif
