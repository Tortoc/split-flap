//Primary defense against the TCP connection-flood crash (crash chain: see
//ServiceMemoryReserveFunctions.cpp; its emergency reserve is only the secondary net).
//
//lwIP calls the tcp_accept() callback for every new connection BEFORE ESPAsyncTCP builds an
//AsyncClient for it, so rejecting here (tcp_abort()) skips the vulnerable allocation entirely.
//Intercepted via GNU ld's --wrap (`-Wl,--wrap=tcp_accept`, see firmware/README.md). This build has
//only one real registration (AsyncServer::begin()), so a single global slot for the wrapped
//callback is enough. A .cpp, not a .ino - see ServiceMemoryReserveFunctions.h.
//
//Gates on the actual resource condition (largest free block and total free heap, checked at every
//accept) instead of a timing heuristic: a minimum spacing between connections was tried first and
//rejected normal fast sequential requests, OTA chunk uploads included.
#include <Arduino.h>
#include <lwip/priv/tcp_priv.h>
#include "ServiceMemoryReserveFunctions.h"

extern "C" void __real_tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);

static tcp_accept_fn wrappedOriginalAcceptFn = nullptr;

//No String or concatenated Serial output in this path: it runs inside lwIP's TCP input processing,
//and allocating here is what the gate exists to avoid. Counter only, see GET /api/diag?type=tcp.
unsigned long connectionGateRejectedCount = 0;

//Sustained-low-heap threshold - below this the device has crashed before.
#define CONNECTION_GATE_MIN_FREE_HEAP 8000

//The allocation that crashed needed only 24 bytes; 4000 leaves room for everything a new connection
//allocates right after accept, while staying far below the normal largest free block (17-19 KB
//after boot), so it only trips when contiguous heap is genuinely exhausted.
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
    //Deregistration (AsyncServer::end()) - nothing to gate. Not reached at runtime
    //(webServer.end() is never called).
    wrappedOriginalAcceptFn = nullptr;
    __real_tcp_accept(pcb, nullptr);
    return;
  }
  wrappedOriginalAcceptFn = accept;
  __real_tcp_accept(pcb, gateNewConnection);
}
