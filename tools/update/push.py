#!/usr/bin/env python3
"""Install firmware and/or the web page on a ZRP-6 over the network — no JTAG.

  push.py [host] [file ...]     defaults: 10.0.0.77, build/zrp6.bin build/zrp6-ui.bin

Logs in as ZRP6_USER / ZRP6_PASS (default admin/admin), for MQTT and for the
upload's HTTP Basic auth.

Each file is recognised by its header: build/zrp6.bin (firmware, "ZRPB",
96 KB) goes to /update, is written into the slot that is not running, and the
box restarts into it; build/zrp6-ui.bin (the web page, "ZUI2") goes to /ui and
is live at once. The page goes first when both are given, so the new firmware
comes up with its page already there. Needs the v10 PLD and bootloader 3 (a
v8 box takes the 64 KB "ZRP6" image of a v8 build). docs/UPDATING.md has the rest.
"""
import base64
import http.client
import os
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else '10.0.0.77'
paths = sys.argv[2:] or ['build/zrp6-ui.bin', 'build/zrp6.bin']

# Every client logs in (docs/MQTT.md): ZRP6_USER / ZRP6_PASS, default admin/admin.
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def connect_body(cid):
    """CONNECT's variable header and payload, with the login."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    return b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(cid) + f(USER) + f(PASS)
files = []
for path in paths:
    data = open(path, 'rb').read()
    if (data[:4] == b'ZRPB' and len(data) == 98304) or (data[:4] == b'ZRP6' and len(data) == 65536):
        files.append(('firmware', path, data))      # v10 (slots) or v8
    elif data[:4] == b'ZUI2' and 16 < len(data) <= 32768:
        files.append(('page', path, data))
    else:
        sys.exit(f'{path}: neither a firmware image (ZRPB 96 KB, or v8 ZRP6 64 KB) nor a web page (ZUI2)')
files.sort(key=lambda f: f[0] != 'page')          # the page first


def firmware(timeout=4.0):
    """The retained zrp6/firmware/* topics, or None if the box is not up."""
    def pkt(b0, p):
        return bytes([b0, len(p)]) + p
    try:
        s = socket.create_connection((host, 1883), 3)
        s.settimeout(1)
        s.sendall(pkt(0x10, connect_body(b'push')))
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
            out[buf[j + 2:j + 2 + tl].decode()] = buf[j + 2 + tl:j + rem].decode()
        i = j + rem
    return out


def post(route, data):
    """POST, resending on a CRC refusal: that means the bytes the box received
    were not the bytes sent (the board's D4 fault), and nothing was written."""
    for attempt in range(1, 4):
        t0 = time.time()
        c = http.client.HTTPConnection(host, 80, timeout=90)
        c.request('POST', route, body=data, headers={
            'Content-Type': 'application/octet-stream',
            'Authorization': 'Basic ' + base64.b64encode(USER + b':' + PASS).decode()})
        r = c.getresponse()
        answer = r.read().decode(errors='replace').strip()
        print(f'box: {r.status} {answer}  ({time.time() - t0:.1f} s)')
        if 'CRC' not in answer:
            return r.status, answer
        time.sleep(1)
    return r.status, answer


before = firmware()
print(f'{host}: running {before.get("zrp6/firmware/version", "?") if before else "? (no answer on MQTT)"}')
for kind, path, data in files:
    print(f'sending {kind} {path} ({len(data)} bytes, crc {data[4:8][::-1].hex()})...')
    status, answer = post('/ui' if kind == 'page' else '/update', data)
    if status != 200:
        sys.exit(1)
    if kind == 'page' or 'restarting' not in answer:
        continue
    print('waiting for it to install and come back...', end='', flush=True)
    end = time.time() + 120
    time.sleep(5)
    while time.time() < end:
        fw = firmware(2)
        if fw and fw.get('zrp6/firmware/version'):
            print(f' up after {time.time() - end + 125:.0f} s')
            for k in ('version', 'state', 'backup', 'ui'):
                print(f'  {k:8s} {fw.get("zrp6/firmware/" + k, "?")}')
            break
        print('.', end='', flush=True)
        time.sleep(3)
    else:
        sys.exit('\nno answer after 2 minutes: check the status LED (red = installing or restoring)')
