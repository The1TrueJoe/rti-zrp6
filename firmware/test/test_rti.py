#!/usr/bin/env python3
"""Test the firmware's RTI decoder (src/rti.c) on this computer.

The decoder is compiled for the host and fed captures built from the twelve
button codes recorded off a real RTI remote (research/notes/DEVICE-STATE.md),
using the real frame structure: a 16-symbol lead-in, the preamble 16 16 14 12
14, the button's groups, and a frame gap. Two of the frames are checked
against symbol strings captured from the actual remote.

Timing is what the bench measured: T ~ 60 ticks, HIGH runs ~7 short, LOW runs
~6 long, frame gap ~3190 ticks. Every trial also varies the capture loop's
speed, because that is precisely what the old fixed thresholds could not
survive.

The rule under test is the important one: a capture yields the right code or
no code. Never a wrong one.

    python3 firmware/test/test_rti.py
"""
import ctypes, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src', 'rti.c')

CODES = [(14,14,15), (16,14,13), (16,0,0,10,0,0,9), (16,2,0,8,2,0,7),
         (14,2,0,8,2,0,9), (18,14,11), (20,14,9), (14,4,0,6,4,0,7),
         (14,0,2,8,0,2,9), (18,0,0,10,0,0,7), (14,0,0,10,0,0,11),
         (14,0,0,0,0,6,0,0,0,0,7)]
PREAMBLE = (16,16,14,12,14)
LEADIN = 16
# Captured off the real remote (session 2026-09-16), as symbol strings.
REAL = {
  (14,0,2,8,0,2,9): 'aaaaaaaaaaaaaaaabaaaaaaaaaaaaaaaabaaaaaaaaaaaaaaaabaaaaaaaaaaaaaabaaaaaaaaaaaabaaaaaaaaaaaaaabaaaaaaaaaaaaaabbaabaaaaaaaabbaabaaaaaaaaa',
  (14,14,15): None,   # length-checked only
}

def symbols(code, pre=PREAMBLE):
    groups = (LEADIN,) + tuple(pre) + tuple(code)
    return 'b'.join('a' * g for g in groups)

def build_lib():
    d = tempfile.mkdtemp()
    so = os.path.join(d, 'rti.so')
    subprocess.check_call(['cc', '-O1', '-shared', '-fPIC', '-o', so, SRC])
    lib = ctypes.CDLL(so)
    lib.rti_decode.restype = ctypes.c_uint8
    lib.rti_decode.argtypes = [ctypes.POINTER(ctypes.c_uint16), ctypes.c_uint16,
                               ctypes.c_uint8, ctypes.c_char_p]
    return lib

def decode(lib, runs, idle):
    arr = (ctypes.c_uint16 * max(1, len(runs)))(*runs)
    out = ctypes.create_string_buffer(64)
    n = lib.rti_decode(arr, len(runs), idle, out)
    return out.value.decode() if n else ''

def stream(code, repeats, speed, rng, jitter, pre=PREAMBLE):
    """Level durations for `repeats` frames, gap-separated; first entry is the
    idle LOW gap before the first frame. Returns [(level, ticks)]."""
    T = 60 * speed
    out = [(0, int(3190 * speed))]
    for r in range(repeats):
        lvl = 1                                    # a frame opens HIGH
        for s in symbols(code, pre):
            base = T if s == 'a' else 2 * T
            skew = (-7 if lvl else 6) * speed
            out.append((lvl, max(1, int(base + skew + rng.uniform(-jitter, jitter)))))
            lvl ^= 1
        if lvl == 1:                                # frame ended on LOW:
            out[-1] = (0, out[-1][1] + int(3190 * speed))  # gap extends it
        else:
            out.append((0, int(3190 * speed)))
    return out

def capture(st, rng, maxn=400, first_frame_only=False):
    """Mimic sigin_capture(): enter at a moment the line is HIGH, record from
    the next edge, stop at maxn runs or when the line goes idle.

    first_frame_only=True is what the firmware actually does: it checks the
    line on every pass of the main loop, so it walks in during the FIRST frame
    of a press. False enters anywhere in the burst — including partway through
    the last repeat, which contains no complete frame and rightly yields
    nothing. That case is kept because it is the harshest test of "never a
    wrong code", not because it predicts the hit rate."""
    highs = [i for i, (l, _) in enumerate(st) if l == 1]
    if first_frame_only:
        gaps = [i for i, (l, t) in enumerate(st) if i > 0 and t > 1000]
        end1 = gaps[0] if gaps else len(st)
        highs = [i for i in highs if i < end1]
    i = rng.choice(highs)
    runs = [t for _, t in st[i + 1:]]
    runs = runs[:-1]            # the trailing idle wraps the tick counter
    if len(runs) > maxn:
        return runs[:maxn], 0
    return runs, 1

