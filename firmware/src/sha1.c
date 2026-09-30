#include "sha1.h"

/* The message schedule is normally 80 words — 320 bytes, which is more XDATA
 * than a handshake deserves. Only the last 16 are ever read, so a 16-word ring
 * gives the identical result in 64. */
static __xdata uint32_t w[16];
static __xdata uint32_t h[5];
static __xdata uint8_t  blk[64];
static __xdata uint32_t total;          /* bytes hashed, for the length field */
static __data  uint8_t  fill;

static uint32_t rol(uint32_t v, uint8_t n)
{
    return (v << n) | (v >> (32 - n));
}

static void transform(void)
{
    /* static, not on the stack: there are 175 bytes of stack for everything
     * and this function is never re-entered. See firmware/README rule 4. */
    static __xdata uint32_t a, b, c, d, e, f, k, t;
    uint8_t i;

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16)
             | ((uint32_t)blk[i * 4 + 2] << 8) | blk[i * 4 + 3];

    for (i = 0; i < 80; i++) {
        if (i >= 16) {
            t = w[(i + 13) & 15] ^ w[(i + 8) & 15]
              ^ w[(i + 2) & 15]  ^ w[i & 15];
            w[i & 15] = rol(t, 1);
        }
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999UL; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1UL; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCUL; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6UL; }
        t = rol(a, 5) + f + e + k + w[i & 15];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(void) BANKED
{
    h[0] = 0x67452301UL; h[1] = 0xEFCDAB89UL; h[2] = 0x98BADCFEUL;
    h[3] = 0x10325476UL; h[4] = 0xC3D2E1F0UL;
    total = 0;
    fill = 0;
}

void sha1_update(const uint8_t *p, uint16_t n) BANKED
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        blk[fill++] = p[i];
        total++;
        if (fill == 64) { transform(); fill = 0; }
    }
}

void sha1_final(uint8_t out[20]) BANKED
{
    static __xdata uint32_t bits;     /* off the stack — README rule 4 */
    bits = total * 8;
    uint8_t i;

    blk[fill++] = 0x80;
    if (fill > 56) {
        while (fill < 64) blk[fill++] = 0;
        transform();
        fill = 0;
    }
    while (fill < 56) blk[fill++] = 0;
    blk[56] = 0; blk[57] = 0; blk[58] = 0; blk[59] = 0;
    blk[60] = (uint8_t)(bits >> 24); blk[61] = (uint8_t)(bits >> 16);
    blk[62] = (uint8_t)(bits >> 8);  blk[63] = (uint8_t)bits;
    transform();

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static __code const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_20(const uint8_t in[20], char out[29]) BANKED
{
    /* Six bits at a time off an accumulator; 20 bytes is 160 bits, which is
     * 26 characters and 4 bits over, so a 27th and one '='. */
    uint16_t acc = 0;
    uint8_t bits = 0, i = 0, o = 0;
    while (o < 27) {
        if (bits < 6) {
            acc = (uint16_t)(acc << 8) | (i < 20 ? in[i] : 0);
            i++;
            bits += 8;
        }
        bits -= 6;
        out[o++] = b64tab[(acc >> bits) & 0x3F];
    }
    out[27] = '=';
    out[28] = 0;
}

static uint8_t b64val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0xFF;
}

uint8_t b64_decode(const uint8_t *in, uint16_t n, uint8_t *out, uint8_t max) BANKED
{
    uint16_t acc = 0;
    uint8_t bits = 0, o = 0, v;
    while (n--) {
        v = b64val(*in++);
        if (v == 0xFF) break;
        acc = (uint16_t)((acc << 6) | v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o == max) break;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}
