#!/usr/bin/env python3
"""zrp6d — exposes every ZRP-6 input and output on MQTT.

    zrp6d.py [--broker HOST] [--port 1883] [--base zrp6] [--device 10.0.0.77]

Topic tree. Anything ending `/set` is a command in; everything else is state
out, and state is RETAINED so a UI that connects later sees the world as it is
rather than an empty page.

    zrp6/status                     online | offline        (retained, LWT)
    zrp6/relay/<1-3>/set            ON | OFF
    zrp6/relay/<1-3>/state          ON | OFF                (retained ack)
    zrp6/trigger/<1-3>/set          ON | OFF
    zrp6/trigger/<1-3>/state        ON | OFF                (retained ack)
    zrp6/sense/<1-3>/state          OPEN | CLOSED           (retained, on change)
    zrp6/ir/<1-7>/send              "nec <addr> <cmd>" | Pronto hex
    zrp6/ir/<1-7>/sent              the code, echoed after transmission
    zrp6/serial/<1-2>/tx            bytes to send
    zrp6/serial/<1-2>/rx            bytes received
    zrp6/remote/press               button name, or the raw code if unlearned
    zrp6/remote/code                the raw code, always
    zrp6/remote/learn               name to attach to the next press seen
    zrp6/remote/codes               the learned name->code map  (retained)
    zrp6/automation/rules           the rule set, JSON           (retained)
    zrp6/automation/rules/set       replace the rule set
    zrp6/automation/fired           name of a rule that just ran

Automations run HERE, not in the browser. This box sits at a television; rules
that only fire while a config page happens to be open would be useless.

Every relay and trigger command answers with its `/state` whether or not the
value changed, so a caller never has to guess whether a command landed.

Why this is a service and not MQTT inside the 8032: reading RTI remotes needs
~7 us sampling in a blocking loop, and that cannot share a single-threaded 8032
with a TCP keepalive. Doing it on the board means moving capture into a Timer 1
ISR first. The device protocol below is deliberately thin so that port stays
open. See docs/ARCHITECTURE.md.
"""
import argparse
import json
import os
import queue
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zrp6 import Zrp6, nec_pairs, pronto                      # noqa: E402
from zrp6.rti import frames_from, code_of, load as load_codes  # noqa: E402

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit('zrp6d needs paho-mqtt:  python3 -m pip install paho-mqtt')

RELAYS = (1, 2, 3)
TRIGGERS = (1, 2, 3)
SENSES = (1, 2, 3)
IR_PORTS = range(1, 8)
SERIALS = (1, 2)            # RS-232. Ports 3/4 are RS-485, not wired up yet.