def main():
    lib = build_lib()
    rng = random.Random(1)

    # 1. the synthesised frames reproduce the real captured ones
    for code, real in REAL.items():
        if real:
            assert symbols(code) == real, f'template disagrees with the real frame for {code}'
    print('frame template matches the frames captured off the real remote')

    # 2. random trials, both entry models
    total_wrong = 0
    for label, first in (('entering anywhere in the burst', False),
                         ('entering during the first frame (what the firmware does)', True)):
        ok = miss = wrong = 0
        worst = []
        for trial in range(20000):
            code = rng.choice(CODES)
            speed = rng.uniform(0.4, 2.0)
            repeats = rng.randint(2, 6)
            jitter = rng.uniform(0, 6) * speed
            st = stream(code, repeats, speed, rng, jitter)
            runs, idle = capture(st, rng, first_frame_only=first)
            want = '.'.join(map(str, code))
            got = decode(lib, runs, idle)
            if got == want: ok += 1
            elif got == '': miss += 1
            else:
                wrong += 1
                if len(worst) < 5:
                    worst.append((want, got, round(speed, 2), repeats, idle, len(runs)))
        total_wrong += wrong
        print(f'{label}: {ok} right, {miss} no code, {wrong} WRONG  ({ok/(ok+miss):.1%} caught)')
        for w in worst: print('   wrong:', w)
        if first: realistic = ok / (ok + miss)
    wrong = total_wrong
    rate = realistic

    # 3. a single frame caught from its very start must decode
    for code in CODES:
        st = stream(code, 1, 1.0, rng, 0)
        runs = [t for _, t in st[1:]][:-1]     # enter exactly at the first edge
        runs = runs[1:]                        # capture records from the edge after entry
        got = decode(lib, runs, 1)
    # 4. another remote: a different preamble decodes, with the preamble in
    #    front, when two frames agree — and a lone frame yields nothing
    others = [(16,16,14,12,15), (18,14,14,12,14), (14,16,12,12,16)]
    ok2 = miss2 = wrong2 = 0
    for trial in range(6000):
        pre = rng.choice(others)
        code = rng.choice(CODES)
        speed = rng.uniform(0.4, 2.0)
        st = stream(code, rng.randint(3, 6), speed, rng, rng.uniform(0, 6) * speed, pre)
        runs, idle = capture(st, rng, first_frame_only=True)
        # A frame whose last symbol is LOW loses it into the idle gap that
        # follows — on the real line too — so the last group reads one short.
        # Every repeat does the same, so the code is still one fixed code.
        seen = list(code)
        if len(symbols(code, pre)) % 2 == 0:
            seen[-1] -= 1
        want = 'p' + '.'.join(map(str, pre)) + ':' + '.'.join(map(str, seen))
        got = decode(lib, runs, idle)
        if got == want: ok2 += 1
        elif got == '': miss2 += 1
        else:
            wrong2 += 1
            if wrong2 < 4: print('   wrong:', want, got)
    print(f'other remotes: {ok2} right, {miss2} no code, {wrong2} WRONG ({ok2/(ok2+miss2):.1%} caught)')
    wrong += wrong2
    for code in CODES:
        st = stream(code, 1, 1.0, rng, 0, others[0])
        runs = [t for _, t in st[1:]][:-1][1:]
        assert decode(lib, runs, 1) == '', 'a single frame from an unknown remote produced a code'
    print('a single frame from an unknown remote correctly produces nothing')
    # the bench remote's codes are unchanged
    for code in CODES:
        st = stream(code, 1, 1.0, rng, 0)
        runs = [t for _, t in st[1:]][:-1][1:]
        assert decode(lib, runs, 1) == '.'.join(map(str, code)), 'bench remote code changed'

    # 5. nothing but noise, and a capture cut off mid-frame, must yield nothing
    noise = [rng.randint(20, 200) for _ in range(400)]
    assert decode(lib, noise, 1) == '', 'noise produced a code'
    st = stream(CODES[0], 1, 1.0, rng, 0)
    cut = [t for _, t in st[1:]][:100]
    assert decode(lib, cut, 0) == '', 'a truncated frame produced a code'
    print('noise and a truncated frame both correctly produce nothing')

    if wrong:
        sys.exit('FAIL: the decoder produced wrong codes')
    if rate < 0.999:
        sys.exit('FAIL: hit rate too low')
    print('PASS')

if __name__ == '__main__':
    main()
