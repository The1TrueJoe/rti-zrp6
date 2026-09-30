#include "board.h"
#include "bus.h"
#include "io.h"
#include "delay.h"
#include "w5100.h"

/* Kept apart deliberately: payload and pointer failures have very different
 * consequences and lumping them together hid that for hours. A bad payload
 * byte spoils one byte of one page; a bad pointer spoils everything after it. */
__xdata uint16_t w5100_rewrites;    /* payload bytes rewritten */
__xdata uint16_t w5100_badbytes;    /* payload bytes still wrong after three */
__xdata uint16_t w5100_ptr_retry;   /* ring-pointer writes rewritten */
__xdata uint16_t w5100_ptr_bad;     /* ring-pointer writes still wrong */

/* Reads until two consecutive reads agree, because RX_RSR and TX_FSR free-run
 * and a byte pair can tear — but only a bounded number of times. An unbounded
 * loop here hung the board solid: a register read between a close and an open
 * never settles, and the box stopped answering anything. A probe must always
 * return.
 *
 * Agreement is not proof. If a weak data line fails as a function of the
 * previous bus state rather than at random, two reads fail identically and
 * this returns a wrong value confidently — which is why TX_WR is no longer
 * read at all. */
static uint16_t rd16(uint16_t off)
{
    uint16_t a, b;
    uint8_t t;
    a = ((uint16_t)W(off) << 8) | W(off + 1);
    for (t = 0; t < 8; t++) {
        b = ((uint16_t)W(off) << 8) | W(off + 1);
        if (a == b) return a;
        a = b;
    }
    return a;
}

/* Plain. Sn_PORT is written between a close and an open and does not read back
 * in that window; verifying it reported thousands of failures on an idle board
 * and was my instrumentation, not the hardware. */
/* Plain. Sn_PORT is written between a close and an open and does not read back
 * in that window; verifying it reported thousands of failures on an idle board
 * and was my instrumentation, not the hardware. */
/* A pointer read that survives a bus which drops bits.
 *
 * rd16() reads twice and accepts agreement, which defeats a tearing counter
 * but not this board: D4 fails as a function of what was on the bus a moment
 * earlier, not at random, so two back-to-back reads of the same address fail
 * identically and agree on the wrong value. A trace caught TX_WR reading
 * 0xCA7A where it held 0xDB7A — one bit — after which tcp_commit() added the
 * chunk length to the corrupted pointer, wrote it back, and every later
 * segment carried the ring one wrap behind under correct checksums.
 *
 * So the reads here are separated by a read of an unrelated register. That
 * changes the bus state each attempt sees, which is the one thing the fault
 * depends on, and three agreements are then worth something.
 *
 * Holding TX_WR in firmware instead was tried and is worse: the value still
 * has to be read once when the socket opens, and a bad read there poisons a
 * whole connection rather than one chunk of it. */
static uint16_t rd16_safe(uint16_t off)
{
    uint16_t a, b, c;
    uint8_t t;
    a = rd16(off);
    for (t = 0; t < 6; t++) {
        (void)W(WR_MR);
        b = rd16(off);
        (void)W(WR_RCR);
        c = rd16(off);
        if (a == b && b == c) return a;
        a = c;
    }
    return a;
}

static void wr16(uint16_t off, uint16_t v)
{
    W(off) = (uint8_t)(v >> 8);
    W(off + 1) = (uint8_t)v;
}

/* Verified, for registers written while a socket is open — in practice the two
 * ring pointers, and TX_WR is the one that matters. A dropped bit in a payload
 * byte spoils one byte; a dropped bit in TX_WR moves a segment boundary, and
 * the far end receives a correctly-checksummed segment full of whatever the
 * ring held last time round. */
static void wr16v(uint16_t off, uint16_t v)
{
    uint8_t t;
    for (t = 0; t < 4; t++) {
        W(off) = (uint8_t)(v >> 8);
        W(off + 1) = (uint8_t)v;
        /* rd16_safe, not rd16: the check is worthless if the read-back can
         * fail the same way the write did. Responses came back 16 bytes short
         * — bit 4 of a pointer's low byte — while the plain read-back happily
         * confirmed them. */
        if (rd16_safe(off) == v) return;
        w5100_ptr_retry++;
    }
    w5100_ptr_bad++;
}