class Bridge:
    def __init__(self, a):
        self.base = a.base
        self.dev = Zrp6(host=a.device)
        self.lock = threading.Lock()        # one board, one command at a time
        self.work = queue.Queue()
        self.codes = load_codes()
        self.rules = self._load_rules()
        self.learn_as = None
        self.sense = {}
        # paho 2.x requires an explicit callback API version; 1.x has no such
        # argument, and this has to run against whichever the host has.
        try:
            self.cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                                   client_id='zrp6d', clean_session=True)
        except AttributeError:
            self.cli = mqtt.Client(client_id='zrp6d', clean_session=True)
        self.cli.will_set(f'{self.base}/status', 'offline', retain=True)
        self.cli.on_connect = self._on_connect
        self.cli.on_message = self._on_message
        self.broker = (a.broker, a.port)

    # ---- plumbing --------------------------------------------------------
    def pub(self, topic, payload, retain=False):
        self.cli.publish(f'{self.base}/{topic}', payload, retain=retain)

    def _on_connect(self, cli, *_):
        self.pub('status', 'online', retain=True)
        for t in ('relay/+/set', 'trigger/+/set', 'ir/+/send', 'serial/+/tx',
                  'automation/rules/set', 'remote/learn'):
            cli.subscribe(f'{self.base}/{t}')
        print(f'connected to {self.broker[0]}:{self.broker[1]}, base {self.base}')
        self.work.put(('sync', None))

    def _on_message(self, cli, _u, msg):
        # Never touch the board from the MQTT callback: it would serialise the
        # network thread behind a multi-second IR burst.
        self.work.put(('msg', (msg.topic[len(self.base) + 1:], msg.payload)))

    # ---- device actions --------------------------------------------------
    def _set_out(self, kind, n, on):
        with self.lock:
            (self.dev.relay if kind == 'relay' else self.dev.trigger)(n, on)
        self.pub(f'{kind}/{n}/state', 'ON' if on else 'OFF', retain=True)

    def _send_ir(self, port, payload):
        text = payload.decode('utf-8', 'replace').strip()
        with self.lock:
            if text.lower().startswith('nec'):
                p = text.split()
                pairs = nec_pairs(int(p[1], 0), int(p[2], 0))
                reps = int(p[3]) if len(p) > 3 else 1
                for _ in range(reps):
                    self.dev.ir(pairs, port=port)
                    time.sleep(0.11)
            else:
                hz, once, rep = pronto(text)
                self.dev.ir(once or rep, port=port)
        self.pub(f'ir/{port}/sent', text)

    def _sync(self):
        """Publish current state so a fresh UI is never blank."""
        with self.lock:
            s = self.dev.contacts()
        for i, v in s.items():
            self.sense[i] = v
            self.pub(f'sense/{i}/state', 'OPEN' if v else 'CLOSED', retain=True)
        # Outputs are write-only in hardware terms; publish the safe rest state
        # rather than inventing a reading we cannot take.
        for n in RELAYS:
            self.pub(f'relay/{n}/state', 'OFF', retain=True)
        for n in TRIGGERS:
            self.pub(f'trigger/{n}/state', 'OFF', retain=True)

    def _dispatch(self, topic, payload):
        p = topic.split('/')
        on = payload.decode('utf-8', 'replace').strip().upper() in ('ON', '1', 'TRUE')
        if p[0] in ('relay', 'trigger') and p[2] == 'set':
            self._set_out(p[0], int(p[1]), on)
        elif p[0] == 'ir' and p[2] == 'send':
            self._send_ir(int(p[1]), payload)
        elif p[0] == 'serial' and p[2] == 'tx':
            with self.lock:
                self.dev.serial_write(payload, ch=int(p[1]) - 1)
        elif topic == 'automation/rules/set':
            self.rules = json.loads(payload.decode() or '[]')
            self._save_rules()
            self.pub('automation/rules', json.dumps(self.rules), retain=True)
        elif topic == 'remote/learn':
            self.learn_as = payload.decode().strip() or None

    # ---- automations -----------------------------------------------------
    RULES = os.path.expanduser('~/.zrp6_rules.json')

    def _load_rules(self):
        try:
            return json.load(open(self.RULES))
        except Exception:
            return []

    def _save_rules(self):
        json.dump(self.rules, open(self.RULES, 'w'), indent=1)

    def _save_codes(self):
        json.dump({json.dumps(list(k)): v for k, v in self.codes.items()},
                  open(os.path.expanduser('~/.zrp6_rti_codes.json'), 'w'), indent=1)
        self.pub('remote/codes',
                 json.dumps({v: list(k) for k, v in self.codes.items()}),
                 retain=True)

    def _run_rules(self, button):
        """A rule is {name, when: <button>, do: [{topic, payload}, ...]}.

        Actions are expressed as MQTT publishes into this same tree, so a rule
        can reach anything the bridge exposes without a second vocabulary.
        """
        for r in self.rules:
            if r.get('when') != button:
                continue
            self.pub('automation/fired', r.get('name', button))
            for act in r.get('do', []):
                t, pl = act.get('topic', ''), str(act.get('payload', ''))
                if t.startswith(f'{self.base}/'):
                    t = t[len(self.base) + 1:]
                self.work.put(('msg', (t, pl.encode())))

    # ---- background pollers ---------------------------------------------
    def _poll_inputs(self):
        while True:
            time.sleep(0.5)
            try:
                with self.lock:
                    s = self.dev.contacts()
                    rx = {n: self.dev.serial_read(ch=n - 1, timeout=0.02)
                          for n in SERIALS}
            except Exception:
                continue
            for i, v in s.items():
                if self.sense.get(i) != v:
                    self.sense[i] = v
                    self.pub(f'sense/{i}/state', 'OPEN' if v else 'CLOSED',
                             retain=True)
            for n, data in rx.items():
                if data:
                    self.pub(f'serial/{n}/rx', data)

    def _poll_remote(self):
        """RTI presses. The capture blocks the 8032, so it sleeps between
        goes — a tight loop here stops the board answering anything else."""
        recent = {}
        while True:
            time.sleep(0.15)
            try:
                with self.lock:
                    lvl, runs = self.dev.capture(200)
            except Exception:
                continue
            runs = [r for r in runs if r]
            if len(runs) < 20:
                continue
            for sym in frames_from(lvl, runs):
                c = code_of(sym)
                if c is None:
                    continue
                now = time.time()
                if now - recent.get(c, 0) < 0.6:      # debounce the repeats
                    continue
                recent[c] = now
                if self.learn_as:
                    self.codes[c] = self.learn_as
                    self.learn_as = None
                    self._save_codes()
                name = self.codes.get(c)
                self.pub('remote/code', json.dumps(list(c)))
                self.pub('remote/press', name or json.dumps(list(c)))
                if name:
                    self._run_rules(name)

    def run(self):
        self.cli.connect(*self.broker, keepalive=30)
        self.cli.loop_start()
        threading.Thread(target=self._poll_inputs, daemon=True).start()
        threading.Thread(target=self._poll_remote, daemon=True).start()
        while True:
            kind, arg = self.work.get()
            try:
                if kind == 'sync':
                    self._sync()
                    self.pub('automation/rules', json.dumps(self.rules), retain=True)
                    self.pub('remote/codes',
                             json.dumps({v: list(k) for k, v in self.codes.items()}),
                             retain=True)
                else:
                    self._dispatch(*arg)
            except Exception as e:
                self.pub('error', f'{kind}: {e}')
                print(f'  {kind} failed: {e}', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--broker', default=os.environ.get('MQTT_HOST', 'localhost'))
    ap.add_argument('--port', type=int, default=int(os.environ.get('MQTT_PORT', 1883)))
    ap.add_argument('--base', default=os.environ.get('ZRP6_TOPIC', 'zrp6'))
    ap.add_argument('--device', default=os.environ.get('ZRP6_HOST', '10.0.0.77'))
    Bridge(ap.parse_args()).run()


if __name__ == '__main__':
    main()
