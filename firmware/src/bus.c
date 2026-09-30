#include "board.h"
#include "bus.h"

static volatile __xdata uint8_t *__data xptr;
static __data uint8_t stage;

uint8_t xrd(uint16_t a)
{
    xptr = (volatile __xdata uint8_t *)a;
    return *xptr;
}

void pwrite(uint16_t addr, uint8_t val)
{
    uint8_t keep = P3;
    stage = val;                    /* IRAM, not XDATA — see bus.h */
    P3 = keep & ~0x02;              /* clear ONLY P3.1 */
    XB(addr) = stage;
    P3 = keep;
}

void pwrite_p3(uint16_t addr, uint8_t val, uint8_t p3)
{
    stage = val;
    P3 = p3 | 0xC0;                 /* P3.6/P3.7 are /WR and /RD; leave them */
    XB(addr) = stage;
    P3 = P3_IDLE;
}


uint8_t xrd3(uint16_t a)
{
    uint8_t v = xrd(a);
    (void)xrd(0x2000);
    v |= xrd(a);
    (void)xrd(0x2001);
    return (uint8_t)(v | xrd(a));
}

uint8_t str_len(const char *s) { uint8_t n = 0; while (s[n]) n++; return n; }

uint8_t str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
