/***************************************************************************
 *   Copyright (C) 10/2020 by Olaf Rempel                                  *
 *   razzor@kopf-tisch.de                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; version 2 of the License,               *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
/***************************************************************************
 * MODIFIED FORK - split-flap project (https://github.com/Tortoc/split-flap)
 *
 * Based on unmodified upstream https://github.com/orempel/twiboot
 * commit 559a403836e8d91a2d5b962e9541af679150b7a9 (2021-02-20).
 *
 * Change 1: the TWI/I2C address is no longer a fixed compile-time value.
 * Instead, main() reads the same 6 DIP-switch address pins (A0-A3, D3-D4)
 * that Unit.ino's getaddress() already reads, and adds that to
 * TWI_ADDRESS_BASE. This keeps a single compiled bootloader image usable on
 * every unit, and makes DIP-switch/jumper changes take effect immediately
 * for firmware updates too, without needing a per-unit bootloader reflash.
 * See Unit/BOOTLOADER_SETUP.md for the full rationale.
 *
 * Change 2 (2026-09-09): an EEPROM byte at EEPROM_UPDATE_MARKER_ADDR now
 * gates the app-boot decision (see the for(;;) wrapper around the main
 * command loop below). Application pages are written 1..N-1 first, page 0
 * (the reset vector) last - but that only protects the jump instruction
 * itself, not its target: on the atmega328p the real entry point the reset
 * vector jumps to already lands in page 1, i.e. exactly the pages rewritten
 * FIRST. An update aborted partway through (e.g. a power loss between
 * pages) followed by any reset would otherwise auto-boot the old (untouched)
 * page 0 straight into a mix of old and new code once the idle timeout (or
 * an explicit CMD_SWITCH_APPLICATION) fires. The marker makes twiboot stay
 * resident instead - see ESPMaster/ServiceUnitFirmwareFunctions.ino's
 * pushFirmwareToUnit(), which arms the marker before writing any page and
 * only clears it after page 0 is written AND verified.
 *
 * Change 3 (2026-09-09, CODE_REVIEW.md #9): unmodified upstream TWI_data_write()
 * sets boot_timeout = 0 ("abort countdown") on every real command (chip-info
 * read, flash/eeprom access, WAIT heartbeat) and NEVER sets it back to a
 * nonzero value anywhere else in this file - so the ~1000ms idle-boot
 * countdown isn't restarted, it's permanently disabled for the rest of this
 * power cycle the moment any real command arrives, which is true of nearly
 * every real session. A master that then abandons the session before ever
 * arming the Change-2 EEPROM marker (e.g. rejects the target after reading
 * chip info, or crashes/loses network before starting to write) leaves the
 * unit stuck in the bootloader with no working automatic recovery path at
 * all - confirmed by direct code trace and reproduced live during this
 * session (a unit left this way needed a manual power cycle; twiboot also
 * disables the watchdog on entry, see disable_wdt_timer() below, so there's
 * no independent hardware fallback either). Changed to re-arm
 * TIMER_MSEC2IRQCNT(TIMEOUT_MS) instead of disabling on every command,
 * making it a rolling idle timer: the unit now auto-boots ~1s after the
 * master goes quiet, in EVERY abandonment scenario, not just the ones an
 * ESPMaster-side fix could enumerate up front. This does not weaken Change
 * 2's protection - jump_to_app()'s marker check still runs on every trip
 * through the for(;;) loop regardless of which of the three paths
 * (idle timeout / explicit switch-app / SLA+W junk byte) set
 * cmd = CMD_BOOT_APPLICATION, and sends it back to CMD_WAIT if a write is
 * genuinely still in progress.
 *
 * Everything else in this file is unmodified upstream code. This file
 * remains licensed under GPL-2.0 (see LICENSE in this directory), as
 * required for redistributing a modified version.
 ***************************************************************************/
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>

#define VERSION_STRING          "TWIBOOT v3.2"
#define EEPROM_SUPPORT          1
#define LED_SUPPORT             1

#ifndef USE_CLOCKSTRETCH
#define USE_CLOCKSTRETCH        0
#endif

#ifndef VIRTUAL_BOOT_SECTION
#define VIRTUAL_BOOT_SECTION    0
#endif

#ifndef TWI_ADDRESS_BASE
#define TWI_ADDRESS_BASE        0x29
#endif
/* Actual TWI address = TWI_ADDRESS_BASE + (DIP switch reading), computed at runtime in main() */

/* 16MHz, NOT twiboot's upstream 8MHz default - this hardware has a real external 16MHz crystal
 * (Y2). Must be paired with the Makefile's AVRDUDE_FUSES (lfuse=0xFF, hfuse=0xDA) - a mismatch
 * here silently runs the chip wrong, no error message. See Unit/BOOTLOADER_SETUP.md. */
