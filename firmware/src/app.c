#include "board.h"
#include "config.h"
#include "io.h"
#include "ir.h"
#include "uart.h"
#include "capture.h"
#include "rti.h"
#include "rules.h"
#include "broker.h"
#include "app.h"
#include "http.h"
#include "bus.h"
#include "w5100.h"
#include "ota_app.h"
#include "net.h"
#include "users.h"
#include "../boot/usb.h"

static void publish_firmware(void);
static void publish_ir_mode(uint8_t port);
static void publish_serial(uint8_t ch);

#define SIG_RUNS 400            /* see rti.h for why 400 */

static __xdata char     tbuf[40];
static __xdata uint8_t  pbuf[24];
static __xdata uint16_t runs[SIG_RUNS];
static __xdata uint8_t  last_contacts;
static __xdata uint8_t  serial_on;      /* bit per channel */

/* Acknowledgements are queued, not published where they arise.
 *
 * A command arrives several calls deep — broker_poll, handle, the command
 * callback, the handler — and publishing its /state from there adds the
 * entire publish path (fanout, send, the W5100 ring) on top. With 175 bytes of
 * stack for everything, that was one frame too many: an IR command, the
 * deepest of them, rebooted the box, and it looked like an IR bug. Handlers
 * now touch the hardware and queue the answer; app_poll() publishes it from
 * the top of the main loop, in order, a moment later. */
#define PQ_LEN 4
static __xdata struct {
    uint8_t retain, len;
    char    topic[40];
    uint8_t payload[24];
} pq[PQ_LEN];
static __xdata uint8_t pq_head, pq_count;

static void enqueue(const uint8_t *p, uint8_t len, uint8_t retain)
{
    uint8_t slot, i;
    if (pq_count == PQ_LEN) return;     /* four commands in one poll: the
                                         * state topics still end up right,
                                         * because the latest one wins */
    slot = (uint8_t)((pq_head + pq_count) % PQ_LEN);
    for (i = 0; i < sizeof(pq[0].topic) - 1 && tbuf[i]; i++) pq[slot].topic[i] = tbuf[i];
    pq[slot].topic[i] = 0;
    if (len > sizeof(pq[0].payload)) len = sizeof(pq[0].payload);
    for (i = 0; i < len; i++) pq[slot].payload[i] = p[i];
    pq[slot].len = len;
    pq[slot].retain = retain;
    pq_count++;
}

/* ---- deferred rule work ---------------------------------------------------
 * Saving a rule erases and rewrites a Flash sector (a second or two), firing
 * one runs an IR burst, and listing them publishes up to fifteen long
 * messages. None of that belongs several calls deep inside the MQTT packet
 * handler, where the stack is nearly spent. The handler records what was
 * asked; app_poll() does it from the top of the loop. */
static __xdata uint8_t  rec[STORE_RULE_SIZE];
static __xdata uint16_t rec_len;
static __xdata uint8_t  save_slot;          /* slot + 1, or 0 for none */
static __xdata uint8_t  list_pending;
static __xdata char     fire_code[RTI_CODE_MAX];
static __xdata uint8_t  fire_pending;
static __xdata char     rtail[RULE_TOPIC_MAX + 1];
static __xdata char     rfull[RULE_TOPIC_MAX + 18];
static __xdata uint8_t  rpay[RULE_PAYLOAD_MAX + 1];

static void rules_fire(const char *code);

static void flush_queue(void)
{
    while (pq_count) {
        broker_publish(pq[pq_head].topic, pq[pq_head].payload,
                       pq[pq_head].len, pq[pq_head].retain);
        pq_head = (uint8_t)((pq_head + 1) % PQ_LEN);
        pq_count--;
    }
}

/* ---- small text helpers -------------------------------------------------
 * <string.h> was measured against these and came out 200 bytes larger for the
 * three functions this firmware uses, because SDCC's versions are generic
 * across storage classes. See ../README.md. */


static uint8_t topic(const char *tail)
{
    const char *b = cfg_get()->topic;
    uint8_t n = 0;
    while (b[n] && n < 16) { tbuf[n] = b[n]; n++; }
    tbuf[n++] = '/';
    while (*tail) tbuf[n++] = *tail++;
    tbuf[n] = 0;
    return n;
}

/* Builds "<base>/<head>/<n>/<tail>" — every per-channel topic has this
 * shape. n is 1-99; an empty tail leaves off the last slash. */
