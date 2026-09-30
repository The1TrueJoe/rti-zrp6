/* app.h — what the box actually does, expressed as MQTT topics.
 *
 * Everything below this line is hardware (io, ir, uart, capture); everything
 * above it is transport (broker, ws, http). This is the only file that knows
 * both, and the only one that has to change when a topic is added.
 *
 * The topic tree is documented in ../../docs/MQTT.md and is the same one the
 * service speaks, so a client cannot tell whether it is talking to the box or
 * to the bridge.
 */
#ifndef ZRP6_APP_H
#define ZRP6_APP_H
#include <stdint.h>
#include "bank.h"

void app_init(void) BANKED;

/* Publishes anything that changed: contacts, serial bytes, remote presses.
 * Never blocks for longer than one RTI remote frame. */
void app_poll(void) BANKED;

/* Publish <base>/debug/stackfree, retained. */
void app_stack(uint8_t free) BANKED;

/* Republish the network topics: the address changed (net.c). */
void app_net(void) BANKED;

/* Every PUBLISH the broker receives is offered here. Topics are full paths
 * including the base. */
void app_command(const char *topic, const uint8_t *payload,
                 uint16_t len) BANKED __reentrant;

#endif
