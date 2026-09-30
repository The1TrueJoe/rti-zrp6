/* sha1.h — needed for exactly one thing: the WebSocket handshake.
 *
 * RFC 6455 makes the server prove it understood the request by returning
 * base64(SHA-1(key + GUID)). There is no way around it and no smaller digest
 * that will do, so an 8032 that wants a browser talking to it has to compute
 * SHA-1. This is the whole of the cryptography in this firmware.
 */
#ifndef ZRP6_SHA1_H
#define ZRP6_SHA1_H
#include <stdint.h>
#include "bank.h"

void sha1_init(void) BANKED;
void sha1_update(const uint8_t *p, uint16_t n) BANKED;
void sha1_final(uint8_t out[20]) BANKED;

/* 20 bytes in, 28 characters plus a NUL out. */
void b64_20(const uint8_t in[20], char out[29]) BANKED;

/* Base64 text (up to the first character that is not base64) into bytes, at
 * most max of them. Returns how many. For HTTP Basic auth. */
uint8_t b64_decode(const uint8_t *in, uint16_t n, uint8_t *out, uint8_t max) BANKED;

#endif
