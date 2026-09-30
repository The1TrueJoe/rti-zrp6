/* rules.h — "when this remote button is pressed, do this".
 *
 * A rule is a button code and any command the box already accepts over MQTT,
 * written as one line:
 *
 *     14.0.2.8.0.2.9|ir/1/send|nec 0x04 0x08 0
 *     14.14.15|relay/2/set|ON
 *     16.14.13|serial/1/tx|PWR ON\r
 *
 * so a rule can do anything a client could: IR in NEC or Pronto, relays,
 * triggers, serial. Several rules may share a code; they run in slot order.
 * Rules live in the secondary Flash (store.h) and survive power cuts. The box
 * needs nothing else on the network to act on the remote.
 */
#ifndef ZRP6_RULES_H
#define ZRP6_RULES_H
#include <stdint.h>
#include "store.h"

#define RULE_CODE_MAX     47
#define RULE_TOPIC_MAX    31
#define RULE_PAYLOAD_MAX  420

/* Parse "code|topic|payload" into a Flash record. Returns its length, or 0
 * if the text is not a valid rule. */
uint16_t rule_parse(const uint8_t *in, uint16_t n, uint8_t *rec);

/* 1 if slot holds a valid rule for this code. */
uint8_t rule_matches(uint8_t slot, const char *code);

/* Copy a rule's topic (NUL-terminated) and payload (NUL-terminated, length
 * returned) out of Flash. */
uint16_t rule_action(uint8_t slot, char *topic, uint8_t *payload);

/* "code|topic|payload" for a slot, or 0 if the slot is empty. */
uint16_t rule_text(uint8_t slot, uint8_t *out);

#endif