static void ring_byte(uint16_t addr, uint8_t v)
{
    uint8_t t;
    W(addr) = v;
    /* xrd(), not W(): SDCC elides a volatile read that follows a write to the
     * same address and substitutes the value just written, which would make
     * this check confirm itself. */
    for (t = 0; t < 3; t++) {
        if (xrd(0x8000u + addr) == v) return;
        w5100_rewrites++;
        W(addr) = v;
    }
    if (xrd(0x8000u + addr) != v) w5100_badbytes++;
}

/* Staged through IRAM: `XB(a) = buf[i]` with buf in XDATA puts two MOVX cycles
 * back to back, which is the hazard bus.c exists for. */
static void ring_put(uint16_t base, uint16_t wr, const uint8_t *buf, uint16_t len)
{
    static __data uint8_t stage[16];
    uint8_t i, n;
    while (len) {
        n = len > sizeof(stage) ? (uint8_t)sizeof(stage) : (uint8_t)len;
        for (i = 0; i < n; i++) stage[i] = buf[i];
        for (i = 0; i < n; i++) ring_byte(base + ((wr + i) & BUFMASK), stage[i]);
        buf += n;
        wr += n;
        len -= n;
    }
}

static void ring_put_code(uint16_t base, uint16_t wr,
                          __code const uint8_t *src, uint16_t len)
{
    static __data uint8_t stage[16];
    uint8_t i, n;
    while (len) {
        n = len > sizeof(stage) ? (uint8_t)sizeof(stage) : (uint8_t)len;
        for (i = 0; i < n; i++) stage[i] = src[i];
        for (i = 0; i < n; i++) ring_byte(base + ((wr + i) & BUFMASK), stage[i]);
        src += n;
        wr += n;
        len -= n;
    }
}

static void cmd(uint8_t s, uint8_t c)
{
    uint16_t guard = 40000;
    W(S_CR(s)) = c;
    while (W(S_CR(s)) && --guard)
        ;
}

void w5100_init(const uint8_t *mac, const uint8_t *ip,
                const uint8_t *netmask, const uint8_t *gateway)
{
    uint8_t i;
    W(WR_MR) = 0x80;                        /* software reset */
    for (i = 0; i < 40; i++) dly(0);
    W(WR_MR) = 0x00;
    for (i = 0; i < 40; i++) dly(0);
    for (i = 0; i < 6; i++) W(WR_SHAR + i) = mac[i];
    for (i = 0; i < 4; i++) W(WR_SIPR + i) = ip[i];
    for (i = 0; i < 4; i++) W(WR_GAR + i) = gateway[i];
    for (i = 0; i < 4; i++) W(WR_SUBR + i) = netmask[i];
    W(WR_RMSR) = 0x55;                      /* 2 KB per socket */
    W(WR_TMSR) = 0x55;
    W(WR_RTR) = 0x07; W(WR_RTR + 1) = 0xD0;
    W(WR_RCR) = 0x08;
}

/* Confirmed, because a decision hangs on it and one bit decides everything:
 * ESTABLISHED is 0x17, and 0x17 with D4 low is 0x07. A single misread of this
 * register tore down live connections — the box would answer two requests and
 * then go quiet on that port — once ws.c started acting on non-ESTABLISHED
 * states instead of ignoring them.
 *
 * The reads are separated by a read of an unrelated register for the same
 * reason rd16_safe() does it: the fault depends on the previous bus state, so
 * two adjacent reads of the same address are not independent evidence. */
uint8_t sock_status(uint8_t s)
{
    uint8_t a, b, t;
    a = W(S_SR(s));
    for (t = 0; t < 4; t++) {
        (void)W(WR_MR);
        b = W(S_SR(s));
        if (a == b) return a;
        a = b;
    }
    return a;
}
void sock_close(uint8_t s)     { cmd(s, CR_CLOSE); }

