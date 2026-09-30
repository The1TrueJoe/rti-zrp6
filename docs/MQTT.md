# MQTT interface

The box is its own broker. There is nothing to stand up first:

```bash
mosquitto_sub -h <zrp6> -u admin -P admin -t 'zrp6/#' -v      # port 1883
mosquitto_pub -h <zrp6> -u admin -P admin -t zrp6/relay/1/set -m ON
```

## Logging in

Every MQTT client — a controller on 1883 or the web page over its WebSocket —
logs in with a username and password in its CONNECT. A missing or wrong login
is answered with CONNACK return code 5 (not authorized) and the connection is
closed; after a failed login the box refuses every login for 2 seconds, which
makes guessing slow. A client that has not logged in can do nothing else.

A new box has one user, **`admin` / `admin`**. Change it. Up to 8 users; every
user can do everything, including managing users. Names are 1-15 characters
of `A-Z a-z 0-9 . _ -`; passwords are 1-32 bytes. The box keeps only a salted
SHA-1 of each password, in Flash beside the rules.

| topic | payload | |
|---|---|---|
| `zrp6/users/<name>/set` | the new password: adds the user, or changes its password. Empty deletes the user | command |
| `zrp6/users/state` | `saved <name>` `deleted <name>` `invalid` `full` `last` (the last user cannot be deleted) `busy` `error` | answer |
| `zrp6/users/get` | anything | command |
| `zrp6/users/list` | the names, comma-separated, e.g. `admin,joe` | answer to `get` |

A `users/<name>/set` message is acted on and never passed on to other
subscribers, so a password is not echoed to anyone watching `zrp6/#`.

On the web, loading the page (`GET /`) needs no login — the page holds no data
and gets everything over the logged-in WebSocket. Uploading firmware or the
page (`POST /update`, `POST /ui`) needs HTTP Basic auth with the same users:
`Authorization: Basic base64(user:password)`, or the answer is
`401 Unauthorized`. Nothing is encrypted: there is no TLS on an 8032, so keep
the box on a trusted network.

The W5100 network chip has four sockets, and that is the hardware limit on
connections: every open browser tab holds one, every MQTT client holds one,
and loading the page borrows one for a moment and hangs up. The only UDP is
the box's own DHCP client (below), which borrows an idle listener for a
moment when it renews its lease.

Who gets them, in order:

