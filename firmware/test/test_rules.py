#!/usr/bin/env python3
"""End-to-end test of remote-button rules, against a real ZRP-6 over MQTT.

Sets rules, reads them back, fires them by publishing a remote code (which the
box treats exactly like a press), checks the outputs answer, stores a
full-length Pronto code, deletes a rule — and then power-cycles the box and
checks the rules are still there. Leaves the box with no rules and all outputs
off.

    python3 firmware/test/test_rules.py [host]

Needs the bench plug (tools/flash/bench_power.py) for the power-cycle step.
"""
import os
import socket
import subprocess
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('ZRP6_HOST', '10.0.0.77')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
KASA = os.path.expanduser('~/.zrp6/kasaenv/bin/python')

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


class Box:
    def __init__(self, cid=b'test'):
        self.s = socket.create_connection((HOST, 1883), 8)
        self.s.settimeout(0.3)
        self.buf = b''
        self.s.sendall(pkt(0x10, connect_body(cid)))
        time.sleep(0.3)
        assert self.s.recv(4) == b'\x20\x02\x00\x00', 'no CONNACK, or login refused'
        t = b'zrp6/#'
        self.s.sendall(pkt(0x82, b'\x00\x01' + len(t).to_bytes(2, 'big') + t + b'\x00'))
        time.sleep(0.8)
        self.messages()                       # discard the retained replay

    def pub(self, topic, payload=b''):
        t = topic.encode() if isinstance(topic, str) else topic
        p = payload.encode() if isinstance(payload, str) else payload
        self.s.sendall(pkt(0x30, len(t).to_bytes(2, 'big') + t + p))

    def messages(self):
        try:
            while True:
                c = self.s.recv(8192)
                if not c:
                    break
                self.buf += c
        except socket.timeout:
            pass
        out = []
        while len(self.buf) >= 2:
            j, rem, m = 1, 0, 1
            while True:
                if j >= len(self.buf):
                    return out
                rem += (self.buf[j] & 0x7F) * m
                m *= 128
                j += 1
                if not self.buf[j - 1] & 0x80:
                    break
            if len(self.buf) < j + rem:
                break
            b0, body = self.buf[0], self.buf[j:j + rem]
            self.buf = self.buf[j + rem:]
            if b0 & 0xF0 == 0x30:
                tl = int.from_bytes(body[:2], 'big')
                out.append((body[2:2 + tl].decode(), body[2 + tl:].decode(errors='replace')))
        return out

    def wait(self, want, secs=8):
        """Collect messages until every (topic, payload) in want has been seen."""
        seen, end = [], time.time() + secs
        while time.time() < end:
            seen += self.messages()
            if all(w in seen for w in want):
                return seen
        return seen

    def close(self):
        self.s.close()


fails = []


def check(name, ok, detail=''):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f'   {detail}' if not ok and detail else ''))
    if not ok:
        fails.append(name)


A = '14.0.2.8.0.2.9'
B = '14.14.15'
RULES = {
    1: f'{A}|ir/1/send|nec 0x04 0x08 0',
    2: f'{A}|relay/2/set|ON',
    3: f'{B}|relay/2/set|OFF',
}
# A realistic long Pronto code (NEC, 34 pairs) — the payload rules must carry.
PRONTO = ('0000 006D 0022 0002 0157 00AC 0015 0016 0015 0016 0015 0041 0015 0016 '
          '0015 0016 0015 0016 0015 0016 0015 0016 0015 0041 0015 0041 0015 0016 '
          '0015 0041 0015 0041 0015 0041 0015 0041 0015 0041 0015 0016 0015 0016 '
          '0015 0016 0015 0041 0015 0016 0015 0016 0015 0016 0015 0016 0015 0041 '
          '0015 0041 0015 0041 0015 0016 0015 0041 0015 0041 0015 0041 0015 0041 '
          '0015 0689 0157 0056 0015 0E94')
RULES[4] = f'16.14.13|ir/6/send|{PRONTO}'


def list_rules(box):
    box.pub('zrp6/rules/get')
    msgs = box.wait([('zrp6/rules/listed', 'done')], 10)
    return {int(t.split('/')[2]): p for t, p in msgs
            if t.startswith('zrp6/rules/') and t.split('/')[2].isdigit()
            and len(t.split('/')) == 3}


box = Box()
print('clearing slots 1-15')
for n in range(1, 16):
    box.pub(f'zrp6/rules/{n}/set')
    box.wait([(f'zrp6/rules/{n}/state', 'deleted')], 10)
check('start empty', list_rules(box) == {})

