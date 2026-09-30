/* net.h — the box's address: DHCP, a static address, or the fallback.
 *
 * The W5100 has no DHCP client, so this is one, as small as it can be: a UDP
 * socket on port 68 for the moment of each exchange, taken from the listeners
 * (ws_borrow) once the box is serving. docs/MQTT.md, "Network".
 */
#ifndef ZRP6_NET_H
#define ZRP6_NET_H
#include <stdint.h>
#include "bank.h"

#define NM_DHCP      0
#define NM_STATIC    1
#define NM_FALLBACK  2          /* DHCP wanted, nobody answered: cfg's address */

/* Start the W5100 and get an address: a static one at once, or DHCP for up to
 * ~10 s before settling on the fallback. Runs before the broker listens. */
void net_start(void) BANKED;

/* DHCP housekeeping — renewing, or retrying from fallback. Cheap when there
 * is nothing to do; call every loop. */
void net_poll(void) BANKED;

/* After the W5100 has been reset under us: give it the address again. */
void net_reapply(void) BANKED;

uint8_t net_mode(void) BANKED;
const uint8_t *net_ip(void) BANKED;
const uint8_t *net_mask(void) BANKED;
const uint8_t *net_gw(void) BANKED;

/* Set when the address or mode changed after boot. The main loop drops every
 * client (their connections were to the old address) and republishes. */
extern __xdata uint8_t net_changed;

#endif
