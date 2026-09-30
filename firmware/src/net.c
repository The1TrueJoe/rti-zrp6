#include "board.h"
#include "io.h"
#include "config.h"
#include "w5100.h"
#include "ws.h"
#include "ota_app.h"
#include "net.h"
#include "../boot/ota.h"

/* DHCP (RFC 2131), the four messages a client needs and nothing more:
 * DISCOVER, OFFER, REQUEST, ACK (or NAK). One exchange at a time, driven by
 * net_poll() so nothing blocks: a message goes out, and later polls look for
 * the answer.
 *
 * Written for code size: messages are a template out of Flash staged straight
 * into the W5100 and patched there, all state is static __xdata, and pointers
 * are __xdata — under SDCC's large model a generic pointer makes every access
 * a library call, and the first version of this file was 5.6 KB.
 *
 * Every byte read back is read three times and ORed (tcp_read_sure): the
 * board's D4 fault turns 1s into 0s, and an address with a bit missing would
 * be a very confusing thing to be given. */

#define ST_BOUND     0          /* holding a lease; or static; or fallback */
#define ST_DISCOVER  1          /* DISCOVER sent, waiting for an OFFER */
#define ST_REQUEST   2          /* REQUEST for an offer sent, waiting for ACK */
#define ST_RENEW     3          /* REQUEST for our lease sent, waiting for ACK */

#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define ACK      5
#define NAK      6

#define ANSWER   TICKS_S(2)     /* how long to wait for each answer */
#define RETRY_S  60             /* fallback, or a failed renewal: again after */
#define FOREVER  0xFFFFFFFFul
#define NM_NONE  0xFF           /* booting: no address yet */

__xdata uint8_t net_changed;

static __xdata uint8_t mode_ = NM_STATIC, st, sock = 0xFF, tries, booting;
static __xdata uint8_t ip[4], mask[4], gw[4];
/* An answer's fields, then the server's: o_ip, o_mask, o_gw, server. */
static __xdata uint8_t o[16];
#define O_IP    0
#define O_MASK  4
#define O_GW    8
#define O_SRV   12
static __xdata uint32_t lease_s, next_at, t2_at, end_at;
static __xdata uint16_t rp;
static __xdata uint8_t xid[4];
static __xdata uint16_t sent_at;
static __xdata uint8_t zero4[4], bcast[4] = { 255, 255, 255, 255 };
static __xdata uint8_t k;

uint8_t net_mode(void) BANKED { return mode_; }
const uint8_t *net_ip(void) BANKED { return ip; }
const uint8_t *net_mask(void) BANKED { return mask; }
const uint8_t *net_gw(void) BANKED { return gw; }

static void apply(__xdata const uint8_t *a, __xdata const uint8_t *m, __xdata const uint8_t *g)
{
    for (k = 0; k < 4; k++) { ip[k] = a[k]; mask[k] = m[k]; gw[k] = g[k]; }
    w5100_addr(ip, mask, gw);
}

/* ---- the socket ---------------------------------------------------------- */

static uint8_t take(void)
{
    if (!booting) {
        sock = ws_borrow();
        if (sock == 0xFF) return 0;         /* every socket busy: later */
    }
    udp_open(sock, 68);
    return 1;
}

static void give_back(void)
{
    if (sock == 0xFF) return;
    if (booting) {
        udp_open(sock, 68);                 /* stays ours; just flushed */
        return;
    }
    ws_return(sock);
    sock = 0xFF;
}

/* ---- sending ------------------------------------------------------------- */

/* The options every message carries, from the magic cookie. Patched: the
 * message type at 6, the MAC at 10, the host name's last four characters at
 * 23. Then 50 (requested IP) and 54 (server) for a REQUEST that answers an
 * offer, and the end. */