#define F_CPU                   16000000ULL
/* Must come after F_CPU above - util/delay.h calibrates against it at include time. */
#include <util/delay.h>

#define TIMER_DIVISOR           1024
/* Upstream's 25ms assumes F_CPU=8MHz (~195 Timer0 ticks, fits uint8_t). At our 16MHz that would
 * be ~390 ticks and overflow uint8_t - 12ms keeps the same margin at 16MHz (~188 ticks). */
#define TIMER_IRQFREQ_MS        12
#define TIMEOUT_MS              1000

#define TIMER_MSEC2TICKS(x)     ((x * F_CPU) / (TIMER_DIVISOR * 1000ULL))
#define TIMER_MSEC2IRQCNT(x)    (x / TIMER_IRQFREQ_MS)

#if (LED_SUPPORT)
#define LED_INIT()              DDRB = ((1<<PORTB4) | (1<<PORTB5))
#define LED_RT_ON()             PORTB |= (1<<PORTB4)
#define LED_RT_OFF()            PORTB &= ~(1<<PORTB4)
#define LED_GN_ON()             PORTB |= (1<<PORTB5)
#define LED_GN_OFF()            PORTB &= ~(1<<PORTB5)
#define LED_GN_TOGGLE()         PORTB ^= (1<<PORTB5)
#define LED_OFF()               PORTB = 0x00
#else
#define LED_INIT()
#define LED_RT_ON()
#define LED_RT_OFF()
#define LED_GN_ON()
#define LED_GN_OFF()
#define LED_GN_TOGGLE()
#define LED_OFF()
#endif /* LED_SUPPORT */

#if !defined(TWCR) && defined(USICR)
#define USI_PIN_INIT()          { PORTB |= ((1<<PORTB0) | (1<<PORTB2)); \
                                  DDRB |= (1<<PORTB2); \
                                }
#define USI_PIN_SDA_INPUT()     DDRB &= ~(1<<PORTB0)
#define USI_PIN_SDA_OUTPUT()    DDRB |= (1<<PORTB0)
#define USI_PIN_SCL()           (PINB & (1<<PINB2))

#if (USE_CLOCKSTRETCH == 0)
#error "USI peripheral requires enabled USE_CLOCKSTRETCH"
#endif

#define USI_STATE_MASK          0x0F
#define USI_STATE_IDLE          0x00    /* wait for Start Condition */
#define USI_STATE_SLA           0x01    /* wait for Slave Address */
#define USI_STATE_SLAW_ACK      0x02    /* ACK Slave Address + Write (Master writes) */
#define USI_STATE_SLAR_ACK      0x03    /* ACK Slave Address + Read (Master reads) */
#define USI_STATE_NAK           0x04    /* send NAK */
#define USI_STATE_DATW          0x05    /* receive Data */
#define USI_STATE_DATW_ACK      0x06    /* transmit ACK for received Data */
#define USI_STATE_DATR          0x07    /* transmit Data */
#define USI_STATE_DATR_ACK      0x08    /* receive ACK for transmitted Data */
#define USI_WAIT_FOR_ACK        0x10    /* wait for ACK bit (2 SCL clock edges) */
#define USI_ENABLE_SDA_OUTPUT   0x20    /* SDA is output (slave transmitting) */
#define USI_ENABLE_SCL_HOLD     0x40    /* Hold SCL low after clock overflow */
#endif /* !defined(TWCR) && defined(USICR) */

#if (VIRTUAL_BOOT_SECTION)
/* unused vector to store application start address */
#define APPVECT_NUM             EE_RDY_vect_num

/* each vector table entry is a 2byte RJMP opcode */
#define RSTVECT_ADDR            0x0000
#define APPVECT_ADDR            (APPVECT_NUM * 2)
#define RSTVECT_PAGE_OFFSET     (RSTVECT_ADDR % SPM_PAGESIZE)
#define APPVECT_PAGE_OFFSET     (APPVECT_ADDR % SPM_PAGESIZE)

/* create RJMP opcode for the vector table */
#define OPCODE_RJMP(addr)       (((addr) & 0x0FFF) | 0xC000)

#elif (!defined(ASRE) && !defined (RWWSRE))
#error "Device without bootloader section requires VIRTUAL_BOOT_SECTION"
#endif

