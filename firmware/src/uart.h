/* uart.h — the 16C554 quad UART at 0x3000.
 *
 * Channels 0 and 1 are the RS-232 ports; 2 and 3 are RS-485. Every register
 * access needs P3.1 held LOW, exactly as RTI's own driver does it — with P3.1
 * high the whole window reads back as address echo and the chip looks absent.
 */
#ifndef ZRP6_UART_H
#define ZRP6_UART_H
#include <stdint.h>

/* divisor 96 is 9600 baud from the board's 14.7456 MHz UART clock */
void uart_config(uint8_t ch, uint8_t dll, uint8_t dlm, uint8_t lcr);
void uart_write(uint8_t ch, const uint8_t *buf, uint8_t n);
uint8_t uart_read(uint8_t ch, uint8_t *buf, uint8_t max);

#endif