static void topic_n(const char *head, uint8_t n, const char *tail)
{
    uint8_t i = topic(head);
    tbuf[i++] = '/';
    if (n >= 10) tbuf[i++] = (char)('0' + n / 10);
    tbuf[i++] = (char)('0' + n % 10);
    if (*tail) { tbuf[i++] = '/'; while (*tail) tbuf[i++] = *tail++; }
    tbuf[i] = 0;
}

/* Skips the base topic and its slash. Returns 0 if the topic is not ours. */
static const char *strip(const char *t)
{
    const char *b = cfg_get()->topic;
    uint8_t i = 0;
    while (b[i] && i < 16) { if (t[i] != b[i]) return 0; i++; }
    return t[i] == '/' ? t + i + 1 : 0;
}


static uint8_t is_on(const uint8_t *p, uint16_t n)
{
    /* "ON", "on", "1", "true" — a UI, a shell and a home controller each write
     * this differently and none of them is wrong. */
    if (!n) return 0;
    if (p[0] == '1' || p[0] == 'o' || p[0] == 'O' || p[0] == 't' || p[0] == 'T')
        return !(n > 1 && (p[1] == 'f' || p[1] == 'F'));
    return 0;
}

/* Decimal, or hex when it starts 0x. Advances *pp past the number. */
static uint16_t num(const char **pp)
{
    const char *p = *pp;
    uint16_t v = 0;
    while (*p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        for (;;) {
            uint8_t c = (uint8_t)*p, d;
            if (c >= '0' && c <= '9')      d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else break;
            v = (uint16_t)(v * 16 + d);
            p++;
        }
    } else {
        while (*p >= '0' && *p <= '9') { v = (uint16_t)(v * 10 + (*p - '0')); p++; }
    }
    *pp = p;
    return v;
}

/* ---- outputs ------------------------------------------------------------ */

static void ack_relay(uint8_t n, uint8_t on)
{
    relay_set(n, on);
    topic_n("relay", n, "state");
    enqueue((const uint8_t *)(on ? "ON" : "OFF"), on ? 2 : 3, 1);
}

static void ack_trigger(uint8_t n, uint8_t on)
{
    trigger_set(n, on);
    topic_n("trigger", n, "state");
    enqueue((const uint8_t *)(on ? "ON" : "OFF"), on ? 2 : 3, 1);
}

/* ---- infrared ----------------------------------------------------------- */

/* NEC: 9 ms mark, 4.5 ms space, then 32 bits as 560 us marks with 560 us or
 * 1690 us spaces, expressed here in 38 kHz carrier periods. */
static void ir_nec(uint8_t port, uint8_t addr, uint8_t cmd, uint8_t repeats)
{
    uint8_t r, b;
    static __xdata uint32_t frame;    /* off the stack — README rule 4 */
    frame = (uint32_t)addr | ((uint32_t)(uint8_t)~addr << 8)
                   | ((uint32_t)cmd << 16) | ((uint32_t)(uint8_t)~cmd << 24);
    for (r = 0; r <= repeats; r++) {
        (void)ticks();                  /* 255 repeats is 27 s: the watchdog */
        ir_begin(port, 30);
        ir_mark(342);                   /* 9.0 ms  */
        ir_space(171);                  /* 4.5 ms  */
        for (b = 0; b < 32; b++) {
            ir_mark(21);                /* 560 us  */
            ir_space((frame & 1) ? 64 : 21);
            frame >>= 1;
        }
        ir_mark(21);                    /* stop bit */
        ir_end();
    }
}

/* Pronto: "0000 <freq> <once> <repeat>" then mark/space pairs, already counted
 * in carrier periods — which is exactly what ir_mark takes, so the numbers go
 * through untouched. Only the carrier has to be converted. */
static void ir_pronto(uint8_t port, const char *p)
{
    uint16_t freq, once, rpt, i;
    uint8_t half;
    (void)num(&p);                      /* leading 0000 */
    freq = num(&p);
    once = num(&p);
    rpt  = num(&p);
    if (!freq) return;
    /* half=30 measured at 38 kHz, which is Pronto code 109. The dly loop has
     * fixed overhead so this is proportional rather than exact; anything from
     * 36 to 40 kHz works on a real receiver either way. */
    half = (uint8_t)(((uint32_t)freq * 30) / 109);
    if (half < 8) half = 8;
    ir_begin(port, half);
    for (i = 0; i < (uint16_t)((once ? once : rpt) * 2); i++) {
        uint16_t v = num(&p);
        if (!*p && !v) break;
        if (i & 1) { ir_space(v); (void)ticks(); } else ir_mark(v);
    }
    ir_end();
}

