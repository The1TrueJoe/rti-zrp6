/* core.c — what the bootloader decides, independent of the hardware.
 *
 * Everything here goes through hal.h, so the same file is compiled by SDCC for
 * the box and by the host compiler for test/sim.c, which cuts the power at
 * every write it can and checks that the box always comes back.
 *
 * Two rules keep this file safe to call from the application (ota_op):
 *   - no static or global variables: the application owns all of RAM, and a
 *     bootloader variable would sit on top of one of its own;
 *   - small stack frames: the call runs on the application's stack.
 *
 * Bootloader 3: two slots, each a whole image, and each image's header holds
 * its own state — generation, confirmed, tries, failed. Booting picks a slot
 * and at most programs one status byte; an update writes only the slot that
 * is not running. Nothing is ever copied, so no power cut can leave a slot
 * half-copied: every intermediate state is one of "the new slot has no
 * magic yet" (so it does not exist) or a status byte that is set or not.
 */
#include "ota.h"
#include "hal.h"
#include "core.h"

/* ---- CRC-32, the same one as zlib.crc32 and layout.py ------------------
 * A nibble at a time: 64 bytes of table where a byte-wide one takes 1 KB,
 * which USB rescue needed. It runs only when an image or page is committed,
 * never at boot. */
static __code const uint32_t crc_tab[16] = {
    0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul,
    0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
    0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul,
    0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul
};

uint32_t crc32_byte(uint32_t c, uint8_t b)
{
    c ^= b;
    c = (c >> 4) ^ crc_tab[(uint8_t)c & 15];
    return (c >> 4) ^ crc_tab[(uint8_t)c & 15];
}

#define CRC_STEP(c, b)  ((c) = crc32_byte((c), (b)))

/* ---- a slot's header --------------------------------------------------- */
#define hrd(s, o)       hal_rd(OTA_PAGE((s), 0), (o))
#define hprog(s, o, v)  hal_prog(OTA_PAGE((s), 0), (o), (v))
#define is_set(s, o)    (hrd((s), (o)) != 0xFF)

uint8_t slot_valid(uint8_t s)
{
    return hrd(s, 0) == 'Z' && hrd(s, 1) == 'R' && hrd(s, 2) == 'P' && hrd(s, 3) == 'B'
        && hrd(s, OTA_H_FORMAT) == OTA_FORMAT;
}

uint16_t slot_gen(uint8_t s)
{
    return (uint16_t)hrd(s, OTA_H_GEN) | (uint16_t)hrd(s, OTA_H_GEN + 1) << 8;
}

static uint8_t usable(uint8_t s)
{
    return slot_valid(s) && !is_set(s, OTA_H_FAILED);
}

uint8_t slot_tries(uint8_t s)
{
    uint8_t n = 0;
    while (n < OTA_MAX_TRIES && is_set(s, OTA_H_TRIES + n)) n++;
    return n;
}

uint32_t slot_crc(uint8_t s)
{
    return (uint32_t)hrd(s, OTA_H_CRC)
         | (uint32_t)hrd(s, OTA_H_CRC + 1) << 8
         | (uint32_t)hrd(s, OTA_H_CRC + 2) << 16
         | (uint32_t)hrd(s, OTA_H_CRC + 3) << 24;
}

/* CRC of everything after the header: the rest of the common sector, then
 * both banks whole. o counts up and stops when it wraps. */
uint32_t slot_body_crc(uint8_t s)
{
    uint32_t c = 0xFFFFFFFFul;
    uint16_t o = OTA_HDR_SIZE;
    uint8_t p;
    for (p = 0; p < OTA_PARTS; p++) {
        do {
            uint8_t b = hal_rd(OTA_PAGE(s, p), o);
            CRC_STEP(c, b);
        } while (++o < OTA_SECTOR);
        o = 0;
    }
    return ~c;
}

uint8_t slot_erase(uint8_t s)
{
    uint8_t p;
    for (p = 0; p < OTA_PARTS; p++)
        if (hal_erase(OTA_PAGE(s, p))) return OTA_E_FLASH;
    return OTA_OK;
}

/* ---- boot -------------------------------------------------------------- */
/* The usable slot with the newest image, or 0xFF. Generations are compared
 * as a wrapping difference, so the count may run past 0xFFFF. */
uint8_t boot_pick(void)
{
    uint8_t a = usable(0), b = usable(1);
    if (a && b)
        return (int16_t)(slot_gen(1) - slot_gen(0)) > 0 ? 1 : 0;
    return a ? 0 : b ? 1 : 0xFF;
}

/* The slot to run, or 0xFF when there is nothing to run.
 *
 * The newest usable image runs. If it has not confirmed itself it spends one
 * of its OTA_MAX_TRIES boots doing so; once they are gone it is marked failed
 * and the other slot — the image that was running before it — runs instead.
 * A slot marked failed still runs if it is the only image there is. */
uint8_t boot_decide(void)
{
    uint8_t k, c, n;
    for (k = 0; k < 2; k++) {
        c = boot_pick();
        if (c == 0xFF) break;
        if (is_set(c, OTA_H_CONFIRMED)) return c;
        n = slot_tries(c);
        if (n < OTA_MAX_TRIES) {
            hprog(c, OTA_H_TRIES + n, 0x00);
            return c;
        }
        hal_led(HAL_LED_BUSY);
        hprog(c, OTA_H_FAILED, 0x00);       /* then the other one */
    }
    for (c = 0; c < OTA_SLOTS; c++)
        if (slot_valid(c) && is_set(c, OTA_H_CONFIRMED)) return c;
    for (c = 0; c < OTA_SLOTS; c++)
        if (slot_valid(c)) return c;
    return 0xFF;
}

