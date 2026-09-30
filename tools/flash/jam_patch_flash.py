#!/usr/bin/env python3
"""Rewrite the main-Flash payload inside a PSDsoft-generated JAM STAPL file.

The JAM carries main Flash as gMAIN_DATA[N], a big-endian hex literal holding
one 10-bit group per byte, LSB-group first, encoded as (byte << 2) | 3.
Everything else in the file -- crucially the PLD/ACR fuse array -- is untouched,
so this swaps firmware without re-running the PSDsoft fitter.

  jam_patch_flash.py in.jam out.jam sector.bin ...   (in the order the JAM lists them)
  jam_patch_flash.py --boot boot.bin in.jam out.jam sector0.bin [sector1.bin]
  jam_patch_flash.py --extract in.jam out.bin [--boot-out boot.bin]

--boot replaces the secondary-Flash payload, gBOOT_DATA, the same way. The v7
JAM carries exactly one boot sector there: csboot2, the bootloader (8 KB).
PSDsoft only puts sectors in a JAM that had data when it was generated, and
the JAM erases only those, which is why a JTAG flash leaves the store
(csboot0/1) and the update slots (fs2-fs6) alone.
"""
import re, sys

TAG = 'gMAIN_DATA['


def _span(txt, tag=None):
    tag = tag or TAG
    i = txt.index(tag)
    n = int(txt[i + len(tag):txt.index(']', i)])
    eq = txt.index('=', i)
    end = txt.index(';', eq)
    return i, n, eq + 1, end


def extract(txt, tag=None):
    _, n, a, b = _span(txt, tag)
    big = int(''.join(re.findall(r'[0-9A-Fa-f]+', txt[a:b])), 16)
    out = bytearray()
    for g in range(n // 10):
        v = (big >> (10 * g)) & 0x3FF
        if v & 3 != 3:
            raise SystemExit('group %d has low bits %d, not 3' % (g, v & 3))
        out.append((v >> 2) & 0xFF)
    return bytes(out)


def build(txt, payload, tag=None):
    """Re-emit the literal exactly the way PSDsoft does: CRLF, 102 hex chars per
    line, a '$' hex prefix on the first one."""
    i, n, a, b = _span(txt, tag)
    if len(payload) * 10 != n:
        raise SystemExit('payload is %d bytes, jam wants %d' % (len(payload), n // 10))
    big = 0
    for g, byte in enumerate(payload):
        big |= ((byte << 2) | 3) << (10 * g)
    s = '%0*X' % (n // 4, big)
    chunks = [s[k:k + 102] for k in range(0, len(s), 102)]
    body = '\r\n  $' + chunks[0]
    for c in chunks[1:]:
        body += '\r\n   ' + c
    return txt[:a] + body + txt[b:]


def main(argv):
    if argv[1] == '--extract':
        txt = open(argv[2], newline='', errors='replace').read()
        open(argv[3], 'wb').write(extract(txt))
        if '--boot-out' in argv:
            open(argv[argv.index('--boot-out') + 1], 'wb').write(extract(txt, 'gBOOT_DATA['))
        return
    boot = None
    if argv[1] == '--boot':
        boot = open(argv[2], 'rb').read()
        argv = argv[:1] + argv[3:]
    txt = open(argv[1], newline='', errors='replace').read()
    if boot is not None:
        _, n, _, _ = _span(txt, 'gBOOT_DATA[')
        if n // 10 != len(boot):
            raise SystemExit('this JAM carries %d bytes of secondary Flash, boot.bin is %d — '
                             'is it the v7 JAM?' % (n // 10, len(boot)))
        txt = build(txt, boot, 'gBOOT_DATA[')
    cur = extract(txt)
    sectors = [open(p, 'rb').read() for p in argv[3:]]
    payload = bytearray(cur)
    for k, sec in enumerate(sectors):
        if len(sec) != 32768:
            raise SystemExit('%s is %d bytes, want 32768' % (argv[3 + k], len(sec)))
        payload[k * 32768:(k + 1) * 32768] = sec
    open(argv[2], 'w', newline='').write(build(txt, bytes(payload)))
    print('wrote %s (%d bytes of main Flash replaced%s)' % (
        argv[2], 32768 * len(sectors), ', bootloader too' if boot is not None else ''))


main(sys.argv)
