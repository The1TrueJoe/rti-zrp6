#include "delay.h"


/* Both of these are __naked, so they must save every register they touch.
 * SDCC does not track inline-assembly register use and will hand the CALLER
 * the same register — a delay that clobbered r7 once ate a loop counter and
 * hung the firmware before it reached its first useful instruction. */

void dly(uint8_t n) __naked
{
    n;
    __asm
        push 0x07
        mov  r7, dpl
      00001$:
        nop
        djnz r7, 00001$
        pop  0x07
        ret
    __endasm;
}

