/* io.h — relays, 12 V triggers, contact inputs, front panel, IR port pins. */
#ifndef ZRP6_IO_H
#define ZRP6_IO_H
#include <stdint.h>

void io_init(void);

/* A coarse clock. Timer 0 free-runs as a 16-bit counter and ticks() counts
 * its overflows — 21.8 ms each (board.h, TICKS_S) — with no interrupt
 * involved. Call it often; missing an overflow only makes time pass slower,
 * which is why nothing here measures anything short. uptime() is whole
 * seconds since boot on the same clock. */
void ticks_init(void);
uint16_t ticks(void);
uint32_t uptime(void);

/* The hardware watchdog. Once started it resets the whole chip unless ticks()
 * keeps being called — and ticks() only feeds it while the main loop has
 * called heartbeat() within the last 30 s. So a stall anywhere resets the box
 * in a few seconds, and so does a loop that waits on ticks() for ever. Pause
 * it around a bootloader call, which cannot call ticks(). */
void wdt_start(void);
void wdt_pause(void);
void wdt_resume(void);
void heartbeat(void);

/* n is 1-3. Relays are Port C, active high; triggers are P1, active low. */
void relay_set(uint8_t n, uint8_t on);
void trigger_set(uint8_t n, uint8_t on);
void outputs_release(void);

/* Bit 0 = contact 1 (P3.0), bit 1 = contact 2 (Port C.7), bit 2 = contact 3
 * (P1.4). A set bit is an OPEN contact — they read high at rest. */
uint8_t contacts(void);

/* mask bit 0 lights IR LED 1 ... bit 5 lights IR LED 6. There is no LED 7:
 * the seventh IR output is HIGH OUT and has its own indicator. */
void leds_ir(uint8_t mask);

/* Rewrite the panel latch from the shadow. The latch shares its chip select
 * with the 16C554, so anything that touches the UART can disturb it; call
 * this after doing so. */
void leds_refresh(void);

/* The status LED: LED_GREEN, LED_RED, both or neither. */
void leds_status(uint8_t green_red);

#endif