static __code const uint8_t OPTS[32] = {
    99, 130, 83, 99,  53, 1, 0,  61, 7, 1, 0, 0, 0, 0, 0, 0,
    12, 9, 'z', 'r', 'p', '6', '-', 0, 0, 0, 0,  55, 3, 1, 3, 51 };
#define OPT_AT  236
#define B       ota_buf

static uint8_t hexd(uint8_t v) { v &= 15; return (uint8_t)(v < 10 ? '0' + v : 'A' + v - 10); }

/* A BOOTP request is 236 bytes before its options, nearly all zeros: the
 * first 44 are built in ota_buf, the 192 zeros of sname and file come from
 * the same buffer after it is cleared, then the options. */
static void send(uint8_t type, uint8_t renew)
{
    __xdata const uint8_t *mac = cfg_get()->mac;
    uint16_t off;
    uint8_t n;

    for (n = 0; n < 48; n++) B[n] = 0;
    for (off = 44; off < OPT_AT; off += 48) tcp_stage(sock, off, B, 48);
    B[0] = 1; B[1] = 1; B[2] = 6;               /* BOOTREQUEST, Ethernet */
    for (k = 0; k < 4; k++) {
        B[4 + k] = xid[k];
        if (renew) B[12 + k] = ip[k];           /* ciaddr */
    }
    if (!renew) B[10] = 0x80;                   /* answer by broadcast */
    for (k = 0; k < 6; k++) B[28 + k] = mac[k];
    tcp_stage(sock, 0, B, 44);

    for (n = 0; n < sizeof(OPTS); n++) B[n] = OPTS[n];
    B[6] = type;
    for (k = 0; k < 6; k++) B[10 + k] = mac[k];
    B[23] = hexd(mac[4] >> 4); B[24] = hexd(mac[4]);
    B[25] = hexd(mac[5] >> 4); B[26] = hexd(mac[5]);
    if (type == REQUEST && !renew) {
        B[32] = 50; B[33] = 4; B[38] = 54; B[39] = 4;
        for (k = 0; k < 4; k++) { B[34 + k] = o[O_IP + k]; B[40 + k] = o[O_SRV + k]; }
        n = 44;
    }
    B[n++] = 255;
    tcp_stage(sock, OPT_AT, B, n);

    /* Renewing goes to the server that gave the lease; after T2, and for
     * everything else, broadcast. */
    udp_to(sock, (renew && uptime() < t2_at) ? o + O_SRV : bcast, 67);
    tcp_commit(sock, OPT_AT + n);
    sent_at = ticks();
}

/* ---- receiving ----------------------------------------------------------- */

static uint8_t opt4(uint8_t at)
{
    for (k = 0; k < 4; k++) o[at + k] = B[rp + 2 + k];
    return 0;
}

/* The type of a DHCP answer to us waiting on the socket (OFFER, ACK, NAK),
 * with its fields in o[]; 0 if there is none. Anything else is thrown away. */