/* SLA+R */
#define CMD_WAIT                0x00
#define CMD_READ_VERSION        0x01
#define CMD_ACCESS_MEMORY       0x02
/* internal mappings */
#define CMD_ACCESS_CHIPINFO     (0x10 | CMD_ACCESS_MEMORY)
#define CMD_ACCESS_FLASH        (0x20 | CMD_ACCESS_MEMORY)
#define CMD_ACCESS_EEPROM       (0x30 | CMD_ACCESS_MEMORY)
#define CMD_WRITE_FLASH_PAGE    (0x40 | CMD_ACCESS_MEMORY)
#define CMD_WRITE_EEPROM_PAGE   (0x50 | CMD_ACCESS_MEMORY)

/* SLA+W */
#define CMD_SWITCH_APPLICATION  CMD_READ_VERSION
/* internal mappings */
#define CMD_BOOT_BOOTLOADER     (0x10 | CMD_SWITCH_APPLICATION) /* only in APP */
#define CMD_BOOT_APPLICATION    (0x20 | CMD_SWITCH_APPLICATION)

/* CMD_SWITCH_APPLICATION parameter */
#define BOOTTYPE_BOOTLOADER     0x00    /* only in APP */
#define BOOTTYPE_APPLICATION    0x80

/* CMD_{READ|WRITE}_* parameter */
#define MEMTYPE_CHIPINFO        0x00
#define MEMTYPE_FLASH           0x01
#define MEMTYPE_EEPROM          0x02

/* "Update complete" gate for the app-boot decision - see Change 2 in the file header comment
 * above. E2END (not a hardcoded address) so this always matches whatever MCU this is built for.
 * Deliberately the opposite end of the EEPROM from Unit.ino's own calOffset (address 0-1), to
 * keep the two completely separate. 0xFF (the EEPROM-erased default) means "OK to boot" - no
 * migration step needed for a unit that has never gone through an update with this mechanism, or
 * right after a fresh ISP chip-erase. Only an explicit 0x00 (armed by pushFirmwareToUnit() before
 * writing any flash page) blocks booting - EEPROM needs no separate erase cycle, so clearing it
 * back to 0xFF after a verified-complete update is a perfectly ordinary write. */
#define EEPROM_UPDATE_MARKER_ADDR      E2END
#define EEPROM_UPDATE_MARKER_INPROGRESS 0x00

/*
 * LED_GN flashes with 20Hz (while bootloader is running)
 * LED_RT flashes on TWI activity
 *
 * bootloader twi-protocol:
 * - abort boot timeout:
 *   SLA+W, 0x00, STO
 *
 * - show bootloader version
 *   SLA+W, 0x01, SLA+R, {16 bytes}, STO
 *
 * - start application
 *   SLA+W, 0x01, 0x80, STO
 *
 * - read chip info: 3byte signature, 1byte page size, 2byte flash size, 2byte eeprom size
 *   SLA+W, 0x02, 0x00, 0x00, 0x00, SLA+R, {8 bytes}, STO
 *
 * - read one (or more) flash bytes
 *   SLA+W, 0x02, 0x01, addrh, addrl, SLA+R, {* bytes}, STO
 *
 * - read one (or more) eeprom bytes
 *   SLA+W, 0x02, 0x02, addrh, addrl, SLA+R, {* bytes}, STO
 *
 * - write one flash page
 *   SLA+W, 0x02, 0x01, addrh, addrl, {* bytes}, STO
 *
 * - write one (or more) eeprom bytes
 *   SLA+W, 0x02, 0x02, addrh, addrl, {* bytes}, STO
 */

const static uint8_t info[16] = VERSION_STRING;
const static uint8_t chipinfo[8] = {
    SIGNATURE_0, SIGNATURE_1, SIGNATURE_2,
    SPM_PAGESIZE,

    (BOOTLOADER_START >> 8) & 0xFF,
    BOOTLOADER_START & 0xFF,

#if (EEPROM_SUPPORT)
    ((E2END +1) >> 8 & 0xFF),
    (E2END +1) & 0xFF
#else
    0x00, 0x00
#endif
};

static uint8_t boot_timeout = TIMER_MSEC2IRQCNT(TIMEOUT_MS);
static uint8_t cmd = CMD_WAIT;

/* flash buffer */
static uint8_t buf[SPM_PAGESIZE];
static uint16_t addr;

#if (VIRTUAL_BOOT_SECTION)
/* reset/application vectors received from host, needed for verify read */
static uint8_t rstvect_save[2];
static uint8_t appvect_save[2];
#endif /* (VIRTUAL_BOOT_SECTION) */

/* *************************************************************************
 * write_flash_page
 * ************************************************************************* */