static void do_ir(uint8_t port, const uint8_t *payload, uint16_t len)
{
    const char *p = (const char *)payload;
    if (len < 3 || port < 1 || port > 7) return;
    if (cfg_get()->ir_mode[port - 1] & IRM_BAUD) return;     /* a serial port */
    if (p[0] == 'n' && p[1] == 'e' && p[2] == 'c') {
        uint8_t a, c, r;
        p += 3;
        a = (uint8_t)num(&p);
        c = (uint8_t)num(&p);
        r = (uint8_t)num(&p);
        ir_nec(port, a, c, r);
    } else {
        ir_pronto(port, p);
    }
    topic_n("ir", port, "sent");
    enqueue(payload, (uint8_t)(len > 24 ? 24 : len), 0);
}

/* ---- serial ------------------------------------------------------------- */

/* Channels are opt-in, one at a time, and that is deliberate — including
 * port 1, which used to start at boot.
 *
 * An RS-232 port with nothing plugged into it floats. Most of what that
 * produces is a break condition, which uart_read() discards — but this board's
 * channel 1 also raises data-ready with no error flags at all, which is the
 * fake-receive quirk recorded in docs/REVERSE-ENGINEERING.md. Polling it
 * published a NUL to serial/2/rx several times a second for no reason.
 *
 * So a port starts the first time anything is published to its
 * serial/<n>/tx. 9600 8N1; a /serial/<n>/config topic is the obvious next
 * addition. */
/* The 16C554 runs from 14.7456 MHz: divisor = 921600 / baud, which is a
 * whole number for every rate in the table. */
static void serial_apply(uint8_t ch)
{
    uint8_t s = cfg_get()->ser[ch], b = (uint8_t)(s & 0x0F), f = (uint8_t)(s >> 4);
    uint16_t div;
    if (!b || b > BAUD_COUNT) b = BAUD_9600;
    if (f >= SER_FORMATS) f = 0;
    div = (uint16_t)(921600ul / baud_rate[b]);
    uart_config(ch, (uint8_t)div, (uint8_t)(div >> 8), ser_lcr[f]);
}

static void serial_start(uint8_t ch)
{
    if (serial_on & (1 << ch)) return;
    serial_apply(ch);
    serial_on |= (uint8_t)(1 << ch);
}

/* Received bytes are held and published as one message per reply: on a
 * newline, when 128 bytes have built up, or after ~25 ms of silence — at 9600
 * baud a byte arrives every millisecond, so that much quiet means the device
 * has finished. Publishing each UART read as it came split one reply into
 * several messages at arbitrary points. */
#define SER_BUF 128
static __xdata uint8_t  ser_buf[2][SER_BUF];
static __xdata uint8_t  ser_n[2];
static __xdata uint16_t ser_at[2];

static void serial_flush(uint8_t ch)
{
    if (!ser_n[ch]) return;
    topic_n("serial", (uint8_t)(ch + 1), "rx");
    broker_publish(tbuf, ser_buf[ch], ser_n[ch], 0);
    ser_n[ch] = 0;
}

static void serial_pump(uint8_t ch)
{
    uint8_t n = uart_read(ch, pbuf, sizeof(pbuf)), i;
    for (i = 0; i < n; i++) {
        ser_buf[ch][ser_n[ch]++] = pbuf[i];
        if (ser_n[ch] == SER_BUF || pbuf[i] == '\n') serial_flush(ch);
    }
    if (n) ser_at[ch] = ticks();
}

/* ---- the RTI remote ----------------------------------------------------- */

static __xdata char     rcode[RTI_CODE_MAX];
static __xdata char     last_code[RTI_CODE_MAX];
static __xdata uint16_t last_at;

static uint8_t same_as_last(void)
{
    uint8_t i;
    for (i = 0; i < RTI_CODE_MAX; i++) {
        if (rcode[i] != last_code[i]) return 0;
        if (!rcode[i]) return 1;
    }
    return 1;
}

