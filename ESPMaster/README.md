# ESPMaster

Firmware for the ESP8266 that drives the display: web UI, WiFi, time keeping, and the I2C link to
the units. Current version: `espVersion` in `ESPMaster.ino` (git tags `esp-vX.Y.Z`).

- [`API.md`](API.md) - every HTTP endpoint, with `curl` examples.
- [`../firmware/README.md`](../firmware/README.md) - build options, prebuilt binaries, flashing.
- [`../firmware/BUILD_INFO.md`](../firmware/BUILD_INFO.md) - what changed in each build.
- [`../README.md`](../README.md) - hardware, setup, security and multi-device overview.

## Building

Always build with [`../firmware/compile-espmaster.sh`](../firmware/compile-espmaster.sh). It sets
the three options this firmware depends on:

- FQBN `esp8266:esp8266:generic:eesz=4M1M,mmu=4816H` - 4 MB flash with 1 MB LittleFS at `0x300000`.
  A build without `eesz=4M1M` compiles but can't find its settings and breaks OTA updates.
  `mmu=4816H` adds a second ~19 KB heap in IRAM, used for the OTA buffers.
- `-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept` - the TCP connection gate and memory reserve
  (`ServiceConnectionGateFunctions.cpp`, `ServiceMemoryReserveFunctions.cpp`). The link fails
  without them, on purpose.

The Arduino IDE does not pass these linker flags by default, so a plain IDE build fails.

The web UI is `data/index.html`. The device serves it gzip-compressed from LittleFS
(`index.html.gz`); see `firmware/README.md` for building `littlefs.bin`, or replace just the page
with `POST /api/web-file`.

## Feature flags

At the top of `ESPMaster.ino`:

| Flag | Default | Effect |
| --- | --- | --- |
| `SERIAL_ENABLE` | `true` | Serial log at `SERIAL_BAUDRATE` (115200). |
| `UNIT_CALLS_DISABLE` | `false` | No I2C calls to the units - for testing the ESP alone. |
| `WEB_FIRMWARE_UPDATE_ENABLE` | `true` | Firmware upload from the browser (**⋮ menu > Firmware Update**). |
| `UNIT_BOOTLOADER_ENABLE` | `true` | Push `Unit.ino` updates over I2C via twiboot, see [`../Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md). |
| `UNITS_AMOUNT_MAX` | `64` | Highest unit count; the real count is detected at startup (I2C scan from address 0). |
| `WIFI_USE_DIRECT` | `false` | `false`: WiFiManager setup hotspot `Split-Flap-AP`. `true`: fixed credentials in `wifiDirectSsid`/`wifiDirectPassword`. |
| `WIFI_STATIC_IP` | `false` | Static IP from `wifiDeviceStaticIp` etc. instead of DHCP. |
| `USE_MULTICAST` | `true` | mDNS name `<name>.local`; the name is set in the web UI and must be unique per device. |
| `LED_ENABLE` | `true` | Error blink on `LED_DEBUG_PIN` (GPIO 2) when WiFi failed at startup - only while debug mode is on. |
| `DEBUG_ENABLE` | `true` | Startup debug page, error panel and serial log in the web UI, switched at runtime with `POST /api/debug-mode`. |
| `MQTT_ENABLE` | `false` | MQTT / Home Assistant integration. Off saves the PubSubClient buffer. |
| `FAE_MOD` | defined | fae-pcb with ESP-12F module. |

Settings that used to be compile-time constants (time zone, mDNS name, password, quiet hours,
device role, ...) are set in the web UI and stored on LittleFS.

## Source files

| File | Content |
| --- | --- |
| `ESPMaster.ino` | Flags, globals, `setup()`/`loop()`, all web handlers. |
| `ServiceDisplayJobFunctions.ino`, `ServiceFlapFunctions.ino` | Non-blocking display updates, line splitting, alignment, animations. |
| `ServiceRotationFunctions.ino`, `ServiceScheduledMessageFunctions.ino`, `ServiceCountdownFunctions.ino`, `ServiceCountupFunctions.ino`, `ServiceQuietHoursFunctions.ino` | Display modes and schedules. |
| `ServiceAccessControlFunctions.ino` | Admin password (PBKDF2 hash), login sessions, per-category access control. |
| `ServiceExtensionFunctions.ino` | Multi-device link: health checks, signed display pushes, pairing. |
| `ServiceWifiFunctions.ino` | WiFiManager setup hotspot and its password on the display. |
| `ServiceAsyncTimeSyncFunctions.ino`, `ServiceRtcFunctions.ino` | Non-blocking NTP/time zone lookup, optional DS3231 RTC. |
| `ServiceFileSystemFunctions.ino` | LittleFS settings, boot snapshot, JSON files. |
| `ServiceUnitFirmwareFunctions.ino`, `ServiceI2CRecoveryFunctions.ino` | Unit firmware over I2C (twiboot), I2C bus recovery. |
| `ServiceCrashLogFunctions.*`, `CrashLog.h` | On-device crash log (`GET /api/crash-log`). |
| `ServiceConnectionGateFunctions.cpp`, `ServiceMemoryReserveFunctions.*` | Protection against TCP connection floods and out-of-memory aborts. |
| `ServiceMqttFunctions.ino` | MQTT, only with `MQTT_ENABLE`. |
| `Helpers*.ino`, `Classes.h` | Serial wrappers, string helpers, shared structs. |

## Diagnostics

- Serial log (115200 baud) - also prints the setup hotspot password when the hotspot opens.
- Debug mode (`POST /api/debug-mode`): startup debug page and in-page serial log.
- `GET /api/crash-log` - exceptions and resets, kept across reboots.
- `GET /api/diag` - heap, flash layout, WiFi and other diagnostics (`type=heap`, `type=health`, ...).
  After a cable flash, compare `flashConfigured` with `flashReal` in `type=heap`.

Free heap after boot is only about 20 KB; avoid large allocations, and build HTML for firmware
pages sparingly.

## Security and multiple displays

See the [Security](../README.md#security) and [Multiple Displays](../README.md#multiple-displays)
sections of the main README; the protocol details are in [`API.md`](API.md#authentication).

## History

Started from the split-flap-scientress ESPMaster v2.3.0 ("Scott" version: non-blocking NTP
sync, `yield()` in display loops, WiFi strength indicator, LED setup codes). Most of that has
since been reworked; the per-version changes are in `firmware/BUILD_INFO.md` and the git tags.