#ifdef ZRP6_BRIDGE
uint8_t tcp_connect(uint8_t s, const uint8_t *ip, uint16_t port)
{
    uint8_t i;
    uint16_t guard;
    cmd(s, CR_CLOSE);
    W(S_MR(s)) = MODE_TCP;
    /* Any source port will do; make it distinct so a reconnect is not mistaken
     * for the old connection by anything in between. */
    W(S_PORT(s)) = 0xC0;
    W(S_PORT(s) + 1) = 0x01;
    cmd(s, CR_OPEN);
    if (W(S_SR(s)) != SOCK_INIT) return 1;
    for (i = 0; i < 4; i++) W(S_DIPR(s) + i) = ip[i];
    wr16(S_DPORT(s), port);
    cmd(s, CR_CONNECT);
    for (guard = 0; guard < 20000; guard++) {
        uint8_t st = W(S_SR(s));
        if (st == SOCK_ESTAB) return 0;
        if (st == SOCK_CLOSED) return 1;
        dly(200);
    }
    return 1;
}
#endif

#ifdef ZRP6_BRIDGE
uint16_t tcp_recv(uint8_t s, uint8_t *buf, uint16_t max)
{
    uint16_t n = rd16(S_RX_RSR(s)), rd, i;
    if (!n) return 0;
    if (n > max) n = max;
    rd = rd16_safe(S_RX_RD(s));
    for (i = 0; i < n; i++)
        buf[i] = W(RXBASE(s) + ((rd + i) & BUFMASK));
    /* RECV frees nothing unless RX_RD has first been advanced past the data. */
    wr16v(S_RX_RD(s), rd + n);
    cmd(s, CR_RECV);
    return n;
}
#endif

uint8_t tcp_send(uint8_t s, const uint8_t *buf, uint16_t len)
{
    uint16_t guard;
    if (!len) return 0;
    guard = 40000;
    while (rd16(S_TX_FSR(s)) < len && --guard)
        ;
    if (!guard) return 1;
    ring_put(TXBASE(s), rd16_safe(S_TX_WR(s)), buf, len);
    tcp_commit(s, len);             /* ADVANCE TX_WR and wait for the drain;
                                     * rewriting it makes the second packet the
                                     * last one that ever leaves */
    return 0;
}

/* 1 if the W5100 still holds the address it was given. A hardware reset of the
 * chip (P4.1, a brown-out) wipes MAC and IP, and nothing else announces it:
 * the sockets simply stop answering. Read twice with an unrelated access in
 * between, because D4 on this board is marginal and one misread here would
 * drop every client for nothing. */
uint8_t w5100_ip_ok(const uint8_t *ip)
{
    uint8_t i, bad = 0;
    for (i = 0; i < 4; i++) if (W(WR_SIPR + i) != ip[i]) bad = 1;
    if (!bad) return 1;
    (void)W(WR_MR);
    for (i = 0; i < 4; i++) if (W(WR_SIPR + i) != ip[i]) return 0;
    return 1;
}

#ifdef TX_DEBUG
/* A temporary trace of the transmit pointers (TX_DEBUG builds only): at
 * re-listen, page start and page end, per socket. Frozen a few entries after a
 * page starts with the ring not empty; dumped over MQTT (app.c debug/txdump). */
__xdata uint8_t txd[48][8];
__xdata uint8_t txd_i, txd_left = 0xFF, txd_hits;
void txd_log(uint8_t ev, uint8_t s)
{
    uint16_t r, w, f;
    if (!txd_left) return;
    r = rd16_safe(S_TX_RD(s));
    w = rd16_safe(S_TX_WR(s));
    f = rd16(S_TX_FSR(s));
    if (ev == 'S' && (r != w || f != 0x0800)) {
        txd_hits++;
        if (txd_left == 0xFF) txd_left = 6;
    }
    if (txd_left != 0xFF) txd_left--;
    txd[txd_i][0] = ev; txd[txd_i][1] = s;
    txd[txd_i][2] = (uint8_t)(r >> 8); txd[txd_i][3] = (uint8_t)r;
    txd[txd_i][4] = (uint8_t)(w >> 8); txd[txd_i][5] = (uint8_t)w;
    txd[txd_i][6] = (uint8_t)(f >> 8); txd[txd_i][7] = (uint8_t)f;
    txd_i = (uint8_t)((txd_i + 1) % 48);
}
#endif

