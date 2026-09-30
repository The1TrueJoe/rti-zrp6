#!/usr/bin/env python3
"""Logins, users, and the settings kept in Flash, on the box (docs/MQTT.md).

    python3 firmware/test/test_settings.py [host] [--net]

  1. logins: a wrong password and no login are refused (CONNACK 5); after a
     failure every login is refused for 2 s, then the right one works
  2. users: add, list, log in as, change, delete; `last` and `invalid`; a
     password published to users/<name>/set reaches no other subscriber
  3. uploads need HTTP Basic auth (401 without it)
  4. IR port 2 as a serial port: send is ignored, tx is sent; serial port 1's
     settings; both survive a restart
  5. with --net: a static address and back to DHCP (two restarts)

Leaves the box as it found it: admin/admin, no other users, IR port 2 IR,
serial 1 at 9600 8N1, DHCP. Needs admin/admin to start with.
"""
import base64
import http.client
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
HOST = args[0] if args else os.environ.get('ZRP6_HOST', '10.0.0.77')
NET = '--net' in sys.argv


from mqttc import M  # noqa: E402
import mqttc  # noqa: E402
mqttc.HOST = HOST


fails = []


def check(cond, msg):
    print(('  ok    ' if cond else '  FAIL  ') + msg)
    if not cond:
        fails.append(msg)


def post(route, auth):
    c = http.client.HTTPConnection(HOST, 80, timeout=20)
    h = {'Content-Length': '0'}
    if auth:
        h['Authorization'] = 'Basic ' + base64.b64encode(auth).decode()
    c.request('POST', route, body=b'', headers=h)
    r = c.getresponse()
    return r.status, r.read().decode(errors='replace').strip()


def wait_up(host, limit=90):
    end = time.time() + limit
    time.sleep(6)
    while time.time() < end:
        try:
            m = M(cid=b'!wu', host=host)
            if m.ok:
                return m
        except OSError:
            pass
        time.sleep(3)
    return None


def restart(m):
    m.pub('zrp6/system/restart', '1')
    m.close()
    time.sleep(1)
    return wait_up(HOST)


print('1. logins')
check(M(b'admin', b'wrong', b'l1').ack == b'\x20\x02\x00\x05', 'a wrong password is refused (5)')
time.sleep(2.2)
check(M(None, None, b'l2').ack == b'\x20\x02\x00\x05', 'no login is refused (5)')
check(M(b'admin', b'admin', b'l3').ack == b'\x20\x02\x00\x05', 'right after a failure, even the right one waits')
time.sleep(2.2)
# '!': a priority client, never bumped to make room (docs/MQTT.md). The box
# may be serving a Control4 driver meanwhile, and with only four sockets an
# ordinary long-lived session here would be the one to give way.
m = M(cid=b'!st')
check(m.ok, 'then admin/admin gets in')
m.sub('zrp6/#')
m.msgs(1.5)

print('2. users')
w = M(cid=b'watch')                            # watches for leaked passwords
w.sub('zrp6/#')
w.msgs(1.0)
try:
    check(m.ask('zrp6/users/get', '1', 'zrp6/users/list') == 'admin', 'the list is just admin')
    check(m.ask('zrp6/users/joe/set', 'secret-1', 'zrp6/users/state') == 'saved joe', 'add joe')
    leaked = [t for t, ps in w.msgs(1.0).items() if any(b'secret' in p for p in ps)]
    check(not leaked, f'the password reached no other subscriber {leaked or ""}')
    w.close()                                  # a socket back for the logins below
    check(m.ask('zrp6/users/get', '1', 'zrp6/users/list') == 'admin,joe', 'the list is admin,joe')
    check(M(b'joe', b'secret-1', b'j1').ok, 'joe logs in')
    check(m.ask('zrp6/users/joe/set', 'secret-2', 'zrp6/users/state') == 'saved joe', 'change joe')
    check(not M(b'joe', b'secret-1', b'j2').ok, "joe's old password is refused")
    time.sleep(2.2)
    check(M(b'joe', b'secret-2', b'j3').ok, "joe's new one works")
    check(m.ask('zrp6/users/nobody/set', '', 'zrp6/users/state') == 'invalid', 'deleting a user that is not there: invalid')
    check(m.ask('zrp6/users/bad name/set', 'x', 'zrp6/users/state') == 'invalid', 'a name with a space: invalid')
    check(m.ask('zrp6/users/admin/set', '', 'zrp6/users/state') == 'deleted admin', 'delete admin (joe remains)')
    time.sleep(2.2)
    check(not M(b'admin', b'admin', b'a1').ok, 'admin is gone')
    check(m.ask('zrp6/users/joe/set', '', 'zrp6/users/state') == 'last', 'the last user cannot be deleted')
