#include "board.h"
#include "w5100.h"
#include "http.h"
#include "ota_app.h"
#include "../boot/ota.h"

#define UI_HDR  8               /* magic(4) length(2) reserved(2) */

#ifdef ZRP6_BOOT
/* The page has a Flash sector of its own, fs7 (boot/ota.h), read through the
 * data window at page 8 — never as code, so it costs no code space — and
 * copied into the W5100 a piece at a time with the page register back at 0
 * before the W5100 is touched. It is updated on its own (POST /ui). */
static __xdata uint8_t  src_page;       /* where the body being sent lives */
static __xdata uint16_t src_at;

static void win_read(uint16_t pos, __xdata uint8_t *dst, uint16_t n)
{
    volatile __xdata uint8_t *p = (volatile __xdata uint8_t *)(OTA_WINDOW + src_at + pos);
    PAGE_WINDOW(src_page);
    while (n--) *dst++ = *p++;
    PAGE_NORMAL();
}

/* 0 when the sector holds no complete page: the magic goes in last. */
uint16_t http_ui_size(void) BANKED
{
    volatile __xdata uint8_t *h = (volatile __xdata uint8_t *)OTA_WINDOW;
    uint16_t n = 0;
    PAGE_WINDOW(OTA_PAGE_UI);
    if (h[0] == 'Z' && h[1] == 'U' && h[2] == 'I' && h[3] == '2')
        n = (uint16_t)h[OTA_U_LEN] | (uint16_t)h[OTA_U_LEN + 1] << 8;
    PAGE_NORMAL();
    return n > OTA_UI_MAX ? 0 : n;
}

/* One ring-half of the page, staged from ota_buf a buffer at a time. `pos`
 * is an offset into the gzip data. */
static void stage_ui(uint8_t s, uint16_t pos, uint16_t chunk)
{
    uint16_t off = 0;
    while (off < chunk) {
        uint16_t n = (uint16_t)(chunk - off);
        if (n > OTA_BUF) n = OTA_BUF;
        win_read((uint16_t)(pos + off), ota_buf, n);
        tcp_stage(s, off, ota_buf, n);
        off += n;
    }
}


#else
/* v5: page 0 of main Flash is fs1, mapped at code 0x8000-0xFFFF, and the UI
 * sits at its bottom. The firmware itself is fs0 at 0x0000-0x7FFF and is not
 * paged, so switching pages under a running program is safe. */
#define UI_PAGE 0
#define UI_BASE 0x8000u

static __code const uint8_t *ui_at(uint16_t off)
{
    PSD_PAGE = UI_PAGE;
    return (__code const uint8_t *)(UI_BASE + off);
}

uint16_t http_ui_size(void) BANKED
{
    __code const uint8_t *p = ui_at(0);
    if (p[0] != 'Z' || p[1] != 'U' || p[2] != 'I' || p[3] != '1') return 0;
    return ((uint16_t)p[4] << 8) | p[5];
}

static void stage_ui(uint8_t s, uint16_t pos, uint16_t chunk)
{
    tcp_stage_code(s, 0, ui_at((uint16_t)(UI_HDR + pos)), chunk);
}
#endif

static uint16_t stage_str(uint8_t s, uint16_t off, __code const char *p)
{
    uint16_t n = 0;
    while (p[n]) n++;
    tcp_stage_code(s, off, (__code const uint8_t *)p, n);
    return n;
}

static uint16_t stage_num(uint8_t s, uint16_t off, uint16_t v)
{
    static __xdata uint8_t d[5];      /* off the stack — README rule 4 */
    uint8_t n = 0, i;
    do { d[n++] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n / 2; i++) {
        uint8_t t = d[i]; d[i] = d[n - 1 - i]; d[n - 1 - i] = t;
    }
    tcp_stage(s, off, d, n);
    return n;
}

