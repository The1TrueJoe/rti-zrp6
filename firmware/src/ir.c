#include "board.h"
#include "io.h"
#include "delay.h"
#include "ir.h"
#include "config.h"

static __data uint8_t on_val, off_val, half_t;
static __data uint8_t t0, bits;             /* ir_serial() */

/* What Port A holds between transmissions: IR_IDLE, with the jack of every
 * TTL serial port held high. */
static __xdata uint8_t idle_a = IR_IDLE;

/* Port A with one port's jack driven high or low. Ports 1-6 are active high;
 * 7, HIGH OUT, is active low. */
static uint8_t jack(uint8_t v, uint8_t port, uint8_t high)
{
    uint8_t bit = port >= 7 ? IR_HIGHOUT : (uint8_t)(1 << (port - 1));
    if (port >= 7) high = !high;
    return high ? (uint8_t)(v | bit) : (uint8_t)(v & ~bit);
}

static void rest(void)
{
    off_val = idle_a;                   /* staged in IRAM: README rule 2 */
    XB(CS_DOUT_A) = off_val;
}

void ir_modes(const uint8_t *modes)
{
    uint8_t p, v = IR_IDLE;
    for (p = 1; p <= 7; p++)
        if ((modes[p - 1] & IRM_BAUD) && (modes[p - 1] & IRM_TTL)) v = jack(v, p, 1);
    idle_a = v;
    rest();
}

void ir_begin(uint8_t port, uint8_t half)
{
    half_t = half;
    off_val = jack(idle_a, port, 0);
    on_val = jack(idle_a, port, 1);
    leds_ir(port >= 7 ? 0 : (uint8_t)(1 << (port - 1)));   /* HIGH OUT has none */
    XB(CS_CTL_A) = 0x00;
    XB(CS_DIR_A) = IR_PORT_DIR;
    XB(CS_DOUT_A) = off_val;
}

void ir_mark(uint16_t periods)
{
    /* __data deliberately, and it matters. The build is --model-large, which
     * would otherwise put this counter in XDATA and add a MOVX to both halves
     * of every carrier period — enough to move 38 kHz somewhere else. */
    static uint16_t __data n;
    n = periods;
    while (n--) {
        XB(CS_DOUT_A) = on_val;
        dly(half_t);
        XB(CS_DOUT_A) = off_val;
        dly(half_t);
    }
}

void ir_space(uint16_t periods)
{
    /* the same instruction path as a mark, so a space is exactly as long as a
     * mark of the same count — only the emitter stays dark */
    static uint16_t __data n;
    n = periods;
    while (n--) {
        XB(CS_DOUT_A) = off_val;
        dly(half_t);
        XB(CS_DOUT_A) = off_val;
        dly(half_t);
    }
}

void ir_end(void)
{
    rest();
    leds_ir(0);
}

/* A bit period in sixteenths of a Timer 0 count (3.00 MHz, board.h), by baud
 * index. The sixteenths carry over from bit to bit, so the frame is as long
 * as it should be rather than rounded 10 times. */
static __code const uint16_t bit16[BAUD_COUNT + 1] = {
    0, 40020, 20010, 10005, 5002, 3335, 2501, 1251, 834, 417 };

/* Wait until `n` counts past t0, then move t0 there. TL0 is read alone: its
 * difference from t0 is right as long as it is sampled more often than every
 * 256 counts, and the loop takes about two. */
static void until(uint16_t n)
{
    while (n > 200) {
        while ((uint8_t)(TL0 - t0) < 200)
            ;
        t0 += 200;
        n -= 200;
    }
    bits = (uint8_t)n;
    while ((uint8_t)(TL0 - t0) < bits)
        ;
    t0 += bits;
}

void ir_serial(uint8_t port, uint8_t mode, const uint8_t *buf, uint16_t len)
{
    static __xdata uint16_t per, acc;
    static __xdata uint8_t b, k;
    uint8_t ttl = (uint8_t)(mode & IRM_TTL);
    if ((mode & IRM_BAUD) == 0 || (mode & IRM_BAUD) > BAUD_COUNT) return;
    per = bit16[mode & IRM_BAUD];
    /* on_val is a space (logic 0, the start bit), off_val a mark (logic 1,
     * idle and the stop bit). RS-232 polarity: a mark is the jack LOW. */
    on_val = jack(idle_a, port, !ttl);
    off_val = jack(idle_a, port, ttl);
    leds_ir(port >= 7 ? 0 : (uint8_t)(1 << (port - 1)));
    XB(CS_DOUT_A) = off_val;
    acc = 0;
    t0 = TL0;
    until(per >> 3);                    /* two bits of idle before the first */
    while (len--) {
        (void)ticks();                  /* keeps the clock, feeds the watchdog */
        b = *buf++;
        for (k = 0; k < 10; k++) {
            if (k == 0)                 XB(CS_DOUT_A) = on_val;
            else if (k == 9 || (b & 1)) XB(CS_DOUT_A) = off_val;
            else                        XB(CS_DOUT_A) = on_val;
            if (k && k < 9) b >>= 1;
            acc += per;
            until(acc >> 4);
            acc &= 15;
        }
    }
    rest();
    leds_ir(0);
}
