#!/usr/bin/env python3
"""Network firmware updates on the bench: install, power cuts, rollback.

Needs a v10 box (or v7/v8 with that build's images) and the Kasa bench plug (tools/flash/bench_power.py). Build the
three images first — two ordinary builds that differ only in their ID, and one
that never confirms itself:

  ZRP6_BUILD_ID=release-A firmware/build.sh && cp build/zrp6.bin /tmp/A.bin
  ZRP6_BUILD_ID=release-B firmware/build.sh && cp build/zrp6.bin /tmp/B.bin
  ZRP6_BUILD_ID=noconfirm ZRP6_EXTRA_CFLAGS=-DOTA_NEVER_CONFIRM firmware/build.sh \\
      && cp build/zrp6.bin /tmp/NC.bin
  python3 firmware/test/test_ota.py /tmp/A.bin /tmp/B.bin /tmp/NC.bin

Checks, in order (about 15 minutes):
  1. installing A, then B: each comes back running, confirmed, with a backup;
  2. the power cut 3, 8, 13 and 18 s into an install: it comes back running the
     image being installed, every time;
  3. NC: on trial for three boots, rolled back to the previous image on the
     fourth, and stays there.
Measured 2026-09-25: all pass. docs/UPDATING.md.
"""
import base64
import os
import socket
import subprocess
import sys
import time

HOST = os.environ.get('ZRP6_HOST', '10.0.0.77')
KASA = [os.environ.get('KASA_PY', os.path.expanduser('~/.zrp6/kasaenv/bin/python')),
        'tools/flash/bench_power.py']
A, B, NC = sys.argv[1:4]

# Every client logs in (docs/MQTT.md): ZRP6_USER / ZRP6_PASS, default admin/admin.
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def connect_body(cid):
    """CONNECT's variable header and payload, with the login."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    return b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(cid) + f(USER) + f(PASS)



def pkt(b0, p):
    return bytes([b0, len(p)]) + p


def firmware(timeout=2.0):
    """The retained zrp6/firmware/* topics, or None if the box is not up."""
    try:
        s = socket.create_connection((HOST, 1883), 3)
        s.settimeout(1)
        s.sendall(pkt(0x10, connect_body(b'test')))
        if s.recv(4) != b'\x20\x02\x00\x00':
            return None
        t = b'zrp6/firmware/#'
        s.sendall(pkt(0x82, b'\x00\x01' + len(t).to_bytes(2, 'big') + t + b'\x00'))
        buf, end = b'', time.time() + timeout
        while time.time() < end:
            try:
                c = s.recv(1024)
                if not c:
                    break
                buf += c
            except socket.timeout:
                break
        s.close()
    except OSError:
        return None
    out, i = {}, 0
    while i + 2 <= len(buf):
        rem, mul, j = 0, 1, i + 1
        while True:
            rem += (buf[j] & 0x7F) * mul
            mul *= 128
            j += 1
            if not buf[j - 1] & 0x80:
                break
        if buf[i] & 0xF0 == 0x30:
            tl = buf[j] << 8 | buf[j + 1]
            out[buf[j + 2:j + 2 + tl].decode().split('/')[-1]] = buf[j + 2 + tl:j + rem].decode()
        i = j + rem
    return out if out.get('version') else None


def wait_up(limit=150):
    t = time.time()
    while time.time() - t < limit:
        fw = firmware()
        if fw:
            return fw
        time.sleep(3)
    return None


def upload(path):
    """POST until the box accepts it (a CRC refusal is safe to resend)."""
    img = open(path, 'rb').read()
    for _ in range(4):
        s = socket.create_connection((HOST, 80), 10)
        s.settimeout(90)
        auth = base64.b64encode(USER + b':' + PASS)
        s.sendall(b'POST /update HTTP/1.1\r\nHost: z\r\nAuthorization: Basic ' + auth
                  + b'\r\nContent-Length: ' + str(len(img)).encode() + b'\r\n\r\n' + img)
        r = b''
        try:
            while True:
                c = s.recv(4096)
                if not c:
                    break
                r += c
        except OSError:
            pass
        if b'restarting' in r:
            # The old image answers for another half second before the
            # watchdog resets it; wait for it to go, or the next reading is
            # of the image being replaced.
            end = time.time() + 15
            while time.time() < end and firmware(1):
                time.sleep(0.5)
            return True
        time.sleep(3)
    return False


def power_cycle():
    subprocess.run(KASA + ['cycle', '3'], capture_output=True)
    time.sleep(4)


fails = []


def check(cond, msg):
    print(('  ok    ' if cond else '  FAIL  ') + msg)
    if not cond:
        fails.append(msg)


print('1. install')
names = {}
for path in (A, B):
    check(upload(path), f'{path} accepted')
    fw = wait_up()
    names[path] = fw and fw['version']
    check(fw and fw['state'] == 'running' and fw['backup'] == 'yes',
          f'{path}: back as {fw and fw["version"]}, {fw and fw["state"]}, backup {fw and fw["backup"]}')

print('2. power cut during the install')
for d in (3, 8, 13, 18):
    cur = wait_up()['version']
    path = A if cur == names[B] else B
    upload(path)
    time.sleep(d)
    power_cycle()
    fw = wait_up()
    check(fw and fw['version'] == names[path], f'cut {d:2d} s in: running {fw and fw["version"]}')

print('2b. power cut part-way through the upload itself')
# v10 writes into the slot that is not running, and its magic goes in last:
# a cut mid-upload leaves the running image as it was, and the half-written
# slot is no backup. The next upload then works as ever.
cur = wait_up()['version']
path = A if cur == names[B] else B
import threading
threading.Thread(target=upload, args=(path,), daemon=True).start()
time.sleep(10)
power_cycle()
fw = wait_up()
check(fw and fw['version'] == cur and fw['state'] == 'running' and fw['backup'] == 'no',
      f'cut mid-upload: running {fw and fw["version"]}, {fw and fw["state"]}, backup {fw and fw["backup"]}')
check(upload(path), f'{path} accepted after the cut')
fw = wait_up()
check(fw and fw['version'] == names[path] and fw['backup'] == 'yes',
      f'then installs: {fw and fw["version"]}, backup {fw and fw["backup"]}')

print('3. rollback of an image that never confirms')
before = wait_up()['version']
upload(NC)
fw = wait_up()
check(fw and fw['state'] == 'on trial, boot 1 of 3', f'NC installed: {fw and fw["state"]}')
for k in (2, 3):
    power_cycle()
    fw = wait_up()
    check(fw and fw['state'] == f'on trial, boot {k} of 3', f'boot {k}: {fw and fw["state"]}')
power_cycle()
fw = wait_up()
check(fw and fw['version'] == before and fw['state'] == 'running', f'boot 4: rolled back to {fw and fw["version"]}')
power_cycle()
fw = wait_up()
check(fw and fw['version'] == before, f'and stays: {fw and fw["version"]}')

print('PASS' if not fails else f'FAIL: {len(fails)}')
sys.exit(1 if fails else 0)
