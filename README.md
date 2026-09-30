# RTI ZRP-6, open firmware

An RTI ZRP-6 control processor running firmware written from scratch, after a
JTAG full-chip erase took out both the original firmware and the PLD
configuration that made it addressable.

The box is self-contained. It is its own MQTT broker, serves its own control
interface out of Flash, runs its own remote-button automations, and speaks
nothing but MQTT on the network. It has:

- seven IR outputs, each switchable to one-way serial;
- three relays, three 12 V triggers and three contact inputs;
- two RS-232 ports;
- RTI handheld remotes, decoded on the box.

Everything is on the topic tree.

There is a Control4 driver for it, and it updates itself over the network or
USB with an A/B fallback, so JTAG is for the bootloader only.

Not affiliated with or endorsed by Remote Technologies Inc. "RTI" and "ZRP-6"
are theirs. None of RTI's firmware is in this repository: the dumps made while
reverse-engineering the box live in `research/`, which is not published.

```
firmware/src      C for the uPSD3434EV's Turbo 8032 (SDCC), in two code banks
firmware/boot     the bootloader: A/B slots, trial boots, USB rescue mode
firmware/ui       the control interface (Preact), stored gzipped in its own Flash sector
firmware/test     bench tests against a real box, and host-side tests
drivers/control4  the Control4 driver (.c4z), with its own tests and CI
tools/            flashing: USB, network, JTAG, and the one front door to them
docs/             how it works, and how it was worked out
service/          the old UDP bridge: obsolete, do not run it against current firmware
```

## Running it

```bash
firmware/build.sh                          # firmware + page -> build/
python3 tools/zrp6flash.py --status        # what can reach the box
python3 tools/zrp6flash.py                 # install: USB, else network, else JTAG
python3 tools/zrp6flash.py --bootloader    # bootloader + PLD too (needs JTAG)
```

The box takes an address by DHCP and announces itself over mDNS as
`zrp6-XXXX.local`; the interface is at `http://<box>/`. Default login
**admin / admin**, for the page and for MQTT alike:

```bash
mosquitto_sub -h <box> -u admin -P admin -t 'zrp6/#' -v
```

Forgotten password or unreachable settings: power the box on three times in
quick succession, then leave it on. Users and network go back to defaults.
Firmware that will not come up: plug in USB and see [docs/USB.md](docs/USB.md).

## What it does

| | |
|---|---|
| **IR** | ports 1-6 plus HIGH OUT, 38 kHz, NEC or Pronto hex; any port can be one-way serial instead |
| **Relays** | 3, with a state acknowledgement on every command |
| **Triggers** | 3 x 12 V, likewise |
| **Contacts** | 3 inputs, published on change |
| **Serial** | 2 x RS-232, transmit and receive, configurable. RS-485 later |
| **Remote** | RTI handheld presses decoded on the box, published as `remote/code` |
| **Rules** | remote button → IR, relay, trigger or serial actions, run on the box |
| **Users** | logins for MQTT and the page; priority MQTT clients are never dropped |
| **Updates** | network or USB, into the slot not running; 3 trial boots, then automatic rollback |

## Reading

- [docs/DEVICE.md](docs/DEVICE.md): everything known about the hardware.
  The parts, every pin, the memory map, the errata, JTAG, and RTI's
  original firmware.
- [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md): how it was
  found out, including the several confident conclusions that turned out to
  be wrong.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how a broker, a web server
  and a WebSocket fit in an 8032 with 175 bytes of stack.
- [docs/MQTT.md](docs/MQTT.md): the topic tree.
- [docs/UPDATING.md](docs/UPDATING.md): network updates, the bootloader, code
  banks.
- [docs/USB.md](docs/USB.md): USB flashing and rescue mode.
- [drivers/control4/README.md](drivers/control4/README.md): the Control4
  driver.

## A caution

`tools/flash/` talks to the target over JTAG and can erase it: a JAM
`PROGRAM` erases the whole chip, and `flash.sh` saves and restores the
settings store around it. If programming fails with `Full chip erase
failed`, the part is secured. Run `jam -aERASE` before `-aPROGRAM`, and drive
the target reset line high rather than leaving it floating. See
[tools/README.md](tools/README.md).

The box answers USB on RTI's own vendor and product IDs (13BD:1019), with a
command prefix that keeps it from mistaking Integration Designer's traffic for
its own. That suits boxes run in installs you control. Firmware passed on to
others should move to its own ID first ([docs/USB.md](docs/USB.md)).

## License

MIT; see [LICENSE](LICENSE).
