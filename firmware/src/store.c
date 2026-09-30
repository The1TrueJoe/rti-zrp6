#include "board.h"
#include "bus.h"
#include "flash.h"
#include "store.h"

#define MARK_OFF 0x00F0

uint16_t crc16(const uint8_t *p, uint16_t n) BANKED
{
    /* CRC-16/CCITT. */
    uint16_t crc = 0xFFFF;
    uint8_t b;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

/* 0 if the sector's marker is invalid, else its sequence number + 1 so that
 * sequence 0xFFFF still reads as present. */
static uint32_t mark(uint16_t base)
{
    /* xrd3: one dropped bit 4 in a sequence number makes the older copy look
     * newer, and every setting silently goes back a save (seen 2026-09-30). */
    if (xrd3(base + MARK_OFF) != 0xA5 || xrd3(base + MARK_OFF + 3) != 0x5A)
        return 0;
    return (uint32_t)(xrd3(base + MARK_OFF + 1) | ((uint16_t)xrd3(base + MARK_OFF + 2) << 8)) + 1;
}

uint16_t store_live(void)
{
    static __xdata uint32_t a, b;     /* off the stack — README rule 4 */
    a = mark(FLASH_A);
    b = mark(FLASH_B);
    if (a && b)
        return (int16_t)((uint16_t)(b - 1) - (uint16_t)(a - 1)) > 0 ? FLASH_B : FLASH_A;
    return (b && !a) ? FLASH_B : FLASH_A;
}

uint16_t store_slot(uint8_t slot) BANKED
{
    return store_live() + STORE_RULE_OFF + (uint16_t)slot * STORE_RULE_SIZE;
}

static uint8_t commit_once(uint16_t lo, uint16_t size, const uint8_t *rec, uint16_t len)
{
    uint16_t src = store_live();
    uint16_t dst = (src == FLASH_A) ? FLASH_B : FLASH_A;
    uint16_t seq = (uint16_t)mark(src);         /* old seq + 1 = new seq */
    uint16_t off;
    uint8_t v;

    if (flash_erase(dst)) return 1;
    for (off = 0; off < 0x2000; off++) {
        if (off >= MARK_OFF && off < MARK_OFF + 4) continue;
        if (off >= lo && off < lo + size)
            v = (off - lo < len) ? rec[off - lo] : 0xFF;
        else
            v = xrd3(src + off);                /* read before programming; three
                                                 * reads ORed, or a dropped bit is
                                                 * copied into the new sector */
        if (flash_program(dst + off, v)) return 1;
    }
    /* The marker, last. Until these four bytes are in, this copy does not
     * exist as far as store_live() is concerned. */
    if (flash_program(dst + MARK_OFF + 1, (uint8_t)seq)) return 1;
    if (flash_program(dst + MARK_OFF + 2, (uint8_t)(seq >> 8))) return 1;
    if (flash_program(dst + MARK_OFF + 3, 0x5A)) return 1;
    if (flash_program(dst + MARK_OFF, 0xA5)) return 1;
    return 0;
}

/* Twice, from the erase: flash.c already retries each step, and a byte that
 * lost a bit can only be recovered by erasing the sector again. The live copy
 * is untouched throughout, so a failure costs nothing but the new write. */
uint8_t store_commit_at(uint16_t off, uint16_t size, const uint8_t *rec, uint16_t len)
{
    if (!commit_once(off, size, rec, len)) return 0;
    return commit_once(off, size, rec, len);
}

uint8_t store_commit(uint8_t slot, const uint8_t *rec, uint16_t len) BANKED
{
    return store_commit_at(STORE_RULE_OFF + (uint16_t)slot * STORE_RULE_SIZE,
                           STORE_RULE_SIZE, rec, len);
}
