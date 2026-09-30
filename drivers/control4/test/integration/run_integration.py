#!/usr/bin/env python3
"""End-to-end test of the driver's MQTT client against a real mosquitto.

Starts a private mosquitto on 127.0.0.1 (never the box) with a password file,
runs the real driver.lua under the mock C4 (harness.lua) with its network
binding bridged to a real TCP socket, and checks what arrives on the broker
and what the driver tells Control4.

    python3 test/integration/run_integration.py build/stage/driver.lua [lua|luajit ...]
"""
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/stage/driver.lua')
INTERPRETERS = sys.argv[2:] or ['lua']
HOST = '127.0.0.1'


def free_port():
    s = socket.socket()
    s.bind((HOST, 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Broker:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix='zrp6-mqtt-')
        self.port = free_port()
        pw = os.path.join(self.dir, 'passwd')
        open(pw, 'w').close()
        os.chmod(pw, 0o600)
        subprocess.run(['mosquitto_passwd', '-b', pw, 'admin', 'admin'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        conf = os.path.join(self.dir, 'mosquitto.conf')
        with open(conf, 'w') as f:
            f.write(f'listener {self.port} {HOST}\nallow_anonymous false\npassword_file {pw}\n'
                    'persistence false\nlog_dest stderr\n')
        self.proc = subprocess.Popen(['mosquitto', '-c', conf], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        for _ in range(50):
            try:
                socket.create_connection((HOST, self.port), 0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        self.sub_log = os.path.join(self.dir, 'sub.log')
        self.sub = subprocess.Popen(['mosquitto_sub', '-h', HOST, '-p', str(self.port), '-u', 'admin', '-P', 'admin',
                                     '-t', 'zrp6/#', '-v'], stdout=open(self.sub_log, 'w'))
        time.sleep(0.3)

    def pub(self, topic, payload, retain=False):
        cmd = ['mosquitto_pub', '-h', HOST, '-p', str(self.port), '-u', 'admin', '-P', 'admin', '-t', topic, '-m', payload]
        if retain:
            cmd.append('-r')
        subprocess.run(cmd, check=True)

    def seen(self):
        with open(self.sub_log) as f:
            return f.read()

    def stop(self):
        for p in (self.sub, self.proc):
            p.terminate()
            p.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


class Driver:
    """Runs harness.lua and plays the controller's network stack for it."""

    def __init__(self, interp, broker, **props):
        args = [interp, os.path.join(HERE, 'harness.lua'), DRIVER, f'MQTT Port={broker.port}']
        args += [f'{k}={v}' for k, v in props.items()]
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.sock = None
        self.port = broker.port
        self.pending = []  # events for the driver (ONLINE / OFFLINE / DATA)
        self.read_until_end()

    def read_until_end(self):
        results = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('harness exited')
            line = line.rstrip('\n')
            if line == 'END':
                return results
            cmd, _, rest = line.partition(' ')
            if cmd == 'CONNECT':
                host, port = rest.split()
                try:
                    self.sock = socket.create_connection((host, int(port)), 2)
                    self.sock.setblocking(False)
                    self.pending.append('ONLINE')
                except OSError:
                    self.pending.append('OFFLINE')
            elif cmd == 'SEND':
                if self.sock:
                    self.sock.setblocking(True)
                    self.sock.sendall(bytes.fromhex(rest))
                    self.sock.setblocking(False)
            elif cmd == 'CLOSE':
                if self.sock:
                    self.sock.close()
                    self.sock = None
            elif cmd == 'RESULT':
                results.append(bytes.fromhex(rest).decode('utf-8', 'replace'))

    def send(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        return self.read_until_end()

    def pump(self):
        if self.sock:
            r, _, _ = select.select([self.sock], [], [], 0.05)
            if r:
                try:
                    data = self.sock.recv(65536)
                except OSError:
                    data = b''
                if data:
                    self.pending.append('DATA ' + data.hex())
                else:
                    self.sock.close()
                    self.sock = None
                    self.pending.append('OFFLINE')
        else:
            time.sleep(0.05)
        while self.pending:
            self.send(self.pending.pop(0))

    def step(self, ms, rounds=1):
        for _ in range(rounds):
            self.send(f'TICK {ms}')
            self.pump()

    def lua(self, code):
        res = self.send('EXEC ' + code.encode().hex())
        return res[0] if res else None

    def wait(self, code, want, timeout=5.0, tick=50):
        end = time.time() + timeout
        got = None
        while time.time() < end:
            got = self.lua(code)
            if got == want:
                return got
            self.step(tick)
        raise AssertionError(f'{code!r}: wanted {want!r}, got {got!r}')

    def quit(self):
        try:
            self.p.stdin.write('QUIT\n')
            self.p.stdin.flush()
        except OSError:
            pass
        self.p.wait(5)
        if self.sock:
            self.sock.close()


passed = failed = 0


def check(name, cond, detail=''):
    global passed, failed
    if cond:
        passed += 1
        print(f'  ok    {name}')
    else:
        failed += 1
        print(f'  FAIL  {name} {detail}')


def run(interp):
    print(f'mosquitto integration ({interp})')
    broker = Broker()
    try:
        broker.pub('zrp6/relay/1/state', 'ON', retain=True)
        broker.pub('zrp6/sense/2/state', 'CLOSED', retain=True)
        for n in range(1, 8):
            broker.pub(f'zrp6/ir/{n}/mode', 'ir', retain=True)
        broker.pub('zrp6/serial/1/config', '9600 8N1', retain=True)
        broker.pub('zrp6/serial/2/config', '9600 8N1', retain=True)

        drv = Driver(interp, broker)
        drv.step(600)
        drv.wait('return d.m.CLIENT.state', 'UP')
        check('logs in to mosquitto with username/password', True)
        drv.step(100, rounds=10)
        proxy = drv.lua('local t={} for _,c in ipairs(C4.proxy) do t[#t+1]=c.binding..":"..c.command end return table.concat(t," ")')
        check('retained relay 1 ON -> binding 301 STATE_CLOSED', '301:STATE_CLOSED' in proxy, proxy)
        check('retained contact 2 CLOSED -> binding 402 STATE_CLOSED', '402:STATE_CLOSED' in proxy, proxy)
        drv.step(500, rounds=4)  # settle: everything already matches
        check('no port-config writes when the broker already agrees', '/set' not in broker.seen(), broker.seen())

        drv.lua('env.ReceivedFromProxy(302, "CLOSE", {}) '
                'env.ReceivedFromProxy(101, "PULSE", {data="<pattern>0000 006D 0000 0001 0010 0020</pattern><repeatcount>2</repeatcount>"}) '
                'env.ReceivedFromProxy(201, "SEND", {DATA="PWR ON"}) return true')
        drv.step(50, rounds=10)
        time.sleep(0.3)
        seen = broker.seen()
        check('relay CLOSE -> zrp6/relay/2/set ON', 'zrp6/relay/2/set ON' in seen, seen)
        check('IR PULSE -> zrp6/ir/1/send with repeats unrolled',
              'zrp6/ir/1/send 0000 006D 0002 0000 0010 0020 0010 0020' in seen, seen)
        check('serial data -> zrp6/serial/1/tx', 'zrp6/serial/1/tx PWR ON' in seen, seen)

        broker.pub('zrp6/relay/2/state', 'ON')
        drv.step(50, rounds=10)
        proxy = drv.lua('local t={} for _,c in ipairs(C4.proxy) do t[#t+1]=c.binding..":"..c.command end return table.concat(t," ")')
        check('live relay 2 ON -> binding 302 STATE_CLOSED', '302:STATE_CLOSED' in proxy, proxy)

        drv.lua('m.REMOTES.Arm("Off") return true')
        broker.pub('zrp6/remote/code', 'p16.16.14.12.15:14.0.2.8')
        drv.wait('return tostring(d.m.REMOTES.data.buttons["p16.16.14.12.15:14.0.2.8"] ~= nil)', 'true')
        drv.lua('m.REMOTES.Upsert({code="p16.16.14.12.15:14.0.2.8", actions={{room=10, action="ROOM_OFF"}}}) C4.deviceCalls = {} return true')
        broker.pub('zrp6/remote/code', 'p16.16.14.12.15:14.0.2.8')
        got = drv.wait('return C4.deviceCalls[1] and C4.deviceCalls[1].command or "none"', 'ROOM_OFF')
        check('learned remote button runs ROOM_OFF in the Den', got == 'ROOM_OFF')

        drv.step(1000, rounds=16)  # 16 s of mock time: one PINGREQ
        drv.step(50, rounds=10)
        state = drv.lua('return d.m.CLIENT.state .. " " .. tostring(C4.now - d.m.CLIENT.lastRx < 2000)')
        check('PINGREQ answered by the broker', state == 'UP true', state)

        drv.lua('env.OnDriverDestroyed() return true')
        drv.quit()

        bad = Driver(interp, broker, Password='nope')
        bad.step(600)
        bad.wait('return d.m.CLIENT.state', 'WAIT')
        err = bad.lua('return C4.props["Last Error"] or ""')
        check('wrong password -> CONNACK refused, driver backs off', 'Login refused' in err, err)
        bad.quit()
    finally:
        broker.stop()


if __name__ == '__main__':
    for interp in INTERPRETERS:
        if shutil.which(interp):
            run(interp)
        else:
            print(f'skipping {interp}: not installed')
    print(f'\n{passed} passed, {failed} failed')
    sys.exit(1 if failed else 0)
