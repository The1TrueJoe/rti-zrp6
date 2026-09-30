#include "board.h"
#include "w5100.h"
#include "sha1.h"
#include "http.h"
#include "broker.h"
#include "ws.h"
#include "io.h"
#include "ota_app.h"
#include "users.h"
#include "../boot/ota.h"

#define M_NONE  0               /* listening, or nothing decided yet */
#define M_WS    1               /* WebSocket frames carrying MQTT */
#define M_RAW   2               /* MQTT straight onto the socket */
#define M_OTA   3               /* a POST /update waiting for ws_ota_run() */

/* The W5100's socket states. The ones that matter are the ones that are
 * neither "waiting for a client" nor "talking to one": FIN_WAIT, CLOSING,
 * TIME_WAIT and LAST_ACK are all perfectly normal ends of a connection, and a
 * socket parked in any of them is a port that has silently stopped accepting.
 * Handling only CLOSED and CLOSE_WAIT left sockets stuck there, which looked
 * for a while like the box serving two pages and then dying. */
#define ST_CLOSED     0x00
#define ST_INIT       0x13
#define ST_LISTEN     0x14
#define ST_SYNRECV    0x16
#define ST_ESTAB      0x17
#define ST_CLOSE_WAIT 0x1C

/* The W5100 has four sockets and that is the whole budget. Who gets them,
 * in order:
 *
 *   1. MQTT clients (a controller, a bridge): never dropped.
 *   2. A socket listening on 1883, so the next MQTT client always gets in.
 *   3. A socket listening on 80, so the page can always be loaded.
 *   4. Browser WebSockets — the page's live view — oldest first to go.
 *
 * Loading the page takes a socket only for the moment it is sent, and gives
 * it back with a graceful close. So with no MQTT client connected, two
 * browsers stay live beside the two doors; a third takes the place of the
 * oldest, which is told why (close code 4000) and does not reconnect by
 * itself. The newest browser is never dropped to reopen the web door — two
 * tabs would take turns forever — only for an MQTT client.
 *
 * A socket's port is chosen when it goes back to listening: 1883 if nothing
 * else listens there, otherwise 80. ws_balance() moves idle listeners between
 * the doors and, when a door is missing and no socket is on its way back,
 * drops a browser. The sniff in ws_poll() means either port in fact accepts
 * either protocol; the numbers are only so no client has to be told an
 * unusual one. */
#define PORT_WEB    80
#define PORT_MQTT   1883

static __xdata uint8_t mode[BR_CLIENTS];
static __xdata uint16_t closing_at[BR_CLIENTS];  /* ticks() when the close began */
static __xdata uint8_t closing[BR_CLIENTS];
static __xdata uint8_t on_web[BR_CLIENTS];      /* 1: port 80, 0: port 1883 */
static __xdata uint16_t born[BR_CLIENTS];       /* handshake order */
static __xdata uint16_t births;
static __xdata uint8_t lent[BR_CLIENTS];        /* borrowed by net.c for DHCP */
static __xdata uint8_t prio[BR_CLIENTS];        /* a priority MQTT client */

/* A WebSocket close frame, code 4000: "the box needed this socket". The page
 * shows that instead of silently retrying — see ws_balance(). */
static __code const uint8_t BYE[4] = { 0x88, 2, 0x0F, 0xA0 };
static __xdata uint8_t hdr[8];

/* 60 bytes: the 24-character key plus RFC 6455's fixed GUID. */
static __code const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* Sockets other than `except` waiting for a client on the web port (web=1)
 * or the MQTT port (web=0). */
static uint8_t listeners(uint8_t web, uint8_t except)
{
    uint8_t t, n = 0;
    for (t = 0; t < BR_CLIENTS; t++)
        if (t != except && on_web[t] == web && sock_status(t) == ST_LISTEN) n++;
    return n;
}

void ws_listen(uint8_t s)
{
    mode[s] = M_NONE;
    closing[s] = 0;
    lent[s] = 0;
    prio[s] = 0;
    on_web[s] = (uint8_t)(listeners(0, s) != 0);      /* MQTT's door first */
    tcp_listen(s, on_web[s] ? PORT_WEB : PORT_MQTT);
}

static void bump(uint8_t t)
{
    if (tcp_txfree(t) >= sizeof(BYE)) {
        tcp_stage_code(t, 0, BYE, sizeof(BYE));
        tcp_commit(t, sizeof(BYE));
    }
    tcp_disconnect(t);          /* mode stays M_WS: ws_poll reports it gone */
}

