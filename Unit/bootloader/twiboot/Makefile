CC	:= avr-gcc
LD	:= avr-ld
OBJCOPY	:= avr-objcopy
OBJDUMP	:= avr-objdump
SIZE	:= avr-size

TARGET = twiboot
SOURCE = $(wildcard *.c)

# select MCU
MCU = attiny85

AVRDUDE_PROG := -c avr910 -b 115200 -P /dev/ttyUSB0
#AVRDUDE_PROG := -c dragon_isp -P usb

# ---------------------------------------------------------------------------

ifeq ($(MCU), atmega8)
# atmega8:
# Fuse L: 0x84 (8Mhz internal RC-Osz., 2.7V BOD)
# Fuse H: 0xda (512 words bootloader)
AVRDUDE_MCU=m8
AVRDUDE_FUSES=lfuse:w:0x84:m hfuse:w:0xda:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega88)
# atmega88:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m88
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x1C00
endif

ifeq ($(MCU), atmega168)
# atmega168:
# Fuse L: 0xc2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfa (512 words bootloader)
AVRDUDE_MCU=m168 -F
AVRDUDE_FUSES=lfuse:w:0xc2:m hfuse:w:0xdd:m efuse:w:0xfa:m

BOOTLOADER_START=0x3C00
endif

ifeq ($(MCU), atmega328p)
# atmega328p - values below are CORRECTED for the split-flap project's actual hardware
# (bare ATmega328P on the fae-pcb, https://github.com/codingcatgirl/split-flap-fae-pcb, with a
# real 16MHz crystal - see Unit/BOOTLOADER_SETUP.md), NOT twiboot's own generic upstream example
# below, which assumes a bare chip on the internal 8MHz RC oscillator. Verified against a
# pristine, factory-fused unit (2026-08-10) - do not change without re-verifying against real
# hardware, a wrong lfuse/F_CPU combination runs the chip at the wrong real clock speed with no
# error message, silently breaking all timing-dependent code (stepper motor control, in our case).
#
# Upstream's own example (bare chip, internal 8MHz RC - WRONG for this project's hardware):
#   Fuse L: 0xc2 (8Mhz internal RC-Osz.)
#   Fuse H: 0xdc (512 words bootloader)
#   Fuse E: 0xfd (2.7V BOD)
#
# Verified correct for this project (external 16MHz crystal, matching the factory-programmed
# fuses this hardware already ships with - F_CPU/TIMER_IRQFREQ_MS in main.c are tuned to match):
#   Fuse L: 0xff (external crystal, matches nano.build.f_cpu=16000000L in Arduino's own boards.txt)
#   Fuse H: 0xda (1024 words bootloader - matches the factory-programmed boot section size)
#   Fuse E: 0xfd (2.7V BOD, unchanged)
AVRDUDE_MCU=m328p -F
AVRDUDE_FUSES=lfuse:w:0xff:m hfuse:w:0xda:m efuse:w:0xfd:m

BOOTLOADER_START=0x7800
endif

ifeq ($(MCU), attiny85)
# attiny85:
# Fuse L: 0xe2 (8Mhz internal RC-Osz.)
# Fuse H: 0xdd (2.7V BOD)
# Fuse E: 0xfe (self programming enable)
AVRDUDE_MCU=t85
AVRDUDE_FUSES=lfuse:w:0xe2:m hfuse:w:0xdd:m efuse:w:0xfe:m

BOOTLOADER_START=0x1C00
CFLAGS_TARGET=-DUSE_CLOCKSTRETCH=1 -DVIRTUAL_BOOT_SECTION=1
endif

# ---------------------------------------------------------------------------

CFLAGS = -pipe -g -Os -mmcu=$(MCU) -Wall -fdata-sections -ffunction-sections
CFLAGS += -Wa,-adhlns=$(*F).lst -DBOOTLOADER_START=$(BOOTLOADER_START) $(CFLAGS_TARGET)
LDFLAGS = -Wl,-Map,$(@:.elf=.map),--cref,--relax,--gc-sections,--section-start=.text=$(BOOTLOADER_START)
LDFLAGS += -nostartfiles

# ---------------------------------------------------------------------------

$(TARGET): $(TARGET).elf
	@$(SIZE) -B -x --mcu=$(MCU) $<

$(TARGET).elf: $(SOURCE:.c=.o)
	@echo " Linking file:  $@"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@$(OBJDUMP) -h -S $@ > $(@:.elf=.lss)
	@$(OBJCOPY) -j .text -j .data -O ihex $@ $(@:.elf=.hex)
	@$(OBJCOPY) -j .text -j .data -O binary $@ $(@:.elf=.bin)

%.o: %.c $(MAKEFILE_LIST)
	@echo " Building file: $<"
	@$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf $(SOURCE:.c=.o) $(SOURCE:.c=.lst) $(addprefix $(TARGET), .elf .map .lss .hex .bin)

install: $(TARGET).elf
	avrdude $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) -U flash:w:$(<:.elf=.hex)

fuses:
	avrdude $(AVRDUDE_PROG) -p $(AVRDUDE_MCU) $(patsubst %,-U %, $(AVRDUDE_FUSES))
