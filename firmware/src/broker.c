#include "board.h"
#include "bus.h"
#include "io.h"
#include "config.h"
#include "w5100.h"
#include "ws.h"
#include "broker.h"
#include "ota_app.h"
#include "users.h"

#define CONNECT    0x10
#define CONNACK    0x20
#define PUBLISH    0x30
#define PUBACK     0x40
#define SUBSCRIBE  0x80
#define SUBACK     0x90
#define UNSUBSCRIBE 0xA0
#define UNSUBACK   0xB0
#define PINGREQ    0xC0
#define PINGRESP   0xD0
#define DISCONNECT 0xE0

/* 1.5x a 60-second keepalive. MQTT says a server SHOULD
 * hang up on a client that has gone quiet for that long, and on a box with
 * three client sockets it is not optional: a client killed without sending a
 * FIN leaves its socket ESTABLISHED for ever, and the third one to do that
 * takes the box off the network. */
#define BR_IDLE_TICKS TICKS_S(90)

struct client {
    uint8_t  used;
    uint16_t seen;                      /* ticks() when it last said anything */
    uint8_t  subs;                      /* how many filters are live */
    char     filter[BR_SUBS][BR_TOPIC];
};

struct retained {
    uint8_t  used;
    uint8_t  len;
    char     topic[BR_TOPIC];
    uint8_t  payload[BR_PAYLOAD];
};

static __xdata struct client cl[BR_CLIENTS];
static __xdata struct retained ret[BR_RETAINED];
/* 768 because this buffer also holds a browser's request headers, and a
 * Chrome GET with a full cookie jar runs well past 512 bytes. */
static __xdata uint8_t rx[768];
static __xdata uint8_t tx[560];   /* a listed rule can be ~480 bytes */
/* Topic buffers live here rather than on the stack: the 8051 has 256 bytes of
 * IRAM for everything including it, and a 28-byte local is a real risk. */
static __xdata char scratch[BR_TOPIC];
static broker_cb on_cmd;

/* After a failed login every login is refused for a while, so a password
 * cannot be guessed at network speed. */
#define LOCKOUT TICKS_S(2)
static __xdata uint16_t failed_at;
static __xdata uint8_t  failed;


