#include "board.h"
#include "bus.h"
#include "uart.h"

/* IRAM deliberately: an XDATA source makes the peripheral latch 0x00. */
static __data uint8_t stage[16];

void uart_config(uint8_t ch, uint8_t dll, uint8_t dlm, uint8_t lcr)
{
    uint16_t b = UART_BASE(ch);
    uint8_t keep = P3;
    /* Everything below is staged in IRAM first. A peripheral write whose
     * source is in XDATA delivers 0x00 — that is what made LCR, DLL and DLM
     * refuse to configure while FCR and MCR, written as constants, worked. */
    uint8_t d0 = dll, d1 = dlm, lc = lcr;
    P3 = keep & ~0x02;
    XB(b + UART_LCR) = 0x80;            /* DLAB */
    XB(b + UART_DLL) = d0;
    XB(b + UART_IER) = d1;
    XB(b + UART_LCR) = lc;              /* DLAB off, frame format */
    XB(b + UART_FCR) = 0x07;            /* enable and clear both FIFOs */
    /* DTR + RTS + OUT2. OUT2 is not a modem signal here: on a 16550-family
     * part it gates the INT output, which is tri-stated without it. That is
     * why P1.0-P1.3 read dead no matter what the UART was doing. */
    XB(b + UART_MCR) = 0x0B;
    /* Receive-data-available interrupt ENABLED — not to take an interrupt
     * (the 8032's are off) but so the chip drives its INT pin, which lands
     * on P1.0-P1.3. That turns "has a byte arrived?" into an SFR read
     * instead of four bus cycles. See app.c. */
    XB(b + UART_IER) = 0x01;
    P3 = keep;
}

void uart_write(uint8_t ch, const uint8_t *buf, uint8_t n)
{
    uint16_t b = UART_BASE(ch);
    uint8_t keep = P3, i, k, chunk;
    uint16_t guard;

    /* Staged through IRAM a chunk at a time rather than one big buffer: a
     * peripheral write whose source is in XDATA delivers 0x00, but IRAM is
     * only 256 bytes and the overlay area needs room too. */
    while (n) {
        chunk = n > sizeof(stage) ? sizeof(stage) : n;
        for (i = 0; i < chunk; i++)
            stage[i] = buf[i];
        P3 = keep & ~0x02;
        for (k = 0; k < chunk; k++) {
            guard = 20000;
            while (!(xrd(b + UART_LSR) & 0x20) && --guard)   /* THRE */
                ;
            XB(b + UART_DLL) = stage[k];
        }
        P3 = keep;
        buf += chunk;
        n -= chunk;
    }
}

uint8_t uart_read(uint8_t ch, uint8_t *buf, uint8_t max)
{
    uint16_t b = UART_BASE(ch);
    uint8_t keep = P3, n = 0;

    /* LSR is checked first and RBR touched only when there is a byte — a
     * register dump starting at offset 0 would pop the FIFO.
     *
     * And the data-ready bit is confirmed by a second read with an unrelated
     * access in between, for the reason set out in w5100.c: this board's D4 is
     * marginal, and a single misread of LSR here does not lose a byte — it
     * invents one, pops an empty FIFO, and publishes a NUL to
     * serial/<n>/rx that nobody sent. */
    P3 = keep & ~0x02;
    for (;;) {
        uint8_t lsr = xrd(b + UART_LSR);
        if (!(lsr & 0x01)) break;               /* nothing waiting */
        (void)xrd(b + UART_SCR);
        if (!(xrd(b + UART_LSR) & 0x01)) break; /* confirmed, see above */
        if (lsr & 0x1E) {
            /* Overrun, parity, framing or break. An RS-232 input with nothing
             * plugged into it floats, which reads as a continuous break and
             * delivers an endless stream of NULs — port 2 was publishing them
             * to serial/2/rx several times a second. The byte still has to be
             * read to clear the condition; it just is not data. */
            (void)xrd(b + UART_DLL);
            continue;
        }
        if (n == max) break;
        buf[n++] = xrd(b + UART_DLL);
    }
    P3 = keep;
    return n;
}
