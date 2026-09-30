#!/usr/bin/env python3
"""Flash and inspect a ZRP-6 over its USB port — no JTAG, no network.

  zrp6usb.py info                      what the box is running, both slots
  zrp6usb.py flash [--wait] [image] [page]
                                       install build/zrp6.bin and/or zrp6-ui.bin;
                                       --wait: first wait for rescue mode
  zrp6usb.py wait                      catch the box at power-up and hold it
                                       in rescue mode (then flash)
  zrp6usb.py rescue                    send a running box to rescue mode
  zrp6usb.py backup-store FILE         save the settings store (users, rules,
                                       MAC, network) as a 16 KB file
  zrp6usb.py reboot

The box answers USB in two ways (docs/USB.md): its firmware, while running,
and its bootloader's rescue mode, which needs nothing in main Flash to work.
Rescue mode starts on its own when there is no image to run; otherwise
power the box with the cable in and run `wait` (the bootloader listens for a
few seconds at power-up), or `rescue` while the firmware still answers.

`flash` works in either mode. From the firmware it is the same as a network
upload: the image goes into the slot that is not running, and an image still
on trial refuses (log in once to confirm it). In rescue mode the slot the
bootloader would boot is kept and the other one written. Either way the new
image then boots on trial, and the kept one comes back if it fails.

If the box does not show up with the cable in, power-cycle it once. The
D+ pull-up is hardware, so a host that met a silent box (firmware without
USB, or plugged in while it was being flashed) can give up on the port. A
power cycle is a fresh attach; a restart of the firmware is not.

The box uses RTI's own IDs, 13BD:1019, and every command starts with the
bytes 'Z', '6', so it never mistakes Integration Designer's frames for its own.
A stock RTI ZRP-6 answers to the same IDs; `info` tells them apart.

Needs hidapi (pip install hidapi). No driver on macOS or Windows; on Linux a
udev rule for 13BD:1019 or sudo.
"""
import struct
import sys
import time

import hid

# (vid, pid, prefix): RTI's IDs with the 'Z6' prefix; the pid.codes test ID
# without it is the first USB bootloader, on the bench box only.
IDS = [(0x13BD, 0x1019, True), (0x1209, 0x0001, False)]

INFO, ERASE, WRITE, HEADER, FINISH = 1, 2, 3, 4, 5
UI_ERASE, UI_WRITE, UI_FINISH, READ, REBOOT, RESCUE, HOLD = 6, 7, 8, 9, 10, 11, 12
RC = {0: 'ok', 1: 'Flash write failed', 2: 'offset out of range',
      3: 'not an image for this bootloader', 4: 'CRC mismatch',
      5: 'bootloader refused the call',
      6: 'the running firmware is on trial: log in to confirm it first',
      0xFF: 'bad command'}

IMAGE_SIZE, HDR, UI_HDR = 0x18000, 0x40, 16


def page(slot, part):
    return 1 + slot * 3 + part


class Box:
    def __init__(self, dev):
        self.d = hid.device()
        self.d.open_path(dev['path'])
        self.magic = any(dev['vendor_id'] == v and dev['product_id'] == p and m
                         for v, p, m in IDS)
        self.seq = 0

    @staticmethod
    def find():
        return [d for v, p, _ in IDS for d in hid.enumerate(v, p)]

    def close(self):
        self.d.close()

    def cmd(self, op, args=b'', timeout=30.0):
        """One command; the reply's data (from byte 3). Raises on a bad rc."""
        self.seq = (self.seq + 1) & 0xFF
        frame = (b'Z6' if self.magic else b'') + bytes([op, self.seq]) + bytes(args)
        frame = frame + b'\0' * (64 - len(frame))
        self.d.write(b'\0' + frame)
        end = time.time() + timeout
        while True:
            try:
                r = bytes(self.d.get_feature_report(0, 65))
            except (OSError, ValueError):
                r = b''
            if len(r) == 65:
                r = r[1:]
            if len(r) >= 3 and r[0] == op and r[1] == self.seq:
                if r[2]:
                    raise RuntimeError(f'command {op}: {RC.get(r[2], r[2])}')
                return r[3:]
            if time.time() > end:
                raise TimeoutError(f'command {op}: no reply')
            time.sleep(0.005)

    def info(self):
        r = self.cmd(INFO)
        if r[:4] != b'ZRP6':
            raise RuntimeError('no answer to our protocol: a stock RTI ZRP-6?')
        i = {'protocol': r[4], 'rescue': bool(r[5]), 'bootloader': r[6], 'slot': r[7]}
        if not i['rescue']:
            i['mac'] = ':'.join(f'{b:02X}' for b in r[8:14])
            i['ip'] = '.'.join(str(b) for b in r[14:18])
            i['build'] = r[18:61].split(b'\0')[0].decode(errors='replace')
        return i

    def read(self, pg, addr, n):
        out = b''
        while n:
            k = min(n, 61)
            out += self.cmd(READ, bytes([pg]) + struct.pack('<HB', addr, k))[:k]
            addr += k
            n -= k
        return out

    def slot(self, s):
        h = self.read(page(s, 0), 0, HDR)
        return {'valid': h[:4] == b'ZRPB', 'gen': h[0x10] | h[0x11] << 8,
                'confirmed': h[0x20] != 0xFF,
                'tries': sum(1 for b in h[0x21:0x24] if b != 0xFF),
                'failed': h[0x24] != 0xFF}

    def ui_present(self):
        return self.read(8, 0, 4) == b'ZUI2'


