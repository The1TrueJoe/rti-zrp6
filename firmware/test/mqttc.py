"""A small MQTT 3.1.1 client for the bench tests: one socket, QoS 0, a login.

    from mqttc import M
    m = M(b'admin', b'admin', b'my-id', host='10.0.0.77')   # m.ok, m.ack
    m.sub('zrp6/#'); m.pub('zrp6/relay/1/set', 'ON')
    m.msgs(1.0)          # {topic: [payload, ...]} of what arrived meanwhile
    m.ask(topic, payload, answer_topic)   # publish, return the last answer
"""
import os
import socket
import time

HOST = os.environ.get('ZRP6_HOST', '10.0.0.77')


def pkt(b0, p):
    n, h = len(p), bytearray([b0])
    while True:
        d, n = n % 128, n // 128
        h.append(d | (0x80 if n else 0))
        if not n:
            return bytes(h) + p


def s16(b):
    return len(b).to_bytes(2, 'big') + b


class M:
    """One MQTT client. ack is the CONNACK's 4 bytes."""

    def __init__(self, user=b'admin', pw=b'admin', cid=b'st', host=None):
        self.s = socket.create_connection((host or HOST, 1883), 5)
        self.s.settimeout(0.3)
        self.buf = b''
        body = b'\x00\x04MQTT\x04' + bytes([0xC2 if user is not None else 0x02]) + b'\x00\x3c' + s16(cid)
        if user is not None:
            body += s16(user) + s16(pw)
        self.s.sendall(pkt(0x10, body))
        end = time.time() + 3
        while len(self.buf) < 4 and time.time() < end:
            try:
                c = self.s.recv(64)
                if not c:
                    break
                self.buf += c
            except socket.timeout:
                pass
        self.ack, self.buf = self.buf[:4], self.buf[4:]
        self.ok = self.ack == b'\x20\x02\x00\x00'

    def sub(self, f):
        self.s.sendall(pkt(0x82, b'\x00\x01' + s16(f.encode()) + b'\x00'))

    def pub(self, t, p=b''):
        self.s.sendall(pkt(0x30, s16(t.encode()) + (p if isinstance(p, bytes) else p.encode())))

    def msgs(self, wait=1.0):
        end = time.time() + wait
        while time.time() < end:
            try:
                c = self.s.recv(4096)
                if not c:
                    break
                self.buf += c
            except (socket.timeout, OSError):
                pass
        out, b, i = {}, self.buf, 0
        while i + 2 <= len(b):
            rem, mul, j = 0, 1, i + 1
            while j < len(b):
                rem += (b[j] & 0x7F) * mul
                mul *= 128
                j += 1
                if not b[j - 1] & 0x80:
                    break
            if j + rem > len(b):
                break
            if b[i] & 0xF0 == 0x30:
                tl = b[j] << 8 | b[j + 1]
                out.setdefault(b[j + 2:j + 2 + tl].decode(), []).append(b[j + 2 + tl:j + rem])
            i = j + rem
        self.buf = b[i:]
        return out

    def ask(self, topic, payload, answer, wait=4.0):
        """Publish, and return the last payload seen on `answer`."""
        self.pub(topic, payload)
        got = self.msgs(wait).get(answer)
        return got[-1].decode() if got else None

    def close(self):
        self.s.close()
