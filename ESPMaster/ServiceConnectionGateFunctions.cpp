//Primary defense for BUGFIX_LOG Punkt 207 (see ServiceMemoryReserveFunctions.cpp's top comment
//for the full crash chain this addresses). The emergency-reserve rescue there is a secondary
//safety net for whatever slips through; THIS is what's meant to stop the actual observed failure
//mode - a burst of near-simultaneous new connections, each triggering AsyncClient's constructor
//and its internal allocation, before any of the earlier ones have had a chance to finish and free
//theirs.
//
//lwIP calls the accept callback registered via tcp_accept() for every new incoming connection on
//the web server's listening socket, BEFORE ESPAsyncTCP constructs any C++ object for it -
//rejecting a connection here (tcp_abort()) means the vulnerable allocation in AsyncClient's
//constructor never runs for it at all. Intercepted the same way as operator new: GNU ld's --wrap
//(`-Wl,--wrap=tcp_accept`, same required build flag as _Znwj - see firmware/README.md), since
//tcp_accept() is a plain function (not a macro, verified in lwip/tcp.h) and this device only has
//ONE real registration in its whole dependency tree (verified via grep across every installed
//library actually linked into this build: AsyncServer::begin(), ESPAsyncTCP.cpp - WiFiServer's/
//AsyncTCP's own tcp_accept() callers exist in OTHER installed libraries but aren't used by this
//project; AsyncServer::end() also calls tcp_accept(_pcb, NULL) to deregister, but only when the
//whole server stops, never per-connection - verified via addr2line against the two other call
//sites this build actually contains, both AsyncServer::begin()/::end() themselves, nothing else).
//A single global slot for the wrapped callback is enough; no general multi-listener registry is
//needed.
//
//A plain .cpp file, not a .ino - see ServiceMemoryReserveFunctions.h's own comment for why
//(applies identically here: extern "C" + Arduino's .ino auto-prototype-generation don't mix).
//
//FIRST DESIGN (2026-09-12, reverted same day - see BUGFIX_LOG Punkt 208) used a minimum spacing
//between accepted connections (a simple "reject if less than 40ms since the last one" timer).
//Live-deployed, this rejected completely normal, non-overlapping sequential requests - a tight
//loop of single curl calls against this same LAN, each with a real but fast (~10-30ms)
//round-trip, can have LESS than 40ms between one connection's accept and the next one's arrival
//even though the two are never actually concurrent. That's a timing PROXY for "too many at once",
//not the real condition - and it clearly has false positives common enough to break ordinary use,
//not just genuine floods. Punkt 208 also found (the hard way - see the OTA chunk-upload stall
//there) that even paced application traffic hits this closer than expected.
//
//THIS design drops the timing heuristic entirely and gates on the actual resource condition
//instead: ESP.getMaxFreeBlockSize() (largest contiguous free heap block), checked fresh at every
//accept. A failed allocation happens when no contiguous block big enough exists - not merely
//because "another connection was accepted recently" - so this can never reject fast-but-genuinely-
//sequential traffic while heap health is fine, only an actual squeeze (whether from one connection
//burst or anything else eating heap at the time). ESP.getFreeHeap() is checked too, independently,
//for a real sustained low-memory condition that a single healthy contiguous block wouldn't catch
//on its own (highly fragmented-but-technically-adequate total free bytes is the opposite risk).
#include <Arduino.h>
#include <lwip/priv/tcp_priv.h>
#include "ServiceMemoryReserveFunctions.h"

extern "C" void __real_tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);

static tcp_accept_fn wrappedOriginalAcceptFn = nullptr;

//Deliberately no String/Serial print with concatenation anywhere in this function or
//__wrap_tcp_accept() below - both run inside lwIP's own TCP input processing, ahead of any C++
//object existing yet for the connection being evaluated; building a String here would itself be
//exactly the kind of allocation this whole mitigation exists to avoid piling on to. Plain counters
//only, exposed via GET /api/diag?type=tcp for after-the-fact monitoring instead of logging live.
unsigned long connectionGateRejectedCount = 0;

//This project's own established "danger zone" (BUGFIX_LOG #41-43/#51) for the sustained-low-heap
//case.
#define CONNECTION_GATE_MIN_FREE_HEAP 8000

//The actual failed allocation in Punkt 207 needed well under 100 bytes (ACErrorTracker's
//make_shared control block, verified via objdump: 24 bytes for the specific allocation that
//crashed). This threshold is deliberately much larger than that single allocation - covers
//everything else a freshly-accepted connection's setup does in short order (AsyncClient itself,
//lwIP's own per-connection bookkeeping, the first bit of request parsing) with real headroom,
//without being anywhere near this device's normal largest-free-block value (17-19KB after boot,
//per this session's own measurements) - so it only trips when something has genuinely eaten deep
//into contiguous heap, not during ordinary operation.
#define CONNECTION_GATE_MIN_FREE_BLOCK 4000

static err_t gateNewConnection(void *arg, struct tcp_pcb *newpcb, err_t err) {
  bool heapTooLow = ESP.getFreeHeap() < CONNECTION_GATE_MIN_FREE_HEAP;
  bool blockTooSmall = ESP.getMaxFreeBlockSize() < CONNECTION_GATE_MIN_FREE_BLOCK;

  if (heapTooLow || blockTooSmall) {
    connectionGateRejectedCount++;
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  return wrappedOriginalAcceptFn(arg, newpcb, err);
}

extern "C" void __wrap_tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept) {
  if (accept == nullptr) {
    //Deregistration (AsyncServer::end(), server fully stopping) - nothing to gate, pass straight
    //through. Never fires in this project's actual runtime (webServer.end() is never called),
    //kept only because AsyncServer's own destructor path could reach it.
    wrappedOriginalAcceptFn = nullptr;
    __real_tcp_accept(pcb, nullptr);
    return;
  }
  wrappedOriginalAcceptFn = accept;
  __real_tcp_accept(pcb, gateNewConnection);
}