static void remote_poll(void)
{
    uint16_t n;
    uint8_t start, len, i;

    /* The line idles LOW. Enter the capture only once it is already high, or
     * the blocking wait inside it would stall the whole box on every pass. */
    P3 |= SIGIN_MASK;
    if (!(P3 & SIGIN_MASK)) return;
    n = sigin_capture(runs, SIG_RUNS, &start);
    len = rti_decode(runs, n, (uint8_t)(n < SIG_RUNS), rcode);
    if (!len) return;

    /* A held button repeats its frame for as long as it is held. Report the
     * press once, and again only after ~0.6 s without that code. */
    if (same_as_last() && (uint16_t)(ticks() - last_at) < TICKS_MS(600)) {
        last_at = ticks();
        return;
    }
    for (i = 0; i < RTI_CODE_MAX; i++) last_code[i] = rcode[i];
    last_at = ticks();
    topic("remote/code");
    broker_publish(tbuf, (const uint8_t *)rcode, len, 0);
    rules_fire(rcode);
}

/* Runs the decoder once over a dummy capture at boot. It finds nothing — the
 * pattern has no RTI preamble — but it walks the decoder's deepest path, so
 * the stack gauge (main.c) has measured it before a real remote ever needs
 * it. Stack exhaustion on this part reboots the box rather than crashing
 * visibly, so the margin has to be known in advance, not discovered. */
static void remote_selftest(void)
{
    uint16_t i;
    for (i = 0; i < SIG_RUNS; i++) runs[i] = (uint16_t)((i % 9) ? 60 : 120);
    (void)rti_decode(runs, SIG_RUNS, 1, rcode);
}

/* ---- contacts ----------------------------------------------------------- */

static void contacts_poll(void)
{
    uint8_t c = contacts(), i;
    if (c == last_contacts) return;
    for (i = 0; i < 3; i++) {
        if (((c ^ last_contacts) >> i) & 1) {
            topic_n("sense", (uint8_t)(i + 1), "state");
            broker_publish_str(tbuf, ((c >> i) & 1) ? "OPEN" : "CLOSED", 1);
        }
    }
    last_contacts = c;
}

/* ---- settings ----------------------------------------------------------- */
/* Commands that change something kept in Flash only change the copy in RAM
 * here; the save is a second or two of Flash work and happens from the top
 * of the loop (settings_work).
 *
 * Text is parsed from one buffer with one cursor, and answers are built in
 * one buffer: the version that passed pointers around was 3 KB larger, which
 * under SDCC's large model is what generic pointers cost. */
static __xdata uint8_t  cfg_dirty;          /* bit 0: save; bit 1: and restart */
static __xdata uint8_t  users_pending;      /* 1: save rec, 2: list, 3: just answer */
static __xdata uint8_t  users_result;
/* Mode and settings answers to publish from the top of the loop: bit n-1 is
 * IR port n's mode, bits 7 and 8 serial ports 1 and 2. Publishing them from
 * inside the command handler took the stack to 11 bytes free (measured). */
static __xdata uint16_t settings_pub;

static __xdata char     arg[48];
static __xdata uint8_t  ap;                 /* the cursor in arg */
static __xdata char     ob[USERS_MAX * (USER_NAME + 1) + 1];
static __xdata uint8_t  on;                 /* bytes in ob */

static void load_arg(const uint8_t *payload, uint16_t len)
{
    uint8_t i;
    for (i = 0; i < sizeof(arg) - 1 && i < len; i++) arg[i] = (char)payload[i];
    arg[i] = 0;
    ap = 0;
}

static void skip_sp(void) { while (arg[ap] == ' ') ap++; }

static uint32_t arg_num(void)
{
    uint32_t v = 0;
    skip_sp();
    while (arg[ap] >= '0' && arg[ap] <= '9') v = v * 10 + (uint8_t)(arg[ap++] - '0');
    return v;
}

/* The next word, case-folded, is w: consume it. */
static uint8_t arg_word(__code const char *w)
{
    uint8_t i = 0;
    skip_sp();
    while (w[i]) {
        if ((arg[ap + i] | 0x20) != (w[i] | 0x20)) return 0;
        i++;
    }
    if (arg[ap + i] && arg[ap + i] != ' ') return 0;
    ap += i;
    return 1;
}

static uint8_t arg_baud(void)
{
    uint32_t v = arg_num();
    uint8_t i;
    for (i = 1; i <= BAUD_COUNT; i++) if (baud_rate[i] == v) return i;
    return 0;
}

/* "10.0.0.77" into out[0..3]. */
static uint8_t arg_quad(__xdata uint8_t *out)
{
    uint32_t v;
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if (i && arg[ap++] != '.') return 0;
        if (arg[ap] < '0' || arg[ap] > '9') return 0;
        v = arg_num();
        if (v > 255) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void ob_c(char c) { ob[on++] = c; }
static void ob_s(__code const char *t) { while (*t) ob[on++] = *t++; }
static void ob_u(uint32_t v)
{
    static __xdata char d[10];
    uint8_t n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) ob[on++] = d[--n];
}
static void ob_pub(uint8_t retain) { broker_publish(tbuf, (const uint8_t *)ob, on, retain); }

