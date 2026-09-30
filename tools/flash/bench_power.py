#!/usr/bin/env python3
"""Control the TP-Link Kasa plug the ZRP-6 is powered from, so the
flash -> power-cycle -> read-back loop can run unattended.

  bench_power.py on | off | cycle [off_seconds]

Credentials are read from the environment, or from openHC's .env if not set.
They are never written to this repo and never printed.
"""
import asyncio, os, sys, pathlib

ALIAS = os.environ.get('BENCH_PLUG_ALIAS', 'Bench')
ENV_FILE = pathlib.Path(os.environ.get(
    'KASA_ENV', pathlib.Path.home() / 'Documents/GitHub/openHC/.env'))


def creds():
    u, p = os.environ.get('KASA_EMAIL'), os.environ.get('KASA_PASSWORD')
    if (not u or not p) and ENV_FILE.exists():
        for line in ENV_FILE.read_text().splitlines():
            if '=' not in line or line.lstrip().startswith('#'):
                continue
            k, v = line.split('=', 1)
            v = v.strip().strip('"').strip("'")
            if k.strip() == 'KASA_EMAIL' and not u:
                u = v
            elif k.strip() == 'KASA_PASSWORD' and not p:
                p = v
    return u, p


HOST = os.environ.get('BENCH_PLUG_HOST', '192.168.1.152')


async def find():
    from kasa import Discover
    u, p = creds()
    last = None
    for attempt in range(4):
        if HOST:
            try:
                d = await Discover.discover_single(HOST, username=u, password=p)
                await d.update()
                return d
            except Exception as e:
                last = e
        try:
            found = await Discover.discover(username=u, password=p, timeout=8)
        except Exception as e:
            last = e; found = {}
        for _, d in found.items():
            try:
                await d.update()
            except Exception:
                continue
            if d.alias == ALIAS:
                return d
        await asyncio.sleep(2)
    raise SystemExit('plug %r not reachable (%s)' % (ALIAS, last))


async def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'cycle'
    off_s = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    d = await find()
    if cmd == 'on':
        await d.turn_on()
    elif cmd == 'off':
        await d.turn_off()
    elif cmd == 'cycle':
        await d.turn_off()
        await asyncio.sleep(off_s)          # let rails fully discharge
        await d.turn_on()
        await asyncio.sleep(2.0)            # give the board time to come up
    else:
        raise SystemExit('usage: bench_power.py on|off|cycle [off_seconds]')
    await d.update()
    print('%s: %s -> on=%s' % (d.alias, cmd, d.is_on))

asyncio.run(main())