void ws_balance(void)
{
    uint8_t t, st, old = 0xFF, newest = 0xFF;
    uint8_t w = listeners(1, 0xFF), m = listeners(0, 0xFF);

    if (m && w) return;
    /* A spare listener on the other port: move it. */
    if ((!m && w) || (!w && m >= 2)) {
        uint8_t from = (uint8_t)(!m ? 1 : 0);
        for (t = 0; t < BR_CLIENTS; t++)
            if (on_web[t] == from && sock_status(t) == ST_LISTEN) {
                on_web[t] = (uint8_t)!from;
                tcp_listen(t, from ? PORT_MQTT : PORT_WEB);
                return;
            }
    }
    /* A socket already on its way back will be a door soon. That includes one
     * serving the page (connected, protocol not yet known). */
    for (t = 0; t < BR_CLIENTS; t++) {
        st = sock_status(t);
        if (st == ST_CLOSED || st == ST_INIT || st == ST_SYNRECV || closing[t]
            || lent[t] || (st == ST_ESTAB && mode[t] == M_NONE))
            return;
    }
    /* Otherwise a browser gives way: the oldest, and for the web door never
     * the only (newest) one. */
    for (t = 0; t < BR_CLIENTS; t++)
        if (mode[t] == M_WS && sock_status(t) == ST_ESTAB) {
            if (old == 0xFF || (int16_t)(born[t] - born[old]) < 0) old = t;
            if (newest == 0xFF || (int16_t)(born[t] - born[newest]) > 0) newest = t;
        }
    if (old != 0xFF) {
        if (!m || old != newest) {
            closing[old] = 1;
            closing_at[old] = ticks();
            bump(old);
        }
        return;
    }
    /* No browser left to give way and no door for MQTT: the oldest ordinary
     * MQTT client goes, so a priority client (ID starting '!', a Control4
     * driver) can always get back in. Priority clients are never dropped. */
    if (m) return;
    for (t = 0; t < BR_CLIENTS; t++)
        if (mode[t] == M_RAW && !prio[t] && sock_status(t) == ST_ESTAB
            && (old == 0xFF || (int16_t)(born[t] - born[old]) < 0)) old = t;
    if (old == 0xFF) return;
    closing[old] = 1;
    closing_at[old] = ticks();
    tcp_disconnect(old);
}

void ws_client(uint8_t s, uint8_t priority)
{
    prio[s] = priority;
    born[s] = ++births;
}

/* A door, for a moment: net.c renews its DHCP lease on it. A second door on
 * the same port is taken first, so both ports keep one if they can. */
uint8_t ws_borrow(void)
{
    uint8_t t, pick = 0xFF;
    for (t = 0; t < BR_CLIENTS; t++) {
        if (lent[t] || closing[t] || sock_status(t) != ST_LISTEN) continue;
        if (pick == 0xFF || listeners(on_web[t], t)) pick = t;
    }
    if (pick != 0xFF) {
        lent[pick] = 1;
        tcp_abort(pick);                /* a listener has nothing in flight */
    }
    return pick;
}

void ws_return(uint8_t s)
{
    ws_listen(s);
}

void ws_close(uint8_t s)
{
    mode[s] = M_NONE;
    tcp_disconnect(s);
}

/* ---- transmit ----------------------------------------------------------- */

void ws_send(uint8_t s, const uint8_t *buf, uint16_t len)
{
    static __xdata uint8_t fh[4];     /* off the stack — README rule 4 */
    uint8_t fl;

    if (mode[s] == M_RAW) {
        tcp_send(s, buf, len);
        return;
    }
    if (mode[s] != M_WS) return;

    /* Server to client is never masked, so the header is 2 bytes below 126
     * payload bytes and 4 below 65536. */
    fh[0] = 0x82;                               /* FIN, binary */
    if (len < 126) {
        fh[1] = (uint8_t)len;
        fl = 2;
    } else {
        fh[1] = 126;
        fh[2] = (uint8_t)(len >> 8);
        fh[3] = (uint8_t)len;
        fl = 4;
    }
    if (tcp_txfree(s) < (uint16_t)(len + fl)) return;
    tcp_stage(s, 0, fh, fl);
    tcp_stage(s, fl, buf, len);
    tcp_commit(s, (uint16_t)(len + fl));
}

