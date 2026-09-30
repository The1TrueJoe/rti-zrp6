/* delay.h — the timing primitives everything else is measured against. */
#ifndef ZRP6_DELAY_H
#define ZRP6_DELAY_H
#include <stdint.h>

/* n * 3 machine cycles; n == 0 means 256. The IR carrier is built out of this,
 * so its cost is part of the calibration: 38 kHz is dly(15) with one Port A
 * write per edge. Changing what the burst writes changes the carrier. */
void dly(uint8_t n) __naked;

/* k * 65536 DJNZ pairs. The host times the reply and divides, which is how the
 * MCU clock was measured (8.95 M machine-cycles/s) without a scope. */
#endif
