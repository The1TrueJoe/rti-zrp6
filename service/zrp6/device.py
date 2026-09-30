"""The ZRP-6's binary protocol, over UDP.

The board answers on 10.0.0.77:5005. Everything here is a thin wrapper over one
request/response exchange; anything that needs judgement belongs a layer up.

Command letters match firmware/src/proto.c.
"""
import os
import socket
import time

HOST = os.environ.get('ZRP6_HOST', '10.0.0.77')
PORT = int(os.environ.get('ZRP6_PORT', '5005'))

# The carrier half-period, in the firmware's dly() units. 30 is 38.0 kHz,
# measured optically against a Global Cache IR Learner -- not derived.
#
# This value belongs to a particular firmware build: it counts the cost of the
# instructions around the delay as well as the delay itself. Splitting the
# monolithic firmware into modules made the inner loop faster (a direct Port A
# write instead of a call), and the same dly(15) that had been 38 kHz became
# 61 kHz. RE-MEASURE against a receiver after touching ir.c; timing it over the
# network measures the loop rather than the light and reads high.
IR_HALF = int(os.environ.get('ZRP6_IR_HALF', '30'), 0)

# The firmware takes arg|0xC0 as the P3 value for a gated access, so 0x3D means
# 0xFD: clear ONLY P3.1. Clearing more also pulls P3.0 (contact input 1) and
# P3.5 (SIGNAL IN) low, which shows as a dim flicker on the panel and injects
# false contact readings.
P3_GATE = 0x3D


