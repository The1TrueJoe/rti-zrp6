#!/usr/bin/env python3
"""Recognise RTI remote button presses arriving on the ZRP-6's SIGNAL IN.

Measured facts this relies on (docs/DEVICE-STATE.md has the working):

  * The RM-433 drives P3.5 with DEMODULATED baseband -- no 40 kHz carrier.
    RTI's remote IR really is 40 kHz, but the receiver strips it.
  * P3.5 idles LOW and pulses HIGH, inverted from a normal IR demodulator.
  * Runs are T or 2T with T ~= 60 capture ticks (~417 us). HIGH runs sit ~7
    ticks short and LOW runs ~6 long, so each polarity is thresholded apart.
  * The encoding is PULSE-DISTANCE: the payload is the spacing between the
    2T symbols, not a Manchester bit stream.
  * Every frame opens with the same [16, 16, 14, 12, 14] -- preamble plus
    system/device ID. The tail identifies the button.
  * Codes are STATIC. 12 buttons gave 12 tails, 1:1, repeating identically.
    Nothing rolling, which matches RTI's fixed library of 256 trigger codes.

The leading gap of a capture is discarded: it only records where sampling
happened to start inside the preamble, and keeping it makes one button look
like several.
"""
import json
import os
import sys
import time

from .device import Zrp6

GAP = 1500
PREFIX = (16, 16, 14, 12, 14)
DB = os.path.expanduser('~/.zrp6_rti_codes.json')


def frames_from(level, runs):
    out, cur, lvl = [], [], level
    for r in runs:
        if r > GAP:
            if len(cur) > 8:
                out.append(cur)
            cur = []
        else:
            cur.append('a' if r < (85 if lvl else 95) else 'b')
        lvl ^= 1
    if len(cur) > 8:
        out.append(cur)
    return [''.join(f) for f in out]


def code_of(sym):
    """-> the button's code tuple, or None if it isn't a complete frame."""
    if len(sym) < 120:
        return None
    g = tuple(len(x) for x in sym.split('b'))[1:]   # drop the partial lead-in
    if g[:5] != PREFIX:
        return None
    return g[5:]


def load():
    try:
        return {tuple(json.loads(k)): v for k, v in json.load(open(DB)).items()}
    except Exception:
        return {}


def save(d):
    json.dump({json.dumps(list(k)): v for k, v in d.items()}, open(DB, 'w'), indent=1)


def listen(z, secs, on_code):
    seen_recent = {}
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            lvl, runs = z.capture(200)
        except Exception:
            time.sleep(0.2)
            continue
        runs = [r for r in runs if r]
        if len(runs) < 20:
            continue
        for sym in frames_from(lvl, runs):
            c = code_of(sym)
            if c is None:
                continue
            now = time.time()
            if now - seen_recent.get(c, 0) > 0.6:      # debounce repeats
                seen_recent[c] = now
                on_code(c)
        time.sleep(0.15)


def main():
    z = Zrp6()
    z.sfr(1, 0xFF)
    names = load()
    mode = sys.argv[1] if len(sys.argv) > 1 else 'watch'
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0

    if mode == 'learn':
        label = sys.argv[2]
        hits = {}
        print('press %r repeatedly for %ds...' % (label, secs))
        listen(z, secs, lambda c: hits.__setitem__(c, hits.get(c, 0) + 1))
        if not hits:
            print('nothing captured')
            return 1
        best = max(hits, key=hits.get)
        names[best] = label
        save(names)
        print('learned %-14s = %s   (%dx)' % (label, list(best), hits[best]))
    else:
        print('listening %ds - press buttons' % secs)
        listen(z, secs, lambda c: print('  %s  %-14s %s'
                                        % (time.strftime('%H:%M:%S'),
                                           names.get(c, '<unknown>'), list(c))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
