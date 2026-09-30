#!/usr/bin/env python3
"""Call the real bootloader binary the way the application does, in the s51
8051 simulator, and check the calling convention and the stack it needs.

  abi.py build/boot/boot.ihx

The data window is plain RAM in the simulator (every page aliases the same
32 KB and nothing erases), which is enough for what this checks:
  - lcall 0x0080 with DPTR -> struct ota_req in XDATA comes back to the caller;
  - rc is written into the struct;
  - how deep into the caller's stack the call goes, per operation;
  - IRAM below the stack is left as it was (only _bp moves, and is restored).
The Flash logic itself is tested in sim.c.
"""
import subprocess
import sys
import zlib

IHX = sys.argv[1]
SP0 = 0x60                 # the caller's SP at the lcall
HALT = 0x1F80              # "sjmp $" planted here as the return target
REQ, BUF = 0x1000, 0x1100


PAGE = 0x20E0              # the PSD page register (board.h CS_PAGE)


def run(op, off=0, length=0, buf=b'', window=None, page=0x00):
    cmds = ['fill iram 0x00 0xff 0x5a',
            'fill xram 0x0000 0xffff 0xff',
            f'set memory rom {HALT:#x} 0x80 0xfe']
    if window:
        for i in range(0, len(window), 16):
            cmds.append(f'set memory xram {0x8000 + i:#x} ' + ' '.join(f'{b:#x}' for b in window[i:i + 16]))
    req = [op, 0xEE, off & 0xFF, (off >> 8) & 0xFF, length & 0xFF, length >> 8, BUF & 0xFF, BUF >> 8, off >> 16]
    cmds.append(f'set memory xram {REQ:#x} ' + ' '.join(f'{b:#x}' for b in req))
    cmds.append(f'set memory xram {PAGE:#x} {page:#x}')
    for i in range(0, len(buf), 16):
        cmds.append(f'set memory xram {BUF + i:#x} ' + ' '.join(f'{b:#x}' for b in buf[i:i + 16]))
    # the return address, as lcall would have pushed it
    cmds += [f'set memory iram {SP0 + 1:#x} {HALT & 0xFF:#x} {HALT >> 8:#x}',
             f'set memory sfr 0x81 {SP0 + 2:#x}',
             f'set memory sfr 0x82 {REQ & 0xFF:#x}', f'set memory sfr 0x83 {REQ >> 8:#x}',
             # the application runs slot A, confirmed (header in the window)
             f'run 0x0080 {HALT:#x}',
             'pc',
             'dump iram 0x00 0xff 16',
             f'dump xram {REQ:#x} {REQ + 7:#x} 8',
             f'dump xram {PAGE:#x} {PAGE:#x} 1',
             'quit']
    out = subprocess.run(['s51', '-q', '-t', '8052', '-c', '-', IHX], input='\n'.join(cmds) + '\n',
                         capture_output=True, text=True, timeout=600).stdout
    iram = bytearray(256)
    rc = pg = None
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 17 and p[0].startswith('0x') and len(p[0]) == 4:
            a = int(p[0], 16)
            for i in range(16):
                iram[a + i] = int(p[1 + i], 16)
        if len(p) >= 9 and p[0].lower() == f'{REQ:#06x}':
            rc = int(p[2], 16)
        if len(p) >= 2 and p[0].lower() == f'{PAGE:#06x}':
            pg = int(p[1], 16)
    pc_ok = f'{HALT:#06x}' in out.lower() or 'breakpoint' in out.lower()
    return iram, rc, pc_ok, pg


fails = []
# In s51 every data-window page is the same 32 KB of RAM, so "slot A" (the
# running one), "slot B" (the target) and the page sector all alias. The
# window holds slot A's header, confirmed, and 0xFF elsewhere; FINISH's CRC
# (the deepest path: over the whole 96 KB body) is computed over that alias.
win = bytearray([0xFF]) * 0x8000
win[0:4] = b'ZRPB'; win[8] = 2; win[0x10] = 1; win[0x11] = 0; win[0x20] = 0
body = bytes(win[o & 0x7FFF] for o in range(0x40, 0x18000))
crc = zlib.crc32(body)
hdr = b'ZRPB' + crc.to_bytes(4, 'little') + bytes([2]) + b'\xff' * 55
win = bytes(win)
cases = [('WRITE', 2, 0x10040, 16, bytes(range(16)), win, 0),
         ('FINISH ok', 3, 0, 0, hdr, win, 0),
         ('FINISH bad crc', 3, 0, 0, hdr[:4] + b'\0\0\0\0' + hdr[8:], win, 4),
         ('FINISH foreign', 3, 0, 0, b'XXXX' + hdr[4:], win, 3),
         # rc not checked: the simulated window is RAM, so the erase's
         # read-back (every byte 0xFF) fails by design here.
         ('ERASE', 1, 0, 0, b'', win, None),
         ('CONFIRM', 4, 0, 0, b'', win, 0),
         ('bad op', 9, 0, 0, b'', None, 5)]
worst = 0
# Called from slot A bank 0 (0x00), and from slot B bank 1 (0xC0): the call
# must come back with bits 6-7 as they were — it returns, and takes
# interrupts, from the slot they select — and the window at 0 (W5100).
for (name, op, off, ln, buf, window, want), page in [(c, p) for p in (0x00, 0xC0) for c in cases]:
    name = f'{name} @{page:02x}'
    iram, rc, pc_ok, pg = run(op, off, ln, buf, window, page)
    used = max((a for a in range(SP0 + 3, 256) if iram[a] != 0x5A), default=SP0 + 2) - SP0
    worst = max(worst, used)
    # 0x08 (_bp) and 0x20 (the bit bank, SDCC's boolean temporaries) are the
    # bootloader's to use: the application's stub (src/ota.c, ota_call)
    # pushes both before the lcall and pops them after. Nothing else.
    below = [hex(a) for a in range(0x08, SP0 + 1) if iram[a] != 0x5A and a not in (0x08, 0x20)]
    print(f'  {name:20s} rc={rc} stack={used:3d} bytes returned={pc_ok} page={pg}')
    if pg != page:
        fails.append(f'{name}: page register {pg} after the call, want {page:#x}')
    if want is not None and rc != want:
        fails.append(f'{name}: rc {rc}, want {want}')
    if not pc_ok:
        fails.append(f'{name}: did not return to the caller')
    if below:
        fails.append(f'{name}: IRAM below the stack changed at {below}')
print(f'worst stack use {worst} bytes (return address included)')
print('PASS' if not fails else 'FAIL: ' + '; '.join(fails))
sys.exit(1 if fails else 0)
