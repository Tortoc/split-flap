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
#      see README.md. -Wl,--wrap=wifi_set_sleep_type (since ESPMaster 4.10.13) keeps Update.begin()
#      from switching the WiFi sleep type mid-session, which sporadically hung OTA uploads
#      (ServiceWifiSleepFunctions.cpp).
#   3. ,mmu=4816H - 16KB flash cache + 48KB IRAM, the IRAM not used by code becoming a second heap
#      (~19KB). Only allocations made under HeapSelectIram land there - currently just the two 4KB
#      OTA buffers (code-review-V7 Punkt C). Without it the build still works, the OTA buffers just
#      fall back to DRAM (the old behavior).
#
# Usage: ./compile-espmaster.sh [extra arduino-cli args...]
# Run from anywhere - always compiles the ESPMaster/ directory next to this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPMASTER_DIR="$(cd "$SCRIPT_DIR/../ESPMaster" && pwd)"

exec arduino-cli compile \
  --fqbn "esp8266:esp8266:generic:eesz=4M1M,mmu=4816H" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept -Wl,--wrap=wifi_set_sleep_type" \
  "$@" \
  "$ESPMASTER_DIR"