void http_reply(uint8_t s, __code const char *status, __code const char *body) BANKED
{
    uint16_t off = 0, bl = 0;
    while (body[bl]) bl++;
    off += stage_str(s, off, "HTTP/1.1 ");
    off += stage_str(s, off, status);
    off += stage_str(s, off, "\r\nContent-Type: text/plain\r\n"
                             "Connection: close\r\nContent-Length: ");
    off += stage_num(s, off, bl);
    off += stage_str(s, off, "\r\n\r\n");
    off += stage_str(s, off, body);
    tcp_commit(s, off);
}

void http_serve(uint8_t s, const uint8_t *req, uint16_t len) BANKED
{
    uint16_t size = http_ui_size();
    uint16_t off = 0, sent = 0;

    /* A browser asks for this on every page load and would otherwise be
     * answered with the whole interface. */
    if (len > 13 && req[4] == '/' && req[5] == 'f' && req[6] == 'a'
                 && req[7] == 'v' && req[8] == 'i' && req[9] == 'c') {
        http_reply(s, "204 No Content", "");
        return;
    }

#ifdef ZRP6_BOOT
    /* No page installed — after an interrupted page upload, say — and the
     * small built-in one that can install it again goes out instead. It is
     * not code: layout.py puts it (src/noui.html) in the first 8 KB of the
     * running slot's image, which the CPU never fetches from, at OTA_DATA_AT
     * behind its length. */
    src_page = OTA_PAGE_UI;
    src_at = OTA_UI_HDR;
    if (!size) {
        src_page = OTA_PAGE(RUN_SLOT(), 0);
        src_at = OTA_DATA_AT;
        win_read(0, ota_buf, 2);
        size = ota_buf[0] | ((uint16_t)ota_buf[1] << 8);
        src_at = OTA_DATA_AT + 2;
        if (size > 0x1B00) size = 0;
    }
#else
    if (!size) {
        http_reply(s, "200 OK",
                    "ZRP-6 firmware is running. No UI is flashed in page 0.\n"
                    "MQTT is on 1883 and on this port; the WebSocket is here.\n");
        return;
    }
#endif

#ifdef TX_DEBUG
    txd_log('S', s);
#endif
    /* The page is already gzip: it is stored compressed and never expanded, on
     * this side or any other. The fallback is plain. */
    off += stage_str(s, off, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n");
#ifdef ZRP6_BOOT
    if (src_page == OTA_PAGE_UI)
#endif
        off += stage_str(s, off, "Content-Encoding: gzip\r\n");
    off += stage_str(s, off, "Cache-Control: no-cache\r\n"
                             "Connection: close\r\nContent-Length: ");
    off += stage_num(s, off, size);
    off += stage_str(s, off, "\r\n\r\n");
    tcp_commit(s, off);

    /* 18 KB will not fit in a 2 KB transmit ring, so it goes out a ring-full
     * at a time, read out of Flash directly into it. tcp_commit() does not
     * return until the ring has drained, so the next chunk can never land on
     * bytes the far end has yet to acknowledge. */
    while (sent < size) {
        uint16_t chunk = (uint16_t)(size - sent);
        /* Half a ring, never a whole one. A 2048-byte chunk into a 2048-byte
         * ring leaves TX_WR exactly equal to TX_RD, which is also what an
         * empty ring looks like, and the segment went out holding the previous
         * chunk's bytes: corruption began at exactly N x 2048 and was the
         * stream shifted by exactly one ring. Half also keeps a chunk inside
         * one TCP segment. */
        if (chunk > TX_RING / 2) chunk = TX_RING / 2;
        if (tcp_txfree(s) < chunk) return;      /* the browser stopped reading:
                                                 * tcp_commit already waited ~4 s */
        stage_ui(s, sent, chunk);
        tcp_commit(s, chunk);
        sent += chunk;
    }
#ifdef TX_DEBUG
    txd_log('E', s);
#endif
    (void)req;
}