static void ws_pong(uint8_t s, const uint8_t *p, uint8_t n)
{
    static __xdata uint8_t fh[2];     /* off the stack — README rule 4 */
    fh[0] = 0x8A;
    fh[1] = n;
    tcp_stage(s, 0, fh, 2);
    tcp_stage(s, 2, p, n);
    tcp_commit(s, (uint16_t)(n + 2));
}

/* ---- the HTTP half of the handshake ------------------------------------- */

static uint8_t lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

/* Header names are case-insensitive and browsers do not agree on the casing of
 * "Sec-WebSocket-Key", so compare with the case folded away. Returns the
 * offset just past the name, or 0xFFFF. */
static uint16_t find_hdr(const uint8_t *buf, uint16_t n, __code const char *name)
{
    uint16_t i;
    uint8_t j;
    for (i = 0; i < n; i++) {
        for (j = 0; name[j]; j++) {
            if (i + j >= n) return 0xFFFF;
            if (lower(buf[i + j]) != lower((uint8_t)name[j])) break;
        }
        if (!name[j]) return (uint16_t)(i + j);
    }
    return 0xFFFF;
}

/* Straight out of Flash into the W5100's transmit ring: no copy, and no
 * stack buffer that a longer header would quietly overrun. */
static uint8_t stage_str(uint8_t s, uint16_t off, __code const char *p)
{
    uint8_t n = 0;
    while (p[n]) n++;
    tcp_stage_code(s, off, (__code const uint8_t *)p, n);
    return n;
}

static uint8_t handshake(uint8_t s, uint8_t *buf, uint16_t n)
{
    uint16_t k = find_hdr(buf, n, "sec-websocket-key:");
    /* static, not on the stack: there are 175 bytes of stack for everything
     * and this function is never re-entered. See firmware/README rule 4. */
    static __xdata uint8_t digest[20];
    static __xdata char accept[29];
    uint16_t off = 0;
    uint8_t i;

    if (k == 0xFFFF) return 1;
    while (k < n && buf[k] == ' ') k++;
    if (k + 24 > n) return 1;

    sha1_init();
    sha1_update(buf + k, 24);
    for (i = 0; WS_GUID[i]; i++) {
        uint8_t c = (uint8_t)WS_GUID[i];
        sha1_update(&c, 1);
    }
    sha1_final(digest);
    b64_20(digest, accept);

    off += stage_str(s, off, "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: ");
    tcp_stage(s, off, (const uint8_t *)accept, 28);
    off += 28;
    /* Echo the subprotocol only if one was asked for: offering one that was
     * not requested makes the browser fail the connection outright. */
    if (find_hdr(buf, n, "sec-websocket-protocol:") != 0xFFFF)
        off += stage_str(s, off, "\r\nSec-WebSocket-Protocol: mqtt");
    off += stage_str(s, off, "\r\n\r\n");
    tcp_commit(s, off);
    return 0;
}


/* ---- POST /update --------------------------------------------------------- */

/* The only thing that is ever POSTed: a firmware image (ota.c). It is not
 * handled here but from the top of the main loop (ws_ota_run), because the
 * call into the bootloader runs on this program's stack, and down here it
 * would start ~20 bytes deeper. Returns 1 when it has queued the upload, 0
 * when it has answered and the connection should close. */
static __xdata uint8_t ota_sock = 0xFF;
static __xdata uint32_t ota_len;
static __xdata uint8_t ota_ui;          /* 1: POST /ui, the web page */

/* "Authorization: Basic base64(user:password)" naming a user. */
static uint8_t basic_auth(const uint8_t *buf, uint16_t hn)
{
    static __xdata uint8_t cred[USER_NAME + 1 + USER_PASS];
    static __code const char basic[] = "basic ";
    static __xdata uint8_t n, i;
    uint16_t k = find_hdr(buf, hn, "authorization:");
    if (k == 0xFFFF) return 0;
    while (k < hn && buf[k] == ' ') k++;
    for (i = 0; basic[i]; i++, k++)
        if (k >= hn || lower(buf[k]) != (uint8_t)basic[i]) return 0;
    n = b64_decode(buf + k, (uint16_t)(hn - k), cred, sizeof(cred));
    for (i = 0; i < n && cred[i] != ':'; i++)
        ;
    return i < n && users_take(cred, i, cred + i + 1, (uint8_t)(n - i - 1)) && users_check();
}