static uint8_t receive(void)
{
    static __xdata uint16_t n, rd, len;
    __xdata const uint8_t *mac = cfg_get()->mac;
    uint8_t type = 0, l, c;

    n = tcp_avail(sock);
    if (n < 8) return 0;
    rd = tcp_rx_ptr(sock);
    tcp_read_sure(sock, rd, B, 8);              /* sender IP, port, length */
    len = ((uint16_t)B[6] << 8) | B[7];
    if (len > 1500 || len + 8 > n || n > 2048) {
        udp_open(sock, 68);                     /* lost track: start clean */
        return 0;
    }
    if (len < 240) goto out;
    tcp_read_sure(sock, rd + 8, B, 240);
    if (B[0] != 2 || B[236] != 99 || B[237] != 130 || B[238] != 83 || B[239] != 99)
        goto out;
    for (k = 0; k < 4; k++) if (B[4 + k] != xid[k]) goto out;
    for (k = 0; k < 6; k++) if (B[28 + k] != mac[k]) goto out;
    for (k = 0; k < 4; k++) o[O_IP + k] = B[16 + k];   /* yiaddr */
    for (k = O_MASK; k < O_SRV; k++) o[k] = 0;
    lease_s = FOREVER;
    n = len - 240;
    if (n > OTA_BUF) n = OTA_BUF;
    tcp_read_sure(sock, rd + 8 + 240, B, n);
    for (rp = 0; rp + 1 < n; rp += 2 + l) {
        c = B[rp];
        if (c == 255) break;
        if (c == 0) { rp--; l = 0; continue; }       /* pad: one byte */
        l = B[rp + 1];
        if (rp + 2 + l > n) break;
        if (c == 53) type = B[rp + 2];
        else if (l < 4) ;
        else if (c == 54) opt4(O_SRV);
        else if (c == 1)  opt4(O_MASK);
        else if (c == 3)  opt4(O_GW);
        else if (c == 51) lease_s = ((uint32_t)B[rp + 2] << 24) | ((uint32_t)B[rp + 3] << 16)
                                  | ((uint16_t)B[rp + 4] << 8) | B[rp + 5];
    }
out:
    tcp_rx_release(sock, rd + 8 + len);
    return type;
}

/* ---- mDNS ----------------------------------------------------------------- */
/* Announce-only mDNS (docs/MQTT.md, "Network"): zrp6-XXXX.local and a
 * _zrp6._tcp service, multicast every minute with a 120 s lifetime, at once
 * after boot and after any change of address. Answering queries would need a
 * socket listening all the time, and there are four; this borrows a listener
 * for the moment of each send instead. The packet is built by layout.py and
 * kept in the image's data-only first 8 KB, at 0x1C00 behind a header of its
 * length and the offsets to fill in. */
#define MDNS_AT     0x1C00
#define MDNS_EVERY  60
static __code const uint8_t MDNS_GROUP[10] = {
    0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB,  224, 0, 0, 251 };
static __xdata uint16_t mdns_t;
static __xdata uint8_t  mdns_due = 1;       /* 1: announce at the next chance */

/* B[6 + at...] = the hex of n bytes of the MAC from byte `from` */
static void hexput(uint8_t at, uint8_t from, uint8_t n)
{
    __xdata const uint8_t *m = cfg_get()->mac + from;
    __xdata uint8_t *d = B + 6 + at;
    while (n--) {
        *d++ = hexd(*m >> 4);
        *d++ = hexd(*m++);
    }
}

static void mdns(void)
{
    volatile __xdata uint8_t *w = (volatile __xdata uint8_t *)(OTA_WINDOW + MDNS_AT);
    uint8_t s, i;

    if (booting || mode_ == NM_NONE) return;
    if (!mdns_due && (uint16_t)(ticks() - mdns_t) < TICKS_S(MDNS_EVERY)) return;
    s = ws_borrow();
    if (s == 0xFF) return;                          /* all busy: next pass */
    PAGE_WINDOW(OTA_PAGE(RUN_SLOT(), 0));           /* the W5100 is away */
    for (i = 0; i < 6 + 128; i++) B[i] = w[i];
    PAGE_NORMAL();
    hexput(B[1], 4, 2);                             /* the host label, twice */
    hexput(B[2], 4, 2);
    hexput(B[3], 0, 6);                             /* the MAC in the TXT */
    for (i = 0; i < 4; i++) B[6 + B[4] + i] = ip[i];
    udp_open_group(s, MDNS_GROUP, 5353);
    tcp_stage(s, 0, B + 6, B[0]);
    tcp_commit(s, B[0]);
    ws_return(s);
    mdns_t = ticks();
    mdns_due = 0;
}

/* ---- the exchange -------------------------------------------------------- */

static void discover(void)
{
    uint16_t t = ticks();
    xid[0] = cfg_get()->mac[5];
    xid[1] = (uint8_t)t;
    xid[2] = TL0;
    xid[3] = (uint8_t)(t >> 8);
    tries = 1;
    st = ST_DISCOVER;
    send(DISCOVER, 0);
}