static void write_flash_page(void)
{
    uint16_t pagestart = addr;
    uint8_t size = SPM_PAGESIZE;
    uint8_t *p = buf;

#if (VIRTUAL_BOOT_SECTION)
    if (pagestart == (RSTVECT_ADDR & ~(SPM_PAGESIZE -1)))
    {
        /* save original vectors for verify read */
        rstvect_save[0] = buf[RSTVECT_PAGE_OFFSET];
        rstvect_save[1] = buf[RSTVECT_PAGE_OFFSET + 1];
        appvect_save[0] = buf[APPVECT_PAGE_OFFSET];
        appvect_save[1] = buf[APPVECT_PAGE_OFFSET + 1];

        /* replace reset vector with jump to bootloader address */
        uint16_t rst_vector = OPCODE_RJMP(BOOTLOADER_START -1);
        buf[RSTVECT_PAGE_OFFSET] = (rst_vector & 0xFF);
        buf[RSTVECT_PAGE_OFFSET + 1] = (rst_vector >> 8) & 0xFF;

        /* replace application vector with jump to original reset vector */
        uint16_t app_vector = rstvect_save[0] | (rstvect_save[1] << 8);
        app_vector = OPCODE_RJMP(app_vector - APPVECT_NUM);

        buf[APPVECT_PAGE_OFFSET] = (app_vector & 0xFF);
        buf[APPVECT_PAGE_OFFSET + 1] = (app_vector >> 8) & 0xFF;
    }
#endif /* (VIRTUAL_BOOT_SECTION) */

    if (pagestart < BOOTLOADER_START)
    {
        boot_page_erase(pagestart);
        boot_spm_busy_wait();

        do {
            uint16_t data = *p++;
            data |= *p++ << 8;
            boot_page_fill(addr, data);

            addr += 2;
            size -= 2;
        } while (size);

        boot_page_write(pagestart);
        boot_spm_busy_wait();

#if defined (ASRE) || defined (RWWSRE)
        /* only required for bootloader section */
        boot_rww_enable();
#endif
    }
} /* write_flash_page */


#if (EEPROM_SUPPORT)
/* *************************************************************************
 * read_eeprom_byte
 * ************************************************************************* */
static uint8_t read_eeprom_byte(uint16_t address)
{
    EEARL = address;
    EEARH = (address >> 8);
    EECR |= (1<<EERE);

    return EEDR;
} /* read_eeprom_byte */


/* *************************************************************************
 * write_eeprom_byte
 * ************************************************************************* */
static void write_eeprom_byte(uint8_t val)
{
    EEARL = addr;
    EEARH = (addr >> 8);
    EEDR = val;
    addr++;

#if defined (EEWE)
    EECR |= (1<<EEMWE);
    EECR |= (1<<EEWE);
#elif defined (EEPE)
    EECR |= (1<<EEMPE);
    EECR |= (1<<EEPE);
#else
#error "EEWE/EEPE not defined"
#endif

    eeprom_busy_wait();
} /* write_eeprom_byte */


#if (USE_CLOCKSTRETCH == 0)
/* *************************************************************************
 * write_eeprom_buffer
 * ************************************************************************* */
static void write_eeprom_buffer(uint8_t size)
{
    uint8_t *p = buf;

    while (size--)
    {
        write_eeprom_byte(*p++);
    }
} /* write_eeprom_buffer */
#endif /* (USE_CLOCKSTRETCH == 0) */
#endif /* EEPROM_SUPPORT */


/* *************************************************************************
 * TWI_data_write
 * ************************************************************************* */