static uint8_t post(uint8_t s, const uint8_t *buf, uint16_t hn)
{
    static __code const char p_fw[] = "POST /update ";
    static __code const char p_ui[] = "POST /ui ";
    uint32_t clen = 0;
    uint16_t k;
    uint8_t fw = 1, ui = 1;

    for (k = 0; p_fw[k]; k++)
        if (k >= hn || buf[k] != (uint8_t)p_fw[k]) { fw = 0; break; }
    for (k = 0; p_ui[k]; k++)
        if (k >= hn || buf[k] != (uint8_t)p_ui[k]) { ui = 0; break; }
    if (!fw && !ui) {
        http_reply(s, "404 Not Found", "POST /update (firmware) or /ui (web page)\n");
        return 0;
    }
    if (!basic_auth(buf, hn)) {
        http_reply(s, "401 Unauthorized", "log in: HTTP Basic auth with a ZRP-6 user\n");
        return 0;
    }
    if (ota_sock != 0xFF) {
        http_reply(s, "409 Conflict", "an upload is already in progress\n");
        return 0;
    }
    k = find_hdr(buf, hn, "content-length:");
    if (k != 0xFFFF) {
        while (k < hn && buf[k] == ' ') k++;
        while (k < hn && buf[k] >= '0' && buf[k] <= '9')
            clen = clen * 10 + (uint8_t)(buf[k++] - '0');
    }
    ota_sock = s;
    ota_len = clen;
    ota_ui = ui;
    mode[s] = M_OTA;
    return 1;
}

void ws_ota_run(void)
{
    __code const char *msg;
    uint8_t s = ota_sock, r;
    if (s == 0xFF) return;
    ota_sock = 0xFF;
    r = ota_http(s, ota_len, ota_ui, &msg);
    if (r) {
        http_reply(s, "200 OK", msg);
        if (r == 1) ota_restart = 1;
    } else {
#ifdef OTA_DEBUG
        ota_dbg_reply(s);
#else
        http_reply(s, (ota_ui || ota_len == OTA_IMAGE_SIZE) ? "422 Unprocessable Entity" : "400 Bad Request", msg);
#endif
    }
    /* mode stays M_OTA: ws_poll() sees the socket closed, re-listens and
     * reports it gone, like any other departure. */
    tcp_disconnect(s);
}

/* ---- receive ------------------------------------------------------------ */

/* Everything after the request line up to the blank line, read once into the
 * caller's buffer rather than peeked a byte at a time. */
static uint16_t http_headers(uint8_t s, uint8_t *buf, uint16_t max, uint16_t avail)
{
    uint16_t n = avail > max ? max : avail, i;
    tcp_read(s, 0, buf, n);
    for (i = 3; i < n; i++)
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n'
         && buf[i - 1] == '\r' && buf[i] == '\n')
            return (uint16_t)(i + 1);
    return 0;
}

static uint16_t poll_ws(uint8_t s, uint8_t *buf, uint16_t max)
{
    uint16_t avail = tcp_avail(s), plen, total, i;
    uint8_t op, hlen, mask[4];

    if (avail < 2) return 0;
    tcp_read(s, 0, hdr, avail < 8 ? (uint16_t)avail : 8);
    op = hdr[0] & 0x0F;
    if (!(hdr[1] & 0x80)) return WS_GONE;       /* unmasked: not a browser */
    plen = hdr[1] & 0x7F;
    if (plen < 126)       hlen = 2;
    else if (plen == 126) { if (avail < 4) return 0;
                            plen = ((uint16_t)hdr[2] << 8) | hdr[3]; hlen = 4; }
    else                  return WS_GONE;       /* 64-bit length: not from us */

    total = (uint16_t)(hlen + 4 + plen);
    if (avail < total) return 0;                /* still arriving */
    tcp_read(s, hlen, mask, 4);

    if (op == 0x8) { tcp_drop(s, total); return WS_GONE; }
    if (op == 0x9) {                            /* ping */
        if (plen <= 125) {
            tcp_read(s, (uint16_t)(hlen + 4), buf, plen);
            for (i = 0; i < plen; i++) buf[i] ^= mask[i & 3];
            ws_pong(s, buf, (uint8_t)plen);
        }
        tcp_drop(s, total);
        return 0;
    }
    if (op == 0xA) { tcp_drop(s, total); return 0; }   /* pong */

    if (plen > max) { tcp_drop(s, total); return 0; }  /* cannot hold it */
    tcp_read(s, (uint16_t)(hlen + 4), buf, plen);
    for (i = 0; i < plen; i++) buf[i] ^= mask[i & 3];
    tcp_drop(s, total);
    return plen;
}

