/* rti.h — recognising RTI remote presses from SIGNAL IN.
 *
 * What arrives (measured 2026-09-16, research/notes/DEVICE-STATE.md): the
 * RM-433 drives P3.5 with demodulated baseband, runs of T or 2T with T about
 * 417 us, idling LOW. The encoding is pulse-distance: a frame is a string of
 * short (a) and long (b) symbols, and what identifies a button is the COUNT of
 * short symbols between successive long ones. Every frame opens with the
 * counts 16, 16, 14, 12, 14 — preamble and system ID — and the rest is the
 * button. Codes are static; twelve buttons gave twelve codes, 1:1.
 *
 * Pure C with no hardware in it, so the same file is built for the 8032 and
 * compiled on a desktop against recorded frames (see firmware/test/).
 */
#ifndef ZRP6_RTI_H
#define ZRP6_RTI_H
#include <stdint.h>

/* Up to 400 runs. A frame is ~135 of them, so 400 always spans the rest of
 * whatever frame the capture walked into plus the whole next one — which is
 * the difference between catching a press and missing it one time in four.
 *
 * runs[0..n-1] are consecutive level durations in capture ticks, alternating
 * polarity. `ended_idle` is 1 if the capture stopped because the line went
 * quiet (so the last frame is complete) and 0 if it ran out of buffer (so the
 * last frame may be cut short and is not trusted).
 *
 * Writes the button code as dotted decimal — "14.0.2.8.0.2.9" — into out
 * (at least RTI_CODE_MAX bytes) and returns its length, or 0 if no complete
 * frame with the RTI preamble was found. It never guesses: a partial or
 * malformed frame is 0, not a wrong code. */
#define RTI_CODE_MAX 48
uint8_t rti_decode(const uint16_t *runs, uint16_t n, uint8_t ended_idle,
                   char *out);

#endif
