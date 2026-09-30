#include "board.h"
#include "bus.h"
#include "flash.h"
#include "config.h"
#include "store.h"

extern __xdata struct cfg cfg_live;       /* cfgacc.c: common code */
#define live cfg_live
static __xdata uint8_t mark_k;              /* the power-cycle reset, below */

__code const uint32_t baud_rate[BAUD_COUNT + 1] = {
    0, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200 };
__code const uint8_t ser_lcr[SER_FORMATS] = { 0x03, 0x1B, 0x0B, 0x1A, 0x0A, 0x07 };
__code const char ser_fmt[SER_FORMATS][4] = { "8N1", "8E1", "8O1", "7E1", "7O1", "8N2" };

/* A new box: DHCP, falling back to the bench's address, base topic zrp6,
 * every IR port IR, both serial ports 9600 8N1. The MAC is filled in. */
static __code const struct cfg DEF = {
    CFG_MAGIC, CFG_VERSION, { 0 }, NET_DHCP,
    { 10, 0, 0, 77 }, { 255, 255, 255, 0 }, { 10, 0, 0, 1 },
    "zrp6", { 0 }, { BAUD_9600, BAUD_9600 }, 0 };

uint16_t cfg_crc(const struct cfg *c)
{
    /* over the record up to but not including the crc field */
    return crc16((const uint8_t *)c, sizeof(struct cfg) - 2);
}


static uint8_t read_copy(uint16_t base)
{
    __xdata uint8_t *p = (__xdata uint8_t *)&live;
    uint8_t i;
    for (i = 0; i < sizeof(struct cfg); i++)
        p[i] = xrd3(base + STORE_CFG_OFF + i);
    if (live.magic != CFG_MAGIC || live.version != CFG_VERSION)
        return 1;
    return live.crc != cfg_crc(&live);
}

/* A new box: DEF, and a MAC that is locally administered (02), "ZR", and
 * three bytes nobody chose. The W5100's 8 KB of buffer RAM powers up holding
 * noise, and nothing has written it yet when this runs (net.c starts it
 * later), so a CRC over each quarter of it is as good a source as this board
 * has. */
static void defaults(void)
{
    __xdata uint8_t *p = (__xdata uint8_t *)&live;
    __code const uint8_t *d = (__code const uint8_t *)&DEF;
    uint16_t a;
    uint8_t i;
    for (i = 0; i < sizeof(struct cfg); i++) p[i] = d[i];
    live.mac[0] = 0x02; live.mac[1] = 0x5A; live.mac[2] = 0x52;
    for (i = 0; i < 3; i++) {
        a = crc16((const uint8_t *)(__xdata uint8_t *)(0xC000u + (uint16_t)i * 0x0800u), 0x0800);
        live.mac[3 + i] = (uint8_t)(a ^ (a >> 8) ^ TL0);
    }
}

void cfg_load(void) BANKED
{
    uint16_t live_base = store_live();
    uint8_t t;
    /* The live copy, a few times over before believing it is bad; then the
     * other copy, which is the previous settings. Only when neither reads is
     * this a new box. */
    for (t = 0; t < 3; t++)
        if (!read_copy(live_base)) return;
    if (!read_copy(live_base == FLASH_A ? FLASH_B : FLASH_A)) return;
    defaults();
    (void)cfg_save();           /* keep the MAC it just made up */
}


uint8_t cfg_save(void) BANKED
{
    mark_k = 0xFF;                      /* the save blanks the boot marks */
    live.crc = cfg_crc(&live);
    return store_commit_at(STORE_CFG_OFF, STORE_CFG_SIZE,
                           (const uint8_t *)&live, sizeof(struct cfg));
}

/* ---- the power-cycle reset ---------------------------------------------- *
 * 112 bytes of the config area past the record, programmed one per boot and
 * never erased except by cfg_save(), which rewrites the area with them blank:
 * 0xFF unused, 0x5A a boot that has not yet been up 15 s, 0x00 one that has.
 * A byte can only lose bits, so the mark and its completion are two programs
 * of the same byte and no erase. A power cut mid-program leaves one odd byte,
 * which reads as "completed". */
#define MARKS_AT   0x80
#define MARKS      (STORE_CFG_SIZE - MARKS_AT)
#define M_QUICK    0x5A


uint8_t cfg_boot_mark(void) BANKED
{
    uint16_t base = store_live() + STORE_CFG_OFF + MARKS_AT;
    uint8_t k, n = 0;
    for (k = 0; k < MARKS && xrd3(base + k) != 0xFF; k++)
        n = xrd3(base + k) == M_QUICK ? (uint8_t)(n + 1) : 0;
    if (k == MARKS) {                   /* full: start the area again */
        (void)cfg_save();
        k = 0;
    }
    mark_k = k;                         /* 0xFF: none this boot */
    (void)flash_program(store_live() + STORE_CFG_OFF + MARKS_AT + k, M_QUICK);
    return n;
}

/* The live sector is looked up again: a rule or user saved since the mark
 * has copied the store, mark included, into the other sector. */
void cfg_boot_ok(void) BANKED
{
    if (mark_k != 0xFF)
        (void)flash_program(store_live() + STORE_CFG_OFF + MARKS_AT + mark_k, 0x00);
    mark_k = 0xFF;
}