static void ir_mode_set(uint8_t port)
{
    uint8_t m;
    if (arg_word("ir")) {
        m = 0;
    } else if (arg_word("serial")) {
        m = arg_baud();
        if (!m) m = BAUD_9600;
        if (arg_word("ttl")) m |= IRM_TTL;
    } else {
        return;
    }
    cfg_edit()->ir_mode[port - 1] = m;
    ir_modes(cfg_get()->ir_mode);
    settings_pub |= (uint16_t)1 << (port - 1);
    cfg_dirty |= 1;
}

static void serial_config_set(uint8_t ch)
{
    uint8_t b = arg_baud(), f;
    if (!b) return;
    for (f = 0; f < SER_FORMATS && !arg_word(ser_fmt[f]); f++)
        ;
    if (f == SER_FORMATS) f = 0;
    cfg_edit()->ser[ch] = (uint8_t)(b | (f << 4));
    if (serial_on & (1 << ch)) serial_apply(ch);
    settings_pub |= (uint16_t)0x80 << ch;
    cfg_dirty |= 1;
}

static void net_set(void)
{
    static __xdata uint8_t q[12];
    __xdata struct cfg *c = cfg_edit();
    uint8_t i;
    if (arg_word("dhcp")) {
        c->net = NET_DHCP;
    } else if (arg_word("static") && (skip_sp(), arg_quad(q)) && (skip_sp(), arg_quad(q + 4))
               && (skip_sp(), arg_quad(q + 8))) {
        c->net = NET_TRIAL;                     /* until a login proves it */
        for (i = 0; i < 4; i++) { c->ip[i] = q[i]; c->netmask[i] = q[4 + i]; c->gateway[i] = q[8 + i]; }
    } else {
        topic("net/state");
        enqueue((const uint8_t *)"invalid", 7, 0);
        return;
    }
    cfg_dirty |= 2;                             /* save, then restart */
}

/* t is "users/<name>/set" */
static void users_set(const char *t, const uint8_t *payload, uint16_t len)
{
    uint8_t n = 0;
    t += 6;
    while (t[n] && t[n] != '/') n++;
    users_pending = 3;                          /* just answer, unless... */
    users_result = US_INVALID;
    if (!str_eq(t + n, "/set") || !users_take((const uint8_t *)t, n, payload, len)) return;
    users_result = users_build(rec, &rec_len);
    if (users_result <= US_DELETED) users_pending = 1;
}

static void settings_work(void)
{
    static __code const char *const said[] = { "saved ", "deleted ", "invalid", "full", "last" };
    uint8_t n;
    for (n = 0; n < 9; n++)
        if (settings_pub & ((uint16_t)1 << n)) {
            if (n < 7) publish_ir_mode((uint8_t)(n + 1)); else publish_serial((uint8_t)(n - 7));
        }
    settings_pub = 0;
    if (cfg_dirty) {
        n = cfg_dirty;
        cfg_dirty = 0;
        topic((n & 2) ? "net/state" : "config/state");
        broker_publish_str(tbuf, cfg_save() ? "error" : ((n & 2) ? "saved, restarting" : "saved"), 0);
        if (n & 2) ota_restart = 1;
    }
    if (!users_pending) return;
    on = 0;
    if (users_pending == 2) {
        on = users_list(ob);
        topic("users/list");
    } else {
        if (users_pending == 1 && store_commit(STORE_USERS_SLOT, rec, rec_len)) {
            ob_s("error");
        } else {
            ob_s(said[users_result]);
            if (users_result <= US_DELETED)
                for (n = 0; n < user_nl; n++) ob_c((char)user_name[n]);
        }
        topic("users/state");
    }
    users_pending = 0;
    ob_pub(0);
}

/* ---- wiring ------------------------------------------------------------- */

/* The topic being handled, past the base: copied here once, because
 * reaching it through the caller's generic pointer at every comparison made
 * this function 2.7 KB. Not re-entered: rules_fire() calls it from the top of
 * the loop, never from inside it. */
static __xdata char ct[BR_TOPIC];

static uint8_t is(uint8_t at, __code const char *w) { return str_eq(ct + at, w); }