static uint8_t differs(void)
{
    for (k = 0; k < 4; k++)
        if (ip[k] != o[O_IP + k] || mask[k] != o[O_MASK + k] || gw[k] != o[O_GW + k]) return 1;
    return 0;
}

static void bound(void)
{
    if (mode_ != NM_DHCP || differs()) {
        apply(o + O_IP, o + O_MASK, o + O_GW);
        if (!booting) net_changed = 1;
        mdns_due = 1;
    }
    mode_ = NM_DHCP;
    if (lease_s < 60) lease_s = 60;
    if (lease_s == FOREVER) {
        next_at = t2_at = end_at = FOREVER;
    } else {
        end_at = uptime();
        next_at = end_at + (lease_s >> 1);                          /* T1 */
        t2_at = end_at + lease_s - (lease_s >> 3);                  /* T2 */
        end_at += lease_s;
    }
    st = ST_BOUND;
    give_back();
}

/* No answer. With a lease, keep it and try again in a minute (it counts as
 * lost once it has run out); without one, the fallback address. */
static void no_answer(void)
{
    give_back();
    st = ST_BOUND;
    next_at = uptime() + RETRY_S;
    if (mode_ == NM_DHCP) {
        if (uptime() >= end_at) mode_ = NM_FALLBACK;   /* the lease ran out */
        return;
    }
    if (mode_ != NM_FALLBACK) {
        apply(cfg_get()->ip, cfg_get()->netmask, cfg_get()->gateway);
        mode_ = NM_FALLBACK;
        mdns_due = 1;
        if (!booting) net_changed = 1;
    }
}

void net_poll(void) BANKED
{
    uint8_t t;
    mdns();
    if (mode_ == NM_STATIC) return;

    if (st == ST_BOUND) {
        if (next_at == FOREVER || uptime() < next_at || !take()) return;
        if (mode_ == NM_DHCP) {
            tries = 1;
            st = ST_RENEW;
            send(REQUEST, 1);
        } else {
            discover();
        }
        return;
    }
    t = receive();
    if (st == ST_DISCOVER) {
        if (t == OFFER) {
            tries = 1;
            st = ST_REQUEST;
            send(REQUEST, 0);
            return;
        }
    } else if (t == ACK) {
        bound();
        return;
    } else if (t == NAK) {
        discover();
        return;
    }
    if ((uint16_t)(ticks() - sent_at) < ANSWER) return;
    if (tries >= (booting ? 5 : 3)) { no_answer(); return; }
    tries++;
    send(st == ST_DISCOVER ? DISCOVER : REQUEST, st == ST_RENEW);
}

void net_start(void) BANKED
{
    __xdata const struct cfg *c = cfg_get();
    uint32_t until;

    w5100_init(c->mac, zero4, zero4, zero4);
    if (c->net != NET_DHCP) {                   /* static, or on trial */
        apply(c->ip, c->netmask, c->gateway);
        mode_ = NM_STATIC;
        return;
    }
    /* DHCP on socket 0 before anything listens. ~10 s, then the fallback. */
    mode_ = NM_NONE;
    booting = 1;
    sock = 0;
    udp_open(0, 68);
    discover();
    until = uptime() + 10;
    while (st != ST_BOUND && uptime() < until) {
        heartbeat();
        net_poll();
    }
    if (st != ST_BOUND) no_answer();
    booting = 0;
    sock_close(0);
    sock = 0xFF;
}

void net_reapply(void) BANKED
{
    w5100_init(cfg_get()->mac, ip, mask, gw);
    /* Any exchange in flight was on a socket the reset destroyed; the broker
     * re-listens every socket after this. Try again in a moment. */
    sock = 0xFF;
    if (st != ST_BOUND) {
        st = ST_BOUND;
        next_at = uptime() + 5;
    }
}
