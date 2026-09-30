#include "board.h"
#include "io.h"
#include "capture.h"

uint16_t sigin_capture(uint16_t *runs, uint16_t max, uint8_t *start)
{
    uint8_t prev, cur;
    uint16_t n;
    uint16_t guard, ticks;

    P3 |= SIGIN_MASK;               /* the latch must be high to read the pin */
    prev = P3 & SIGIN_MASK;
    *start = prev ? 1 : 0;

    guard = 0;
    while (((P3 & SIGIN_MASK) == prev) && ++guard)
        ;
    if (!guard)
        return 0;                   /* nothing arrived */
    prev ^= SIGIN_MASK;

    for (n = 0; n < max; n++) {
        ticks = 0;
        do {
            cur = P3 & SIGIN_MASK;
            if (++ticks == 0)
                break;              /* wrapped: the line went idle */
        } while (cur == prev);
        runs[n] = ticks;
        if (!ticks)
            break;
        prev = cur;
    }
    return n;
}