1. **MQTT clients** (a controller, a bridge) — never dropped.
2. **A socket listening on 1883**, so the next MQTT client always gets in.
3. **A socket listening on 80**, so the page can always be loaded.
4. **Browser WebSockets** (the page's live view) — the oldest gives way first.

Loading the page borrows a socket only while it is sent and hands it back with
a graceful close. With no MQTT client connected, two browser tabs stay live
beside the two doors; a third takes the place of the oldest. A tab that gives
way is sent close code 4000, shows "Connection handed over" with a Reconnect
button, and does not reconnect by itself, so tabs never take turns. The newest
tab is never dropped to reopen the web door — only to make room for MQTT.

Ports are not fixed per socket: each socket sniffs its first byte, so either
port accepts either protocol, and a socket picks its port when it goes back to
listening (1883 first, then 80).

A bump is not instant: it waits for any socket that is still closing, and a
tab that does not answer its close frame is cut off after about a second. So
for one to four seconds after an MQTT client arrives the web door can be shut,
and a page load in that window is refused and retried by the browser.
`firmware/test/test_sockets.py` walks the whole policy on the box (19 checks,
passing on v8 2026-09-29).

A browser reaching `http://<zrp6>/` gets the interface out of Flash and then
upgrades to MQTT over WebSocket on the same port. The page pings every
5 seconds and treats 15 seconds of silence as a lost link.

Limits, which are why it fits in an 8032: QoS 0 only, no persistent sessions,
no wildcards except a trailing `#`, four concurrent clients, 32 retained
topics (the box's own use 29), topics of at most 31 characters, retained
payloads of at most 24 bytes, and packets of at most 768 bytes — a larger one
is dropped. A client that sends several packets back to back should leave the
box a moment between them: it reads 768 bytes at a time (32).

Base topic `zrp6` by default (`--base` on the bridge, or the stored config on
the box). Anything ending `/set` is a command in; everything else is state out.

## State — retained

| topic | payload |
|---|---|
| `zrp6/status` | `online` / `offline` (last will) |
| `zrp6/firmware/version` | the build's UTC timestamp, e.g. `2026-09-25T01:24Z` (retained) |
| `zrp6/firmware/state` | `running`, `on trial, boot N of 3`, or `no bootloader` on the v5 PLD (retained) — docs/UPDATING.md |
| `zrp6/firmware/backup` | `yes` when there is a previous image to roll back to (retained) |
| `zrp6/firmware/ui` | `installed` or `missing`: whether the web page sector holds a page (retained) |
| `zrp6/relay/<1-3>/state` | `ON` / `OFF` |
| `zrp6/trigger/<1-3>/state` | `ON` / `OFF` |
| `zrp6/sense/<1-3>/state` | `OPEN` / `CLOSED` |
| `zrp6/debug/stackfree` | bytes of stack never touched since boot — see firmware/README |
| `zrp6/net/mode` | `dhcp`, `static`, or `fallback` (DHCP asked for but no server answered) |
| `zrp6/net/ip` `zrp6/net/netmask` `zrp6/net/gateway` | the addresses in use, dotted |
| `zrp6/net/mac` | e.g. `02:5A:52:3F:81:C4` |
| `zrp6/ir/<1-7>/mode` | `ir`, or `serial <baud> rs232` / `serial <baud> ttl` |
| `zrp6/serial/<1-2>/config` | e.g. `9600 8N1` |

## Events — not retained

| topic | payload |
|---|---|
| `zrp6/remote/code` | an RTI button, e.g. `14.0.2.8.0.2.9` — once per press. A remote other than the bench one (a different preamble, i.e. another remote or ID) reports `p<preamble>:<button>`, e.g. `p16.16.14.12.15:14.0.2.8`, so two remotes never share a code |
| `zrp6/serial/<1-2>/rx` | bytes received, one message per reply: published on a newline, at 128 bytes, or after ~25 ms of silence |
| `zrp6/ir/<1-7>/sent` | the code, echoed after transmission |


## Commands

| topic | payload |
|---|---|
| `zrp6/relay/<1-3>/set` | `ON` / `OFF` → answers `/state` |
| `zrp6/trigger/<1-3>/set` | `ON` / `OFF` → answers `/state` |
| `zrp6/ir/<1-7>/send` | `nec <addr> <cmd> [repeats]`, or Pronto hex |
| `zrp6/serial/<1-2>/tx` | bytes to transmit (also starts that port) |
| `zrp6/serial/<1-2>/config/set` | `<baud> [8N1\|8E1\|8O1\|7E1\|7O1\|8N2]`, e.g. `19200 8N1` → answers `/config`. Saved in Flash |
| `zrp6/ir/<1-7>/mode/set` | `ir`, or `serial <baud> [rs232\|ttl]` → answers `/mode`. Saved in Flash (`config/state`: `saved` or `error`) |
| `zrp6/ir/<1-7>/tx` | bytes to transmit, on a port in serial mode |
| `zrp6/net/set` | `dhcp`, or `static <ip> <netmask> <gateway>` → answers `net/state` (`saved, restarting`, `invalid` or `error`), then restarts to apply it |
| `zrp6/system/restart` | anything: the box restarts (about 5 s off the network) |


IR port 7 is HIGH OUT, the high-current output. Ports 3 and 4 of the UART are
RS-485 and are not exposed yet.

**IR ports as one-way serial.** Any of the seven IR ports can be switched to a
transmit-only serial port with `ir/<n>/mode/set`, for devices that take
commands on a 3.5 mm serial jack. 8N1, at 1200, 2400, 4800, 9600, 14400, 19200,
38400, 57600 or 115200 baud (57600 and up are generated in software and are the
least tolerant). `rs232` (the default) is RS-232 polarity on the port's 0-5 V
output — idle low, which is what an RS-232 receiver reads as idle; `ttl` is
idle high. In serial mode `ir/<n>/send` is ignored and `ir/<n>/tx` sends the
payload's bytes as they are; in IR mode it is the other way round. The mode
survives restarts. The box blocks for the length of the transmission (a byte
is about 1 ms at 9600), so keep messages short.

**RS-232 port settings** (`serial/<n>/config/set`) are saved and applied at
once; a port still starts only on first use, as below.

**Serial ports are opt-in.** Neither starts at boot; each starts at 9600 8N1
the first time something is published to its `serial/<n>/tx`. An idle
unconfigured UART is what keeps the front panel steady, and an RS-232 input
with nothing plugged in is not quiet — see docs/REVERSE-ENGINEERING.md.

**The remote code** is the RTI button's identity as measured off the air: the
counts of short symbols between long ones after the preamble. Twelve buttons
of the test remote gave twelve distinct codes. Button *names* are kept by the
interface in the browser; the box knows buttons only by code.

## Rules — remote button → action, stored on the box

A rule is a button code and any command above, written as one line and stored
in the box's own Flash. It runs whenever that button arrives, with nothing else
on the network, and survives power cuts.

```
<code>|<topic under the base>|<payload>

14.0.2.8.0.2.9|ir/1/send|nec 0x04 0x08
14.14.15|relay/2/set|ON
16.14.13|serial/1/tx|PWR ON\r
```

| topic | payload | |
|---|---|---|
| `zrp6/rules/<1-14>/set` | a rule line, or empty to delete | command |
| `zrp6/rules/<1-14>/state` | `saved` `deleted` `invalid` `busy` `error` | answer |
| `zrp6/rules/get` | anything | command |
| `zrp6/rules/<1-14>` | the rule line | answer to `get`, one per rule |
| `zrp6/rules/listed` | `done` | after the last one |
| `zrp6/rules/fired` | the slot that just ran | event |
| `zrp6/remote/code` | a code | **published by a client, it counts as a press** |

Several rules may share a code; they run in slot order. A payload can be a
full Pronto code — up to 420 bytes. Saving writes a complete new copy of the
store into the other Flash sector and marks it last, so a power cut mid-save
leaves the previous rules intact.

Publishing a code to `remote/code` is how the interface's *Test* button works,
and how a house controller can drive the box's own rules.

## Diagnostics

| topic | payload |
|---|---|
| `zrp6/debug/stackfree` | bytes of stack never touched since boot (retained) |
| `zrp6/debug/peek` | 4 hex digits: publishes 16 bytes of XDATA from that address to `debug/peeked` (bench tool) |
| `zrp6/debug/latch` | 0-255: write the whole panel latch (command). Bits 0-5 are the IR indicators, bit 6 red, bit 7 green. A status value written here stays until the box reboots or resets its network chip. |

## Examples

```bash
mosquitto_pub -t zrp6/relay/1/set     -m ON
mosquitto_pub -t zrp6/ir/6/send       -m 'nec 0x04 0x08 3'
mosquitto_pub -t zrp6/ir/6/send       -m '0000 0068 0000 0015 0060 0018 ...'
mosquitto_pub -t zrp6/serial/1/tx     -m 'KEY_POWER'

mosquitto_sub -t 'zrp6/#' -v
```

Pronto hex is accepted unmodified: it already expresses every mark and space as
a count of carrier periods, which is exactly what the firmware's IR command
wants.

## Network

The box gets its address by **DHCP**, and asks for host name `zrp6-XXXX` (the
last two bytes of its MAC), so it shows up in the router under that name. Give
it a reservation in the router if anything needs a fixed address for it.

If no DHCP server answers within ~10 s of power-up it comes up on its
**fallback** address — 10.0.0.77/24, gateway 10.0.0.1, unless `net/set` has
stored a static address — reports `net/mode` `fallback`, and keeps asking for a
lease once a minute. `net/set static <ip> <netmask> <gateway>` makes an address
fixed instead; `net/set dhcp` goes back. Either is saved to Flash and applied by
a restart. A wrong static address makes the box unreachable until it is
changed back over JTAG — the web page asks twice before sending one.

The MAC address is made up on the box's first boot (random, locally
administered, `02:5A:52:…`) and stored, so two boxes on one network differ.

## Keeping itself running

A hardware watchdog restarts the box if its main loop stops for more than a
few seconds. A newly installed firmware image that no client has logged in to
within 10 minutes restarts itself, so an image that cannot reach the network
counts its three boots and is rolled back to the previous one within about
half an hour, with no one touching the power (docs/UPDATING.md).
