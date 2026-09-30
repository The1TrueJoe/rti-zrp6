/* store.h — what lives in the secondary Flash, and how it changes safely.
 *
 * Two 8 KB sectors, A at 0x4000 and B at 0x6000, each a complete copy:
 *
 *   0x0000  config: network, IR port modes, serial settings (config.c)
 *   0x00F0  marker: A5 seq_lo seq_hi 5A — written LAST
 *   0x0100  14 rule slots of 512 bytes (rules.c)
 *   0x1D00  the users, in what was the 15th rule slot (users.c)
 *
 * The live copy is the sector with a valid marker and the higher sequence
 * number. A change is made by writing a complete new copy into the OTHER
 * sector, with one slot replaced, and marking it only once everything else is
 * in. Cut the power at any moment and the previous copy is still whole and
 * still preferred: nothing ever erases the live copy to rewrite it.
 */
#ifndef ZRP6_STORE_H
#define ZRP6_STORE_H
#include <stdint.h>
#include "bank.h"

#define STORE_CFG_OFF    0x0000
#define STORE_CFG_SIZE   0x00F0
#define STORE_RULE_OFF   0x0100
#define STORE_RULE_SIZE  512
#define STORE_RULES      14
#define STORE_USERS_SLOT 14             /* store_slot() of the user record */

uint16_t store_live(void);

/* Address of a rule slot in the live copy, readable as XDATA. */
uint16_t store_slot(uint8_t slot) BANKED;

/* New copy with slot replaced by rec[0..len-1] (len 0 empties it). 0 on
 * success. Blocks for a second or two — call it from the top of the loop. */
uint8_t store_commit(uint8_t slot, const uint8_t *rec, uint16_t len) BANKED;

/* The same for any region: [off, off+size) becomes rec[0..len-1], padded with
 * erased bytes. */
uint8_t store_commit_at(uint16_t off, uint16_t size, const uint8_t *rec, uint16_t len);

uint16_t crc16(const uint8_t *p, uint16_t n) BANKED;

#endif
