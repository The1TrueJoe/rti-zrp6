#!/usr/bin/env python3
"""Put the UI into the linker's image and check that everything fits.

    layout.py <build dir> <high code address> <UI space>

Sector 0 (0x0000-0x7FFF) must hold all ordinary code. Page 0 (code
0x8000-0xFFFF) holds the gzipped UI from 0x8000 up and the HIGH code area from
HIGH_AT up, and the two must not meet. The linker is given the whole 64 KB so
that it will place HIGH where it is told; this is what enforces the real
limits, from the linker's own map.
"""
import os
import re
import sys
import zlib


def mdns_template():
    """One unsolicited mDNS response (RFC 6762) announcing the box: PTR
    _zrp6._tcp.local -> zrp6-XXXX._zrp6._tcp.local, its SRV (port 1883,
    target zrp6-XXXX.local) and TXT (mac=...), and the A record, TTL 120,
    with name compression. XXXX, the MAC and the IP are placeholders the
    firmware fills in; their offsets come back with the packet."""
    ttl = (120).to_bytes(4, 'big')
    p = bytearray(b'\x00\x00\x84\x00\x00\x00\x00\x04\x00\x00\x00\x00')
    svc = len(p)                                    # _zrp6._tcp.local
    p += b'\x05_zrp6\x04_tcp\x05local\x00'
    local = svc + 11                                # the "local" label
    p += b'\x00\x0c\x00\x01' + ttl                   # PTR, IN
    inst = len(p) + 2                               # rdata: the instance name
    p += (12).to_bytes(2, 'big') + b'\x09zrp6-XXXX' + bytes([0xC0, svc])
    host1 = inst + 6
    p += bytes([0xC0, inst]) + b'\x00\x21\x80\x01' + ttl       # SRV, flush
    target = len(p) + 2 + 6
    p += (6 + 12).to_bytes(2, 'big') + b'\x00\x00\x00\x00\x07\x5b' + b'\x09zrp6-XXXX' + bytes([0xC0, local])
    host2 = target + 6
    p += bytes([0xC0, inst]) + b'\x00\x10\x80\x01' + ttl       # TXT, flush
    p += (17).to_bytes(2, 'big') + b'\x10mac=' + b'M' * 12
    mac = len(p) - 12
    p += bytes([0xC0, target]) + b'\x00\x01\x80\x01' + ttl     # A, flush
    p += (4).to_bytes(2, 'big') + b'\x00\x00\x00\x00'
    ip = len(p) - 4
    assert len(p) < 256 and max(host1, host2, mac, ip) < 256
    return bytes(p), [host1, host2, mac, ip, 0]


def layout_v8(B):
    """v8 (docs/UPDATING.md, boot/ota.h): code is one run from 0x2000 to at
    most 0xFFFF; the header at 0x0000 carries the magic and a CRC-32 of
    0x0010-0xFFFF. The web page goes to its own sector (fs7) and its own file:
    a 16-byte header ("ZUI2", CRC-32 of the blob, its length) and the blob."""
    img = bytearray(open(f'{B}/image.bin', 'rb').read().ljust(65536, b'\xff'))
    fail = []
    code_end = 0x2000
    for line in open(f'{B}/zrp6.map'):
        m = re.match(r'^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s*=.*CODE', line)
        if m and int(m.group(3), 16):
            start, size = int(m.group(2), 16), int(m.group(3), 16)
            if start < 0x2000:
                fail.append(f'{m.group(1)} at {start:#x} is inside the bootloader')
            code_end = max(code_end, start + size)
    if any(b != 0xFF for b in img[:0x2000]):
        fail.append('the linker put something below 0x2000')
    if code_end > 0x10000:
        fail.append(f'code ends at {code_end:#x}, past 0xFFFF')
    blob = open(f'{B}/ui.blob', 'rb').read()
    blob = blob[8:] if blob[:4] == b'ZUI1' else blob        # pack.py's own header
    if len(blob) > 0x8000 - 16:
        fail.append(f'the page is {len(blob)} bytes; its sector holds {0x8000 - 16}')
    if fail:
        sys.exit('LAYOUT FAILED:\n  ' + '\n  '.join(fail))
    # The page a box with no page serves (http.c): in the first 8 KB of the
    # image, which the CPU never fetches from, behind its length.
    noui = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src', 'noui.html'),
                'rb').read().strip()
    if len(noui) > 0x2000 - 0x12:
        sys.exit(f'LAYOUT FAILED: src/noui.html is {len(noui)} bytes')
    img[0x10:0x12 + len(noui)] = len(noui).to_bytes(2, 'little') + noui
    # The mDNS announcement (net.c), at 0x1C00: a header of six bytes —
    # length, then the offsets the box patches: the two copies of the host
    # label's last four characters, the MAC's twelve hex digits, the IPv4
    # address — then the packet.
    mdns, offs = mdns_template()
    if len(noui) + 0x12 > 0x1C00:
        sys.exit('LAYOUT FAILED: src/noui.html runs into the mDNS template at 0x1C00')
    img[0x1C00:0x1C06 + len(mdns)] = bytes([len(mdns)] + offs) + mdns
    crc = zlib.crc32(bytes(img[0x10:]))
    img[0:16] = (b'ZRP6' + crc.to_bytes(4, 'little') + bytes([1, 0xFF])
                 + b'\x00\x00' + b'\xff' * 4)
    ui = (b'ZUI2' + zlib.crc32(blob).to_bytes(4, 'little') + len(blob).to_bytes(2, 'little')
          + b'\xff' * 6 + blob)
    print(f'v8: code 0x2000-{code_end:#06x}, {0x10000 - code_end} bytes free; '
          f'page {len(blob)} bytes in its own sector ({0x8000 - len(ui)} free); crc {crc:08x}')
    open(f'{B}/zrp6.bin', 'wb').write(img)
    open(f'{B}/zrp6-ui.bin', 'wb').write(ui)
    open(f'{B}/sector0.bin', 'wb').write(img[:0x8000])
    open(f'{B}/sector1.bin', 'wb').write(img[0x8000:])
    open(f'{B}/sector7.bin', 'wb').write(ui.ljust(0x8000, b'\xff'))