static void scopy(char *d, const char *s, uint8_t max)
{
    uint8_t i = 0;
    while (i < max - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}


/* Only a trailing '#' is supported, which is what every real subscriber uses
 * ("zrp6/#"). A full wildcard matcher would cost more than it earns here. */
static uint8_t matches(const char *filter, const char *topic)
{
    uint8_t i = 0;
    while (filter[i] && filter[i] != '#') {
        if (filter[i] != topic[i]) return 0;
        i++;
    }
    if (filter[i] == '#') return 1;
    return topic[i] == 0;
}

void broker_dropall(void)
{
    uint8_t i;
    for (i = 0; i < BR_CLIENTS; i++) {
        cl[i].used = 0;
        cl[i].subs = 0;
        ws_listen(i);
    }
}

void broker_init(void)
{
    uint8_t i;
    for (i = 0; i < BR_RETAINED; i++) ret[i].used = 0;
    broker_dropall();
}

void broker_on_command(broker_cb cb) { on_cmd = cb; }

uint8_t broker_clients(void)
{
    uint8_t i, n = 0;
    for (i = 0; i < BR_CLIENTS; i++) if (cl[i].used) n++;
    return n;
}

/* ---- outgoing ----------------------------------------------------------- */

static uint16_t put_len(uint8_t *p, uint16_t n)
{
    uint8_t i = 0;
    do {
        uint8_t d = n % 128;
        n /= 128;
        if (n) d |= 0x80;
        p[i++] = d;
    } while (n);
    return i;
}

static void send_publish(uint8_t s, const char *topic,
                         const uint8_t *payload, uint16_t len, uint8_t retain)
{
    uint8_t tl = str_len(topic), hl;
    uint16_t n = 2 + tl + len, i;
    if (n + 5 > sizeof(tx)) return;
    hl = (uint8_t)put_len(tx + 1, n);
    tx[0] = (uint8_t)(PUBLISH | (retain ? 1 : 0));
    tx[1 + hl] = 0;
    tx[2 + hl] = tl;
    for (i = 0; i < tl; i++) tx[3 + hl + i] = (uint8_t)topic[i];
    for (i = 0; i < len; i++) tx[3 + hl + tl + i] = payload[i];
    ws_send(s, tx, (uint16_t)(1 + hl + n));
}

static void store_retained(const char *topic, const uint8_t *payload,
                           uint16_t len)
{
    uint8_t i, free_slot = 0xFF;
    for (i = 0; i < BR_RETAINED; i++) {
        if (ret[i].used && str_eq(ret[i].topic, topic)) break;
        if (!ret[i].used && free_slot == 0xFF) free_slot = i;
    }
    if (i == BR_RETAINED) {
        if (free_slot == 0xFF) return;  /* table full: newest loses, rather
                                         * than evicting something a UI needs */
        i = free_slot;
    }
    if (len == 0) { ret[i].used = 0; return; }   /* empty payload clears it */
    ret[i].used = 1;
    scopy(ret[i].topic, topic, BR_TOPIC);
    ret[i].len = (uint8_t)(len > BR_PAYLOAD ? BR_PAYLOAD : len);
    for (uint8_t k = 0; k < ret[i].len; k++) ret[i].payload[k] = payload[k];
}

static void fanout(const char *topic, const uint8_t *payload, uint16_t len,
                   uint8_t retain, uint8_t from)
{
    uint8_t c, f;
    for (c = 0; c < BR_CLIENTS; c++) {
        if (!cl[c].used || c == from) continue;
        for (f = 0; f < cl[c].subs; f++) {
            if (matches(cl[c].filter[f], topic)) {
                send_publish(c, topic, payload, len, retain);
                break;                  /* one copy per client, not per filter */
            }
        }
    }
}

void broker_publish(const char *topic, const uint8_t *payload, uint16_t len,
                    uint8_t retain)
{
    if (retain) store_retained(topic, payload, len);
    fanout(topic, payload, len, retain, 0xFF);
}

void broker_publish_str(const char *topic, const char *s, uint8_t retain)
{
    broker_publish(topic, (const uint8_t *)s, str_len(s), retain);
}

/* ---- incoming ----------------------------------------------------------- */

static void replay_retained(uint8_t c, const char *filter)
{
    uint8_t i;
    for (i = 0; i < BR_RETAINED; i++)
        if (ret[i].used && matches(filter, ret[i].topic))
            send_publish(c, ret[i].topic, ret[i].payload, ret[i].len, 1);
}

/* CONNECT, which must carry a user and password that users_check() knows.
 * Anything else gets return code 5, "not authorized", and the door. Walked
 * with statics and an __xdata pointer: the generic-pointer version was 1.4 KB.
 * Each string field is a 2-byte length and its bytes; `o` lands past it. */
static __xdata uint16_t o, fl_len;
static __xdata uint8_t  fl;

static uint8_t skip_field(__xdata const uint8_t *p, uint16_t rem)
{
    if (o + 2 > rem) return 0;
    fl_len = ((uint16_t)p[o] << 8) | p[o + 1];
    o += 2 + fl_len;
    return o <= rem;
}

static void connect(uint8_t c, __xdata const uint8_t *p, uint16_t rem)
{
    static __code const uint8_t ok[4] = { CONNACK, 2, 0, 0 };
    static __code const uint8_t no[4] = { CONNACK, 2, 0, 5 };
    static __xdata uint16_t u_at, u_len;
    static __xdata uint8_t cid0;
    uint8_t good = 0;

    o = 0;
    if (!skip_field(p, rem) || o + 4 > rem) goto done;             /* "MQTT" */
    fl = p[o + 1];                              /* level, flags, keepalive */
    o += 4;
    if (!skip_field(p, rem)) goto done;                            /* client id */
    cid0 = fl_len ? p[o - fl_len] : 0;
    if ((fl & 0x04) && (!skip_field(p, rem) || !skip_field(p, rem))) goto done;
    if ((fl & 0xC0) != 0xC0 || !skip_field(p, rem)) goto done;     /* user */
    u_at = o - fl_len;
    u_len = fl_len;
    if (!skip_field(p, rem)) goto done;                            /* password */
    if (failed && (uint16_t)(ticks() - failed_at) < LOCKOUT) goto done;
    good = users_take(p + u_at, u_len, p + o - fl_len, fl_len) && users_check();
done:
    if (!good) {
        failed = 1;
        failed_at = ticks();
        ws_send(c, no, 4);
        ws_close(c);
        return;
    }
    cl[c].used = 1;
    cl[c].subs = 0;
    cl[c].seen = ticks();
    ws_client(c, cid0 == '!');
    ws_send(c, ok, 4);
    ota_client_connected();
}

/* <base>/users/... carries passwords: acted on, never passed on or kept. */
static uint8_t private_topic(const char *t)
{
    static __code const char u[] = "/users/";
    const char *b = cfg_get()->topic;
    uint8_t i = 0, j;
    while (b[i] && i < 16) { if (t[i] != b[i]) return 0; i++; }
    for (j = 0; u[j]; j++) if (t[i + j] != u[j]) return 0;
    return 1;
}

static void handle(uint8_t c, __xdata uint8_t *p, uint16_t n)
{
    uint8_t type = p[0] & 0xF0;
    uint8_t flags = p[0] & 0x0F;
    uint16_t i = 1, rem = 0, mul = 1;
    do { rem += (p[i] & 0x7F) * mul; mul *= 128; } while (p[i++] & 0x80);
    if (i + rem > n) return;
    p += i;

    /* Nothing but CONNECT from a client that has not logged in. */
    if (type != CONNECT && !cl[c].used) {
        ws_close(c);
        return;
    }

    switch (type) {
    case CONNECT:
        if (!cl[c].used) connect(c, p, rem);
        break;
    case SUBSCRIBE: {
        uint16_t off = 2;               /* packet id */
        uint8_t g = 0;
        while (off + 3 <= rem && g < BR_SUBS) {
            uint16_t tl = ((uint16_t)p[off] << 8) | p[off + 1];
            uint8_t k;
            for (k = 0; k < tl && k < BR_TOPIC - 1; k++) scratch[k] = (char)p[off + 2 + k];
            scratch[k] = 0;
            if (cl[c].subs < BR_SUBS) {
                scopy(cl[c].filter[cl[c].subs], scratch, BR_TOPIC);
                cl[c].subs++;
            }
            g++;                        /* every grant is QoS 0 */
            off += 2 + tl + 1;
        }
        tx[0] = SUBACK; tx[1] = (uint8_t)(2 + g);
        tx[2] = p[0]; tx[3] = p[1];
        for (i = 0; i < g; i++) tx[4 + i] = 0;
        ws_send(c, tx, (uint16_t)(4 + g));
        /* A subscriber must see the world as it is, not wait for the next
         * change, or a UI opens blank. */
        for (i = 0; i < cl[c].subs; i++) replay_retained(c, cl[c].filter[i]);
        break;
    }
    case PUBLISH: {
        uint16_t tl = ((uint16_t)p[0] << 8) | p[1];
        uint8_t k;
        uint16_t plen;
        if (tl + 2 > rem) break;
        for (k = 0; k < tl && k < BR_TOPIC - 1; k++) scratch[k] = (char)p[2 + k];
        scratch[k] = 0;
        plen = (uint16_t)(rem - 2 - tl);
        if (!private_topic(scratch)) {
            if (flags & 0x01) store_retained(scratch, p + 2 + tl, plen);
            fanout(scratch, p + 2 + tl, plen, 0, c);
        }
        if (on_cmd) on_cmd(scratch, p + 2 + tl, plen);
        break;
    }
    case PINGREQ: {
        static __code const uint8_t r[2] = { PINGRESP, 0 };
        ws_send(c, r, 2);
        break;
    }
    case DISCONNECT:
        cl[c].used = 0;
        ws_close(c);
        break;
    default:
        break;
    }
}

void broker_poll(void)
{
    uint8_t c;
    for (c = 0; c < BR_CLIENTS; c++) {
        uint16_t n;
        if (cl[c].used && (uint16_t)(ticks() - cl[c].seen) > BR_IDLE_TICKS) {
            cl[c].used = 0;
            cl[c].subs = 0;
            ws_close(c);                /* ws_poll re-listens once it has closed */
            continue;
        }
        n = ws_poll(c, rx, sizeof(rx));
        if (n == WS_GONE) {
            /* ws.c owns the socket from here: it re-listens once the close
             * has finished. Listening here aborted it (tcp_disconnect). */
            cl[c].used = 0;
            cl[c].subs = 0;
        } else if (n) {
            cl[c].seen = ticks();
            /* A client is free to put several packets in one frame, and
             * mqtt.js does exactly that when publishes are queued. Walk the
             * buffer rather than acting on the first one and dropping the
             * rest. */
            uint16_t off = 0;
            while (off + 2 <= n) {
                uint16_t rem = 0, mul = 1, i = off + 1;
                do { rem += (uint16_t)(rx[i] & 0x7F) * mul; mul *= 128; }
                while (rx[i++] & 0x80);
                if (i + rem > n) break;
                handle(c, rx + off, (uint16_t)(i - off + rem));
                off = (uint16_t)(i + rem);
            }
        }
    }
    ws_balance();
}
