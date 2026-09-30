#include "board.h"
#include "bus.h"
#include "io.h"

static __data uint8_t led_shadow;
/* The status LED, held apart from the IR bits so either can change without
 * disturbing the other. Red until the box is up — see main.c. */
static __xdata uint8_t led_top = LED_RED;   /* the latch is write-only */

__sfr __at(0xAE) WDKEY;              /* 55h disables, anything else enables */
__sfr __at(0xA6) WDRST;              /* written: the counter restarts from here */

static __xdata uint16_t tickcount, beat;
static __xdata uint8_t  sec_part, wdt_on;
static __xdata uint32_t seconds;

/* How long the main loop may go without a heartbeat() before ticks() stops
 * feeding the watchdog. The longest legitimate absence is a firmware upload
 * (~16 s, ota.c); a W5100 send to a dead peer gives up after 4 s. */
#define LOOP_LIMIT TICKS_S(30)

void ticks_init(void)
{
    TMOD = (TMOD & 0xF0) | 0x01;    /* timer 0, 16-bit, counting the clock */
    TR0 = 1;
}

uint16_t ticks(void)
{
    if (TF0) {
        TF0 = 0;
        tickcount++;
        if (++sec_part >= TICKS_PER_S) { sec_part = 0; seconds++; }
    }
    /* WDRST = 0 restarts the watchdog's 24-bit count from the bottom: the
     * longest period it has, several seconds. */
    if (wdt_on && (uint16_t)(tickcount - beat) < LOOP_LIMIT) WDRST = 0x00;
    return tickcount;
}

uint32_t uptime(void) { return seconds; }

void heartbeat(void) { beat = tickcount; }

void wdt_start(void)
{
    beat = tickcount;
    WDRST = 0x00;
    WDKEY = 0x00;
    wdt_on = 1;
}

void wdt_pause(void)
{
    if (wdt_on) WDKEY = 0x55;
}

void wdt_resume(void)
{
    if (wdt_on) { WDRST = 0x00; WDKEY = 0x00; }
}

void io_init(void)
{
    /* Ports 1, 3 and 4 to plain I/O. RTI's own init does exactly this; the
     * SFR names come from SDCC's uPSD33xx.h. */
    P1SFS0 = 0; P1SFS1 = 0; P3SFS = 0; P4SFS0 = 0; P4SFS1 = 0;

    /* The longest MOVX read and write strobes (7 clocks, from 6), prefetch
     * queue and branch cache still on. The board's D4 line is slow to rise
     * (docs/REVERSE-ENGINEERING.md); a longer strobe gives it time on every
     * bus cycle — the W5100, the UARTs, and the PSD's own Flash. */
    BUSCON = 0xFF;

    P3 = P3_IDLE;                   /* quasi-bidirectional inputs idle high;
                                     * P3.5 is SIGNAL IN and reads nothing
                                     * unless its latch is set */
    P1 |= P1_INPUTS;

    XB(CS_CTL_B)  = 0x7F;           /* Port B: latched address A8-A14. Table 121
                                     * wants Control AND Direction, or the high
                                     * address never leaves the chip. */
    XB(CS_DIR_B)  = 0x7F;
    XB(CS_DIR_C)  = 0x7F;
    XB(CS_DIR_D)  = 0x03;
    XB(CS_MASKAB) = 0xFF;
    XB(CS_MASKBC) = 0x00;
    XB(CS_DRIVE_D) = 0x04;

    XB(CS_CTL_A)  = 0x00;           /* Port A is the seven IR outputs */
    XB(CS_DIR_A)  = IR_PORT_DIR;
    XB(CS_DOUT_A) = IR_IDLE;        /* PA6 is active low — 0x00 would drive
                                     * HIGH OUT continuously */
    outputs_release();
    /* Red: powered, not yet on the network. main.c turns it green. This runs
     * before crt0, so the value is written directly rather than via led_top,
     * which crt0 has not initialised yet. */
    pwrite(LED_LATCH, LED_RED);
}

void relay_set(uint8_t n, uint8_t on)
{
    uint8_t bit = 0x04 << (n - 1);
    uint8_t cur = XB(CS_DOUT_C);
    XB(CS_DOUT_C) = on ? (cur | bit) : (cur & ~bit);
}

void trigger_set(uint8_t n, uint8_t on)
{
    uint8_t bit = 0x20 << (n - 1);
    /* active low, and P1.0-P1.4 must stay high: they are the UART interrupt
     * lines and contact input 3 */
    P1 = on ? ((P1 | P1_INPUTS) & ~bit) : (P1 | P1_INPUTS | bit);
}

void outputs_release(void)
{
    XB(CS_DOUT_C) = XB(CS_DOUT_C) & ~RELAY_MASK;
    P1 = P1 | P1_INPUTS | TRIGGER_MASK;
}

uint8_t contacts(void)
{
    return (uint8_t)((P3 & 0x01)
                   | ((XB(CS_DIN_C) >> 6) & 0x02)
                   | ((P1 >> 2) & 0x04));
}

void leds_ir(uint8_t mask)
{
    led_shadow = mask & LED_IR_MASK;        /* only the LEDs are state */
    pwrite(LED_LATCH, (uint8_t)(led_top | led_shadow));
}

void leds_refresh(void)
{
    pwrite(LED_LATCH, (uint8_t)(led_top | (led_shadow & LED_IR_MASK)));
}

void leds_status(uint8_t green_red)
{
    led_top = green_red & (LED_GREEN | LED_RED);
    leds_refresh();
}

/* There is deliberately no way to clear bits 6-7 from outside this file. */
