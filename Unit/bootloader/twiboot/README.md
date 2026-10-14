# twiboot - a TWI / I2C bootloader for AVR MCUs ##
twiboot is a simple/small bootloader for AVR MCUs written in C. It uses the integrated TWI or USI peripheral of the controller to implement a I2C slave.
It was originally created to update I2C controlled BLMCs (Brushless Motor Controller) without an AVR ISP adapter.

## This fork (split-flap project) ##
`main.c` in this directory is a lightly patched fork of upstream twiboot, vendored for the
[split-flap project](https://github.com/Tortoc/split-flap) to update its ATmega328p "Unit" boards
over I2C. It is based on unmodified upstream commit
[`559a403`](https://github.com/orempel/twiboot/commit/559a403836e8d91a2d5b962e9541af679150b7a9)
(2021-02-20).

**How much is actually different:** diffed directly against that upstream commit
(898 lines) against this fork's `main.c` (1027 lines) - 7 contiguous change blocks, roughly
5% of the file's functional code (the rest, including the entire TWI/I2C protocol below, is
byte-for-byte identical to upstream). About half of the added lines are a documentation header
at the top of `main.c` explaining the changes in place; see that header for the full,
line-referenced rationale behind each one. Summary:

1. **Dynamic I2C address via address jumpers** (~20 new lines + renaming `TWI_ADDRESS` to
   `TWI_ADDRESS_BASE`): instead of a fixed compile-time slave address, `main()` reads the same
   6 address-jumper pins the application firmware (`Unit.ino`) already reads and adds that to
   `TWI_ADDRESS_BASE`. This lets one compiled bootloader image be flashed onto every unit -
   jumper changes take effect immediately, with no per-unit bootloader rebuild/reflash.
2. **Hardware-matched timing** (2 lines): `F_CPU` changed from upstream's 8MHz default to this
   project's actual 16MHz crystal, with the idle-timer interval adjusted accordingly (25ms ->
   12ms) to avoid a counter overflow at the higher clock.
3. **EEPROM update-in-progress marker** (~30 lines): a dedicated EEPROM byte now gates whether
   the bootloader is allowed to boot into the application at all. The application-update code on
   the ESPMaster side arms this marker before writing any flash page and only clears it once the
   full image is written and verified - so an interrupted update can never leave the unit
   auto-booting into a half-written, inconsistent application.
4. **Idle-timeout re-arm fix** (1 line): unmodified upstream disables the ~1000ms idle-boot
   countdown the moment any real bootloader command arrives, and never re-enables it - so a
   master that abandons a session early (crash, lost network, wrong target) left the unit stuck
   in the bootloader indefinitely, needing a manual power cycle. Changed to re-arm the countdown
   on every command instead, making it a rolling idle timer that always recovers on its own.

**This fork's own version:** upstream tags releases as MAJOR.MINOR only (v1.0, v1.1, v1.2, v2.0,
v2.1, v3.0 on GitHub), and this fork's base commit already carries `"TWIBOOT v3.2"` in
`VERSION_STRING` (5 commits past the v3.0 tag, version bumped in source ahead of a formal tag).
Continuing that same scheme forward through the three changes above (each new capability a MINOR
bump, the bugfix a MINOR bump as well since upstream has no separate patch level): v3.2 -> **v3.3**
(dynamic addressing) -> **v3.4** (EEPROM update marker) -> **v3.5** (idle-timeout fix).
`VERSION_STRING` reports it over I2C as `"SF-TWIBOOT v3.5"` ("Read bootloader info" in the web UI);
a unit flashed with an older build reports that build's string. The bootloader itself can only be
updated via ISP - there is no OTA path for it, by design. Everything else - chip info, flash/EEPROM read/write, application start,
clockstretching - is exactly the upstream protocol.

twiboot acts as a slave device on a TWI/I2C bus and allows reading/writing of the internal flash memory.
As a compile time option (EEPROM_SUPPORT) twiboot also allows reading/writing of the whole internal EEPROM memory.
The bootloader is not able to update itself (only application flash memory region accessible).

Currently the following AVR MCUs are supported:

AVR MCU | Flash bytes used (.text + .data) | Bootloader region size
--- | --- | ---
attiny85 | 954 (0x3BA) | 512 words
atmega8 | 786 (0x312) | 512 words
atmega88 | 810 (0x32A) | 512 words
atmega168 | 810 (0x32A) | 512 words
atmega328p | 810 (0x32A) | 512 words

(Compiled on Ubuntu 18.04 LTS (gcc 5.4.0 / avr-libc 2.0.0) with EEPROM and LED support)