finally:
    # back to admin/admin alone, whatever happened above
    m.ask('zrp6/users/admin/set', 'admin', 'zrp6/users/state')
    m.ask('zrp6/users/joe/set', '', 'zrp6/users/state')
check(m.ask('zrp6/users/get', '1', 'zrp6/users/list') == 'admin', 'restored: just admin')
time.sleep(2.2)
check(M(cid=b'a2').ok, 'restored: admin/admin works')

print('3. uploads need a login')
check(post('/ui', None)[0] == 401, 'POST /ui with no login: 401')
check(post('/update', b'admin:nope')[0] == 401, 'POST /update, wrong password: 401')
st, body = post('/update', b'admin:admin')
check(st == 400 and 'expected' in body, f'POST /update, right login, no image: {st} {body[:40]}')

print('4. IR port as serial, serial settings, kept over a restart')
check(m.ask('zrp6/ir/2/mode/set', 'serial 19200', 'zrp6/ir/2/mode') == 'serial 19200 rs232', 'IR 2 to serial 19200')
check(m.ask('zrp6/ir/2/send', 'nec 0x04 0x08', 'zrp6/ir/2/sent', 2) is None, 'an IR code to it is ignored')
check(m.ask('zrp6/ir/2/tx', 'PWR ON\r', 'zrp6/ir/2/sent', 2) == 'PWR ON\r', 'serial bytes go out')
check(m.ask('zrp6/ir/3/tx', 'x', 'zrp6/ir/3/sent', 2) is None, 'tx on an IR port is ignored')
check(m.ask('zrp6/serial/1/config/set', '19200 8E1', 'zrp6/serial/1/config') == '19200 8E1', 'serial 1 to 19200 8E1')
check(m.ask('zrp6/ir/5/mode/set', 'serial 115200 ttl', 'zrp6/ir/5/mode') == 'serial 115200 ttl', 'IR 5 to serial 115200 ttl')
m.msgs(4)                                      # let the saves finish
m = restart(m)
check(m is not None, 'the box restarted and came back')
if m:
    m.sub('zrp6/#')
    got = m.msgs(2)
    mode = lambda t: got.get(t, [b''])[-1].decode()
    check(mode('zrp6/ir/2/mode') == 'serial 19200 rs232', f'IR 2 kept: {mode("zrp6/ir/2/mode")}')
    check(mode('zrp6/ir/5/mode') == 'serial 115200 ttl', f'IR 5 kept: {mode("zrp6/ir/5/mode")}')
    check(mode('zrp6/serial/1/config') == '19200 8E1', f'serial 1 kept: {mode("zrp6/serial/1/config")}')
    m.ask('zrp6/ir/2/mode/set', 'ir', 'zrp6/ir/2/mode')
    m.ask('zrp6/ir/5/mode/set', 'ir', 'zrp6/ir/5/mode')
    check(m.ask('zrp6/serial/1/config/set', '9600 8N1', 'zrp6/serial/1/config') == '9600 8N1', 'serial 1 back to 9600 8N1')
    m.msgs(4)

if NET and m:
    print('5. static, then DHCP')
    got = m.msgs(0.1)
    m.sub('zrp6/net/#')
    net = {t: v[-1].decode() for t, v in m.msgs(1.5).items()}
    ip, mask, gw = net.get('zrp6/net/ip'), net.get('zrp6/net/netmask'), net.get('zrp6/net/gateway')
    check(net.get('zrp6/net/mode') == 'dhcp', f'on DHCP now, at {ip}')
    check(m.ask('zrp6/net/set', f'static {ip} {mask} {gw}', 'zrp6/net/state') == 'saved, restarting',
          'the same address, static')
    m.close()
    m = wait_up(ip)
    check(m is not None, f'back at {ip}')
    if m:
        m.sub('zrp6/net/#')
        check(m.msgs(1.5).get('zrp6/net/mode', [b''])[-1] == b'static', 'mode static')
        check(m.ask('zrp6/net/set', 'dhcp', 'zrp6/net/state') == 'saved, restarting', 'back to DHCP')
        m.close()
        m = wait_up(ip)
        check(m is not None, f'back on DHCP (the router gave {ip} again)')
        if m:
            m.sub('zrp6/net/mode')
            check(m.msgs(1.5).get('zrp6/net/mode', [b''])[-1] == b'dhcp', 'mode dhcp')

print('PASS' if not fails else f'FAIL: {len(fails)}')
sys.exit(1 if fails else 0)
