/* mqtt.h — an MQTT 3.1.1 client, on the box itself.
 *
 * Only the parts a device needs: CONNECT, SUBSCRIBE, PUBLISH at QoS 0, and
 * PINGREQ. No QoS 1/2, no session state, no TLS. That is the whole reason it
 * fits: the W5100 runs TCP in hardware, so what is left is byte packing.
 */
#ifndef ZRP6_MQTT_H
#define ZRP6_MQTT_H
#include <stdint.h>

/* Called for every PUBLISH that arrives. `topic` is NUL-terminated and is the
 * part AFTER the base topic and its slash, so a handler matches on
 * "relay/1/set" rather than the whole path. */
typedef void (*mqtt_cb)(const char *topic, const uint8_t *payload,
                        uint16_t len) __reentrant;

void mqtt_init(mqtt_cb on_message);

/* Drive the client. Call often; it never blocks for long. Returns 1 while
 * connected. Handles connecting, the keepalive ping and reconnection. */
uint8_t mqtt_poll(void);

uint8_t mqtt_connected(void);

/* Publish below the base topic. `retain` matters: state is retained so a UI
 * that connects later sees the world as it is, commands are not. */
void mqtt_pub(const char *topic, const uint8_t *payload, uint16_t len,
              uint8_t retain);
void mqtt_pub_str(const char *topic, const char *s, uint8_t retain);

void mqtt_sub(const char *filter);

#endif
