/* ota.c — the application's half of firmware updates.
 *
 * The bootloader (firmware/boot) does everything that touches main Flash;
 * this receives an image over HTTP, hands it over a piece at a time, tells the
 * bootloader when the running image has proved itself, and reboots. The
 * design and the reasons are in docs/UPDATING.md.
 *
 * Bootloader 3: the box runs one of two slots (A, B). An upload is written
 * into the other slot, and the restart after it boots that slot on trial.
 * The bootloader decides which slot each call touches; this side only reads
 * headers, through the data window.
 */
#include "board.h"
#include "../boot/ota.h"
#include "w5100.h"
#include "io.h"
#include "ota_app.h"
#include "http.h"

__sfr __at(0xAE) WDKEY;
__sfr __at(0xA6) WDRST;

__xdata uint8_t ota_buf[OTA_BUF];
static __xdata struct ota_req req;
static __xdata uint8_t hdr[OTA_HDR_SIZE];
static __xdata uint8_t confirmed_this_boot;
__xdata uint8_t ota_restart;
__xdata uint8_t ota_changed;

/* ---- reaching the bootloader ------------------------------------------- */
/* bank.s: lcall 0x0080 with DPTR -> the request. The bootloader moves only
 * the data window; the slot and bank bits stay as they are. */
void boot_api(__xdata struct ota_req *r);

/* Reads need no bootloader: the page register's window bits move only the
 * data window, never the code this is running from. */
uint8_t ota_peek(uint8_t page, uint16_t off)
{
    uint8_t v;
    PAGE_WINDOW(page);
    v = *(volatile __xdata uint8_t *)(OTA_WINDOW + off);
    PAGE_NORMAL();
    return v;
}

uint8_t ota_call(uint8_t op, uint32_t at, uint16_t len, __xdata uint8_t *buf)
{
    req.op = op;
    req.off = (uint16_t)at;
    req.hi = (uint8_t)(at >> 16);
    req.len = len;
    req.buf = buf;
    req.rc = 0xEE;
    wdt_pause();                        /* an erase is seconds of no ticks() */
    boot_api(&req);
    wdt_resume();
    return req.rc;
}

/* ---- what the box reports ---------------------------------------------- */
static uint8_t valid(uint8_t slot)
{
    uint8_t p = OTA_PAGE(slot, 0);
    return ota_peek(p, 0) == 'Z' && ota_peek(p, 1) == 'R' && ota_peek(p, 2) == 'P' && ota_peek(p, 3) == 'B';
}

uint8_t ota_state(void) BANKED
{
    uint8_t p = OTA_PAGE(RUN_SLOT(), 0), n;
    if (!valid(RUN_SLOT())) return OTA_ST_NONE;
    if (ota_peek(p, OTA_H_CONFIRMED) != 0xFF) return OTA_ST_OK;
    for (n = 0; n < OTA_MAX_TRIES && ota_peek(p, OTA_H_TRIES + n) != 0xFF; n++)
        ;
    return (uint8_t)(OTA_ST_TRIAL + n);         /* boots used so far */
}

/* The other slot holds an image the bootloader would still fall back to. */
uint8_t ota_backup(void) BANKED
{
    uint8_t o = (uint8_t)(RUN_SLOT() ^ 1);
    return valid(o) && ota_peek(OTA_PAGE(o, 0), OTA_H_FAILED) == 0xFF;
}

/* The first client to log in proves the network, the broker and the HTTP
 * path all work — the things needed to install the next image. Until then the
 * bootloader counts this image's boots and goes back to the other slot after
 * a few. */
void ota_client_connected(void) BANKED
{
#ifdef OTA_NEVER_CONFIRM
    /* A test build: stays on trial, so the bootloader must roll it back.
     * ZRP6_EXTRA_CFLAGS=-DOTA_NEVER_CONFIRM firmware/build.sh */
    return;
#endif
    if (confirmed_this_boot) return;
    confirmed_this_boot = 1;
    if (ota_state() >= OTA_ST_TRIAL) {
        ota_call(OTA_OP_CONFIRM, 0, 0, 0);
        ota_changed = 1;
    }
}

/* A watchdog reset resets the whole chip, PSD module included, which is the
 * only way back into the bootloader with everything as it is at power-up. */