## Operation ##
twiboot is installed in the bootloader section and executed directly after reset (BOOTRST fuse is programmed).
For MCUs without bootloader section see [Virtual bootloader section](#virtual-bootloader-section) below.

While running, twiboot configures the TWI/USI peripheral as slave device and waits for valid protocol messages
directed to its address on the TWI/I2C bus. The slave address is configured during compile time of twiboot.
When receiving no messages for 1000ms after reset, the bootloader exits and executes the main application at address 0x0000.

A TWI/I2C master can use the protocol to
- abort the boot timeout
- query information about the device (bootloader version, AVR signature bytes, flash/eeprom size, flash page size)
- read internal flash / eeprom memory (byte wise)
- write the internal flash (page wise)
- write the internal eeprom (byte wise)
- exit the bootloader and start the application

As a compile time option (LED_SUPPORT) twiboot can output its state with two LEDs.
One LED will flash with a frequency of 20Hz while twiboot is active (including boot wait time).
A second LED will flash when the bootloader is addressed on the TWI/I2C bus.


### Virtual Bootloader Section ###
For MCUs without bootloader section twiboot will patch the vector table on the fly during flash programming to stay active.
The reset vector is patched to execute twiboot instead of the application code.

Another vector entry will be patched to store the original entry point of the application.
This vector entry is overridden and MUST NOT be used by the application.
twiboot uses this vector to start the application after the initial timeout.

This live patching changes the content of the vector table, which would result in a verification error after programming.
To counter this kind of error, twiboot caches the original vector table entries in RAM and return those on a read command.
The real content of the vector table is only returned after a reset.


## Build and install twiboot ##
twiboot uses gcc, avr-libc and GNU Make for building, avrdude is used for flashing the MCU.
The build and install procedures are only tested under linux.

The selection of the target MCU and the programming interface can be found in the Makefile,
TWI/I2C slave address and optional components (EEPROM / LED support) are configured
in the main.c source.

To build twiboot for the selected target:
``` shell
$ make
```

To install (flash download) twiboot with avrdude on the target:
``` shell
$ make install
```

Set AVR fuses with avrdude on the target (internal RC-Osz, enable BOD, enable BOOTRST):
``` shell
$ make fuses
```


## TWI/I2C Protocol ##
A TWI/I2C master can use the following protocol for accessing the bootloader.

Function | TWI/I2C data | Comment
--- | --- | ---
Abort boot timeout | **SLA+W**, 0x00, **STO** |
Show bootloader version | **SLA+W**, 0x01, **SLA+R**, {16 bytes}, **STO** | ASCII, not null terminated
Start application | **SLA+W**, 0x01, 0x80, **STO** |
Read chip info | **SLA+W**, 0x02, 0x00, 0x00, 0x00, **SLA+R**, {8 bytes}, **STO** | 3byte signature, 1byte page size, 2byte flash size, 2byte eeprom size
Read 1+ flash bytes | **SLA+W**, 0x02, 0x01, addrh, addrl, **SLA+R**, {* bytes}, **STO** |
Read 1+ eeprom bytes | **SLA+W**, 0x02, 0x02, addrh, addrl, **SLA+R**, {* bytes}, **STO** |
Write one flash page | **SLA+W**, 0x02, 0x01, addrh, addrl, {* bytes}, **STO** | page size as indicated in chip info
Write 1+ eeprom bytes | **SLA+W**, 0x02, 0x02, addrh, addrl, {* bytes}, **STO** | write 0 < n < page size bytes at once

**SLA+R** means Start Condition, Slave Address, Read Access

**SLA+W** means Start Condition, Slave Address, Write Access

**STO** means Stop Condition

A flash page / eeprom write is only triggered after the Stop Condition.
During the write process twiboot will NOT acknowledge its slave address.

The multiboot_tool repository contains a simple linux application that uses
this protocol to access the bootloader over linux i2c device.

The ispprog programming adapter can also be used as a avr910/butterfly to twiboot protocol bridge.


## TWI/I2C Clockstretching ##
While a write is in progress twiboot will not respond on the TWI/I2C bus and the
TWI/I2C master needs to retry/poll the slave address until the write has completed.

As a compile time option (USE_CLOCKSTRETCH) the previous behavior of twiboot can be restored:
TWI/I2C Clockstretching is then used to inform the master of the duration of the write.
Please note that there are some TWI/I2C masters that do not support clockstretching.


## Development ##
Issue reports, feature requests, patches or simply success stories are much appreciated.
