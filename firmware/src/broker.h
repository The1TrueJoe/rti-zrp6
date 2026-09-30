/* broker.h — the ZRP-6 is its own MQTT broker.
 *
 * The box is useful on its own: point a client at it and everything it has is
 * there, with no broker to stand up first. Joining a house broker is a
 * separate, optional thing (bridge.c) rather than a precondition.
 *
 * Scope is deliberately small, and the limits are the reason it fits in an
 * 8032: QoS 0 only, no persistent sessions, no wildcards except a trailing
 * '#', and a fixed number of clients and retained topics. That covers a
 * control surface and a house controller, which is all this has to serve.
 */
#ifndef ZRP6_BROKER_H
#define ZRP6_BROKER_H
#include <stdint.h>
#include "bank.h"

#define BR_CLIENTS   4          /* every W5100 socket */
#define BR_SUBS      4          /* filters per client */
#define BR_RETAINED  32
#define BR_TOPIC     32          /* "zrp6/users/<15 characters>/set" */
#define BR_PAYLOAD   24

void broker_init(void);

/* Forget every client and listen again, keeping retained state. Used after the
 * W5100 has been reinitialised, when the sockets those clients were on no
 * longer exist. */
void broker_dropall(void);
void broker_poll(void);

/* Publish from the box itself: goes to every matching subscriber, is stored if
 * retained, and is offered to the bridge. */
void broker_publish(const char *topic, const uint8_t *payload, uint16_t len,
                    uint8_t retain);
void broker_publish_str(const char *topic, const char *s, uint8_t retain);

/* Anything a client publishes that the box should act on arrives here. */
/* __reentrant because SDCC passes the arguments of an indirect call on the
 * stack rather than in the static overlay it cannot analyse; three arguments
 * is past what the register path carries. */
typedef void (*broker_cb)(const char *topic, const uint8_t *payload,
                          uint16_t len) __reentrant BANKED;
void broker_on_command(broker_cb cb);

uint8_t broker_clients(void);

#endif
