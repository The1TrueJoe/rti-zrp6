# How the system fits together

The target is a box that is **useful on its own**. Plug it into a network and
it serves its own control surface and is its own MQTT broker; joining a house
broker is optional and additive, not a precondition.

```
                    browser ──ws://zrp6:80/mqtt──┐
   Control4 ────────mqtt://zrp6:1883─────────────┤
                                                 ▼
   RTI remote ))) RM-433 ──wire──┐      ┌──────────────────┐
                                 └─────►│                  │
   TV / AVR ◄──IR────────────────┬──────┤      ZRP-6       │
   TV / AVR ◄──RS-232────────────┤      │  uPSD3434EV      │
   contacts ─────────────────────┤      │                  │
   relays / triggers ◄───────────┘      │  http + broker   │
                                        │  + the UI in     │
                                        │    Flash         │
                                        └────────┬─────────┘
                                                 │ optional
                                        ┌────────▼─────────┐
                                        │  house broker    │
                                        └──────────────────┘
```

## Everything runs on the box

| | |
|---|---|
| **MQTT broker** | all four W5100 sockets, ports assigned as needed, newest browser wins. QoS 0, retained messages, trailing-`#` wildcards |
| **WebSocket** | so a browser can be an MQTT client directly |
| **HTTP** | serves the built UI out of Flash, gzipped |
| **RTI decoder** | remote presses decoded on the box, published as `remote/code` |

The W5100 runs TCP/IP in hardware, so none of this is a network stack — the
8032 does socket bookkeeping and byte packing. That is the whole reason it fits.

### There is no side channel

Nothing but MQTT is on the network. An earlier build kept a UDP admin channel
as a way back in; it was removed on 2026-09-23 because a bridge polling through
it was what made the front panel blink, and because a box meant to be driven
over MQTT should not have a second, undocumented way in. A bad image is
recovered over JTAG. Joining a house broker (`mqtt.c`) would need a socket, and
all four are listeners, so it is not built.

## Where things live in Flash

```
  main Flash 256 KB, 8 x 32 KB
    sector 0     firmware                     code 0x0000-0x7FFF
    sectors 1-7  the built UI, gzipped        paged at code 0x8000-0xFFFF
  secondary Flash 32 KB
    0x4000       config copy A                XDATA
    0x6000       config copy B                XDATA
```

The UI is read with `MOVC` through the page register, so **serving** it needs
no PLD change. Writing those sectors does — see [UPDATING.md](UPDATING.md).

## Identity comes out of Flash, not the source

There is no factory-unique ID a uPSD3434 exposes to the CPU: the JTAG IDCODE
is device-type and identical on every part. So the MAC address is **stored**,
in a config record in secondary Flash, with the broker address, IP and base
topic beside it.

Two copies live in two different 8 KB sectors, each with a CRC. A save erases
and rewrites the spare first, so a power cut mid-write always leaves one
complete record. If neither verifies, the firmware falls back to a
locally-administered default and still comes up on the network — an
unprovisioned box is reachable, not bricked.

## What the service is still for

`service/` is no longer required to run the box. It remains useful as:

* a development client (`zrp6.device` is the quickest way to poke hardware),
* the RTI remote decoder, while that still runs host-side,
* a bridge for anyone who wants the box on an existing broker without
  configuring the box itself.

## Automations

Rules live on the box, in Flash, and run there. This sits at a television; a
rule that only fired while a configuration page happened to be open would be
useless. A rule is a remote button and a list of publishes, and the action
vocabulary is the topic tree itself — so a new capability becomes automatable
without inventing a second command language.