def cross_bank_calls(B):
    """Direct calls that cross into a code bank without the trampoline.

    A function in a bank can be reached from outside that bank only through
    __sdcc_banked_call, i.e. only if it is declared BANKED. A plain lcall or
    ljmp to it from common code, or from the other bank, runs whatever the
    other bank holds at that address — a crash nothing else would catch. So
    every call in every module's .asm is checked against where its target is
    defined."""
    group, where, own = {}, {}, {}
    for line in open(f'{B}/GROUPS'):
        g, mods = line.split(':')
        for m in mods.split():
            group['zrp6' if m == 'main' else m] = g     # main.c compiles to zrp6.asm
    asm = {}
    for m in group:
        try:
            asm[m] = open(f'{B}/{m}.asm').read()
        except FileNotFoundError:
            sys.exit(f'LAYOUT FAILED: no {m}.asm to check calls in')
        own[m] = set(re.findall(r'(?m)^(_[A-Za-z_][A-Za-z_0-9]*):\s*$', asm[m]))
        globl = set(re.findall(r'(?m)^\s+\.globl\s+(_[A-Za-z_][A-Za-z_0-9]*)', asm[m]))
        for f in own[m] & globl:
            where[f] = group[m]
    bad = set()
    for m, src in asm.items():
        for op, f in re.findall(r'(?m)^\s+(lcall|ljmp|acall|ajmp)\s+(_[A-Za-z_][A-Za-z_0-9]*)', src):
            if f in own[m]:
                continue                        # the module's own (maybe static)
            g = where.get(f)
            if g and g != 'common' and g != group[m]:
                bad.add(f'{m} ({group[m]}) calls {f} in {g} directly: declare it BANKED')
    return sorted(bad)