void ota_reboot(void) BANKED
{
    WDKEY = 0x00;                       /* anything but 55h enables it */
    WDRST = 0xFF;                       /* 64K machine cycles to overflow */
    for (;;)
        ;
}

/* ---- the upload -------------------------------------------------------- */
static __code const char *const why[] = {
    "installed; restarting\n",
    "Flash write failed\n",
    "offset out of range\n",
    "not a ZRP-6 image for this bootloader (build/zrp6.bin from a v10 build)\n",
    "image is damaged (CRC mismatch)\n",
    "bootloader refused the call\n",
    "the running firmware is on trial: log in to confirm it, then upload again\n",
};

/* POST /update or POST /ui. The request line and headers are already
 * consumed; `clen` is the Content-Length.
 *
 *   firmware  the 96 KB image build.sh writes as build/zrp6.bin, into the
 *             slot that is not running; the box then restarts into it
 *   ui        build/zrp6-ui.bin, the page's 16-byte header and its blob;
 *             written straight into the page sector, no restart
 *
 * Both are the same stream: the header comes first and is held back, to be
 * committed last with FINISH once the bootloader has checked the CRC.
 *
 * This blocks the main loop for the seconds it takes (sector erases and
 * programming): nothing else runs meanwhile, and nothing needs to. Returns 0
 * on failure, 1 when an image is in place and the box should restart, 3
 * when the page is installed. */
uint8_t ota_http(uint8_t s, uint32_t clen, uint8_t ui, __code const char **msg) BANKED
{
    uint32_t got = 0;
    uint16_t idle = ticks(), rd;
    uint8_t rc, hsz = ui ? OTA_UI_HDR : OTA_HDR_SIZE;

    if (ui ? (clen <= OTA_UI_HDR || clen > OTA_UI_SIZE) : clen != OTA_IMAGE_SIZE) {
        *msg = ui ? "expected build/zrp6-ui.bin\n" : "expected the 98304-byte build/zrp6.bin\n";
        return 0;
    }
    rc = ota_call(ui ? OTA_OP_UI_ERASE : OTA_OP_ERASE, 0, 0, 0);
    if (rc) {
        *msg = why[rc < 7 ? rc : 5];
        return 0;
    }
    rd = tcp_rx_ptr(s);
    while (got < clen) {
        uint16_t n, i = 0;
        /* The W5100's own RX_RD can go wrong behind our back: a trace caught
         * it at 0xE7E9 while we had released 0xF7E9 — bit 4 of the high byte,
         * the D4 fault — and RX_RSR, which the chip derives from it, then
         * claimed 0x1000 bytes that had not arrived. So: the chip's pointer
         * must match ours before its count is believed, and no count can
         * exceed the 2 KB ring. */
        if (tcp_rx_ptr(s) != rd) {
            tcp_rx_release(s, rd);
            continue;
        }
        n = tcp_avail(s);
        if (n > 2048) {
            tcp_rx_release(s, rd);
            continue;
        }
        if (!n) {
            if (sock_status(s) != SOCK_ESTAB
                || (uint16_t)(ticks() - idle) > TICKS_S(10)) {   /* silence */
                *msg = "upload stopped part-way\n";
                return 0;
            }
            continue;
        }
        idle = ticks();
        if (n > OTA_BUF) n = OTA_BUF;
        if (got + n > clen) n = (uint16_t)(clen - got);
        tcp_read_sure(s, rd, ota_buf, n);
        rd += n;
        tcp_rx_release(s, rd);
        /* The header is held back and sent last, with FINISH. */
        while (got + i < hsz && i < n) {
            hdr[(uint8_t)(got + i)] = ota_buf[i];
            i++;
        }
        if (i < n) {
            rc = ota_call(ui ? OTA_OP_UI_WRITE : OTA_OP_WRITE, got + i, (uint16_t)(n - i), ota_buf + i);
            if (rc) {
                *msg = why[rc < 7 ? rc : 5];
                return 0;
            }
        }
        got += n;
    }
    rc = ota_call(ui ? OTA_OP_UI_FINISH : OTA_OP_FINISH, 0, 0, hdr);
    if (rc) {
        *msg = why[rc < 7 ? rc : 5];
        return 0;
    }
    if (ui) {
        *msg = "page installed\n";
        ota_changed = 1;                /* republish firmware/ui */
        return 3;
    }
    *msg = why[0];
    return 1;
}