static uint8_t TWI_data_write(uint8_t bcnt, uint8_t data)
{
    uint8_t ack = 0x01;

    switch (bcnt)
    {
        case 0:
            switch (data)
            {
                case CMD_SWITCH_APPLICATION:
                case CMD_ACCESS_MEMORY:
                    /* no break */

                case CMD_WAIT:
                    /* Re-arm the idle-boot countdown rather than permanently disabling it - see
                     * Change 3 in the file header comment above. */
                    boot_timeout = TIMER_MSEC2IRQCNT(TIMEOUT_MS);
                    cmd = data;
                    break;

                default:
                    /* boot app now */
                    cmd = CMD_BOOT_APPLICATION;
                    ack = 0x00;
                    break;
            }
            break;

        case 1:
            switch (cmd)
            {
                case CMD_SWITCH_APPLICATION:
                    if (data == BOOTTYPE_APPLICATION)
                    {
                        cmd = CMD_BOOT_APPLICATION;
                    }

                    ack = 0x00;
                    break;

                case CMD_ACCESS_MEMORY:
                    if (data == MEMTYPE_CHIPINFO)
                    {
                        cmd = CMD_ACCESS_CHIPINFO;
                    }
                    else if (data == MEMTYPE_FLASH)
                    {
                        cmd = CMD_ACCESS_FLASH;
                    }
#if (EEPROM_SUPPORT)
                    else if (data == MEMTYPE_EEPROM)
                    {
                        cmd = CMD_ACCESS_EEPROM;
                    }
#endif /* (EEPROM_SUPPORT) */
                    else
                    {
                        ack = 0x00;
                    }
                    break;

                default:
                    ack = 0x00;
                    break;
            }
            break;

        case 2:
        case 3:
            addr <<= 8;
            addr |= data;
            break;

        default:
            switch (cmd)
            {
#if (EEPROM_SUPPORT)
#if (USE_CLOCKSTRETCH)
                case CMD_ACCESS_EEPROM:
                    write_eeprom_byte(data);
                    break;
#else
                case CMD_ACCESS_EEPROM:
                    cmd = CMD_WRITE_EEPROM_PAGE;
                    /* fall through */

                case CMD_WRITE_EEPROM_PAGE:
#endif /* (USE_CLOCKSTRETCH) */
#endif /* (EEPROM_SUPPORT) */
                case CMD_ACCESS_FLASH:
                {
                    uint8_t pos = bcnt -4;

                    buf[pos] = data;
                    if (pos >= (SPM_PAGESIZE -1))
                    {
                        if (cmd == CMD_ACCESS_FLASH)
                        {
#if (USE_CLOCKSTRETCH)
                            write_flash_page();
#else
                            cmd = CMD_WRITE_FLASH_PAGE;
#endif
                        }

                        ack = 0x00;
                    }
                    break;
                }

                default:
                    ack = 0x00;
                    break;
            }
            break;
    }

    return ack;
} /* TWI_data_write */


/* *************************************************************************
 * TWI_data_read
 * ************************************************************************* */
static uint8_t TWI_data_read(uint8_t bcnt)
{
    uint8_t data;

    switch (cmd)
    {
        case CMD_READ_VERSION:
            bcnt %= sizeof(info);
            data = info[bcnt];
            break;

        case CMD_ACCESS_CHIPINFO:
            bcnt %= sizeof(chipinfo);
            data = chipinfo[bcnt];
            break;

        case CMD_ACCESS_FLASH:
            switch (addr)
            {
/* return cached values for verify read */
#if (VIRTUAL_BOOT_SECTION)
                case RSTVECT_ADDR:
                    data = rstvect_save[0];
                    break;

                case (RSTVECT_ADDR + 1):
                    data = rstvect_save[1];
                    break;

                case APPVECT_ADDR:
                    data = appvect_save[0];
                    break;

                case (APPVECT_ADDR + 1):
                    data = appvect_save[1];
                    break;
#endif /* (VIRTUAL_BOOT_SECTION) */

                default:
                    data = pgm_read_byte_near(addr);
                    break;
            }

            addr++;
            break;

#if (EEPROM_SUPPORT)
        case CMD_ACCESS_EEPROM:
            data = read_eeprom_byte(addr++);
            break;
#endif /* (EEPROM_SUPPORT) */

        default:
            data = 0xFF;
            break;
    }

    return data;
} /* TWI_data_read */


#if defined (TWCR)
/* *************************************************************************
 * TWI_vect
 * ************************************************************************* */
static void TWI_vect(void)
{
    static uint8_t bcnt;
    uint8_t control = TWCR;

    switch (TWSR & 0xF8)
    {
        /* SLA+W received, ACK returned -> receive data and ACK */
        case 0x60:
            bcnt = 0;
            LED_RT_ON();
            break;

        /* prev. SLA+W, data received, ACK returned -> receive data and ACK */
        case 0x80:
            if (TWI_data_write(bcnt++, TWDR) == 0x00)
            {
                /* the ACK returned by TWI_data_write() is not for the current
                 * data in TWDR, but for the next byte received
                 */
                control &= ~(1<<TWEA);
            }
            break;

        /* SLA+R received, ACK returned -> send data */
        case 0xA8:
            bcnt = 0;
            LED_RT_ON();
            /* fall through */

        /* prev. SLA+R, data sent, ACK returned -> send data */
        case 0xB8:
            TWDR = TWI_data_read(bcnt++);
            break;

        /* prev. SLA+W, data received, NACK returned -> IDLE */
        case 0x88:
            TWI_data_write(bcnt++, TWDR);
            /* fall through */

        /* STOP or repeated START -> IDLE */
        case 0xA0:
#if (USE_CLOCKSTRETCH == 0)
            if ((cmd == CMD_WRITE_FLASH_PAGE)
#if (EEPROM_SUPPORT)
                || (cmd == CMD_WRITE_EEPROM_PAGE)
#endif
               )
            {
                /* disable ACK for now, re-enable after page write */
                control &= ~(1<<TWEA);
                TWCR = (1<<TWINT) | control;

#if (EEPROM_SUPPORT)
                if (cmd == CMD_WRITE_EEPROM_PAGE)
                {
                    write_eeprom_buffer(bcnt -4);
                }
                else
#endif /* (EEPROM_SUPPORT) */
                {
                    write_flash_page();
                }
            }
#endif /* (USE_CLOCKSTRETCH) */

            bcnt = 0;
            /* fall through */

        /* prev. SLA+R, data sent, NACK returned -> IDLE */
        case 0xC0:
            LED_RT_OFF();
            control |= (1<<TWEA);
            break;

        /* illegal state(s) -> reset hardware */
        default:
            control |= (1<<TWSTO);
            break;
    }

    TWCR = (1<<TWINT) | control;
} /* TWI_vect */
#endif /* defined (TWCR) */

