/* http.h — the UI, served from Flash.
 *
 * The whole interface is one gzipped file sitting in main Flash page 0. Main
 * Flash is code-only in this PLD, so it is read with MOVC and handed straight
 * to the W5100's transmit ring: serving the page costs no XDATA at all, which
 * matters when there are 8 KB of it in total.
 *
 * The blob is placed there by firmware/build.sh, not by the firmware. See
 * ../ui/pack.py for the four-byte header this expects.
 */
#ifndef ZRP6_HTTP_H
#define ZRP6_HTTP_H
#include <stdint.h>
#include "bank.h"

/* req holds the request headers, already consumed from the socket. */
void http_serve(uint8_t s, const uint8_t *req, uint16_t len) BANKED;

/* A short plain-text response: status is e.g. "200 OK". */
void http_reply(uint8_t s, __code const char *status, __code const char *body) BANKED;

/* 0 if no UI has been flashed. */
uint16_t http_ui_size(void) BANKED;

#endif
