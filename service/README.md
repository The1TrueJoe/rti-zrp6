# zrp6d

The MQTT face of the ZRP-6, and the runtime that executes automation rules.

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python zrp6d.py --broker localhost --device 10.0.0.77
```

Topic tree: [../docs/MQTT.md](../docs/MQTT.md).
Why this is a service rather than firmware: [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

## Layout

| | |
|---|---|
| `zrp6d.py` | the bridge and the rule runtime |
| `zrp6/device.py` | the board's binary UDP protocol |
| `zrp6/rti.py` | decoding RTI remote frames off SIGNAL IN |

`zrp6/device.py` is also a CLI, which is the quickest way to poke the hardware
without a broker:

```bash
.venv/bin/python -m zrp6.device selftest
.venv/bin/python -m zrp6.device relay 1 on
.venv/bin/python -m zrp6.device nec 0x04 0x08 15 1 6
```

## One trap

Every P3-gated access must clear **only P3.1**. Clearing more also pulls `P3.0`
(contact input 1) and `P3.5` (SIGNAL IN) low — which shows up as a dim flicker
on the panel and injects false contact readings. `P3_GATE` in `device.py` is
that value; do not pass a raw `0x00`.