#if defined (USICR)
/* *************************************************************************
 * usi_statemachine
 * ************************************************************************* */
static void usi_statemachine(uint8_t usisr)
{
    static uint8_t usi_state;
    static uint8_t bcnt;

    uint8_t data = USIDR;
    uint8_t state = usi_state & USI_STATE_MASK;

    /* Start Condition detected */
    if (usisr & (1<<USISIF))
    {
        /* wait until SCL goes low */
        while (USI_PIN_SCL());

        usi_state = USI_STATE_SLA | USI_ENABLE_SCL_HOLD;
        state = USI_STATE_IDLE;
    }

    /* Stop Condition detected */
    if (usisr & (1<<USIPF))
    {
        LED_RT_OFF();
        usi_state = USI_STATE_IDLE;
        state = USI_STATE_IDLE;
    }

    if (state == USI_STATE_IDLE)
    {
        /* do nothing */
    }
    /* Slave Address received => prepare ACK/NAK */
    else if (state == USI_STATE_SLA)
    {
        bcnt = 0;

        /* SLA+W received -> send ACK */
        if (data == ((TWI_ADDRESS_BASE<<1) | 0x00))
        {
            LED_RT_ON();
            usi_state = USI_STATE_SLAW_ACK | USI_WAIT_FOR_ACK | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
            USIDR = 0x00;
        }
        /* SLA+R received -> send ACK */
        else if (data == ((TWI_ADDRESS_BASE<<1) | 0x01))
        {
            LED_RT_ON();
            usi_state = USI_STATE_SLAR_ACK | USI_WAIT_FOR_ACK | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
            USIDR = 0x00;
        }
        /* not addressed -> send NAK */
        else
        {
            usi_state = USI_STATE_NAK | USI_WAIT_FOR_ACK | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
            USIDR = 0x80;
        }
    }
    /* sent NAK -> go to idle */
    else if (state == USI_STATE_NAK)
    {
        usi_state = USI_STATE_IDLE;
    }
    /* sent ACK after SLA+W -> wait for data */
    /* sent ACK after DAT+W -> wait for more data */
    else if ((state == USI_STATE_SLAW_ACK) ||
             (state == USI_STATE_DATW_ACK)
            )
    {
        usi_state = USI_STATE_DATW | USI_ENABLE_SCL_HOLD;
    }
    /* data received -> send ACK/NAK */
    else if (state == USI_STATE_DATW)
    {
        if (TWI_data_write(bcnt++, data))
        {
            usi_state = USI_STATE_DATW_ACK | USI_WAIT_FOR_ACK | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
            USIDR = 0x00;
        }
        else
        {
            usi_state = USI_STATE_NAK | USI_WAIT_FOR_ACK | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
            USIDR = 0x80;
        }
    }
    /* sent ACK after SLA+R -> send data */
    /* received ACK after DAT+R -> send more data */
    else if ((state == USI_STATE_SLAR_ACK) ||
             ((state == USI_STATE_DATR_ACK) && !(data & 0x01))
            )
    {
        USIDR = TWI_data_read(bcnt++);
        usi_state = USI_STATE_DATR | USI_ENABLE_SDA_OUTPUT | USI_ENABLE_SCL_HOLD;
    }
    /* sent data after SLA+R -> receive ACK/NAK */
    else if (state == USI_STATE_DATR)
    {
        usi_state = USI_STATE_DATR_ACK | USI_WAIT_FOR_ACK | USI_ENABLE_SCL_HOLD;
        USIDR = 0x80;
    }
    /* received NAK after DAT+R -> go to idle */
    else if ((state == USI_STATE_DATR_ACK) && (data & 0x01))
    {
        usi_state = USI_STATE_IDLE;
    }
    /* default -> go to idle */
    else
    {
        usi_state = USI_STATE_IDLE;
    }

    /* set SDA direction according to current state */
    if (usi_state & USI_ENABLE_SDA_OUTPUT)
    {
        USI_PIN_SDA_OUTPUT();
    }
    else
    {
        USI_PIN_SDA_INPUT();
    }

    if (usi_state & USI_ENABLE_SCL_HOLD)
    {
        /* Enable TWI Mode, hold SCL low after counter overflow, count both SCL edges */
        USICR = (1<<USIWM1) | (1<<USIWM0) | (1<<USICS1);
    }
    else
    {
        /* Enable TWI, hold SCL low only after start condition, count both SCL edges */
        USICR = (1<<USIWM1) | (1<<USICS1);
    }

    /* clear start/overflow/stop condition flags */
    usisr &= ((1<<USISIF) | (1<<USIOIF) | (1<<USIPF));
    if (usi_state & USI_WAIT_FOR_ACK)
    {
        /* count 2 SCL edges (ACK/NAK bit) */
        USISR = usisr | ((16 -2)<<USICNT0);
    }
    else
    {
        /* count 16 SCL edges (8bit data) */
        USISR = usisr | ((16 -16)<<USICNT0);
    }
} /* usi_statemachine */
#endif /* defined (USICR) */


