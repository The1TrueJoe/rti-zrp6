#include "rti.h"

#ifndef __SDCC
#define __xdata
#endif

/* Everything is measured from the capture itself rather than from constants.
 * The previous decoder used fixed tick thresholds, and ticks are iterations of
 * a polling loop whose speed depends on how the compiler laid that loop out —
 * the build flags changed and every threshold silently moved. Here each
 * polarity's short-symbol length is estimated from its own runs:
 *
 *   m1 = mean of all runs of that polarity           (gaps drag this up)
 *   m2 = mean of those shorter than 3 * m1           (gaps excluded)
 *   short/long split at 1.35 * m2, frame gap above 5 * m2
 *
 * About nine symbols in ten are short, so m2 sits near 1.1 T and the split
 * lands near 1.5 T — midway between T and 2T — for any loop speed. HIGH runs
 * measure a few ticks short and LOW runs a few long, which is why the two
 * polarities are calibrated apart. */

#define MAX_GROUPS  24
static const uint8_t PREAMBLE[5] = { 16, 16, 14, 12, 14 };

static __xdata uint8_t  groups[MAX_GROUPS];
/* The last unknown-preamble frame seen in this capture, formatted. */
static __xdata char     cand[RTI_CODE_MAX];
static __xdata uint16_t thr[2], gap[2];

static void calibrate(const uint16_t *runs, uint16_t n)
{
    uint8_t p;
    uint16_t i;                         /* not uint8_t: i + 2 past 254 would
                                         * wrap to 0 and never end */
    for (p = 0; p < 2; p++) {
        static __xdata uint32_t sum, lim;   /* off the stack */
        sum = 0;
        uint16_t cnt = 0, m1, m2;
        for (i = p; i < n; i += 2) { sum += runs[i]; cnt++; }
        if (!cnt) { thr[p] = 0xFFFF; gap[p] = 0xFFFF; continue; }
        m1 = (uint16_t)(sum / cnt);
        lim = (uint32_t)m1 * 3;
        sum = 0; cnt = 0;
        for (i = p; i < n; i += 2)
            if (runs[i] < lim) { sum += runs[i]; cnt++; }
        m2 = cnt ? (uint16_t)(sum / cnt) : m1;
        thr[p] = (uint16_t)(((uint32_t)m2 * 27) / 20);    /* 1.35 * m2 */
        gap[p] = (uint16_t)((uint32_t)m2 * 5);
    }
}

/* Appends groups[from..to) to out as dotted decimals; 0 if it will not fit. */
static uint8_t put_groups(char *out, uint8_t o, uint8_t from, uint8_t to)
{
    uint8_t i, k, v;
    for (i = from; i < to; i++) {
        v = groups[i];
        if (o + 5 >= RTI_CODE_MAX) return 0;
        if (i > from) out[o++] = '.';
        k = 0;
        if (v >= 100) { out[o++] = (char)('0' + v / 100); k = 1; }
        if (v >= 10 || k) out[o++] = (char)('0' + (v / 10) % 10);
        out[o++] = (char)('0' + v % 10);
    }
    out[o] = 0;
    return o;
}

/* Validates one frame's groups and, if it is an RTI frame, formats its code.
 *
 * groups[0] is the lead-in, which only records where sampling happened to
 * start; 1..5 are the preamble; the button is the rest. A real frame is at
 * least 120 symbols, and anything shorter was cut off.
 *
 * The preamble identifies the remote (or its ID setting). The one measured on
 * the bench, 16 16 14 12 14, decodes from a single frame to the codes rules
 * already use. Any other preamble — another remote — is accepted only when
 * two consecutive frames in the capture agree exactly, which a burst of
 * interference will not do, and its code carries the preamble in front,
 * "p16.16.14.12.15:14.0.2.8", so two remotes never share a button code. */
static uint8_t emit(uint8_t ng, uint16_t syms, char *out)
{
    uint8_t i, o, known = 1;
    if (syms < 120 || ng < 7) return 0;
    for (i = 0; i < 5; i++) {
        if (groups[1 + i] != PREAMBLE[i]) known = 0;
        if (groups[1 + i] < 6 || groups[1 + i] > 40) return 0;
    }
    if (known) return put_groups(out, 0, 6, ng);

    out[0] = 'p';
    o = put_groups(out, 1, 1, 6);
    if (!o) return 0;
    out[o++] = ':';
    o = put_groups(out, o, 6, ng);
    if (!o) return 0;
    for (i = 0; i <= o; i++)
        if (cand[i] != out[i]) break;
    if (i > o) return o;                /* the same frame twice */
    for (i = 0; i <= o; i++) cand[i] = out[i];
    return 0;
}

uint8_t rti_decode(const uint16_t *runs, uint16_t n, uint8_t ended_idle,
                   char *out)
{
    uint16_t i;
    uint8_t ng = 0, cur = 0, pol, len;
    uint16_t syms = 0;

    if (n < 20) return 0;
    cand[0] = 0;
    calibrate(runs, n);

    for (i = 0; i < n; i++) {
        pol = (uint8_t)(i & 1);
        if (runs[i] > gap[pol]) {
            /* A frame gap: what came before it is complete. If the capture
             * began mid-frame, that first stretch fails the preamble check
             * and is simply dropped. */
            if (ng < MAX_GROUPS) groups[ng++] = cur;
            if ((len = emit(ng, syms, out)) != 0) return len;
            ng = 0; cur = 0; syms = 0;
            continue;
        }
        syms++;
        if (runs[i] < thr[pol]) {
            if (cur < 0xFF) cur++;
        } else {
            if (ng < MAX_GROUPS) groups[ng++] = cur;
            else return 0;              /* longer than any RTI frame */
            cur = 0;
        }
    }
    /* The last frame counts only if the line went quiet after it: a capture
     * that ran out of buffer mid-frame would otherwise yield a truncated code
     * that looks perfectly valid. */
    if (ended_idle) {
        if (ng < MAX_GROUPS) groups[ng++] = cur;
        return emit(ng, syms, out);
    }
    return 0;
}
