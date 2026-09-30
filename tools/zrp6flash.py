#!/usr/bin/env python3
"""Put the build in build/ on a ZRP-6 by the best way that is connected.

  tools/zrp6flash.py                   firmware and web page
  tools/zrp6flash.py --bootloader      the bootloader and PLD too (needs JTAG)
  tools/zrp6flash.py --status          what is connected, and what it would do
  options:  --host IP   --via usb|net|jtag   --dry-run

It looks for three ways in, and uses the quickest one that can do the job:

  firmware + page   USB (about 15 s)  >  network (about 20 s)  >  JTAG (about 45 min)
  bootloader        JTAG with USB: --quick (JTAG writes only the boot sectors
                    and PLD, then USB installs the rest)  >  JTAG alone (full)

USB is the box on its USB port, answering as its firmware or as the
bootloader's rescue mode (tools/usb/zrp6usb.py, docs/USB.md). The network is
an MQTT login to --host, $ZRP6_HOST, the address the box reports over USB,
or the last address that worked (~/.zrp6/last_host). JTAG is the adapter
on $ZRP6_JTAG_DEV (default /dev/cu.usbmodem1203), with the bench plug for the
power cycles tools/flash/flash.sh does.

The bootloader cannot be read back over USB or the network, so nothing here
guesses whether it needs changing: say --bootloader when it does.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'usb'))
import zrp6usb  # noqa: E402

JTAG_DEV = os.environ.get('ZRP6_JTAG_DEV', '/dev/cu.usbmodem1203')
LAST_HOST = os.path.expanduser('~/.zrp6/last_host')
USER = os.environ.get('ZRP6_USER', 'admin').encode()
PASS = os.environ.get('ZRP6_PASS', 'admin').encode()


def mqtt_up(host, timeout=2.0):
    """1 if the box at host takes our login on 1883."""
    f = lambda b: len(b).to_bytes(2, 'big') + b
    body = b'\x00\x04MQTT\x04\xc2\x00\x3c' + f(b'!zrp6flash') + f(USER) + f(PASS)
    try:
        s = socket.create_connection((host, 1883), timeout)
        s.settimeout(timeout)
        s.sendall(bytes([0x10, len(body)]) + body)
        ok = s.recv(4) == b'\x20\x02\x00\x00'
        s.close()
        return ok
    except OSError:
        return False


def look(args):
    """What is connected: {'usb': info or None, 'host': ip or None, 'jtag': bool}."""
    found = {'usb': None, 'host': None, 'jtag': os.path.exists(JTAG_DEV)}
    for dev in zrp6usb.Box.find():
        try:
            b = zrp6usb.Box(dev)
            found['usb'] = b.info()
            b.close()
            break
        except Exception as e:
            found['usb_error'] = str(e)
    hosts = [args.host, os.environ.get('ZRP6_HOST')]
    if found['usb'] and not found['usb']['rescue']:
        hosts.append(found['usb']['ip'])
    try:
        hosts.append(open(LAST_HOST).read().strip())
    except OSError:
        pass
    for h in hosts:
        if h and h != '0.0.0.0' and mqtt_up(h):
            found['host'] = h
            break
    return found


def plan(found, bootloader, via):
    """(what, command) for the quickest way that can do it, or (reason, None)."""
    usb, host, jtag = found['usb'], found['host'], found['jtag']
    py = sys.executable
    flash = ['tools/flash/flash.sh']
    env_host = host or os.environ.get('ZRP6_HOST', '10.0.0.77')
    if bootloader:
        if not jtag or via in ('usb', 'net'):
            return ('the bootloader and PLD change only over JTAG, and no JTAG adapter '
                    f'is on {JTAG_DEV}', None)
        if usb and via != 'jtag':
            return ('JTAG for the bootloader and PLD, then USB for the rest (--quick)',
                    ['env', f'ZRP6_HOST={env_host}'] + flash + ['--quick', '20'])
        return ('JTAG for everything (no USB: the slow, full flash)',
                ['env', f'ZRP6_HOST={env_host}'] + flash + ['20'])
    if usb and via in (None, 'usb'):
        return (f'USB, to the box\'s {"rescue mode" if usb["rescue"] else "firmware"}',
                [py, 'tools/usb/zrp6usb.py', 'flash', 'build/zrp6-ui.bin', 'build/zrp6.bin'])
    if host and via in (None, 'net'):
        return (f'the network, to {host}',
                [py, 'tools/update/push.py', host, 'build/zrp6-ui.bin', 'build/zrp6.bin'])
    if jtag and via in (None, 'jtag'):
        return ('JTAG (no USB and no network: the slow, full flash)',
                ['env', f'ZRP6_HOST={env_host}'] + flash + ['20'])
    return ('nothing to reach the box with: plug in USB (and power-cycle it once if it '
            'does not appear), give --host, or connect JTAG', None)


def show(found):
    u = found['usb']
    if u:
        what = 'rescue mode' if u['rescue'] else f'firmware {u["build"]} at {u["ip"]}'
        print(f'USB      yes: {what}')
    else:
        print(f'USB      no{" (" + found["usb_error"] + ")" if "usb_error" in found else ""}')
    print(f'network  {"yes: " + found["host"] if found["host"] else "no"}')
    print(f'JTAG     {"yes: " + JTAG_DEV if found["jtag"] else "no"}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bootloader', action='store_true', help='the bootloader and PLD too')
    ap.add_argument('--status', action='store_true', help='report only')
    ap.add_argument('--host')
    ap.add_argument('--via', choices=['usb', 'net', 'jtag'])
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    os.chdir(ROOT)

    found = look(args)
    show(found)
    what, cmd = plan(found, args.bootloader, args.via)
    print(f'→ {what}')
    if args.status or args.dry_run or not cmd:
        if cmd:
            print('  ' + ' '.join(cmd))
        sys.exit(0 if cmd or args.status else 1)
    for f in ('build/zrp6.bin', 'build/zrp6-ui.bin'):
        if not os.path.exists(f):
            sys.exit(f'{f} missing: run firmware/build.sh first')

    t0 = time.time()
    rc = subprocess.run(cmd).returncode
    if rc and cmd[1:2] == ['tools/usb/zrp6usb.py'] and not found['usb']['rescue']:
        # The firmware refuses while its own image is on trial; rescue mode
        # writes around the image the bootloader would boot instead.
        print('→ the firmware refused; trying again from rescue mode')
        subprocess.run([sys.executable, 'tools/usb/zrp6usb.py', 'rescue'])
        rc = subprocess.run([sys.executable, 'tools/usb/zrp6usb.py', 'flash', '--wait',
                             'build/zrp6-ui.bin', 'build/zrp6.bin']).returncode
    if rc:
        sys.exit(f'failed ({rc}) after {time.time() - t0:.0f} s')

    host = found['host'] or os.environ.get('ZRP6_HOST')
    if host and 'flash.sh' not in ' '.join(cmd):
        # The old image answers for a moment before its restart: wait for it
        # to go before waiting for the new one to come.
        print(f'waiting for {host}…')
        end = time.time() + 15
        while time.time() < end and mqtt_up(host, 1.0):
            time.sleep(0.5)
        end = time.time() + 150
        while time.time() < end and not mqtt_up(host):
            time.sleep(3)
        print(f'{"up" if mqtt_up(host) else "NOT answering"} at {host}')
    if host:
        os.makedirs(os.path.dirname(LAST_HOST), exist_ok=True)
        open(LAST_HOST, 'w').write(host)
    print(f'done in {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
