#!/usr/bin/env bash
# Compiles ESPMaster with the three settings it needs (details: README.md in this folder):
#
#   1. :eesz=4M1M - 4 MB flash with LittleFS at 0x300000. The bare "esp8266:esp8266:generic" FQBN
#      silently uses the much smaller 1M64 layout: the device then can't find its settings (looks
#      factory-reset) and OTA uploads fail at a fixed offset.
#   2. -Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept - the TCP connection gate and memory reserve
#      (ServiceConnectionGateFunctions.cpp, ServiceMemoryReserveFunctions.cpp) call
#      __real__Znwj/__real_tcp_accept, which only exist with these flags. Without them the build
#      fails on purpose.
#   3. mmu=3232 - the full 32 KB flash cache. mmu=4816H (4.10.11-5.0.0) delayed outgoing TCP
#      connections by 2-4 s, so a primary couldn't reach its extensions.
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