/* *************************************************************************
 * TIMER0_OVF_vect
 * ************************************************************************* */
static void TIMER0_OVF_vect(void)
{
    /* restart timer */
    TCNT0 = 0xFF - TIMER_MSEC2TICKS(TIMER_IRQFREQ_MS);

    /* blink LED while running */
    LED_GN_TOGGLE();

    /* count down for app-boot */
    if (boot_timeout > 1)
    {
        boot_timeout--;
    }
    else if (boot_timeout == 1)
    {
        /* trigger app-boot */
        cmd = CMD_BOOT_APPLICATION;
    }
} /* TIMER0_OVF_vect */


#if (VIRTUAL_BOOT_SECTION)
static void (*jump_to_app)(void) __attribute__ ((noreturn)) = (void*)APPVECT_ADDR;
#else
static void (*jump_to_app)(void) __attribute__ ((noreturn)) = (void*)0x0000;
#endif


/* *************************************************************************
 * init1
 * ************************************************************************* */
void init1(void) __attribute__((naked, section(".init1")));
void init1(void)
{
  /* make sure r1 is 0x00 */
  asm volatile ("clr __zero_reg__");

  /* on some MCUs the stack pointer defaults NOT to RAMEND */
#if defined(__AVR_ATmega8__) || defined(__AVR_ATmega8515__) || \
    defined(__AVR_ATmega8535__) || defined (__AVR_ATmega16__) || \
    defined (__AVR_ATmega32__) || defined (__AVR_ATmega64__)  || \
    defined (__AVR_ATmega128__) || defined (__AVR_ATmega162__)
  SP = RAMEND;
#endif
} /* init1 */


/*
 * For newer devices the watchdog timer remains active even after a
 * system reset. So disable it as soon as possible.
 * automagically called on startup
 */
#if defined (__AVR_ATmega88__) || defined (__AVR_ATmega168__) || \
    defined (__AVR_ATmega328P__)
/* *************************************************************************
 * disable_wdt_timer
 * ************************************************************************* */
void disable_wdt_timer(void) __attribute__((naked, section(".init3")));
void disable_wdt_timer(void)
{
    MCUSR = 0;
    WDTCSR = (1<<WDCE) | (1<<WDE);
    WDTCSR = (0<<WDE);
} /* disable_wdt_timer */
#endif


/* *************************************************************************
 * main
 * ************************************************************************* */
