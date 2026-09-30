#!/usr/bin/env python3
"""The driver against a real ZRP-6: the same harness as run_integration.py,
but the network binding goes to the box rather than a private mosquitto.

    python3 test/integration/run_box.py <box-ip> [build/stage/driver.lua]

Checks the driver logs in, gets the box's retained state onto its bindings,
drives a relay and reads the box's answer, switches an IR port to serial and
back through its property, and maps a remote code to a room action. Leaves
relay 2 open and IR port 4 in IR mode. Logs in as ZRP6_USER / ZRP6_PASS
(default admin/admin).
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BOX = sys.argv[1]
sys.argv = [sys.argv[0], sys.argv[2] if len(sys.argv) > 2 else 'build/stage/driver.lua', 'lua']
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'firmware', 'test'))
import run_integration as R                      # noqa: E402
from mqttc import M                              # noqa: E402  (a plain MQTT client)

USER = os.environ.get('ZRP6_USER', 'admin')
PASS = os.environ.get('ZRP6_PASS', 'admin')


class Box:
    port = 1883


def settle(drv, secs):
    """Mock time and real time together: the box answers in real time."""
    end = time.time() + secs
    while time.time() < end:
        drv.step(50)
        time.sleep(0.05)


def proxy(drv):
    return drv.lua('local t={} for _,c in ipairs(C4.proxy) do t[#t+1]=c.binding..":"..c.command end return table.concat(t," ")')


def state(m, topic, wait=3.0):
    got = m.msgs(wait).get(topic)
    return got[-1].decode() if got else None


print(f'driver against the box at {BOX}')
watch = M(USER.encode(), PASS.encode(), b'c4watch', host=BOX)
watch.sub('zrp6/#')
watch.msgs(1.5)
drv = R.Driver('lua', Box(), **{'Box IP Address': BOX, 'Username': USER, 'Password': PASS})
try:
    drv.step(600)
    drv.wait('return d.m.CLIENT.state', 'UP', timeout=10)
    R.check('logs in to the box', True)
    settle(drv, 2)
    p = proxy(drv)
    R.check('the box\'s contacts reach bindings 401-403', all(f'{b}:STATE_' in p for b in (401, 402, 403)), p)
    R.check('the box\'s relays reach bindings 301-303', all(f'{b}:STATE_' in p for b in (301, 302, 303)), p)

    drv.lua('env.ReceivedFromProxy(302, "CLOSE", {}) return true')
    settle(drv, 1.5)
    R.check('binding 302 CLOSE -> the box closes relay 2', state(watch, 'zrp6/relay/2/state') == 'ON')
    settle(drv, 1)
    R.check('and the box\'s answer reaches binding 302', '302:CLOSED' in proxy(drv) or '302:STATE_CLOSED' in proxy(drv), proxy(drv))
    drv.lua('env.ReceivedFromProxy(302, "OPEN", {}) return true')
    settle(drv, 1.5)
    R.check('binding 302 OPEN -> the box opens relay 2', state(watch, 'zrp6/relay/2/state') == 'OFF')

    drv.lua('d.props["IR 4 Mode"] = "Serial (one-way)" env.OnPropertyChanged("IR 4 Mode") return true')
    settle(drv, 2)
    mode = state(watch, 'zrp6/ir/4/mode', 5)
    R.check('IR 4 Mode -> Serial switches the box\'s port', (mode or '').startswith('serial'), mode)
    drv.lua('d.props["IR 4 Mode"] = "IR" env.OnPropertyChanged("IR 4 Mode") return true')
    settle(drv, 2)
    mode = state(watch, 'zrp6/ir/4/mode', 5)
    R.check('and back to IR', mode == 'ir', mode)

    code = 'p9.9.9.9.9:1.2.3'
    drv.lua('m.REMOTES.Upsert({code="' + code + '", actions={{room=10, action="ROOM_OFF"}}}) C4.deviceCalls = {} return true')
    watch.pub('zrp6/remote/code', code)
    got = drv.wait('return C4.deviceCalls[1] and C4.deviceCalls[1].command or "none"', 'ROOM_OFF', timeout=8)
    R.check('a remote code from the box runs ROOM_OFF', got == 'ROOM_OFF')
    drv.lua('env.OnDriverDestroyed() return true')
finally:
    drv.quit()
    watch.close()
print(f'\n{R.passed} passed, {R.failed} failed')
sys.exit(1 if R.failed else 0)
