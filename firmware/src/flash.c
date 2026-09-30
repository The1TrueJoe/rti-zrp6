#include "board.h"
#include "bus.h"
#include "io.h"
#include "flash.h"

/* JEDEC command addresses are relative to the sector being programmed. */
#define UNLOCK1(base) ((base) + 0x555)
#define UNLOCK2(base) ((base) + 0xAAA)

/* DQ6 toggles on every read while the array is busy; DQ5 going high means the
 * operation failed and the toggle will never stop on its own.
 *
 * Timed in ticks() rather than loop iterations. The first version counted
 * 60 000 iterations — about 0.6 s — and an 8 KB sector erase can take longer
 * than that, so a successful erase would have been reported as a failure. */
static uint8_t poll(uint16_t addr, uint16_t limit)
{
    uint8_t a, b;
    uint16_t t0 = ticks();
    for (;;) {
        a = xrd(addr);
        b = xrd(addr);
        if (!((a ^ b) & 0x40)) return 0;
        if (a & 0x20) {
            a = xrd(addr);
            b = xrd(addr);
            return ((a ^ b) & 0x40) ? 1 : 0;
        }
        if ((uint16_t)(ticks() - t0) > limit) return 1;
    }
}

/* After every operation the array must be put back in read mode. Leaving it in
 * status mode makes every later read return 27 67 27 67..., which looks
 * exactly like a dead bus. */
static void reset_array(uint16_t base)
{
    XB(base) = 0xF0;
}

/* Every erase and every byte is checked by reading it back, and retried a
 * few times before the caller is told it failed.
 *
 * Without that, a command sequence the Flash never accepted looked exactly
 * like success: the status bits read steady, so poll() said "done", the
 * sector was never erased, the programs that followed tried to turn 0 bits
 * back into 1s and left the whole array in error mode (reading 27 67 27 67),
 * and the store answered "saved" for a rule that was gone. It happened with
 * some builds and not others, depending only on where the code landed — the
 * signature of the board's D4 bus fault (docs/REVERSE-ENGINEERING.md). */
static uint8_t erase_once(uint16_t base)
{
    uint8_t rc;
    uint16_t off;
    XB(UNLOCK1(base)) = 0xAA;
    XB(UNLOCK2(base)) = 0x55;
    XB(UNLOCK1(base)) = 0x80;
    XB(UNLOCK1(base)) = 0xAA;
    XB(UNLOCK2(base)) = 0x55;
    XB(base) = 0x30;                    /* sector erase */
    rc = poll(base, TICKS_S(10));
    reset_array(base);
    if (rc) return rc;
    for (off = 0; off < 0x2000; off++)
        if (xrd3(base + off) != 0xFF) return 1;
    return 0;
}

uint8_t flash_erase(uint16_t base)
{
    uint8_t t;
    for (t = 0; t < 3; t++)
        if (!erase_once(base)) return 0;
    return 1;
}

/* One byte, by value. Taking the byte as an argument rather than a pointer is
 * deliberate: the secondary Flash is one array, and while a program operation
 * runs, a read ANYWHERE in it returns status bits instead of data. A copy from
 * sector A into sector B must therefore read each source byte before starting
 * the operation that writes it — which a by-value interface makes impossible
 * to get wrong. */
uint8_t flash_program(uint16_t addr, uint8_t value)
{
    uint16_t base = addr & 0xE000;
    uint8_t t, got;
    if (value == 0xFF) return 0;        /* erased Flash already reads 0xFF */
    for (t = 0; t < 3; t++) {
        XB(UNLOCK1(base)) = 0xAA;
        XB(UNLOCK2(base)) = 0x55;
        XB(UNLOCK1(base)) = 0xA0;       /* byte program */
        XB(addr) = value;
        if (poll(addr, TICKS_MS(100))) reset_array(base);   /* a byte takes us */
        /* Three reads ORed. The D4 fault only drops bits on a read, so a
         * single read-back can show a 0 where the program left a 1 — and pass
         * a byte that is actually wrong. */
        got = xrd3(addr);
        if (got == value) return 0;
        /* A program can only clear bits: once a bit that should be 1 is 0,
         * no retry will help, and the caller has to start the sector again. */
        if (value & ~got) return 1;
    }
    return 1;
}