int main(void) __attribute__ ((OS_main, section (".init9")));
int main(void)
{
    LED_INIT();
    LED_GN_ON();

#if (VIRTUAL_BOOT_SECTION)
	/* load current values (for reading flash) */
    rstvect_save[0] = pgm_read_byte_near(RSTVECT_ADDR);
    rstvect_save[1] = pgm_read_byte_near(RSTVECT_ADDR + 1);
    appvect_save[0] = pgm_read_byte_near(APPVECT_ADDR);
    appvect_save[1] = pgm_read_byte_near(APPVECT_ADDR + 1);
#endif /* (VIRTUAL_BOOT_SECTION) */

    /* timer0: running with F_CPU/1024 */
#if defined (TCCR0)
    TCCR0 = (1<<CS02) | (1<<CS00);
#elif defined (TCCR0B)
    TCCR0B = (1<<CS02) | (1<<CS00);
#else
#error "TCCR0(B) not defined"
#endif

#if defined (TWCR)
    /* Read the address jumpers (same pins/bit mapping as Unit.ino's getaddress()) so the
     * bootloader's I2C address always matches the app's - one image for every unit, jumper
     * changes apply immediately without a reflash. bit 0..3 = A0-A3 (PC0-PC3), bit 4..5 = D3-D4
     * (PD3-PD4); D5/D6 stay unread. Must stay in sync with Unit.ino's getaddress(). */
    DDRD &= ~((1<<PD3) | (1<<PD4));  /* inputs */
    PORTD |= (1<<PD3) | (1<<PD4);    /* enable pull-ups */
    DDRC &= ~((1<<PC0) | (1<<PC1) | (1<<PC2) | (1<<PC3));  /* inputs */
    PORTC |= (1<<PC0) | (1<<PC1) | (1<<PC2) | (1<<PC3);    /* enable pull-ups */
    _delay_us(50); /* let pull-ups settle */

    uint8_t pinsD = PIND;
    uint8_t pinsC = PINC;
    uint8_t dipAddress = 0;
    if (!(pinsC & (1<<PC0))) dipAddress |= 1;
    if (!(pinsC & (1<<PC1))) dipAddress |= 2;
    if (!(pinsC & (1<<PC2))) dipAddress |= 4;
    if (!(pinsC & (1<<PC3))) dipAddress |= 8;
    if (!(pinsD & (1<<PD3))) dipAddress |= 16;
    if (!(pinsD & (1<<PD4))) dipAddress |= 32;

    /* TWI init: set address, auto ACKs */
    TWAR = ((TWI_ADDRESS_BASE + dipAddress) << 1);
    TWCR = (1<<TWEA) | (1<<TWEN);
#elif defined (USICR)
    USI_PIN_INIT();
    usi_statemachine(0x00);
#else
#error "No TWI/USI peripheral found"
#endif

    /* Outer loop, see Change 2 in the file header comment above: cmd can become
     * CMD_BOOT_APPLICATION three different ways (the idle timeout in TIMER0_OVF_vect, an explicit
     * CMD_SWITCH_APPLICATION/BOOTTYPE_APPLICATION, or the SLA+W-junk-byte shortcut in
     * TWI_data_write) - gating all of them at once, right here at the only place that actually
     * calls jump_to_app() below, is simpler and provably complete rather than tracking three
     * separate call sites. Read fresh from EEPROM every time (never cached in a variable) - a
     * push that clears the marker during THIS same bootloader session (recovering from an earlier
     * aborted one) must be able to boot immediately once it does, without needing a second reset. */
    for (;;)
    {
        while (cmd != CMD_BOOT_APPLICATION)
        {
#if defined (TWCR)
            if (TWCR & (1<<TWINT))
            {
                TWI_vect();
            }
#elif defined (USICR)
            if (USISR & ((1<<USISIF) | (1<<USIOIF) | (1<<USIPF)))
            {
                usi_statemachine(USISR);
            }
#endif

#if defined (TIFR)
            if (TIFR & (1<<TOV0))
            {
                TIMER0_OVF_vect();
                TIFR = (1<<TOV0);
            }
#elif defined (TIFR0)
            if (TIFR0 & (1<<TOV0))
            {
                TIMER0_OVF_vect();
                TIFR0 = (1<<TOV0);
            }
#else
#error "TIFR(0) not defined"
#endif
        }

#if (EEPROM_SUPPORT)
        /* Refuse to boot into a possibly-inconsistent app while an update is armed/in-progress
         * (see EEPROM_UPDATE_MARKER_ADDR's comment and Change 2 above) - go back to waiting for
         * I2C instead of falling through to jump_to_app() below. */
        if (read_eeprom_byte(EEPROM_UPDATE_MARKER_ADDR) == EEPROM_UPDATE_MARKER_INPROGRESS)
        {
            cmd = CMD_WAIT;
            continue;
        }
#endif /* (EEPROM_SUPPORT) */

        break;
    }

#if defined (TWCR)
    /* Disable TWI but keep address! */
    TWCR = 0x00;
#elif defined (USICR)
    /* Disable USI peripheral */
    USICR = 0x00;
#endif

    /* disable timer0 */
#if defined (TCCR0)
    TCCR0 = 0x00;
#elif defined (TCCR0B)
    TCCR0B = 0x00;
#else
#error "TCCR0(B) not defined"
#endif

    LED_OFF();

#if (LED_SUPPORT)
    uint16_t wait = 0x0000;
    do {
        __asm volatile ("nop");
    } while (--wait);
#endif /* (LED_SUPPORT) */

    jump_to_app();
} /* main */