void tcp_listen(uint8_t s, uint16_t port)
{
    cmd(s, CR_CLOSE);
    W(S_IR(s)) = 0x1F;
    W(S_MR(s)) = MODE_TCP;
    wr16(S_PORT(s), port);
    cmd(s, CR_OPEN);
    if (W(S_SR(s)) != SOCK_INIT) return;
#ifdef TX_DEBUG
    txd_log('L', s);
#endif
    cmd(s, CR_LISTEN);
}

/* A graceful close: FIN, and nothing else. The chip then finishes the close
 * handshake on its own — waiting, among other things, for the far end to
 * acknowledge the data already sent — and ws_poll() re-listens once it
 * reaches CLOSED, or forces it after a second.
 *
 * This used to issue CLOSE straight after DISCON. That abandons a connection
 * whose last segment the client has not acknowledged yet (macOS delays ACKs
 * up to 200 ms), and the W5100 kept that segment queued: the NEXT connection
 * on the socket was sent it first, as its opening bytes. About one page load
 * in ten on v7 began with the previous page's last 1460 bytes. */
void tcp_disconnect(uint8_t s)
{
    cmd(s, CR_DISCON);
}

void tcp_abort(uint8_t s)
{
    cmd(s, CR_CLOSE);
}

uint16_t tcp_avail(uint8_t s) { return rd16(S_RX_RSR(s)); }

uint8_t tcp_peek(uint8_t s, uint16_t off)
{
    return W(RXBASE(s) + ((rd16_safe(S_RX_RD(s)) + off) & BUFMASK));
}

void tcp_read(uint8_t s, uint16_t off, uint8_t *buf, uint16_t len)
{
    uint16_t rd = rd16_safe(S_RX_RD(s)) + off, i;
    for (i = 0; i < len; i++)
        buf[i] = W(RXBASE(s) + ((rd + i) & BUFMASK));
}

/* tcp_read() for data where every byte counts — a firmware image. The D4
 * fault only ever turns a 1 into a 0, and only bit 4, so the OR of reads that
 * do not share a bus history is the true byte unless all of them failed. The
 * reads are separated by reads of unrelated registers for that independence,
 * as in rd16_safe(); these hold 0x55, so D4 is high before each read.
 *
 * Measured 2026-09-24 with 64 KB uploads, of which the box checks the CRC:
 * one read per byte and the pointer re-read for every chunk, 1 in 5 intact;
 * three reads per byte, 3 in 10; three reads per byte AND the ring pointer
 * read once per upload and tracked here (tcp_rx_ptr), 9 in 10. The pointer
 * was the main culprit — see tcp_rx_ptr(). The CRC catches what is left, and
 * the uploader retries. */
void tcp_read_sure(uint8_t s, uint16_t rd, uint8_t *buf, uint16_t len)
{
    uint16_t i, a;
    uint8_t v;
    for (i = 0; i < len; i++) {
        a = RXBASE(s) + ((rd + i) & BUFMASK);
        (void)W(WR_RMSR);
        v = W(a);
        (void)W(WR_TMSR);
        v |= W(a);
        (void)W(WR_RMSR);
        v |= W(a);
        buf[i] = v;
    }
}

/* For a long read that tracks the ring pointer itself (ota.c): read RX_RD
 * once, then hand back each new value. Re-reading the pointer for every chunk
 * gave the bus fault ~500 chances per upload to move it. */
uint16_t tcp_rx_ptr(uint8_t s) { return rd16_safe(S_RX_RD(s)); }

void tcp_rx_release(uint8_t s, uint16_t rd)
{
    wr16v(S_RX_RD(s), rd);
    cmd(s, CR_RECV);
}

void tcp_drop(uint8_t s, uint16_t n)
{
    if (!n) return;
    wr16v(S_RX_RD(s), rd16_safe(S_RX_RD(s)) + n);
    cmd(s, CR_RECV);
}