/* ---- calls from the application ---------------------------------------- */
/* `run` is the slot the application is running (the page register's slot bit
 * at the call); firmware writes go to the other one. */

static uint8_t op_erase(uint8_t run)
{
    /* Writing the other slot while this one is unproven would leave no proven
     * image anywhere. The application confirms on its first client login. */
    if (slot_valid(run) && !is_set(run, OTA_H_CONFIRMED)) return OTA_E_TRIAL;
    return slot_erase((uint8_t)(run ^ 1));
}

static uint8_t op_write(__xdata struct ota_req *r, uint8_t run)
{
    uint8_t s = (uint8_t)(run ^ 1), p = (uint8_t)(r->hi << 1 | r->off >> 15);
    uint16_t o = r->off & 0x7FFF, i;
    uint32_t at = (uint32_t)r->hi << 16 | r->off;
    if (at < OTA_HDR_SIZE || at + r->len > OTA_IMAGE_SIZE) return OTA_E_RANGE;
    for (i = 0; i < r->len; i++) {
        if (hal_prog(OTA_PAGE(s, p), o, r->buf[i])) return OTA_E_FLASH;
        if (++o == OTA_SECTOR) { o = 0; p++; }
    }
    return OTA_OK;
}

/* Check the other slot against the header, then commit: the generation
 * first, the rest of the header, and the magic last of all. */
static uint8_t op_finish(__xdata struct ota_req *r, uint8_t run)
{
    __xdata uint8_t *h = r->buf;
    uint8_t s = (uint8_t)(run ^ 1), k;
    uint16_t gen;
    uint32_t want;
    if (h[0] != 'Z' || h[1] != 'R' || h[2] != 'P' || h[3] != 'B'
        || h[OTA_H_FORMAT] != OTA_FORMAT)
        return OTA_E_HEADER;
    want = (uint32_t)h[OTA_H_CRC] | (uint32_t)h[OTA_H_CRC + 1] << 8
         | (uint32_t)h[OTA_H_CRC + 2] << 16 | (uint32_t)h[OTA_H_CRC + 3] << 24;
    if (slot_body_crc(s) != want) return OTA_E_CRC;
    gen = (uint16_t)((slot_valid(run) ? slot_gen(run) : 0) + 1);
    if (hprog(s, OTA_H_GEN, (uint8_t)gen) || hprog(s, OTA_H_GEN + 1, (uint8_t)(gen >> 8)))
        return OTA_E_FLASH;
    for (k = 4; k < OTA_H_GEN; k++)
        if (hprog(s, k, h[k])) return OTA_E_FLASH;
    for (k = 0; k < 4; k++)
        if (hprog(s, k, h[k])) return OTA_E_FLASH;
    return OTA_OK;
}

static uint8_t op_confirm(uint8_t run)
{
    if (!slot_valid(run) || is_set(run, OTA_H_CONFIRMED)) return OTA_OK;
    return hprog(run, OTA_H_CONFIRMED, 0x00) ? OTA_E_FLASH : OTA_OK;
}

/* ---- the web page sector ----------------------------------------------- */
static uint8_t op_ui_write(__xdata struct ota_req *r)
{
    uint16_t i;
    if (r->off < OTA_UI_HDR || (uint32_t)r->off + r->len > OTA_UI_SIZE)
        return OTA_E_RANGE;
    for (i = 0; i < r->len; i++)
        if (hal_prog(OTA_PAGE_UI, (uint16_t)(r->off + i), r->buf[i]))
            return OTA_E_FLASH;
    return OTA_OK;
}

static uint8_t op_ui_finish(__xdata struct ota_req *r)
{
    __xdata uint8_t *h = r->buf;
    uint32_t want, c = 0xFFFFFFFFul;
    uint16_t len, o;
    uint8_t i;
    if (h[0] != 'Z' || h[1] != 'U' || h[2] != 'I' || h[3] != '2') return OTA_E_HEADER;
    len = (uint16_t)h[OTA_U_LEN] | (uint16_t)h[OTA_U_LEN + 1] << 8;
    if (!len || len > OTA_UI_MAX) return OTA_E_HEADER;
    want = (uint32_t)h[OTA_U_CRC] | (uint32_t)h[OTA_U_CRC + 1] << 8
         | (uint32_t)h[OTA_U_CRC + 2] << 16 | (uint32_t)h[OTA_U_CRC + 3] << 24;
    for (o = OTA_UI_HDR; o < OTA_UI_HDR + len; o++) {
        uint8_t b = hal_rd(OTA_PAGE_UI, o);
        CRC_STEP(c, b);
    }
    if (~c != want) return OTA_E_CRC;
    for (i = 4; i < OTA_UI_HDR + 4; i++) {             /* 4..15, then the magic */
        uint8_t k = (uint8_t)(i & (OTA_UI_HDR - 1));
        if (hal_prog(OTA_PAGE_UI, k, h[k])) return OTA_E_FLASH;
    }
    return OTA_OK;
}

void ota_op(__xdata struct ota_req *r)
{
    uint8_t rc, run = hal_running();
    switch (r->op) {
    case OTA_OP_ERASE:   rc = op_erase(run); break;
    case OTA_OP_WRITE:   rc = op_write(r, run); break;
    case OTA_OP_FINISH:  rc = op_finish(r, run); break;
    case OTA_OP_CONFIRM: rc = op_confirm(run); break;
    case OTA_OP_UI_ERASE:  rc = hal_erase(OTA_PAGE_UI) ? OTA_E_FLASH : OTA_OK; break;
    case OTA_OP_UI_WRITE:  rc = op_ui_write(r); break;
    case OTA_OP_UI_FINISH: rc = op_ui_finish(r); break;
    default:             rc = OTA_E_OP; break;
    }
    hal_page(OTA_PAGE_NORMAL);
    r->rc = rc;
}
