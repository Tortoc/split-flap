#!/usr/bin/env bash
# Compiles ESPMaster with the three settings this project treats as mandatory, not optional:
#
#   1. :eesz=4M1M - the real chip is 4MB (see GET /api/diag?type=heap's flashReal field), but the
#      bare "esp8266:esp8266:generic" FQBN silently defaults to the much smaller 1M64 layout
#      instead. LittleFS lives at flash offset 0x300000, matching ONLY the 4M1M layout - a sketch
#      compiled without this flag expects LittleFS at a different offset entirely. This exact
#      mistake (omitting :eesz=4M1M) has caused real incidents twice: once making the device look
#      factory-reset (Punkt 136), and once (2026-09-16) causing deterministic OTA chunk-upload
#      failures at a fixed byte offset that were first misdiagnosed as WiFi flakiness. See
#      README.md in this folder for the flash-layout background.
#   2. -Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept - required since Punkt 207/209 (TCP connection-flood
#      fix); ServiceMemoryReserveFunctions.cpp/ServiceConnectionGateFunctions.cpp reference
#      __real__Znwj/__real_tcp_accept, which only exist as linkable symbols when the linker is
#      actually invoked with these --wrap flags. The build fails loudly without it - intentional,
#      see README.md.
#   3. ,mmu=3232 - the full 32KB flash cache. mmu=4816H (4.10.11-5.0.0) halved the cache for a
#      second heap in IRAM (OTA buffers); with 5.0.0's code size that made outgoing TCP connections
#      stall for 2-4 s (first data segment only sent on retransmit), so the primary never reached
#      its extensions (firmware/BUILD_INFO.md Punkt 260). The OTA buffers (HeapSelectIram) fall back
#      to DRAM.
#
# Usage: ./compile-espmaster.sh [extra arduino-cli args...]
# Run from anywhere - always compiles the ESPMaster/ directory next to this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPMASTER_DIR="$(cd "$SCRIPT_DIR/../ESPMaster" && pwd)"

exec arduino-cli compile \
  --fqbn "esp8266:esp8266:generic:eesz=4M1M,mmu=3232" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept" \
  "$@" \
  "$ESPMASTER_DIR"
