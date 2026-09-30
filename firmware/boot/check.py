#!/usr/bin/env python3
"""Check the bootloader link for the properties the application relies on.

  check.py boot.map boot.ihx

- code fits in the 8 KB window, with room to spare below 0x2000, where the
  application's code starts (the prefetch queue reads a few bytes ahead);
- the fixed entries are where ota.h says: reset at 0x0000, the call entry at
  0x0080, interrupt forwarders to 0x2000 + vector;
- no RAM of its own on the application's call path: core and hal, which
  ota_op runs, have no data areas at all, and in IRAM there is nothing but
  SDCC's frame pointer _bp, which every function saves and restores (the
  application's stub saves it too). Anything more would sit on the
  application's variables during a call. USB rescue mode (usb, usbcmd,
  rescue) runs only when the application does not, and may keep XDATA —
  above 0x00F0, where the rescue request lives.
"""
import re
import sys

mem = open(sys.argv[1].replace('.map', '.mem')).read()
mapt = open(sys.argv[1]).read()
code = bytearray(b'\xff' * 0x2000)
for line in open(sys.argv[2]):
    n, a, t = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
    if t == 0:
        code[a:a + n] = bytes.fromhex(line[9:9 + 2 * n])

fails = []

rom = re.search(r'ROM/EPROM/FLASH\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)', mem)
end = int(rom.group(2), 16)
if end >= 0x1F00:
    fails.append(f'code ends at 0x{end:04x}, past 0x1F00')

import os
for m in ('core', 'hal', 'main', 'vectors'):
    rel = os.path.join(os.path.dirname(sys.argv[1]), m + '.rel')
    if not os.path.exists(rel):
        continue
    for a, size in re.findall(r'^A (\S+) size ([0-9A-Fa-f]+)', open(rel).read(), re.M):
        if a in ('DSEG', 'ISEG', 'XSEG', 'XISEG', 'PSEG', 'BSEG', 'OSEG', 'IABS', 'XABS') \
                and int(size, 16):
            fails.append(f'{m}: {a} of {int(size, 16)} bytes on the call path')
xs = re.search(r'EXTERNAL RAM\s+0x([0-9a-f]+)', mem)
if xs and int(xs.group(1), 16) < 0x0100:
    fails.append(f'XDATA from 0x{int(xs.group(1), 16):04x}, over the rescue flag')

cells = ''.join(re.findall(r'^0x[0-9a-f]{2}:\|(.*)\|$', mem, re.M)).replace('|', '')
data = [i for i, c in enumerate(cells) if c.isalpha() and c.islower() or c in 'QI']
if len(data) > 1:
    fails.append(f'uses IRAM beyond _bp: {[hex(i) for i in data]}')


def sym(name):
    m = re.search(r'C:\s+([0-9A-F]{8})\s+' + re.escape(name) + r'\b', mapt)
    return int(m.group(1), 16) if m else None


def ljmp(at):
    return code[at + 1] << 8 | code[at + 2] if code[at] == 0x02 else None


if ljmp(0x0000) != sym('__sdcc_gsinit_startup'):
    fails.append('0x0000 does not jump to the startup code')
if ljmp(0x0080) != sym('_ota_op'):
    fails.append('0x0080 does not jump to ota_op')
for v in range(0x03, 0x74, 8):
    if ljmp(v) != 0x2000 + v:
        fails.append(f'vector 0x{v:04x} does not forward to 0x{0x2000 + v:04x}')

print(f'boot: code to 0x{end:04x} ({0x2000 - end - 1} bytes free), '
      f'ota_op at 0x{sym("_ota_op"):04x}')
if fails:
    print('BOOT LAYOUT FAIL: ' + '; '.join(fails))
    sys.exit(1)