static uint16_t poll_raw(uint8_t s, uint8_t *buf, uint16_t max)
{
    uint16_t avail = tcp_avail(s), rem = 0, mul = 1, total;
    uint8_t i = 1;

    if (avail < 2) return 0;
    tcp_read(s, 0, hdr, avail < 5 ? (uint16_t)avail : 5);
    /* The remaining-length varint is 1 to 4 bytes and each byte past the first
     * only exists if the one before had its top bit set. */
    do {
        if (i > 4) return WS_GONE;      /* malformed: it will never complete */
        if ((uint16_t)i >= avail) return 0;
        rem += (uint16_t)(hdr[i] & 0x7F) * mul;
        mul *= 128;
    } while (hdr[i++] & 0x80);

    total = (uint16_t)(i + rem);
    if (avail < total) return 0;
    if (total > max) { tcp_drop(s, total); return 0; }
    tcp_read(s, 0, buf, total);
    tcp_drop(s, total);
    return total;
}

uint16_t ws_poll(uint8_t s, uint8_t *buf, uint16_t max)
{
    uint8_t st;
    uint16_t avail, hn;

    if (lent[s]) return 0;              /* net.c has it */
    st = sock_status(s);

    if (st == ST_LISTEN || st == ST_SYNRECV) return 0;   /* waiting for a client */

    if (st != ST_ESTAB) {
        /* The connection is over, or ending. Report it gone once, and let the
         * chip finish the close handshake before listening again: aborting it
         * leaves the last unacknowledged segment queued for whoever connects
         * next (see tcp_disconnect). The closing states are normal ends of a
         * connection but must not become a port that never listens again, so
         * each gets a second before it is forced. */
        uint8_t was = mode[s];
        mode[s] = M_NONE;
        if (was == M_OTA && ota_sock == s) ota_sock = 0xFF;   /* left before its turn */
        if (st == ST_CLOSED || st == ST_INIT) {
            ws_listen(s);
        } else if (!closing[s]) {
            closing[s] = 1;
            closing_at[s] = ticks();
            if (st == ST_CLOSE_WAIT) tcp_disconnect(s);     /* they closed: FIN back */
        } else if ((uint16_t)(ticks() - closing_at[s]) > TICKS_S(1)) {
            tcp_abort(s);
            ws_listen(s);
        }
        return was != M_NONE ? WS_GONE : 0;
    }

    if (mode[s] == M_OTA) return 0;             /* ws_ota_run() has it */
    if (mode[s] == M_WS)  return poll_ws(s, buf, max);
    if (mode[s] == M_RAW) return poll_raw(s, buf, max);

    /* Undecided. Look at the first byte. */
    avail = tcp_avail(s);
    if (!avail) return 0;
    if (tcp_peek(s, 0) == 0x10) { mode[s] = M_RAW; return poll_raw(s, buf, max); }
    if (tcp_peek(s, 0) != 'G' && tcp_peek(s, 0) != 'P') { ws_close(s); return 0; }

    hn = http_headers(s, buf, max, avail);
    if (!hn && buf[0] == 'P') {
        if (avail < max) return 0;              /* still arriving */
        http_reply(s, "431 Request Header Fields Too Large", "headers too long\n");
        tcp_disconnect(s);
        return 0;
    }
    if (!hn) {
        if (avail < max) return 0;              /* still arriving */
        /* Headers longer than the buffer. A GET is still a GET — serve it and
         * hang up, which needs no exact header length. Only a handshake,
         * whose key might be past the end, is lost. */
        http_serve(s, buf, max);
        tcp_disconnect(s);
        mode[s] = M_NONE;
        return 0;
    }
    /* Sec-WebSocket-Key is the marker, not "Upgrade: websocket": the upgrade
     * header's spelling and spacing vary between browsers, and a request
     * carrying a key is a handshake by definition. */
    if (buf[0] == 'P') {
        tcp_drop(s, hn);
        if (!post(s, buf, hn)) tcp_disconnect(s);
        return 0;
    }
    if (find_hdr(buf, hn, "sec-websocket-key:") != 0xFFFF) {
        tcp_drop(s, hn);
        if (handshake(s, buf, hn)) { ws_close(s); return 0; }
        mode[s] = M_WS;
        born[s] = ++births;             /* ws_balance() makes room if need be */
        return 0;
    }
    /* An ordinary page request. Serve it and hang up — the UI is one file, so
     * there is never a second request to keep the socket open for. */
    tcp_drop(s, hn);
    http_serve(s, buf, hn);
    tcp_disconnect(s);
    return 0;
}
