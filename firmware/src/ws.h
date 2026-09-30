/* ws.h — one listener, three protocols.
 *
 * A browser cannot open a TCP socket, so the UI has to reach the broker over a
 * WebSocket; a house controller wants plain MQTT on 1883; and both of them
 * first have to fetch the page. Rather than spend a scarce W5100 socket on
 * each, every connection is sniffed on its first byte:
 *
 *   'G'   an HTTP request — either a WebSocket upgrade, or a page to serve
 *   0x10  an MQTT CONNECT arriving raw, with no HTTP in front of it
 *
 * After that the socket carries MQTT packets either way and the broker above
 * never learns which. Reassembly happens in the W5100's own 2 KB per socket
 * (see w5100.h), so three concurrent clients cost this layer a few bytes of
 * state rather than three stream buffers.
 */
#ifndef ZRP6_WS_H
#define ZRP6_WS_H
#include <stdint.h>

#define WS_GONE 0xFFFF          /* the client left; the caller should re-listen */

void ws_listen(uint8_t s);
/* Keep a listener on both ports while there are sockets to spare, and the
 * last one on the page's port. Cheap; call it every loop. */
void ws_balance(void);
void ws_close(uint8_t s);

/* A client has logged in on s (broker.c). A priority client — client ID
 * starting '!' — is never dropped to make room; see ws_balance(). */
void ws_client(uint8_t s, uint8_t priority);

/* Take an idle listener for a moment (DHCP, net.c) and give it back. 0xFF
 * when every socket is busy. */
uint8_t ws_borrow(void);
void ws_return(uint8_t s);

/* Returns the length of one complete MQTT packet — or of several, back to
 * back, when a client coalesces them into one WebSocket frame. 0 means
 * nothing is ready yet, which is the usual answer. */
uint16_t ws_poll(uint8_t s, uint8_t *buf, uint16_t max);

void ws_send(uint8_t s, const uint8_t *buf, uint16_t len);

/* Run a queued POST /update, if there is one. Call from the top of the main
 * loop, where the stack is shallowest: see ws.c. */
void ws_ota_run(void);

#endif
