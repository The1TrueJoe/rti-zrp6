#include "board.h"
#include "config.h"
#include "delay.h"
#include "w5100.h"
#include "mqtt.h"

#define CONNECT     0x10
#define CONNACK     0x20
#define PUBLISH     0x30
#define SUBSCRIBE   0x82
#define SUBACK      0x90
#define PINGREQ     0xC0
#define PINGRESP    0xD0

#define KEEPALIVE   60          /* seconds. Generously long: a SIGNAL IN
                                 * capture blocks for tens of milliseconds and
                                 * must never look like a dead client. */

static mqtt_cb on_msg;
static __xdata uint8_t buf[320];
static __xdata uint8_t rx[320];
static __data uint8_t state;            /* 0 down, 1 connecting, 2 up */
static __data uint16_t since_ping;
static __data uint16_t backoff;
static __data uint16_t packet_id;

static uint8_t topic_len(void)
{
    const struct cfg *c = cfg_get();
    uint8_t n = 0;
    while (n < sizeof(c->topic) && c->topic[n]) n++;
    return n;
}

/* The remaining-length field is a base-128 varint, low 7 bits first, top bit
 * meaning "another byte follows". */
static uint8_t put_len(uint8_t *p, uint16_t n)
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

static uint16_t put_str(uint8_t *p, const char *s, uint8_t n)
{
    uint8_t i;
    p[0] = 0; p[1] = n;
    for (i = 0; i < n; i++) p[2 + i] = (uint8_t)s[i];
    return n + 2;
}

static uint8_t slen(const char *s)
{
    uint8_t n = 0;
    while (s[n]) n++;
    return n;
}

/* base + '/' + topic, built in place */
static uint16_t put_full_topic(uint8_t *p, const char *topic)
{
    const struct cfg *c = cfg_get();
    uint8_t bl = topic_len(), tl = slen(topic), i;
    uint16_t n = bl + 1 + tl;
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
    for (i = 0; i < bl; i++) p[2 + i] = (uint8_t)c->topic[i];
    p[2 + bl] = '/';
    for (i = 0; i < tl; i++) p[3 + bl + i] = (uint8_t)topic[i];
    return n + 2;
}

void mqtt_init(mqtt_cb cb)
{
    on_msg = cb;
    state = 0;
    backoff = 0;
    packet_id = 1;
}

uint8_t mqtt_connected(void) { return state == 2; }

static uint8_t send_connect(void)
{
    const struct cfg *c = cfg_get();
    uint16_t n = 0;
    uint8_t hdr[4], hl;
    uint8_t bl = topic_len();
    /* client id: the base topic plus the last two MAC octets, so two boxes on
     * one broker never collide even if both are unprovisioned. */
    char id[24];
    uint8_t i, il = 0;
    for (i = 0; i < bl; i++) id[il++] = c->topic[i];
    id[il++] = '-';
    for (i = 4; i < 6; i++) {
        uint8_t v = c->mac[i];
        id[il++] = "0123456789abcdef"[v >> 4];
        id[il++] = "0123456789abcdef"[v & 15];
    }

    n += put_str(buf + 10, "MQTT", 4);
    buf[10 + n++] = 0x04;               /* protocol level 3.1.1 */
    buf[10 + n++] = 0x02;               /* clean session, no will */
    buf[10 + n++] = 0x00;
    buf[10 + n++] = KEEPALIVE;
    n += put_str(buf + 10 + n, id, il);

    hl = put_len(hdr + 1, n);
    hdr[0] = CONNECT;
    for (i = 0; i < hl; i++) buf[10 - hl + i] = hdr[1 + i];
    buf[10 - hl - 1] = CONNECT;
    return tcp_send(SOCK_BRIDGE, buf + 10 - hl - 1, n + hl + 1);
}

void mqtt_pub(const char *topic, const uint8_t *payload, uint16_t len,
              uint8_t retain)
{
    uint16_t n, i;
    uint8_t hdr[4], hl;
    if (state != 2) return;
    n = put_full_topic(buf + 8, topic);
    for (i = 0; i < len && n + i < sizeof(buf) - 10; i++)
        buf[8 + n + i] = payload[i];
    n += i;
    hl = put_len(hdr, n);
    for (i = 0; i < hl; i++) buf[8 - hl + i] = hdr[i];
    buf[8 - hl - 1] = (uint8_t)(PUBLISH | (retain ? 1 : 0));
    tcp_send(SOCK_BRIDGE, buf + 8 - hl - 1, n + hl + 1);
}

void mqtt_pub_str(const char *topic, const char *s, uint8_t retain)
{
    mqtt_pub(topic, (const uint8_t *)s, slen(s), retain);
}

void mqtt_sub(const char *filter)
{
    uint16_t n, i;
    uint8_t hdr[4], hl;
    if (state != 2) return;
    buf[8] = (uint8_t)(packet_id >> 8);
    buf[9] = (uint8_t)packet_id;
    packet_id++;
    n = 2 + put_full_topic(buf + 10, filter);
    buf[8 + n++] = 0;                   /* QoS 0 */
    hl = put_len(hdr, n);
    for (i = 0; i < hl; i++) buf[8 - hl + i] = hdr[i];
    buf[8 - hl - 1] = SUBSCRIBE;
    tcp_send(SOCK_BRIDGE, buf + 8 - hl - 1, n + hl + 1);
}

static void handle_publish(const uint8_t *p, uint16_t n)
{
    uint16_t tl, i;
    uint8_t bl = topic_len(), skip;
    char topic[64];
    if (n < 2) return;
    tl = ((uint16_t)p[0] << 8) | p[1];
    if (tl + 2 > n) return;
    /* Strip the base topic and its slash, so handlers match on the tail. */
    skip = (tl > bl && p[2 + bl] == '/') ? (uint8_t)(bl + 1) : 0;
    for (i = 0; i + skip < tl && i < sizeof(topic) - 1; i++)
        topic[i] = (char)p[2 + skip + i];
    topic[i] = 0;
    if (on_msg)
        on_msg(topic, p + 2 + tl, (uint16_t)(n - 2 - tl));
}

uint8_t mqtt_poll(void)
{
    const struct cfg *c = cfg_get();
    uint16_t n;

    if (state != 2) {
        if (backoff) { backoff--; return 0; }
        if (sock_status(SOCK_BRIDGE) != SOCK_ESTAB) {
            if (tcp_connect(SOCK_BRIDGE, c->broker, c->broker_port)) {
                backoff = 2000;         /* ~ a couple of seconds of main loop */
                return 0;
            }
        }
        if (send_connect()) { sock_close(SOCK_BRIDGE); backoff = 2000; return 0; }
        state = 1;
        return 0;
    }

    n = tcp_recv(SOCK_BRIDGE, rx, sizeof(rx));
    if (n) {
        uint8_t type = rx[0] & 0xF0;
        uint16_t i = 1, rem = 0, mul = 1;
        do { rem += (rx[i] & 0x7F) * mul; mul *= 128; } while (rx[i++] & 0x80);
        if (type == PUBLISH) handle_publish(rx + i, rem);
    }
    if (sock_status(SOCK_BRIDGE) != SOCK_ESTAB) {
        state = 0;
        sock_close(SOCK_BRIDGE);
        backoff = 1000;
        return 0;
    }
    if (++since_ping > 30000) {
        uint8_t ping[2] = { PINGREQ, 0 };
        since_ping = 0;
        tcp_send(SOCK_BRIDGE, ping, 2);
    }
    return 1;
}

/* Called by the app once CONNACK has been seen. Kept separate so the app
 * decides what to subscribe to and what to announce. */
void mqtt_established(void);
