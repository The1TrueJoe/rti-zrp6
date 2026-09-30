#!/usr/bin/env python3
"""The four-socket budget and who gets it (ws.c, docs/MQTT.md):

  1. MQTT clients are never dropped;
  2. a socket always listens on 1883, so the next MQTT client gets in;
  3. a socket always listens on 80, so the page loads;
  4. browser WebSockets give way, oldest first — but the newest browser is
     never dropped just to reopen the web door, only for MQTT.

A browser that gives way is sent WebSocket close code 4000.

    python3 firmware/test/test_sockets.py [host]
"""
import base64
import os
import socket
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('ZRP6_HOST', '10.0.0.77')
BUMP = b'\x88\x02\x0f\xa0'

# Every client logs in (docs/MQTT.md): ZRP6_USER / ZRP6_PASS, default admin/admin.
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def connect_body(cid):
    """CONNECT's variable header and payload, with the login."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    return b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(cid) + f(USER) + f(PASS)



def pkt(b0, p):
    return bytes([b0, len(p)]) + p


class Browser:
    """A tab: loads the page, then holds an MQTT-over-WebSocket session."""
    born = 0

    def __init__(self, name):
        self.name = name
        self.buf = b''
        key = base64.b64encode(os.urandom(16))
        self.s = socket.create_connection((HOST, 80), 8)
        self.s.settimeout(3)
        self.s.sendall(b'GET / HTTP/1.1\r\nHost: z\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                       b'Sec-WebSocket-Key: ' + key + b'\r\nSec-WebSocket-Version: 13\r\n'
                       b'Sec-WebSocket-Protocol: mqtt\r\n\r\n')
        r = b''
        while b'\r\n\r\n' not in r:
            c = self.s.recv(1024)
            if not c:
                raise OSError('closed during handshake')
            r += c
        m = os.urandom(4)
        body = pkt(0x10, connect_body(name.encode()[:2].ljust(2, b'x')))
        self.s.sendall(b'\x82' + bytes([0x80 | len(body)]) + m + bytes(b ^ m[i & 3] for i, b in enumerate(body)))

    def bumped(self):
        self.s.settimeout(0.3)
        try:
            while True:
                c = self.s.recv(256)
                if not c:
                    break
                self.buf += c
        except (socket.timeout, OSError):
            pass
        return BUMP in self.buf


class Mqtt:
    def __init__(self, name):
        self.s = socket.create_connection((HOST, 1883), 5)
        self.s.settimeout(3)
        self.s.sendall(pkt(0x10, connect_body(name.encode()[:2])))
        self.ok = self.s.recv(4) == b'\x20\x02\x00\x00'

    def alive(self):
        try:
            self.s.sendall(b'\xc0\x00')                    # PINGREQ
            self.s.settimeout(2)
            return self.s.recv(8)[:1] == b'\xd0'
        except OSError:
            return False


def page_loads():
    """Up to three tries a second and a half apart, as a browser retries:
    right after a bump the web door can be closed for about a second."""
    for _ in range(3):
        try:
            return len(urllib.request.urlopen(f'http://{HOST}/', timeout=8).read()) > 1000
        except Exception:
            time.sleep(1.5)
    return False


fails = []


def check(cond, msg):
    print(('  ok    ' if cond else '  FAIL  ') + msg)
    if not cond:
        fails.append(msg)


def settle():
    # a bump waits for sockets still closing, and a bumped client that does
    # not answer the close (this script) is cut off after a second
    time.sleep(4.5)


print('two browsers and the doors')
check(page_loads(), 'page loads')
a = Browser('ta'); time.sleep(0.5)
check(page_loads(), 'page loads with one browser')
b = Browser('tb'); settle()
check(not a.bumped() and not b.bumped(), 'two browsers stay')
check(page_loads(), 'page still loads (web door)')

print('an MQTT client arrives')
c = Mqtt('mc'); settle()
check(c.ok, 'MQTT client gets in on 1883')
check(a.bumped(), 'the oldest browser gave way (close 4000)')
check(not b.bumped(), 'the newer browser stays')
check(page_loads(), 'page still loads')

print('another browser')
d = Browser('td'); settle()
check(b.bumped(), 'the older browser gave way to the newest')
check(not d.bumped(), 'the newest stays')
check(c.alive(), 'the MQTT client is untouched')

print('more MQTT clients')
e = Mqtt('me'); settle()
check(e.ok and c.alive(), 'a second MQTT client gets in, the first stays')
check(not d.bumped(), 'the only browser is not dropped for the web door')
f = Mqtt('mf'); settle()
check(f.ok, 'a third MQTT client gets in')
check(d.bumped(), 'the browser gave way to MQTT')
check(c.alive() and e.alive() and f.alive(), 'all three MQTT clients alive')

for x in (a, b, d):
    x.s.close()
for x in (c, e, f):
    x.s.close()
time.sleep(2.5)
check(page_loads(), 'page loads after everyone leaves')
check(Mqtt('mz').ok, 'MQTT on 1883 after everyone leaves')

print('PASS' if not fails else 'FAIL: ' + '; '.join(fails))
sys.exit(1 if fails else 0)
