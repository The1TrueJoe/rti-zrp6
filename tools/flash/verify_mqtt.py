#!/usr/bin/env python3
"""Prove a freshly flashed ZRP-6 is up: MQTT CONNECT on 1883, subscribe to
<base>/status, and expect the retained "online". Plain sockets, no libraries.

  verify_mqtt.py [host] [base]      exit 0 when up
"""
import os, socket, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else '10.0.0.77'
base = (sys.argv[2] if len(sys.argv) > 2 else 'zrp6').encode()

# Every client logs in (docs/MQTT.md): ZRP6_USER / ZRP6_PASS, default admin/admin.
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def connect_body(cid):
    """CONNECT's variable header and payload, with the login."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    return b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(cid) + f(USER) + f(PASS)

def pkt(b0, p):
    return bytes([b0, len(p)]) + p

try:
    s = socket.create_connection((host, 1883), 4)
    s.settimeout(3)
    s.sendall(pkt(0x10, connect_body(b'verify')))
    r = s.recv(4)
    if r[:2] != b'\x20\x02':
        sys.exit('no CONNACK')
    if r[3:4] != b'\x00':
        sys.exit('login refused (ZRP6_USER / ZRP6_PASS)')
    t = base + b'/status'
    s.sendall(pkt(0x82, b'\x00\x01' + len(t).to_bytes(2, 'big') + t + b'\x00'))
    buf, end = b'', time.time() + 3
    while time.time() < end and b'online' not in buf:
        try:
            buf += s.recv(512)
        except socket.timeout:
            break
    s.close()
except OSError as e:
    sys.exit(f'MQTT: {e}')
if b'online' not in buf:
    sys.exit('connected, but no retained status/online')
print(f'{host}: MQTT up, {base.decode()}/status = online')
