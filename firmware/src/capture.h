/* capture.h — SIGNAL IN, where RTI remote presses arrive.
 *
 * The RM-433 hands us DEMODULATED baseband; there is no 40 kHz carrier to
 * strip. The pin idles LOW and pulses HIGH, inverted from a normal IR
 * demodulator — which is why it reads 0 at rest and looks dead until you watch
 * for edges rather than levels.
 */
#ifndef ZRP6_CAPTURE_H
#define ZRP6_CAPTURE_H
#include <stdint.h>

/* Record up to `max` transitions as run lengths. Returns the count and sets
 * *start to the level the capture began at.
 *
 * This BLOCKS until it sees its edges. The board stops answering UDP while it
 * runs — it still answers ICMP, because the W5100 does ping in hardware, which
 * makes the symptom confusing. Any caller polling this must leave gaps. */
uint16_t sigin_capture(uint16_t *runs, uint16_t max, uint8_t *start);

#endif
