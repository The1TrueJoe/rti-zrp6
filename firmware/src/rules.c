#include "board.h"
#include "bus.h"
#include "rules.h"

/* Record, as stored in a 512-byte slot:
 *   [0]   'R' when used; 0xFF (erased) when empty
 *   [1]   code length   [2] topic length   [3..4] payload length
 *   [5..6] CRC-16 of the data
 *   [7..] code, topic, payload — back to back, no terminators
 * A slot whose CRC does not check is treated as empty rather than acted on. */
#define HDR 7

static uint8_t fb(uint16_t a) { return xrd3(a); }    /* D4: bus.h */

/* Written with walking pointers throughout. The first version indexed with
 * 16-bit counters — rec[HDR + cl + i] and the like — and under --model-large
 * SDCC spends a full generic-pointer calculation on every such access: the
 * parser alone came to 1.5 KB and pushed the image 2.5 KB past its sector. */

uint16_t rule_parse(const uint8_t *in, uint16_t n, uint8_t *rec)
{
    static __xdata uint16_t len[3], crc;
    static __xdata uint8_t seg;
    uint8_t *q = rec + HDR;
    uint8_t c;

    seg = 0;
    len[0] = len[1] = len[2] = 0;
    while (n--) {
        c = *in++;
        if (c == '|' && seg < 2) { seg++; continue; }
        *q++ = c;
        len[seg]++;
    }
    if (seg != 2 || !len[0] || len[0] > RULE_CODE_MAX || !len[1]
        || len[1] > RULE_TOPIC_MAX || len[2] > RULE_PAYLOAD_MAX) return 0;
    rec[0] = 'R';
    rec[1] = (uint8_t)len[0];
    rec[2] = (uint8_t)len[1];
    rec[3] = (uint8_t)len[2];
    rec[4] = (uint8_t)(len[2] >> 8);
    n = len[0] + len[1] + len[2];
    crc = crc16(rec + HDR, n);
    rec[5] = (uint8_t)crc;
    rec[6] = (uint8_t)(crc >> 8);
    return HDR + n;
}

/* Valid used slot: returns its data length, else 0. The CRC runs over the
 * Flash contents directly — they are ordinary XDATA reads. */
static uint16_t valid(uint16_t a)
{
    uint16_t len;
    if (fb(a) != 'R') return 0;
    len = fb(a + 1) + fb(a + 2) + (fb(a + 3) | ((uint16_t)fb(a + 4) << 8));
    if (len > STORE_RULE_SIZE - HDR) return 0;
    if (crc16((const uint8_t *)(__xdata uint8_t *)(a + HDR), len)
        != (fb(a + 5) | ((uint16_t)fb(a + 6) << 8))) return 0;
    return len;
}

uint8_t rule_matches(uint8_t slot, const char *code)
{
    uint16_t a = store_slot(slot);
    uint16_t p = a + HDR;
    uint8_t cl;
    if (fb(a) != 'R') return 0;             /* cheap reject before the CRC */
    for (cl = fb(a + 1); cl; cl--)
        if ((uint8_t)*code++ != fb(p++)) return 0;
    if (*code) return 0;
    return valid(a) != 0;
}

/* Copies n bytes of Flash at `src` to `dst` and terminates them. */
static void take(uint8_t *dst, uint16_t src, uint16_t n)
{
    while (n--) *dst++ = fb(src++);
    *dst = 0;
}

uint16_t rule_action(uint8_t slot, char *topic, uint8_t *payload)
{
    uint16_t a = store_slot(slot);
    uint16_t pl = fb(a + 3) | ((uint16_t)fb(a + 4) << 8);
    uint16_t p = a + HDR + fb(a + 1);
    take((uint8_t *)topic, p, fb(a + 2));
    take(payload, p + fb(a + 2), pl);
    return pl;
}

uint16_t rule_text(uint8_t slot, uint8_t *out)
{
    uint16_t a = store_slot(slot), len, p;
    uint8_t cl, tl;
    uint8_t *o = out;
    if (!(len = valid(a))) return 0;
    cl = fb(a + 1);
    tl = fb(a + 2);
    p = a + HDR;
    take(o, p, cl);          o += cl; *o++ = '|';
    take(o, p + cl, tl);     o += tl; *o++ = '|';
    take(o, p + cl + tl, len - cl - tl);
    return len + 2;
}