class Zrp6:
    def __init__(self, host=HOST, port=PORT, timeout=1.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.addr = (host, port)

    # ---- transport -------------------------------------------------------
    def tx(self, data, retries=4, timeout=None):
        if timeout:
            self.sock.settimeout(timeout)
        try:
            last = None
            for _ in range(retries):
                try:
                    self.sock.sendto(bytes(data), self.addr)
                    return self.sock.recvfrom(2048)[0]
                except socket.timeout as e:
                    last = e
            raise TimeoutError('no reply from %s:%d' % self.addr) from last
        finally:
            self.sock.settimeout(1.0)

    # ---- memory ----------------------------------------------------------
    def banner(self):
        return self.tx(b'P')[1:].decode('ascii', 'replace')

    def read(self, addr, n=1):
        return self.tx([ord('R'), addr >> 8, addr & 0xFF, n])[1:]

    def write(self, addr, data):
        if isinstance(data, int):
            data = [data]
        self.tx([ord('W'), addr >> 8, addr & 0xFF, len(data)] + list(data))

    def code(self, addr, n=16):
        return self.tx([ord('A'), addr >> 8, addr & 0xFF, n])[1:]

    def read_gated(self, addr, n=1, p3=P3_GATE):
        """Read with P3 forced. The 16C554 needs P3.1 low or the whole window
        reads back as address echo."""
        return self.tx([ord('U'), p3, addr >> 8, addr & 0xFF, n])[1:]

    def write_gated(self, addr, data, p3=P3_GATE):
        if isinstance(data, int):
            data = [data]
        return self.tx([ord('V'), p3, addr >> 8, addr & 0xFF,
                        len(data)] + list(data))[1:]

    def pwrite(self, addr, val):
        """One peripheral write, gated and staged in IRAM by the firmware."""
        return self.tx([ord('B'), addr >> 8, addr & 0xFF, val])[1:]

    # ---- ports -----------------------------------------------------------
    def sfr(self, which=0xFF, val=0):
        """which: 0 P1, 1 P3, 2 P4, 3-7 the port special-function selects,
        8 BUSCON. 0xFF reads without writing. Returns (P1, P3, P4)."""
        return tuple(self.tx([ord('F'), which, val])[1:4])

    def relay(self, n, on):
        return self.tx([ord('G'), n, 1 if on else 0])[1:]

    def trigger(self, n, on):
        return self.tx([ord('G'), n + 3, 1 if on else 0])[1:]

    def status(self):
        r = self.tx(b'S')[1:]
        names = ('P1', 'P3', 'P4', 'DIN_C', 'DOUT_C', 'contacts',
                 'S0_SR', 'S0_IR')
        return dict(zip(names, r))

    def contacts(self):
        """(1, 2, 3) -> True when OPEN. They read high at rest."""
        c = self.status()['contacts']
        return {1: bool(c & 1), 2: bool(c & 2), 3: bool(c & 4)}

    # ---- infrared --------------------------------------------------------
    def ir(self, pairs, port=1, half=IR_HALF):
        """pairs: [(mark, space), ...] in CARRIER PERIODS -- which is exactly
        how Pronto expresses timings, so a database code goes out unmodified.

        Port 7 is HIGH OUT: inverted in hardware and with no panel LED. The
        firmware handles both, and lights the port's LED only while it fires.
        """
        pkt = [ord('I'), 0, 0, port, len(pairs), half]
        for m, s in pairs:
            pkt += [m >> 8, m & 0xFF, s >> 8, s & 0xFF]
        return self.tx(pkt, timeout=8.0)

    # ---- serial ----------------------------------------------------------
    def serial_config(self, ch=0, baud=9600, lcr=0x03):
        div = int(round(14745600 / (16.0 * baud)))
        return self.serial_write(b'', ch=ch, baud=baud, lcr=lcr)

    def serial_write(self, data, ch=0, baud=9600, lcr=0x03):
        div = int(round(14745600 / (16.0 * baud)))
        if isinstance(data, str):
            data = data.encode()
        last = None
        for i in range(0, max(len(data), 1), 16):
            chunk = data[i:i + 16]
            last = self.tx([ord('J'), ch, 0, div & 0xFF, (div >> 8) & 0xFF,
                            lcr, len(chunk)] + list(chunk), timeout=20.0)
        return last[1:] if last else None

    def serial_read(self, ch=0, timeout=0.05):
        """Drain the receive holding register. Channel 1 (RS-232 port 2) has
        unreliable register reads, so its LSR is not trusted -- see docs."""
        base = 0x3000 + ch * 8
        got = bytearray()
        deadline = time.time() + timeout
        while time.time() < deadline:
            regs = self.read_gated(base, 8)
            if len(regs) < 8 or not (regs[5] & 0x01):
                break
            got.append(self.read_gated(base, 1)[0])
            deadline = time.time() + 0.05
        return bytes(got)

    # ---- signal in -------------------------------------------------------
    def capture(self, maxn=200):
        """Run-length capture of SIGNAL IN. Returns (start_level, [ticks]).

        This BLOCKS the board: it stops answering UDP until the capture ends,
        while still answering ICMP because the W5100 does ping in hardware.
        Anything polling this must leave gaps.
        """
        r = self.tx([ord('L'), 1, 0x20, maxn], timeout=20.0)[1:]
        if not r:
            return None, []
        runs = [(r[i] << 8) | r[i + 1] for i in range(1, len(r) - 1, 2)]
        return r[0], runs

    def activity(self, loops=40):
        """Which input bits moved during a window. (changed, level) of P1/P3/C."""
        r = self.tx([ord('M'), loops], timeout=20.0)[1:]
        return dict(zip(('P1', 'P3', 'DIN_C'), r[0:3])), \
               dict(zip(('P1', 'P3', 'DIN_C'), r[3:6]))

    # ---- timing ----------------------------------------------------------
    def spin_seconds(self, k=60):
        """Wall time for k*65536 DJNZ pairs, less the round trip. Divide to get
        the machine-cycle rate -- this is how the 8.95 MHz figure was taken."""
        rtt = sorted(self._rtt() for _ in range(5))[2]
        t = time.time()
        self.tx([ord('T'), k], retries=1, timeout=60.0)
        return (time.time() - t) - rtt

    def _rtt(self):
        t = time.time()
        self.tx(b'P')
        return time.time() - t


def nec_pairs(addr, cmd, us_per_period=26.316):
    """A standard NEC frame as (mark, space) carrier-period counts."""
    def c(us):
        return max(1, int(round(us / us_per_period)))
    bits = []
    for byte in (addr, (~addr) & 0xFF, cmd, (~cmd) & 0xFF):
        for i in range(8):                      # LSB first
            bits.append((byte >> i) & 1)
    out = [(c(9000), c(4500))]
    for b in bits:
        out.append((c(560), c(1690) if b else c(560)))
    out.append((c(560), c(10000)))
    return out


def pronto(hexstr):
    """Parse Pronto (CCF) hex -> (carrier_hz, once_pairs, repeat_pairs).

    Pronto already counts carrier periods, which is what the firmware's IR
    command wants, so codes copied from a remote database need no conversion.
    """
    w = [int(x, 16) for x in hexstr.replace(',', ' ').split()]
    if len(w) < 4 or w[0] != 0:
        raise ValueError('not a learned Pronto code (word 0 must be 0000)')
    carrier = 1000000.0 / (w[1] * 0.241246)
    once, rep = w[2], w[3]
    body = w[4:]
    if len(body) < 2 * (once + rep):
        raise ValueError('truncated: %d words for %d+%d pairs'
                         % (len(body), once, rep))
    pairs = [(body[i], body[i + 1]) for i in range(0, len(body), 2)]
    return carrier, pairs[:once], pairs[once:once + rep]
