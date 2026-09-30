/* bus.h — reading and writing the external bus safely.
 *
 * Two hazards live here, and both produced convincing false hardware
 * diagnoses before they were understood. See docs/REVERSE-ENGINEERING.md.
 */
#ifndef ZRP6_BUS_H
#define ZRP6_BUS_H
#include <stdint.h>

/* A real read. SDCC elides a volatile read that follows a write to the same
 * address, substituting the value just written — so a readback through XB()
 * confirms whatever you wrote rather than what the hardware holds. */
uint8_t xrd(uint16_t a);

/* A byte of secondary Flash as three reads ORed, with unrelated reads
 * between: the board's D4 fault only ever clears bit 4, and depends on the
 * bus state just before. For the config and user records. */
uint8_t xrd3(uint16_t a);

/* The two string helpers everything uses. <string.h>'s generic versions came
 * out larger (firmware/README). */
uint8_t str_len(const char *s);
uint8_t str_eq(const char *a, const char *b);

/* Write one byte to a peripheral with P3.1 held low for the access.
 *
 * The data is staged in IRAM first: a write whose source operand lives in
 * XDATA compiles to a MOVX read immediately followed by the MOVX write, and
 * the peripheral latches 0x00. An immediate constant works; a variable does
 * not. Everything that writes a peripheral must go through here. */
void pwrite(uint16_t addr, uint8_t val);

/* The same, with P3 forced to `p3` for the access instead of clearing P3.1.
 * Staged through the same single IRAM byte: no buffer, no stack. */
void pwrite_p3(uint16_t addr, uint8_t val, uint8_t p3);

#endif
