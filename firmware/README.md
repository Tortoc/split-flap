# Building and Flashing the Firmware

This folder holds the build script for ESPMaster and the build and flashing notes for all three
components. Prebuilt binaries are not part of the repository: they are attached to the
[GitHub releases](https://github.com/Tortoc/split-flap/releases), one release per component tag:

| Release tag | Files |
| --- | --- |
| `esp-vX.Y.Z` | `ESPMaster.bin` - sketch binary for the ESP8266<br>`littlefs-en.bin`, `littlefs-de.bin` - file system image with the English or German web UI<br>`index-en.html.gz`, `index-de.html.gz` - the web UI alone, for `POST /api/web-file` |
| `unit-vX.Y.Z` | `Unit.hex` - unit firmware as Intel HEX (for ISP)<br>`Unit.bin` - the same firmware as a raw binary (for the I2C update push from the ESPMaster web UI, see [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md)) |
| `twiboot-vX.Y` | `twiboot.hex` - bootloader as Intel HEX (for ISP)<br>`twiboot.bin` - the same as a raw binary |

The binaries are built by hand from the tagged sources (CI only checks that the code compiles).
For development, the sources in [`ESPMaster/`](../ESPMaster), [`Unit/`](../Unit) and
[`Unit/bootloader/twiboot/`](../Unit/bootloader/twiboot) are authoritative.

## Building

### ESPMaster

⚠️ **Always build with [`compile-espmaster.sh`](compile-espmaster.sh), don't type
`arduino-cli compile` by hand.** The script hardcodes the board options and linker flags below.
Both have been forgotten before, each time followed by hours of searching in the wrong direction
(it looked like flaky WiFi). The command is shown only to document what the script does:

```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic:eesz=4M1M,mmu=3232" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept" \
  ESPMaster
```

**`eesz=4M1M` is part of the FQBN, not an optional extra.** The ESP-12F has 4 MB of flash; this
layout gives a ~1019 KB OTA slot and 1 MB of LittleFS at `0x300000`. A build with the bare
`esp8266:esp8266:generic` compiles without any error but uses a much smaller layout: the real
LittleFS partition and the stored WiFi credentials become invisible (the device behaves as if
reset: setup hotspot, empty settings) and the OTA slot shrinks to ~438 KB, so even a web OTA
update with the correct build fails halfway through. Recovery is only possible over the serial
cable (`arduino-cli upload` or `esptool.py`), which writes to absolute flash addresses regardless
of the running layout. **After every cable flash, compare `flashConfigured` with `flashReal` in
`GET /api/diag?type=heap`.** Do the same before building with this profile for a different module:
a layout larger than the real chip causes address errors at boot.

**The two `-Wl,--wrap=...` flags are mandatory.** `ServiceMemoryReserveFunctions.cpp` and
`ServiceConnectionGateFunctions.cpp` (protection against TCP connection floods and out-of-memory
aborts) define `__wrap__Znwj`/`__wrap_tcp_accept` and call `__real__Znwj`/`__real_tcp_accept`,
which only exist when the linker runs with these flags. Without them the build fails with
"undefined reference to `__real__Znwj`" - on purpose, a silent fallback to unprotected behavior
would be worse. The ESP8266 core uses the same mechanism itself (`compiler.c.elf.flags` in
`platform.txt`: `-Wl,-wrap,system_restart_local -Wl,-wrap,spi_flash_read`).

**`mmu=3232`** keeps the full 32 KB flash cache. ESPMaster 4.10.11 to 5.0.0 used `mmu=4816H`
(16 KB cache plus a second heap in IRAM for the two 4 KB OTA buffers). With the code size of 5.0.0
that delayed outgoing TCP connections by 2-4 s (the first data segment was only sent on
retransmit), so a primary could no longer reach its extensions. The OTA buffers now live in DRAM
(`iramFree` in `GET /api/diag?type=heap` is 0); an upload on a very low heap can fail, in which
case the old firmware stays active.

### Web UI (littlefs.bin)

The web UI exists in English and German. [`ESPMaster/data/index.html`](../ESPMaster/data/index.html)
is the English source; [`ESPMaster/i18n/de.json`](../ESPMaster/i18n/de.json) maps its texts to
German. [`build-webui.py`](build-webui.py) (Python 3, standard library only) builds both languages:

```bash
firmware/build-webui.py            # writes build/web/
firmware/build-webui.py --check    # only checks the dictionaries (also run by CI)
```

Per language it writes `index-<lang>.html.gz` and `littlefs-<lang>.bin`. The page is stored
gzip-compressed as `index.html.gz` on the device; `ESPAsyncWebSrv` serves `.gz` files with the right
`Content-Encoding` automatically, and the page is ~55 % smaller on the wire. `mklittlefs` ships with
the ESP8266 core (`~/.arduino15/packages/esp8266/tools/mklittlefs/`, found automatically, or pass
`--mklittlefs`). Its parameters (`-p 256 -b 8192 -s 1024000`) match `spiffs_pagesize`,
`spiffs_blocksize` and `spiffs_start`/`spiffs_end` of `eesz=4M1M` in the core's `boards.txt`.

Translating: only these parts of `index.html` are replaced, so code is never changed by accident -
text between two tags (also with inline tags such as `<code>` or `<b>` inside, then the whole
inner HTML is the key), the attributes `title`, `placeholder`, `aria-label` and `value`, and JS
string literals wrapped in `T(...)`. Wrap every new user-visible JS string in `T(...)` and add it
to `de.json`; `--check` fails on a `T(...)` string without translation and on dictionary entries
that no longer match anything. Dictionary values for JS strings are JS source text, so a `"`
inside a `"..."` string is written as `\"` (in JSON: `\\\"`).

### Unit

The ATmega328P on the fae-pcb runs at 16 MHz like a Nano; the Nano profile with the old bootloader
matches its fuses (`hfuse=0xDA`, see [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md)):

```bash
arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old Unit --output-dir build
avr-objcopy -I ihex -O binary build/Unit.ino.hex build/Unit.ino.bin
```

### Bootloader

Patched twiboot, see [`Unit/bootloader/twiboot/main.c`](../Unit/bootloader/twiboot/main.c) for the
changes and attribution:

```bash
cd Unit/bootloader/twiboot
make MCU=atmega328p CFLAGS_TARGET="-DUSE_CLOCKSTRETCH=1"
```

## Flashing

- **ESPMaster over the serial cable:** `ESPMaster.bin` at `0x0`, then `littlefs-en.bin` or
  `littlefs-de.bin` at `0x300000`, with `arduino-cli upload` or `esptool.py` (if the default
  stub-based upload fails partway through, use `esptool.py --no-stub`).
- **ESPMaster from the browser:** **☰ Menu > Firmware update > ESPMaster** uploads the sketch in
  4 KB chunks (`/api/firmware-start`/`-chunk`/`-finish`); the file system is not touched. The web
  UI alone is replaced with `POST /api/web-file`; the file name must be `index.html.gz`, so upload
  `index-de.html.gz` as `-F "file=@index-de.html.gz;filename=index.html.gz"`. This also switches the
  language. The serial cable stays useful for the serial log, but normal updates don't need it.
- **ESPMaster 5.0.0 and later:** the first start converts the admin password into a hash. A
  primary older than 5.0.0 can't drive a password-protected extension running 5.0.0 or later -
  update both devices, then pair them (see the main [README](../README.md#multiple-displays)).
- **Unit firmware:** once the unit runs twiboot, push `Unit.bin` over I2C from the web UI
  (**☰ Menu > Firmware update > Units**). Over ISP, `Unit.hex` goes on with `avrdude -D` only
  right after the bootloader was written (blank application area); overwriting an existing
  application with `-D` corrupts it, without `-D` the chip erase removes the bootloader (see
  [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md)).
- **Bootloader:** `twiboot.hex` with an ISP programmer, see
  [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md) for the full procedure including the
  fuses - **once per unit, on already-built hardware; go through the test plan there first.**