print('saving rules')
for n, text in RULES.items():
    box.pub(f'zrp6/rules/{n}/set', text)
    got = box.wait([(f'zrp6/rules/{n}/state', 'saved')], 10)
    check(f'rule {n} saved ({len(text)} bytes)', (f'zrp6/rules/{n}/state', 'saved') in got, got[-3:])
box.pub('zrp6/rules/5/set', 'not a rule')
got = box.wait([('zrp6/rules/5/state', 'invalid')], 5)
check('malformed rule refused', ('zrp6/rules/5/state', 'invalid') in got, got[-3:])

listed = list_rules(box)
for n, text in RULES.items():
    check(f'rule {n} reads back exactly', listed.get(n) == text,
          f'{listed.get(n)!r:.80}')
check('nothing extra listed', set(listed) == set(RULES), sorted(listed))

print('firing by publishing remote codes')
box.pub('zrp6/remote/code', A)
got = box.wait([('zrp6/ir/1/sent', 'nec 0x04 0x08 0'), ('zrp6/relay/2/state', 'ON'),
                ('zrp6/rules/fired', '1'), ('zrp6/rules/fired', '2')], 8)
check(f'{A} -> IR 1 sent', ('zrp6/ir/1/sent', 'nec 0x04 0x08 0') in got, got)
check(f'{A} -> relay 2 ON', ('zrp6/relay/2/state', 'ON') in got, got)
check(f'{A} -> both rules fired', ('zrp6/rules/fired', '1') in got
      and ('zrp6/rules/fired', '2') in got, got)
box.pub('zrp6/remote/code', B)
got = box.wait([('zrp6/relay/2/state', 'OFF'), ('zrp6/rules/fired', '3')], 8)
check(f'{B} -> relay 2 OFF', ('zrp6/relay/2/state', 'OFF') in got, got)
box.pub('zrp6/remote/code', '16.14.13')
got = box.wait([('zrp6/rules/fired', '4')], 8)
check('Pronto rule fires IR 6', ('zrp6/rules/fired', '4') in got
      and any(t == 'zrp6/ir/6/sent' for t, _ in got), got)
box.pub('zrp6/remote/code', '99.99')
got = box.wait([('zrp6/rules/fired', 'x')], 2)
check('an unknown code fires nothing', not any(t == 'zrp6/rules/fired' for t, _ in got), got)

print('deleting rule 3')
box.pub('zrp6/rules/3/set')
got = box.wait([('zrp6/rules/3/state', 'deleted')], 10)
check('rule 3 deleted', ('zrp6/rules/3/state', 'deleted') in got, got[-3:])
box.close()

print('power-cycling the box')
subprocess.run([KASA, os.path.join(ROOT, 'tools/flash/bench_power.py'), 'cycle', '5'],
               check=True, stdout=subprocess.DEVNULL)
for _ in range(30):
    time.sleep(2)
    try:
        box = Box(b'after')
        break
    except OSError:
        continue
else:
    sys.exit('the box did not come back after the power cycle')
listed = list_rules(box)
want = {n: t for n, t in RULES.items() if n != 3}
check('rules survive a power cycle', listed == want,
      f'{ {k: v[:30] for k, v in listed.items()} }')
box.pub('zrp6/remote/code', A)
got = box.wait([('zrp6/ir/1/sent', 'nec 0x04 0x08 0'), ('zrp6/relay/2/state', 'ON')], 8)
check('and still fire after it', ('zrp6/relay/2/state', 'ON') in got, got)

print('cleaning up')
for n in list(want):
    box.pub(f'zrp6/rules/{n}/set')
    box.wait([(f'zrp6/rules/{n}/state', 'deleted')], 10)
box.pub('zrp6/relay/2/set', 'OFF')
box.wait([('zrp6/relay/2/state', 'OFF')], 4)
box.pub('zrp6/rules/get')
got = box.wait([('zrp6/rules/listed', 'done')], 10)
check('left with no rules', not any(t.split('/')[2].isdigit() for t, _ in got
                                     if t.startswith('zrp6/rules/') and len(t.split('/')) == 3))
time.sleep(4)
sf = [p for t, p in box.wait([('zrp6/debug/stackfree', '?')], 1) if t == 'zrp6/debug/stackfree']
box.close()
b2 = Box(b'sf')
b2.s.sendall(pkt(0x82, b'\x00\x02\x00\x14zrp6/debug/stackfree\x00'))
sf = [p for t, p in b2.wait([], 1.5) if t == 'zrp6/debug/stackfree']
b2.close()
print(f'\nstack free after all of this: {sf[-1] if sf else "?"} bytes')

print(f'\n{"PASS" if not fails else "FAIL: " + ", ".join(fails)}')
sys.exit(1 if fails else 0)
