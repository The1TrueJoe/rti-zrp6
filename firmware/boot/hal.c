/* hal.c — the bootloader's hardware: the page register, the data window, and
 * the JEDEC program/erase sequences for main Flash.
 *
 * This runs from the secondary Flash, which is what makes it possible at
 * all: main Flash cannot be read — not even for an instruction fetch — while
 * one of its sectors is being programmed or erased.
 *
 * No statics anywhere: the application calls into this and owns all of RAM.
 */
#include "../src/board.h"
#include "ota.h"
#include "hal.h"

#define WIN(off)    XB(OTA_WINDOW + (off))

/* Every page-register write keeps bits 6-7, the code bank and the firmware
 * slot, and changes only the data window. The application calls in from
 * whichever slot it runs in, and returns into it: with bit 7 cleared, the
 * return — and any interrupt taken meanwhile — would fetch from slot A, which
 * an update running from slot B is erasing. The PLD decodes the window from
 * bits 0-3 alone (research/captures/zrp6_v10.abl), so these are the caller's
 * to keep. At reset the register is 0 and main.c sets the slot itself. */
#define PAGE_SET(p) (XB(CS_PAGE) = (uint8_t)((XB(CS_PAGE) & (OTA_PG_BANK | OTA_PG_SLOT)) | (p)))

/* DQ6 toggles on every read while the array is busy; DQ5 set means it
 * failed. `outer` counts 65536 polls of a few microseconds each: nothing here
 * has a timer, because the application owns Timer 0.
 *
 * The status reads are in their own function on purpose. SDCC substitutes
 * the value just written for a volatile read of the same address in the same
 * function (docs/REVERSE-ENGINEERING.md), and the caller has just written the
 * data byte here: inlined, DQ6 would look steady and every operation would
 * look finished at once. */
static uint8_t poll(volatile __xdata uint8_t *p, uint8_t outer)
{
    uint8_t x, y;
    uint16_t inner = 0;
    for (;;) {
        x = *p;
        y = *p;
        if (!((x ^ y) & 0x40)) return 0;
        if (x & 0x20) {
            x = *p;
            y = *p;
            return ((x ^ y) & 0x40) ? 1 : 0;
        }
        if (!--inner && !outer--) return 1;
    }
}

void hal_page(uint8_t page)
{
    PAGE_SET(page);
}

uint8_t hal_running(void)
{
    return (XB(CS_PAGE) & OTA_PG_SLOT) ? 1 : 0;
}

uint8_t hal_rd(uint8_t page, uint16_t off)
{
    PAGE_SET(page);
    return *(volatile __xdata uint8_t *)(OTA_WINDOW + off);
}

/* Every program and erase is read back and retried. A command sequence the
 * Flash did not accept reads exactly like a finished one — the status bits
 * are steady — and the board's D4 fault made that happen in the application
 * (docs/REVERSE-ENGINEERING.md). The CRC checks above catch it here too, but
 * a retry is cheaper than a failed install. */
uint8_t hal_prog(uint8_t page, uint16_t off, uint8_t v)
{
    uint8_t t, got;
    if (v == 0xFF) return 0;            /* erased Flash already reads 0xFF */
    for (t = 0; t < 3; t++) {
        PAGE_SET(page);
        WIN(0x555) = 0xAA;
        WIN(0xAAA) = 0x55;
        WIN(0x555) = 0xA0;              /* byte program */
        WIN(off) = v;
        /* A byte takes 14 us typically and 150 us at worst. */
        if (poll((volatile __xdata uint8_t *)(OTA_WINDOW + off), 0))
            WIN(0) = 0xF0;              /* back to read mode */
        got = *(volatile __xdata uint8_t *)(OTA_WINDOW + off);
        if (got == v) return 0;
        if (v & ~got) return 1;         /* a bit already 0: only an erase helps */
    }
    return 1;
}

uint8_t hal_erase(uint8_t page)
{
    uint8_t t;
    uint16_t o;
    for (t = 0; t < 3; t++) {
        PAGE_SET(page);
        WIN(0x555) = 0xAA;
        WIN(0xAAA) = 0x55;
        WIN(0x555) = 0x80;
        WIN(0x555) = 0xAA;
        WIN(0xAAA) = 0x55;
        WIN(0) = 0x30;                  /* sector erase */
        /* 1 s typical, 10 s at worst for a 32 KB sector: allow well over. */
        (void)poll((volatile __xdata uint8_t *)OTA_WINDOW, 60);
        WIN(0) = 0xF0;
        o = 0;
        do {
            if (*(volatile __xdata uint8_t *)(OTA_WINDOW + o) != 0xFF) break;
        } while (++o < OTA_SECTOR);
        if (o == OTA_SECTOR) return 0;
    }
    return 1;
}

/* The status LED is on the front-panel latch behind the '138, which needs
 * the latched high address out on Port B and P3.1 low for the access — the
 * same setup the application's io_init() does. The value is always an
 * immediate: a latch write sourced from XDATA delivers 0x00. */
void hal_init(void)
{
    BUSCON = 0xFF;                      /* longest MOVX strobes: see io.c */
    P3SFS = 0;
    P3 = P3_IDLE;
    XB(CS_CTL_B) = 0x7F;
    XB(CS_DIR_B) = 0x7F;
}

void hal_led(uint8_t state)
{
    P3 = P3_IDLE & ~0x02;
    if (state) XB(LED_LATCH) = LED_RED;
    else       XB(LED_LATCH) = 0;
    P3 = P3_IDLE;
}