/* The channel digit at ct[at], if it is 1..max; else 0. */
static uint8_t chan(uint8_t at, uint8_t max)
{
    uint8_t n = (uint8_t)(ct[at] - '0');
    return (n >= 1 && n <= max) ? n : 0;
}

void app_command(const char *full, const uint8_t *payload,
                 uint16_t len) BANKED __reentrant
{
    const char *t = strip(full);
    uint8_t n, i;

    if (!t) return;
    for (i = 0; i < BR_TOPIC - 1 && t[i]; i++) ct[i] = t[i];
    ct[i] = 0;
    if (i < 7) return;          /* the shortest commands are "ir/1/tx", "net/set" */

    /* Every per-channel topic is "<thing>/<digit>/<verb>", so the digit is at
     * a fixed offset once the thing is matched. */
    if (ct[0] == 'r' && ct[1] == 'e' && ct[2] == 'l') {          /* relay */
        if ((n = chan(6, 3)) && is(8, "set")) ack_relay(n, is_on(payload, len));
    } else if (ct[0] == 't' && ct[1] == 'r') {                   /* trigger */
        if ((n = chan(8, 3)) && is(10, "set")) ack_trigger(n, is_on(payload, len));
    } else if (ct[0] == 'i' && ct[1] == 'r' && ct[2] == '/') {   /* ir */
        if (!(n = chan(3, 7))) return;
        if (is(5, "send")) {
            do_ir(n, payload, len);
        } else if (is(5, "tx")) {
            i = cfg_get()->ir_mode[n - 1];
            if (i & IRM_BAUD) {
                ir_serial(n, i, payload, len);
                topic_n("ir", n, "sent");
                enqueue(payload, (uint8_t)(len > 24 ? 24 : len), 0);
            }
        } else if (is(5, "mode/set")) {
            load_arg(payload, len);
            ir_mode_set(n);
        }
    } else if (ct[0] == 's' && ct[1] == 'e') {                   /* serial */
        if (!(n = chan(7, 2))) return;
        n--;
        if (is(9, "tx")) {
            serial_start(n);
            uart_write(n, payload, (uint8_t)(len > 255 ? 255 : len));
        } else if (is(9, "config/set")) {
            load_arg(payload, len);
            serial_config_set(n);
        }
    } else if (is(0, "users/get")) {
        if (!users_pending) users_pending = 2;
    } else if (ct[0] == 'u' && ct[5] == '/') {                   /* users/<name>/set */
        if (users_pending || save_slot || cfg_dirty) {
            topic("users/state");
            enqueue((const uint8_t *)"busy", 4, 0);
        } else {
            users_set(ct, payload, len);
        }
    } else if (is(0, "net/set")) {
        load_arg(payload, len);
        net_set();
    } else if (is(0, "system/restart")) {
        ota_restart = 1;
    } else if (is(0, "rules/get")) {
        list_pending = 1;
    } else if (ct[0] == 'r' && ct[1] == 'u') {                   /* rules/<n>/set */
        n = (uint8_t)(ct[6] - '0');
        i = 7;
        if (ct[7] >= '0' && ct[7] <= '9') { n = (uint8_t)(n * 10 + ct[7] - '0'); i = 8; }
        if (n < 1 || n > STORE_RULES || !is(i, "/set")) return;
        topic_n("rules", n, "state");
        if (save_slot || users_pending || cfg_dirty) {   /* one save at a time */
            enqueue((const uint8_t *)"busy", 4, 0);
            return;
        }
        /* An empty payload deletes the rule. Anything else must parse. */
        rec_len = len ? rule_parse(payload, len, rec) : 0;
        if (len && !rec_len) {
            enqueue((const uint8_t *)"invalid", 7, 0);
            return;
        }
        save_slot = n;
#ifdef BENCH
    } else if (is(0, "debug/peek")) {
        /* 16 bytes of XDATA from a hex address, read with xrd(). A bench
         * tool: it is how the store's Flash state is looked at from outside. */
        static __xdata uint8_t pk[16];
        uint16_t a = 0;
        for (i = 0; i < len && i < 4; i++) {
            n = payload[i];
            a = (uint16_t)(a << 4) | (uint16_t)(n <= '9' ? n - '0' : (n | 0x20) - 'a' + 10);
        }
        for (i = 0; i < 16; i++) pk[i] = xrd((uint16_t)(a + i));
        topic("debug/peeked");
        broker_publish(tbuf, pk, 16, 0);
    } else if (is(0, "debug/usb")) {
        static __xdata uint8_t ud[13];
        usb_debug(ud);
        topic("debug/usbstate");
        broker_publish(tbuf, ud, 13, 0);
#endif
#ifdef WDT_TEST
    } else if (is(0, "debug/hang")) {
        /* A test build's way to prove the watchdog: hang with the watchdog
         * unfed (hang), or spin on ticks() with no heartbeat (stall). */
        if (len && payload[0] == 's') for (;;) (void)ticks();
        for (;;) ;
#endif
    } else if (is(0, "remote/code")) {
        /* A code published by a client counts as a press: it runs the rules.
         * That is how a rule is tested from the interface, and how a house
         * controller can drive the box's own automations. */
        for (n = 0; n < RTI_CODE_MAX - 1 && n < len; n++) fire_code[n] = (char)payload[n];
        fire_code[n] = 0;
        fire_pending = 1;
    }
}

