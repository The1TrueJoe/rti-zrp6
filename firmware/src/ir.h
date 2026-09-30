/* ir.h — modulated infrared out.
 *
 * Timings are expressed in CARRIER PERIODS, not microseconds, because that is
 * how Pronto hex expresses them: a code copied from a remote database goes out
 * unmodified.
 */
#ifndef ZRP6_IR_H
#define ZRP6_IR_H
#include <stdint.h>

/* port 1-7. 1-6 are Port A bits 0-5 active high and light their panel LED for
 * the duration of the burst; port 7 is HIGH OUT, active low, no LED.
 * `half` is the carrier half-period in dly() units — 30 is 38 kHz on this
 * build, measured optically against an IR learner rather than derived.
 *
 * That number belongs to this code, not to the hardware: it counts the
 * instructions around the delay too. Change anything in the mark/space loop
 * and re-measure against a receiver. */
void ir_begin(uint8_t port, uint8_t half);
void ir_mark(uint16_t periods);
void ir_space(uint16_t periods);
void ir_end(void);

/* The ports' modes (config.h ir_mode[7]): sets what each port rests at. An IR
 * port and an RS-232-polarity serial port rest low; a TTL serial port rests
 * high. Call at boot and whenever a mode changes. */
void ir_modes(const uint8_t *modes);

/* One-way serial out of an IR port, 8N1, at the port's mode (baud index |
 * IRM_TTL). Timed off Timer 0, so the baud rate is the crystal's, not the
 * instruction count's. Blocks for the whole transmission. */
void ir_serial(uint8_t port, uint8_t mode, const uint8_t *buf, uint16_t len);

#endif