uint16_t tcp_txfree(uint8_t s) { return rd16(S_TX_FSR(s)); }

void tcp_stage(uint8_t s, uint16_t off, const uint8_t *buf, uint16_t len)
{
    ring_put(TXBASE(s), rd16_safe(S_TX_WR(s)) + off, buf, len);
}

void tcp_stage_code(uint8_t s, uint16_t off, __code const uint8_t *src,
                    uint16_t len)
{
    ring_put_code(TXBASE(s), rd16_safe(S_TX_WR(s)) + off, src, len);
}

/* Sends, then waits for the ring to be EMPTY rather than merely sent.
 *
 * SEND_OK means the W5100 finished transmitting, not that the far end
 * acknowledged — and until it does, the W5100 keeps the bytes in the ring so it
 * can retransmit them. Sn_TX_FSR was treated as a licence to refill, and on one
 * page load in fifteen a chunk went in over data that had not been acked yet:
 * the browser was handed the previous ring-full at the new offset. The
 * corruption started at exactly 6 x 2048 and was the stream shifted by exactly
 * one ring, which is that bug and nothing else.
 *
 * Draining costs one round trip per send. On a LAN that is well under a
 * millisecond, and it makes the overwrite impossible rather than unlikely. */
void tcp_commit(uint8_t s, uint16_t len)
{
    uint16_t guard;
    if (!len) return;
    wr16v(S_TX_WR(s), rd16_safe(S_TX_WR(s)) + len);
    W(S_IR(s)) = 0x1F;
    cmd(s, CR_SEND);
    guard = 40000;
    while (!(W(S_IR(s)) & 0x18) && --guard)
        ;
    W(S_IR(s)) = 0x1F;
    /* Drained means acknowledged. Timed, not counted: a delayed ACK takes up
     * to 200 ms and the old 40000-iteration loop gave up before that. */
    guard = ticks();
    /* ~4 s: long enough for the W5100 to retransmit a lost segment a few
     * times (200 ms, then doubling) before the send is given up on. */
    while ((uint16_t)(ticks() - guard) < TICKS_S(4))
        if (rd16(S_TX_FSR(s)) >= TX_RING) return;
    /* Never drained: the peer has gone quiet. Say nothing and let the caller's
     * next status check find the socket closed. */
}

/* ---- UDP ------------------------------------------------------------------ */

void udp_open(uint8_t s, uint16_t port)
{
    cmd(s, CR_CLOSE);
    W(S_IR(s)) = 0x1F;
    W(S_MR(s)) = MODE_UDP;
    wr16(S_PORT(s), port);
    cmd(s, CR_OPEN);
}

/* Each value goes through a local (IRAM) on its way to the chip: README
 * rule 2. */
void udp_to(uint8_t s, const uint8_t *ip, uint16_t port)
{
    uint8_t i, v;
    for (i = 0; i < 4; i++) { v = ip[i]; W(S_DIPR(s) + i) = v; }
    wr16(S_DPORT(s), port);
}

void udp_open_group(uint8_t s, const uint8_t *grp, uint16_t port)
{
    uint8_t i, v;
    cmd(s, CR_CLOSE);
    W(S_IR(s)) = 0x1F;
    W(S_MR(s)) = MODE_UDP | 0x80;
    for (i = 0; i < 6; i++) { v = grp[i]; W(SOCK(s) + 0x06 + i) = v; }      /* Sn_DHAR */
    for (i = 0; i < 4; i++) { v = grp[6 + i]; W(S_DIPR(s) + i) = v; }
    wr16(S_DPORT(s), port);
    wr16(S_PORT(s), port);
    cmd(s, CR_OPEN);
}

void w5100_addr(const uint8_t *ip, const uint8_t *netmask, const uint8_t *gateway)
{
    uint8_t i, v;
    for (i = 0; i < 4; i++) {
        v = ip[i];      W(WR_SIPR + i) = v;
        v = gateway[i]; W(WR_GAR + i) = v;
        v = netmask[i]; W(WR_SUBR + i) = v;
    }
}