def layout_v10(B):
    """v10 (docs/UPDATING.md, boot/ota.h): a 96 KB image of three 32 KB parts —
    header and data then common code (0x2000-0x7FFF), bank 0, bank 1 — for the
    slot that is not running. Also the JTAG sectors for slot A, confirmed."""
    mem, base = {}, 0
    for line in open(f'{B}/zrp6.ihx'):
        n, a, t = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        d = bytes.fromhex(line[9:9 + 2 * n])
        if t == 4:
            base = int.from_bytes(d, 'big') << 16
        elif t == 0:
            for i, b in enumerate(d):
                if base + a + i in mem:
                    sys.exit(f'LAYOUT FAILED: {base + a + i:#x} written twice')
                mem[base + a + i] = b
    WIN = {'common': (0x2000, 0x8000), 'bank0': (0x8000, 0x10000), 'bank1': (0x18000, 0x20000)}
    fail = []
    for line in open(f'{B}/zrp6.map'):
        m = re.match(r'^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s*=.*CODE', line)
        if m and int(m.group(3), 16):
            name, st, z = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            w = 'bank0' if name == 'BANK0' else 'bank1' if name == 'BANK1' else 'common'
            lo, hi = WIN[w]
            if st < lo or st + z > hi:
                fail.append(f'{name} {st:#x}+{z} is outside {w} {lo:#x}-{hi - 1:#x}')
    stray = [a for a in mem if not any(lo <= a < hi for lo, hi in WIN.values())]
    if stray:
        fail.append(f'{len(stray)} bytes outside every window, first at {min(stray):#x}')
    fail += cross_bank_calls(B)

    img = bytearray(b'\xff' * 0x18000)
    for a, b in mem.items():
        if 0x2000 <= a < 0x10000:
            img[a] = b
        elif 0x18000 <= a < 0x20000:
            img[a - 0x8000] = b
    # The data area: the page a box with no page serves (http.c) at 0x40
    # behind its length, and the mDNS announcement (net.c) at 0x1C00.
    here = os.path.dirname(os.path.abspath(__file__))
    noui = open(os.path.join(here, 'src', 'noui.html'), 'rb').read().strip()
    if 0x42 + len(noui) > 0x1C00:
        fail.append(f'src/noui.html is {len(noui)} bytes; it runs into the mDNS template')
    img[0x40:0x42 + len(noui)] = len(noui).to_bytes(2, 'little') + noui
    mdns, offs = mdns_template()
    img[0x1C00:0x1C06 + len(mdns)] = bytes([len(mdns)] + offs) + mdns

    blob = open(f'{B}/ui.blob', 'rb').read()
    blob = blob[8:] if blob[:4] == b'ZUI1' else blob        # pack.py's own header
    if len(blob) > 0x8000 - 16:
        fail.append(f'the page is {len(blob)} bytes; its sector holds {0x8000 - 16}')
    if fail:
        sys.exit('LAYOUT FAILED:\n  ' + '\n  '.join(fail))

    crc = zlib.crc32(bytes(img[0x40:]))
    img[0:0x40] = b'ZRPB' + crc.to_bytes(4, 'little') + bytes([2]) + b'\xff' * 55
    ui = (b'ZUI2' + zlib.crc32(blob).to_bytes(4, 'little') + len(blob).to_bytes(2, 'little')
          + b'\xff' * 6 + blob)
    open(f'{B}/zrp6.bin', 'wb').write(img)
    open(f'{B}/zrp6-ui.bin', 'wb').write(ui)
    # JTAG writes slot A as the one image there is: generation 1, confirmed.
    s0 = bytearray(img[:0x8000])
    s0[0x10:0x12] = b'\x01\x00'
    s0[0x20] = 0x00
    open(f'{B}/sector0.bin', 'wb').write(s0)
    open(f'{B}/sector1.bin', 'wb').write(img[0x8000:0x10000])
    open(f'{B}/sector2.bin', 'wb').write(img[0x10000:])
    open(f'{B}/sector7.bin', 'wb').write(ui.ljust(0x8000, b'\xff'))
    for w, (lo, hi) in WIN.items():
        used = max([a for a in mem if lo <= a < hi], default=lo - 1) - lo + 1
        print(f'v10 {w:6s}: {used:5d} bytes used, {hi - lo - used:5d} free')
    print(f'v10: image 98304 bytes, crc {crc:08x}; page {len(blob)} bytes ({0x8000 - len(ui)} free in its sector)')


if sys.argv[1] == '--v10':
    layout_v10(sys.argv[2])
    sys.exit(0)

if sys.argv[1] == '--v8':
    layout_v8(sys.argv[2])
    sys.exit(0)

B, high_at, ui_max = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3])
img = bytearray(open(f'{B}/image.bin', 'rb').read().ljust(65536, b'\xff'))

areas = {}
for line in open(f'{B}/zrp6.map'):
    m = re.match(r'^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s*=', line)
    if m:
        areas[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

fail = []
if 'HIGH' not in areas or not areas['HIGH'][1]:
    fail.append('no HIGH area: the high modules were not placed')
for name, (start, size) in areas.items():
    if not size:
        continue
    end = start + size
    if name == 'HIGH':
        if start != high_at or end > 0x10000:
            fail.append(f'HIGH at {start:#x}+{size} does not fit {high_at:#x}-0xFFFF')
    elif start < 0x8000 < end:
        fail.append(f'{name} ends at {end:#x}, past sector 0')
if any(b != 0xFF for b in img[0x8000:high_at]):
    fail.append('the linker put something in the UI region')

ui = open(f'{B}/ui.blob', 'rb').read()
if len(ui) > ui_max:
    fail.append(f'UI is {len(ui)} bytes; the page has {ui_max} below the high code')
img[0x8000:0x8000 + len(ui)] = ui

if fail:
    sys.exit('LAYOUT FAILED:\n  ' + '\n  '.join(fail))

low_end = max(s + z for n, (s, z) in areas.items() if s < 0x8000 and z)
hs, hz = areas['HIGH']
print(f'sector 0: code to {low_end:#06x}, {0x8000 - low_end} bytes free')
print(f'page 0:   UI {len(ui)} of {ui_max} bytes; high code {hs:#06x}-{hs + hz:#06x}, '
      f'{0x10000 - hs - hz} bytes free')
open(f'{B}/code.bin', 'wb').write(img[:0x8000])
open(f'{B}/ui.bin', 'wb').write(img[0x8000:])
open(f'{B}/zrp6.bin', 'wb').write(img)