/* Run every rule for this code, in slot order. */
static void rules_fire(const char *code)
{
    static __xdata uint8_t s, i, b;
    static __xdata uint16_t pl;
    for (s = 0; s < STORE_RULES; s++) {
        if (!rule_matches(s, code)) continue;
        pl = rule_action(s, rtail, rpay);
        b = topic(rtail);                       /* "<base>/<tail>" */
        for (i = 0; i <= b; i++) rfull[i] = tbuf[i];
        app_command(rfull, rpay, pl);
        topic("rules/fired");
        b = (uint8_t)(s + 1);
        rpay[0] = (char)(b >= 10 ? '1' : '0' + b);
        rpay[1] = (char)('0' + b % 10);
        if (b >= 10) enqueue(rpay, 2, 0); else enqueue(rpay, 1, 0);
    }
}

static void rules_work(void)
{
    static __xdata uint8_t s;
    static __xdata uint16_t n;
    if (fire_pending) {
        fire_pending = 0;
        rules_fire(fire_code);
    }
    if (save_slot) {
        s = save_slot;
        n = store_commit((uint8_t)(s - 1), rec, rec_len);
        topic_n("rules", s, "state");
        broker_publish_str(tbuf, n ? "error" : (rec_len ? "saved" : "deleted"), 0);
        save_slot = 0;
    }
    if (list_pending) {
        list_pending = 0;
        for (s = 0; s < STORE_RULES; s++) {
            n = rule_text(s, rec);
            if (!n) continue;
            topic_n("rules", (uint8_t)(s + 1), "");
            broker_publish(tbuf, rec, n, 0);
        }
        topic("rules/listed");
        broker_publish_str(tbuf, "done", 0);
    }
}

void app_init(void) BANKED
{
    uint8_t i;
    broker_on_command(app_command);
    remote_selftest();
    /* No serial port starts at boot — both start on first use. An idle,
     * unconfigured 16C554 is what RTI's box and the stable old image both
     * had, and a configured one reacts to whatever is on the line, which
     * means bus reads, which the front panel shows. */
    last_contacts = contacts();

    ir_modes(cfg_get()->ir_mode);

    topic("status");
    broker_publish_str(tbuf, "online", 1);
    publish_firmware();
    app_net();
    for (i = 1; i <= 7; i++) publish_ir_mode(i);
    publish_serial(0);
    publish_serial(1);
    /* Publish the rest state so a UI that connects first sees the truth rather
     * than three blank switches. */
    for (i = 1; i <= 3; i++) {
        topic_n("relay", i, "state");
        broker_publish_str(tbuf, "OFF", 1);
        topic_n("trigger", i, "state");
        broker_publish_str(tbuf, "OFF", 1);
        topic_n("sense", i, "state");
        broker_publish_str(tbuf, ((last_contacts >> (i - 1)) & 1) ? "OPEN" : "CLOSED", 1);
    }
}

/* ---- settings, published ------------------------------------------------ */

static void publish_ir_mode(uint8_t port)
{
    uint8_t m = cfg_get()->ir_mode[port - 1];
    on = 0;
    if (!(m & IRM_BAUD)) {
        ob_s("ir");
    } else {
        ob_s("serial ");
        ob_u(baud_rate[m & IRM_BAUD]);
        ob_s((m & IRM_TTL) ? " ttl" : " rs232");
    }
    topic_n("ir", port, "mode");
    ob_pub(1);
}

