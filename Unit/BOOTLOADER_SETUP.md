# Bootloader Setup for I2C Firmware Updates (patched twiboot)

This describes the **one-time, physical** step that prepares a unit (the ATmega328P on the
[fae-pcb](https://github.com/codingcatgirl/split-flap-fae-pcb)) for firmware updates over I2C.
Afterwards, `Unit.ino` updates for that unit are rolled out from the ESPMaster web UI
(**☰ Menu > Firmware update > Units**), without touching it with an ISP programmer again.

> ⚠️ **This works on real, already-built hardware.** Go through the whole procedure on a spare
> unit first (see the test plan below) before touching an installed one. A wrong fuse value can
> make a unit unusable until it is rescued with a high-voltage programmer - see
> [Clock configuration is hardware-specific](#clock-configuration-is-hardware-specific); this
> happened while developing the patch.

## Fuses must match the fae-pcb

The unit is a bare ATmega328P on the fae-pcb with 3.3 V logic and its own 16 MHz crystal (Y2 in
the schematic). **The fuses and clock settings of the bootloader have to match exactly this
board**, not a generic Nano and not twiboot's own example values - see below.

## Why "patched"?

A unit's I2C address is set in hardware with the address jumpers on J4 (see "Set Unit Address" in
the main [README](../README.md#set-unit-address)), and that should work for firmware updates just
like in normal operation. Upstream [twiboot](https://github.com/orempel/twiboot) only knows a
fixed, compiled-in I2C address.

`Unit/bootloader/twiboot/main.c` is therefore a **lightly patched copy** (see the comment block at
the top of the file for the changes and attribution). At startup the bootloader reads the same
six address pins as `Unit.ino`'s `getaddress()` (A0-A3, D3, D4 - addresses 0-63, see the comment
on `addressPins` in `Unit.ino`) and answers on `0x29 + <jumper value>`. The result:

- **One** compiled bootloader image for **all** units - no per-unit address bookkeeping.
- Moving a jumper takes effect **immediately** for bootloader and application alike, without
  reflashing the bootloader.

## What you need

- An ISP programmer (e.g. USBasp, AVRISP mkII, or a second Arduino as
  ["Arduino as ISP"](https://docs.arduino.cc/built-in-examples/arduino-isp/ArduinoISP/))
- `avr-gcc`, `make` and `avrdude` (part of the Arduino AVR toolchain, or the `avr-gcc`/`avr-libc`/
  `avrdude` packages of your Linux distribution)
- The files in `Unit/bootloader/twiboot/` (vendored, no submodule or checkout needed)

## Clock configuration is hardware-specific

The first draft of the patch used twiboot's example fuses (`lfuse=0xC2`, internal 8 MHz RC
oscillator). **That was wrong for this hardware** and stopped the stepper motor completely, because
the timing ran at the wrong clock rate. The next attempt with a generic "Arduino Nano" value got
the bootloader size wrong. Only **reading the fuses of an untouched, originally programmed spare
unit** gave the correct values - measure, don't guess:

```bash
avrdude -c usbasp -p m328p -B 125kHz -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h
```

Verified on real hardware:

| Fuse | Value | Meaning |
| --- | --- | --- |
| `lfuse` | `0xFF` | External crystal (16 MHz, Y2), NOT twiboot's example `0xC2` (internal 8 MHz RC) |
| `hfuse` | `0xDA` | 1024-word bootloader section starting at `0x7800` - NOT twiboot's `0xDC`/512 words/`0x7C00` |
| `efuse` | `0xFD` | 2.7 V brown-out detection (unchanged from twiboot's example) |

To match, `main.c` uses `F_CPU=16000000ULL` (not twiboot's 8 MHz) and `TIMER_IRQFREQ_MS=12` (not
twiboot's 25 - at 16 MHz the 8-bit Timer0 would overflow otherwise, see the comments in the code).
All four values (`lfuse`, `hfuse`/`BOOTLOADER_START`, `F_CPU`, `TIMER_IRQFREQ_MS`) must fit each
other AND the real hardware - **a wrong value produces no error message, the chip simply runs at
the wrong clock rate or with the bootloader at the wrong address.** These values are the defaults
in the `Makefile` and in `main.c`. If you use a different hardware revision, verify them against an
untouched unit first as shown above instead of copying them blindly.

## Step 1: Build and check the flash budget (before every ISP flash)

```bash
cd Unit/bootloader/twiboot
make clean
make MCU=atmega328p CFLAGS_TARGET="-DUSE_CLOCKSTRETCH=1"
```

`-DUSE_CLOCKSTRETCH=1` makes the bootloader hold the I2C clock line (clock stretching) while it
writes flash, which ESPMaster supports natively (`Wire.setClockStretchLimit()`) - without it
ESPMaster would have to guess the timing.

**Then check the `avr-size` output** that `make` prints (`text` + `data` is the flash actually
used; `bss` is RAM only). The bootloader section is limited by the BOOTSZ fuse to **1024 words =
2048 bytes** (`hfuse=0xDA`, see above). The current build uses **792 of 2048 bytes** (39 %,
`avr-gcc` 16.1.0 / `avr-libc` 2.3.2): 746 bytes with the address patch (Change 1 in `main.c`),
plus two later changes:

- **Change 2 - EEPROM update marker:** an aborted update can't boot a mix of old and new code
  after a reset; the unit stays in the bootloader until a complete image has been written. This is
  no rollback - the old firmware is already partly overwritten. Update the unit again from the web
  UI ("Other unit number..." on the Units tab, since the unit scan no longer sees it).
- **Change 3 - rolling idle-boot timeout:** every real bootloader command used to clear
  `boot_timeout` for good, so a session aborted before the EEPROM marker was armed left the unit
  stuck in the bootloader until a power cycle. The timeout is now a rolling idle timer.

## Step 2: ISP flash - identical for every unit

**Only with the unit you are working on connected**; adjust the programmer options (example for a
USBasp clone):

```bash
# write the bootloader (erases the WHOLE chip, including the application!)
make MCU=atmega328p install AVRDUDE_PROG="-c usbasp -B 125kHz"

# set the fuses (separately - "install" does NOT do this)
make MCU=atmega328p fuses AVRDUDE_PROG="-c usbasp -B 125kHz"
```

`-B 125kHz` lowers the ISP clock; without it we got "target does not answer" errors.
`MCU=atmega328p` has to be given on **every** `make` call (including `install` and `fuses`),
otherwise the Makefile falls back to its ATtiny85 default. For "Arduino as ISP", use something
like `AVRDUDE_PROG="-c stk500v1 -b 19200 -P /dev/ttyUSB0"` (adjust the port).

The same `twiboot.hex` goes onto **every** unit, whatever its jumpers are set to.

**The `install` step erases the whole flash, including any `Unit.ino` already on the chip.**
Flash the application right afterwards, with `-D` so the bootloader you just wrote isn't erased
again:

```bash
avrdude -c usbasp -B 125kHz -p m328p -D -U flash:w:Unit.ino.hex:i
```

**`-D` only works on a blank (`0xFF`) area.** Over ISP the only erase is the full chip erase, and
without it AVR flash can only change bits from 1 to 0. So `-D` is right exactly here: for the
application directly after `make install` erased the chip. Writing over an application or
bootloader that is already on the chip with `-D` produces a mix of old and new content (`avrdude`
reports `Warning: flash verification mismatch`). Leaving `-D` out instead erases the whole chip,
bootloader included - the write verifies cleanly and the unit seems to run normally (it boots
straight into the application), but every I2C firmware update then fails with "Unit did not
respond in bootloader mode".

**Once twiboot is installed, update the application over I2C from the web UI**, not over ISP. If
an ISP write is unavoidable (new bootloader, broken unit), do the full sequence:

1. Back up the calibration offset: `dump eeprom 0 4` in `avrdude -t` shows `calOffset` as a
   little-endian `uint16_t` in the first two bytes (the chip erase also clears the EEPROM).
2. `make MCU=atmega328p install AVRDUDE_PROG="..."` or `avrdude -U flash:w:twiboot.hex:i`
   without `-D` (full chip erase, then the bootloader).
3. The application: `avrdude ... -D -U flash:w:Unit.ino.hex:i` (the area is blank now), or an
   I2C push from the web UI.
4. Set the calibration offset again on the **☰ Menu > Units** page.

After an ISP write, check both parts with `avrdude -c ... -p atmega328p -t`: `dump flash 0x7800 32`
(bootloader) and `dump flash 0x0 16` (application vector table).

## Step 3: Smoke test (on the spare unit, before any installed unit)

Right after `make install`, `make fuses` and flashing the application again, **before** touching
the next unit:

1. The normal `Unit.ino` application still works (I2C address, homing, showing letters) - the
   bootloader stays out of the way as long as no update is started. **Test this several times with
   a reset or power cycle**, not just once - a loose ISP connection can falsify a single test.
2. **Jumper test (the actual point of this patch):** set the unit's jumpers to a known value and
   run `POST /api/units/firmware-push?action=probe` (see `ESPMaster/API.md`). It sends the unit
   into the bootloader and reports the address it answers on, which must be exactly
   `0x29 + <jumper value>`. Then set the jumpers to a **different** value, power-cycle the unit and
   probe again: the bootloader now answers on the **new** address, without another ISP flash. Only
   continue once this works reliably.
3. Push a freshly compiled, functionally unchanged `Unit.ino` to exactly this unit from the web UI
   (**☰ Menu > Firmware update > Units**), then check normal operation again.
4. Simulate an aborted update (e.g. restart ESPMaster in the middle of a push) and check that a
   second attempt gets the unit running normally again.

Only once all four points work cleanly on the spare unit, continue with the installed units - one
at a time, with the same smoke test after each one.

## Background: how the master finds the bootloader

During an update push or a bootloader probe, ESPMaster talks to exactly one address,
`0x29 + <unit number>` - the address twiboot computes from the same jumpers as the application -
and checks the chip signature there before writing anything. It does not scan the whole bootloader
window any more: a full scan refused to recover any unit as soon as a second one sat in the
bootloader (e.g. after an aborted update). Two guards remain: a unit whose bootloader address would
collide with another unit's application address (from 41 units upward) or with the RTC (`0x68`,
i.e. unit 63) is refused before anything is written. The application-side boot scan in ESPMaster
(`scanI2CBus()`, which detects `unitsAmount`) covers addresses `0`-`63` and stops at the first gap.
