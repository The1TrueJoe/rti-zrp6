#!/usr/bin/env python3
"""Do what the interface does, over the WebSocket, and check the stack survived.

The browser reaches the broker through the WebSocket on port 80, and that path
— HTTP upgrade, SHA-1, frames — is deeper than raw MQTT on 1883. A retained
replay pushed through WebSocket framing took the stack margin from 26 bytes to
9 without any raw-MQTT test noticing, so this test exists to go down exactly
that path: page load, upgrade, subscribe (retained replay), list rules, toggle
outputs, press a code — several times — and then read zrp6/debug/stackfree.

    python3 firmware/test/test_ws_session.py [host]
"""
import base64
import hashlib
import os
import socket
import struct
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('ZRP6_HOST', '10.0.0.77')
MIN_STACK = 20          # bytes; below this, treat it as a regression

# Every client logs in (docs/MQTT.md): ZRP6_USER / ZRP6_PASS, default admin/admin.
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def connect_body(cid):
    """CONNECT's variable header and payload, with the login."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    return b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(cid) + f(USER) + f(PASS)



def pkt(b0, p):
    n, hdr = len(p), bytearray([b0])
    while True:
        d, n = n % 128, n // 128
        hdr.append(d | (0x80 if n else 0))
        if not n:
            return bytes(hdr) + p


class Ws:
    def __init__(self):
        key = base64.b64encode(os.urandom(16))
        self.s = socket.create_connection((HOST, 80), 8)
        self.s.settimeout(0.4)
        self.s.sendall(b'GET / HTTP/1.1\r\nHost: z\r\nUpgrade: websocket\r\n'
                       b'Connection: Upgrade\r\nSec-WebSocket-Key: ' + key +
                       b'\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n')
        # Usually 73 ms; about one in twenty, right after a previous session,
        # takes most of a second (cause not yet traced). A browser rides it out.
        self.s.settimeout(3)
        r = self.s.recv(1024).decode(errors='replace')
        self.s.settimeout(0.4)
        want = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest()).decode()
        self.ok = f'Sec-WebSocket-Accept: {want}' in r
        self.buf = b''

    def send(self, mqtt):
        m = os.urandom(4)
        n = len(mqtt)
        hdr = b'\x82' + (bytes([0x80 | n]) if n < 126 else b'\xfe' + struct.pack('>H', n))
        self.s.sendall(hdr + m + bytes(b ^ m[i & 3] for i, b in enumerate(mqtt)))

    def frames(self):
        try:
            while True:
                c = self.s.recv(8192)
                if not c:
                    break
                self.buf += c
        except socket.timeout:
            pass
        out, i = [], 0
        while i + 2 <= len(self.buf):
            ln, o = self.buf[i + 1] & 0x7F, 2
            if ln == 126:
                ln, o = struct.unpack('>H', self.buf[i + 2:i + 4])[0], 4
            if i + o + ln > len(self.buf):
                break
            out.append(self.buf[i + o:i + o + ln])
            i += o + ln
        self.buf = self.buf[i:]
        return out

    def close(self):
        self.s.close()


def pub(ws, topic, payload=b''):
    t = topic.encode()
    ws.send(pkt(0x30, len(t).to_bytes(2, 'big') + t + payload))


fails = []
for round_ in range(3):
    body = urllib.request.urlopen(f'http://{HOST}/', timeout=15).read()
    ws = Ws()
    if not ws.ok:
        fails.append(f'round {round_}: WebSocket handshake')
        continue
    ws.send(pkt(0x10, connect_body(b'ui')))
    time.sleep(0.4)
    if not any(f[:4] == b'\x20\x02\x00\x00' for f in ws.frames()):
        fails.append(f'round {round_}: CONNACK')
    t = b'zrp6/#'
    ws.send(pkt(0x82, b'\x00\x01' + len(t).to_bytes(2, 'big') + t + b'\x00'))
    time.sleep(0.8)
    replay = ws.frames()
    pub(ws, 'zrp6/rules/get')
    time.sleep(1.0)
    ws.frames()
    for n in (1, 2, 3):
        pub(ws, f'zrp6/relay/{n}/set', b'ON'); time.sleep(0.3)
        pub(ws, f'zrp6/relay/{n}/set', b'OFF'); time.sleep(0.3)
        pub(ws, f'zrp6/trigger/{n}/set', b'ON'); time.sleep(0.3)
        pub(ws, f'zrp6/trigger/{n}/set', b'OFF'); time.sleep(0.3)
    pub(ws, 'zrp6/ir/1/send', b'nec 0x04 0x08 0'); time.sleep(1.0)
    pub(ws, 'zrp6/remote/code', b'14.0.2.8.0.2.9'); time.sleep(0.8)
    acks = ws.frames()
    ws.close()
    print(f'  round {round_ + 1}: page {len(body)} B, handshake ok, '
          f'{len(replay)} retained frames, {len(acks)} answers')
    time.sleep(0.5)

time.sleep(4)             # the gauge is sampled every ~3 s
s = socket.create_connection((HOST, 1883), 8)
s.settimeout(1.0)
s.sendall(pkt(0x10, connect_body(b'sf')))
time.sleep(0.3)
s.recv(4)
t = b'zrp6/debug/stackfree'
s.sendall(pkt(0x82, b'\x00\x01' + len(t).to_bytes(2, 'big') + t + b'\x00'))
time.sleep(0.6)
d = s.recv(256)
s.close()
free = int(d[d.index(t) + len(t):]) if t in d else -1
print(f'\nstack free after three browser sessions: {free} bytes (floor {MIN_STACK})')
if free < MIN_STACK:
    fails.append(f'stack margin {free} < {MIN_STACK}')
print('PASS' if not fails else 'FAIL: ' + '; '.join(fails))
sys.exit(1 if fails else 0)