static void publish_serial(uint8_t ch)
{
    uint8_t s = cfg_get()->ser[ch], b = (uint8_t)(s & 0x0F), f = (uint8_t)(s >> 4);
    if (!b || b > BAUD_COUNT) b = BAUD_9600;
    if (f >= SER_FORMATS) f = 0;
    on = 0;
    ob_u(baud_rate[b]);
    ob_c(' ');
    ob_s(ser_fmt[f]);
    topic_n("serial", (uint8_t)(ch + 1), "config");
    ob_pub(1);
}

static void publish_quad(__code const char *t, const uint8_t *a)
{
    uint8_t i;
    on = 0;
    for (i = 0; i < 4; i++) {
        if (i) ob_c('.');
        ob_u(a[i]);
    }
    topic(t);
    ob_pub(1);
}

void app_net(void) BANKED
{
    static __code const char *const names[] = { "dhcp", "static", "fallback" };
    static __code const char hex[] = "0123456789ABCDEF";
    __xdata const uint8_t *mac = cfg_get()->mac;
    uint8_t i;
    topic("net/mode");
    broker_publish_str(tbuf, names[net_mode()], 1);
    publish_quad("net/ip", net_ip());
    publish_quad("net/netmask", net_mask());
    publish_quad("net/gateway", net_gw());
    on = 0;
    for (i = 0; i < 6; i++) {
        if (i) ob_c(':');
        ob_c(hex[mac[i] >> 4]);
        ob_c(hex[mac[i] & 15]);
    }
    topic("net/mac");
    ob_pub(1);
}

/* firmware/version is the build, firmware/state what the bootloader thinks
 * of it — see docs/UPDATING.md. Both retained, for the UI. */
#ifndef BUILD_ID
#define BUILD_ID "unknown"
#endif
static void publish_firmware(void)
{
    static __code const char *const trial[] = {
        "on trial, boot 1 of 3", "on trial, boot 2 of 3", "on trial, boot 3 of 3" };
    uint8_t st = ota_state();
    topic("firmware/version");
    broker_publish_str(tbuf, BUILD_ID, 1);
    topic("firmware/state");
    broker_publish_str(tbuf,
        st == OTA_ST_NONE ? "no bootloader" :
        st == OTA_ST_OK   ? "running" :
        trial[(st - OTA_ST_TRIAL - 1) % 3], 1);
    topic("firmware/backup");
    broker_publish_str(tbuf, ota_backup() ? "yes" : "no", 1);
    topic("firmware/ui");
    broker_publish_str(tbuf, http_ui_size() ? "installed" : "missing", 1);
}

/* Publishes the stack gauge, see main.c. Only ever called when it drops. */
void app_stack(uint8_t free) BANKED
{
    static __xdata uint8_t d[3];      /* off the stack — README rule 4 */
    uint8_t n = 0;
    topic("debug/stackfree");
    if (free >= 100) d[n++] = (uint8_t)('0' + free / 100);
    if (free >= 10)  d[n++] = (uint8_t)('0' + (free / 10) % 10);
    d[n++] = (uint8_t)('0' + free % 10);
    broker_publish(tbuf, d, n, 1);
}

void app_poll(void) BANKED
{
    uint8_t touched = 0;

    if (ota_changed) {
        ota_changed = 0;
        publish_firmware();
    }

    flush_queue();
    rules_work();
    settings_work();

    contacts_poll();
    remote_poll();

    /* The 16C554 is read ONLY when its interrupt line says a byte is waiting —
     * never on a timer, never "just in case".
     *
     * Measured with a camera on the panel: every read of the UART over the bus
     * makes the front-panel IR and status LEDs flash, because they sit on the
     * same chip select. A bridge polling the UARTs every half second made the
     * panel blink all night; with nothing polling it is dead steady. So the
     * idle path is one SFR read and not a single bus cycle.
     *
     * P1.0-P1.3 are the INT lines (live because MCR sets OUT2). Any of them
     * high means something arrived; which channel is found from its own LSR,
     * so the mapping of lines to channels does not have to be known. */
    if ((P1 & 0x0F) && serial_on) {
        if (serial_on & 1) serial_pump(0);
        if (serial_on & 2) serial_pump(1);
        touched = 1;
    }
    /* No bus access here: only the held bytes and the clock. */
    /* > 1 tick: 22-44 ms of quiet */
    if (ser_n[0] && (uint16_t)(ticks() - ser_at[0]) > 1) serial_flush(0);
    if (ser_n[1] && (uint16_t)(ticks() - ser_at[1]) > 1) serial_flush(1);
    if (touched) leds_refresh();
}