def progress(done, total, what):
    sys.stdout.write(f'\r  {what}: {100 * done // total:3d}%')
    sys.stdout.flush()


def flash_image(b, img):
    if len(img) != IMAGE_SIZE or img[:4] != b'ZRPB':
        sys.exit('not a v10 image (build/zrp6.bin, 96 KB, "ZRPB")')
    print('  erasing the other slot')
    b.cmd(ERASE, timeout=120)
    chunks = [(o, img[o:o + 56]) for o in range(HDR, IMAGE_SIZE, 56)]
    todo = [(o, c) for o, c in chunks if c != b'\xff' * len(c)]
    for k, (o, c) in enumerate(todo):
        b.cmd(WRITE, struct.pack('<HBB', o & 0xFFFF, o >> 16, len(c)) + c)
        if k % 64 == 0:
            progress(k, len(todo), 'writing')
    progress(1, 1, 'writing')
    print()
    b.cmd(HEADER, bytes([0, 56]) + img[:56])
    b.cmd(HEADER, bytes([56, 8]) + img[56:64])
    print('  checking the CRC and committing')
    b.cmd(FINISH, timeout=120)


def flash_page(b, blob):
    if blob[:4] != b'ZUI2':
        sys.exit('not a web page (build/zrp6-ui.bin, "ZUI2")')
    print('  erasing the page sector')
    b.cmd(UI_ERASE, timeout=60)
    todo = [(o, blob[o:o + 57]) for o in range(UI_HDR, len(blob), 57)]
    for k, (o, c) in enumerate(todo):
        b.cmd(UI_WRITE, struct.pack('<HB', o, len(c)) + c)
        if k % 64 == 0:
            progress(k, len(todo), 'page')
    progress(1, 1, 'page')
    print()
    b.cmd(UI_FINISH, blob[:UI_HDR], timeout=60)


def open_one():
    devs = Box.find()
    if not devs:
        sys.exit('no ZRP-6 on USB: check the cable, then power-cycle the box once '
                 '(or run `wait` and power it)')
    if len(devs) > 1:
        print(f'{len(devs)} boxes found; using the first')
    return Box(devs[0])


def wait_rescue(limit=120):
    """Poll until a box appears, and HOLD it if it is the bootloader."""
    print('waiting for the box in rescue mode', flush=True)
    end = time.time() + limit
    t0 = time.time()
    while time.time() < end:
        for dv in Box.find():
            if b'rescue' not in (dv.get('product_string') or '').encode():
                continue
            b = None
            try:
                b = Box(dv)
                b.cmd(HOLD, timeout=2)
                if b.info()['rescue']:
                    print(f'  held in rescue mode ({time.time() - t0:.1f} s)')
                    return b
            except Exception as e:
                print(f'  {time.time() - t0:.1f} s: {e}', file=sys.stderr, flush=True)
            if b:
                b.close()
        time.sleep(0.02)
    sys.exit('no box in rescue mode')


def show(b):
    i = b.info()
    mode = 'RESCUE (bootloader)' if i['rescue'] else 'firmware'
    print(f'mode       {mode}, protocol {i["protocol"]}, bootloader {i["bootloader"]}')
    if not i['rescue']:
        print(f'firmware   {i["build"]}  (slot {"AB"[i["slot"]]})')
        print(f'network    {i["ip"]}  MAC {i["mac"]}')
    else:
        print(f'keeping    slot {"AB"[i["slot"]]}')
    for s in (0, 1):
        h = b.slot(s)
        st = ('empty' if not h['valid'] else
              f'gen {h["gen"]}, ' + ('failed' if h['failed'] else 'confirmed' if h['confirmed']
                                     else f'on trial ({h["tries"]} boots)'))
        print(f'slot {"AB"[s]}     {st}')
    print(f'web page   {"present" if b.ui_present() else "missing"}')


def main():
    a = sys.argv[1:] or ['info']
    if a[0] == 'info':
        show(open_one())
    elif a[0] == 'wait':
        print('power the box now, cable in')
        show(wait_rescue())
    elif a[0] == 'rescue':
        b = open_one()
        b.cmd(RESCUE)
        b.close()
        show(wait_rescue(30))
    elif a[0] == 'reboot':
        open_one().cmd(REBOOT)
    elif a[0] == 'backup-store':
        b = open_one()
        data = b.read(0xFF, 0x4000, 0x4000)
        open(a[1], 'wb').write(data)
        print(f'{len(data)} bytes to {a[1]}')
    elif a[0] == 'flash':
        wait = '--wait' in a
        files = [f for f in a[1:] if f != '--wait'] or ['build/zrp6-ui.bin', 'build/zrp6.bin']
        b = wait_rescue(180) if wait else open_one()
        i = b.info()
        print(f'{"rescue mode" if i["rescue"] else "firmware " + i["build"]}')
        blobs = [open(f, 'rb').read() for f in files]
        for blob in sorted(blobs, key=lambda x: x[:4] != b'ZUI2'):    # page first
            if blob[:4] == b'ZUI2':
                flash_page(b, blob)
            else:
                flash_image(b, blob)
        print('restarting')
        b.cmd(REBOOT)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
