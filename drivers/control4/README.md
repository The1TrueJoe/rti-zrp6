# Control4 driver for the ZRP-6

A DriverWorks (Lua) driver, `rti_zrp6.c4z`, that makes the ZRP-6's I/O show
up in Control4 as ordinary connections and turns RTI remote buttons into room
actions. It talks to the box over MQTT (the box is its own broker, see
[docs/MQTT.md](../../docs/MQTT.md)).

| Box I/O | Control4 connection | Topics |
|---|---|---|
| IR 1-7 (7 = HIGH OUT) | `IR_OUT` provider, bindings 101-107 | `ir/<n>/send` |
| IR port in serial mode | `RS_232` provider, bindings 111-117 (replaces the IR_OUT one) | `ir/<n>/mode/set`, `ir/<n>/tx` |
| RS-232 1-2 | `RS_232` provider, 201-202 | `serial/<n>/tx`, `serial/<n>/config/set`, `serial/<n>/rx` |
| Relays 1-3 | `RELAY` provider, 301-303 | `relay/<n>/set`, `relay/<n>/state` |
| 12 V triggers 1-3 | `RELAY` provider, 311-313 | `trigger/<n>/set`, `trigger/<n>/state` |
| Contacts 1-3 | `CONTACT_SENSOR` provider, 401-403 | `sense/<n>/state` |
| RTI 433 MHz remotes | events, variables, room actions | `remote/code` |

## Layout

```
drivers/control4/
  build.sh               stage + validate + package (build/rti_zrp6.c4z)
  package.json           pins esbuild, the one build dependency of the tab
  driver/
    driver.xml           properties, commands, actions, events, static connections
    rti_zrp6.c4zproj     packager manifest
    src/*.lua            driver source, concatenated in name order
    ui/                  the "Remote Buttons" and "Control" tabs (built into www/html/)
    www/documentation.html   shown on Composer's Documentation tab
  test/
    run.sh               everything below
    mock_c4.lua          mock DriverWorks environment with virtual time
    test_driver.lua      unit tests
    ui/harness.js        stand-in Director + DOM shim for the tab tests (node)
    ui/page_test.js      the built Remote Buttons tab against the driver
    ui/control_test.js   the built Control tab against the driver
    fixtures/director/   Director REST responses the tab reads (captured / cited)
    integration/         the MQTT client against a private mosquitto; run_box.py against a real box
```

| Source file | What |
|---|---|
| `01_globals.lua` | constants (binding IDs, box limits, timings), module tables |
| `02_utils.lua`, `03_json.lua` | helpers, JSON for the tab |
| `04_mqtt_codec.lua` | MQTT 3.1.1 packets and a stream parser (partial and coalesced reads) |
| `05_mqtt_client.lua` | the single connection: login, subscribe, keepalive, backoff, paced sending |
| `06_io.lua` | bindings, IR/serial mode switching, relays, contacts, topic routing |
| `07_rooms.lua` | replaying mapped room commands; fallback room/source lookups |
| `08_remotes.lua` | learned remotes and buttons, learn mode, events |
| `09_discovery.lua` | mDNS: parses the box's announcements, the Box picker, following a box's address |
| `10_ui.lua` | the Remote Buttons tab's requests (`UIRequest`) and pushes (`SendDataToUI`) |
| `11_properties.lua` | property handling and the Composer button editor |
| `12_callbacks.lua` | DriverWorks entry points, programming commands |

## Building

```bash
./build.sh            # -> build/rti_zrp6.c4z  (needs node, python3 + lxml; clones the packager into tools/)
./build.sh --stage    # just build/stage/: driver.lua assembled and syntax-checked, the tab built, driver.xml stamped
```

The version in `driver.xml` is the repo's commit count and `<modified>` is the
build time; `driver/driver.xml` itself is never edited by the build.

CI: [`.github/workflows/control4-driver.yml`](../../.github/workflows/control4-driver.yml)
runs on changes under `drivers/control4/`, on releases and by hand. It stages,
runs the tests, packages with `snap-one/drivers-driverpackager`, uploads the
`.c4z` as an artifact and attaches it to a release.

## Installing

1. Copy `rti_zrp6.c4z` into Composer Pro's drivers folder (or use *Driver >
   Add or Update Driver*) and add **RTI ZRP-6 I/O** to a room.
2. **Connection Mode**: *Direct to box (P2P)* (the default) or *Central
   broker* (see *Connection mode* below). Direct: the box is found by itself
   (see *Finding the box*): **Box** lists the boxes heard on the network and
   the first one is picked. With more than one box, pick yours. Set
   **Username** / **Password** (a new box has `admin` / `admin`). Leave
   **MQTT Port** 1883 and **Base Topic** `zrp6` unless the box was changed.
   The base topic must be 11 characters or fewer: the box keeps topics of at
   most 31 characters and `<base>/serial/1/config/set` is the longest.
3. **Connection Status** reads *Connected to ...* once logged in.

## Connection mode

**Direct to box (P2P)**, the default: the driver logs in to the box's own
MQTT broker, found by mDNS or typed into **Box IP Address**.

**Central broker**: the driver logs in to a house MQTT broker instead
(**Broker Host**, **Broker Port**, **Broker Username**, **Broker Password**,
**Broker Base Topic**), and that broker bridges to the box. The box cannot
connect out, so the house broker connects to it as an ordinary MQTT client.
The box/discovery properties are hidden in this mode (and mDNS listening
stops); the broker ones are hidden in P2P. Topics, bindings and remote
mappings are the same in both modes.

A mosquitto bridge to the box (in the house broker's `mosquitto.conf`, or a
file in its `conf.d`):

```conf
connection zrp6
address 10.0.0.112:1883
remote_clientid !bridge-house-zrp6
remote_username admin
remote_password admin
topic zrp6/# both 0
try_private false
cleansession true
keepalive_interval 30
```

- `try_private false`: the box is a plain MQTT 3.1.1 broker and does not know
  mosquitto's bridge protocol extension.
- The bridge is one MQTT client on the box, using one of its four sockets.
- Its client ID must start with `!`: the box never bumps a `!` client to free
  a socket, and it may bump ordinary MQTT clients when that is the only way to
  keep 1883 open. This driver uses `!c4-zrp6-<device id>` in both modes.
- `topic zrp6/# both 0` carries the box's retained state, commands and remote
  codes both ways unchanged, so **Broker Base Topic** stays `zrp6` (or whatever
  the box's base topic is).
- The box's limits still apply through the bridge: topics of at most 31
  characters and packets of at most 768 bytes.

## Finding the box

The box announces itself over mDNS (it cannot spare a socket to answer
queries, so it only announces): at boot, every 60 s and whenever its address
changes it multicasts one response to 224.0.0.251:5353 with

| Record | Name | Data |
|---|---|---|
| PTR | `_zrp6._tcp.local` | `zrp6-XXXX._zrp6._tcp.local` |
| SRV | `zrp6-XXXX._zrp6._tcp.local` | 0 0 1883 `zrp6-XXXX.local` |
| TXT | `zrp6-XXXX._zrp6._tcp.local` | `mac=025A52D17A36` |
| A | `zrp6-XXXX.local` | the box's IPv4 address |

(XXXX = the MAC's last two bytes, TTL 120.) The driver listens for these and:

- lists every box it has heard in **Box** (name, MAC, address) and picks the
  first one if none is chosen yet;
- remembers the chosen box by MAC (persisted), dials its last known address
  straight away after a restart, and when an announcement shows it at a new
  address, disconnects and reconnects there (**Selected Box**, **Box Address
  In Use**);
- *Forget Selected Box* (action) clears the choice.

**Box IP Address** is the override: anything typed there - an address, or a
name like `zrp6-7A36.local` - is used instead, and discovery only keeps the
list up to date. Empty it to go back to the discovered box.

**Listening for multicast is not documented for DriverWorks**, so the driver
tries two ways (**Discovery Listener**, *Auto*): first `C4:CreateServer(5353,
'', true)`, a UDP server socket on port 5353; then a UDP network connection
to 224.0.0.251:5353 with `MIRROR_UDP_PORT` (so its local port is 5353),
received through `ReceivedFromNetwork` - the same shape as Control4's SSDP
module (`drivers-common-public/module/ssdp.lua`). Any mDNS packet at all
proves a listener works; one that hears nothing for 3 minutes is swapped for
the other. **Discovery Status** shows which one is running and how many
packets it has heard. If neither hears anything on a given controller, type
the address (or `zrp6-XXXX.local`, if the controller resolves `.local`) into
**Box IP Address**.

## Binding

In *Connections > Control/AV*, bind:

- IR device drivers (TVs, cable boxes...) to **IR Out 1-7**.
- Serial device drivers to **RS-232 1/2**, or to **IR n Serial Out (one-way)**
  after switching that port (below).
- Relay-type drivers (screens, lifts, doors, garage) to **Relay 1-3**, and
  anything that wants a 12 V trigger to **Trigger 1-3 (12 V)** - a trigger is
  a relay whose "closed" is 12 V on.
- Contact sensor drivers (door, motion, doorbell) to **Contact 1-3**.

## IR ports as one-way serial

Set **IR n Mode** to *Serial (one-way)*. The port's `IR_OUT` connection is
removed and an `RS_232` connection called *IR n Serial Out (one-way)* is added
(re-bind the device after switching), and the driver publishes
`ir/<n>/mode/set serial <baud> rs232|ttl`. **IR n Serial Baud** *Auto* takes
the baud from the bound device driver's `<serialsettings>` when the controller
can read it (`C4:GetDriverConfigInfoById`, OS 4.2+), otherwise 9600.
**Polarity** *RS-232* idles low (the usual 3.5 mm serial jack), *TTL* idles
high.

**RS-232 n Baud / Format** work the same way for the two real serial ports
(`serial/<n>/config/set`).

**The box is the source of truth for port settings.** They can be changed in
three places - these properties in Composer, the box's web page, or MQTT
(`ir/<n>/mode/set`, `serial/<n>/config/set`) - and the box keeps them in Flash.
Whenever the box reports its settings (its retained `ir/<n>/mode` and
`serial/<n>/config`: on every connect, and live when they change), the driver
**adopts** them: the properties above and the IR_OUT / RS_232 connections are
changed to match, and nothing is sent back. The driver sends a setting to the
box only when:

- an installer edits one of these properties in Composer (the driver's own
  property updates are recognised and never sent back), and only if the box
  does not already have that value;
- a port set to *Auto* gets a bound device whose serial settings differ;
- the *Resend Port Configuration* action is run (sends every port's
  properties, whatever the box has).

A reconnect never sends anything. An *Auto* property stays *Auto* when the
box's value is what *Auto* would give; otherwise it is set to the box's value.
**Box Port Settings** shows what the box last reported.

## Remote buttons

Select the driver in Composer Pro and open its **Remote Buttons** tab, next
to Properties. It is the only place remote buttons are managed.

1. *Learn a button*, optionally type a name, *Start*, press the button on the
   RTI remote.
2. The editor opens: name it and add actions. Each action is a room plus one
   of **that room's Control4 commands, as Director lists them** (with inputs
   for the command's parameters), *Select source...* (the room's Watch and
   Listen sources), or *Programming event only*. Several actions run in
   order, and they can target different rooms. A `START_x` command whose
   `STOP_x` exists shows once, as "(tap)": a remote press is a single event,
   so the driver sends both.
3. *Test* runs a button's actions; *Recent presses* shows the last 20 codes
   with an *Add* for any that are not learned yet.

Remotes are told apart by the code's preamble: the bench remote's codes have
none (its id is `""`), any other remote's are `p<preamble>:<button>`. Each
remote gets its own list; *Rename remote* names it.

A saved action is what gets replayed, with `C4:SendToDevice(room, command,
params)`:

```json
{"kind": "command", "room": 14, "roomName": "Room", "command": "SET_VOLUME_LEVEL", "params": {"LEVEL": "35"}, "label": "Set Volume Level"}
{"kind": "command", "room": 14, "command": "START_VOL_UP", "release": "STOP_VOL_UP", "params": {}}
{"kind": "source", "room": 14, "source": 36, "label": "Cable TV"}
{"kind": "event"}
```

Mappings saved by 1.0.0 (fixed action names like `VOLUME_UP`) are turned
into these on load.

**Programming.** Every press fires *Remote Button Pressed*; a learned button
also fires its own event *Button: <remote> / <name>*, an unknown one fires
*Unmapped Remote Button Pressed*. `LAST_REMOTE_CODE`, `LAST_REMOTE_BUTTON` and
`LAST_REMOTE_NAME` hold the last press. The *Press Remote Button* command runs
a learned button from programming.

Mappings are kept with `C4:PersistSetValue` and survive driver updates and
Director restarts.

## Programming commands

| Command | |
|---|---|
| Set Output | Relay 1-3 / Trigger 1-3: On, Off, Toggle, Pulse (Time ms) |
| Send IR | port 1-7, Pronto hex or `nec <addr> <cmd> [repeats]` |
| Send Serial | RS-232 1/2 or an IR port in serial mode; `\r \n \t \\ \xHH` escapes |
| Press Remote Button | runs a learned button's actions (programming, so it stays) |

Variables: `CONNECTED`, `SERIAL_1_RX`, `SERIAL_2_RX` (last line received) and
the remote ones above. Events: *Connected*, *Disconnected*, *RS-232 1/2 Data
Received*, and the remote ones above.

## How it talks to the box

- One MQTT 3.1.1 connection (`C4:CreateNetworkConnection` binding 6001, TCP),
  username and password in CONNECT, client ID `!c4-zrp6-<device id>` (a
  priority client the box never bumps), clean session, subscribe `zrp6/#`,
  QoS 0.
  Director's own reconnect (`KEEP_CONNECTION`) is off: the driver reconnects
  itself, 2-4-8-16-30-60 s apart, and never opens a second socket while one is
  up (the box has four and never drops an MQTT client). A refused login
  (CONNACK 4/5) waits 30 s, since every failed login locks the box for 2 s.
- PINGREQ after 15 s without sending (keepalive 30 s by default, property
  10-60 s); no traffic for 1.5x keepalive drops the connection.
- Packets are sent one at a time, 40 ms apart, and after an IR code or a
  serial string the next one waits until the box has finished sending it: the
  box reads 768 bytes at a time and loses a packet that straddles that.
  Anything bigger than 760 bytes is refused with an error.
- IR codes from device drivers arrive as `PULSE` with the `<ircode>` XML
  (Pronto `<pattern>`, `<repeatcount>`). The box plays a Pronto code once, so
  the driver unrolls the repeat section `repeatcount` times into the
  once-section (fewer if it would not fit in one message).

## Tests

```bash
test/run.sh
```

Builds the stage, runs `test/test_driver.lua` under every interpreter found
(`lua`, `luajit` - controllers run LuaJIT, i.e. Lua 5.1), and, if mosquitto is
installed, `test/integration/run_integration.py`: the real driver.lua with its
network binding bridged to a private mosquitto (random port on 127.0.0.1,
password file) - login, retained state, publishes, remote learning, PINGREQ,
wrong password. `test/ui/page_test.js` runs the **built** tab
(`build/stage/www/html/index.html`, with a small DOM shim) against
driver.lua, with a stand-in Director that answers from
`test/fixtures/director/` (see its README for where each response comes
from): learn, rooms and room commands from Director, parameter inputs,
sources, typed commands, save, the exact commands replayed on a press, edit,
rename, delete, and the fallbacks (no REST access, no socket).
`test/ui/control_test.js` runs the built Control tab the same way and reads
what the driver publishes to the box: live states, header, On/Off/Pulse,
IR and serial sends with and without escapes, received data, a mode switch
on the box, a half-typed code surviving pushes, and the not-connected case.
None of these talk to a ZRP-6.

`python3 test/integration/run_box.py <box-ip>` runs the driver against a real
box (log in, relays, contacts, IR mode switch, a remote code to a room action)
and leaves it as it found it.

## The Control tab

The driver's **Control** tab (next to Properties and Remote Buttons) drives
the box directly - Composer has no double-click popup for a Lua driver, so
this stands in for one:

- **Header**: connection status, the address in use (box or broker), the
  connection mode, the box's firmware version.
- **Relays 1-3 and 12 V triggers 1-3**: live state, *On* / *Off* / *Pulse*
  (Pulse uses **Relay Pulse Time (ms)**).
- **Contacts 1-3**: live OPEN / CLOSED.
- **IR ports 1-7**: the box's mode for each. In IR mode, a box for a Pronto
  code or `nec <addr> <cmd> [repeats]` and *Send*; in serial mode, a text box
  with an *escapes* toggle (`\r \n \t \\ \xNN`) and *Send*.
- **RS-232 ports 1-2**: the box's config, a text box with *escapes* and *Send*,
  and the last data received (`serial/<n>/rx`).

It talks to the driver like the Remote Buttons tab: `UI_CONTROL_GET_STATE`,
`UI_SET_OUTPUT {kind, n, action}`, `UI_SEND_IR {port, code}` and
`UI_SEND_SERIAL {kind, port, data, escapes}` through `UIRequest`, and a
`CONTROL_DATA` push on every change (a relay or contact changing, a port mode,
received data, the connection). Rows are built once and only updated, so a
push never wipes a half-typed code.

## Working on the tabs

The tabs' source is in `driver/ui/`; `build.sh` bundles each with esbuild into
one self-contained file under `www/html/` (`index.html` = Remote Buttons,
`control.html` = Control). The built files are not kept in git.

```
driver/ui/
  shell.html            Remote Buttons page skeleton; /*STYLE*/ and /*SCRIPT*/ are filled in by the build
  control.html          Control page skeleton
  styles.css            shared by both tabs
  build.mjs             esbuild: each page's entry -> one IIFE, styles.css -> inlined
  src/
    main.js             Remote Buttons: wiring, driver messages -> views
    director.js         Director client: REST, the driver's UIRequest bridge, socket.io pushes
    catalog.js          rooms / room commands / sources, from Director with driver fallbacks
    commands.js         Director's command list -> picker entries (params, START_/STOP_ taps)
    store.js            page state
    dom.js              element helpers
    views/lists.js      remotes, buttons, recent presses
    views/learn.js      the learn dialog
    views/editor.js     the button editor
    views/actionRow.js  one action: room, command or source, parameters
    control/main.js     Control: wiring, CONTROL_DATA -> panels
    control/panels.js   Control: relay/trigger, contact, IR and RS-232 rows
```

```bash
npm ci                   # once: installs the pinned esbuild
npm run build:ui         # -> build/ui/index.html, control.html (open to check the markup; data needs Composer)
npm run watch:ui         # rebuild on every change
./build.sh --stage && node test/ui/page_test.js build/stage/driver.lua && node test/ui/control_test.js build/stage/driver.lua
```

Plain JavaScript modules with JSDoc types, no framework and no TypeScript:
each tab is a few lists and dialogs, esbuild bundles them without any other
tooling, and the output (targeting Chrome 80, as Composer's webview is
Chromium-based) is one readable file. To try a change in Composer, build and
update the driver; the tab reloads with it.

## How the Remote Buttons tab works

The tab is declared in `driver.xml` inside `<config>`:

```xml
<tabs>
  <tab name="Remote Buttons" file="html/index.html"/>
</tabs>
```

and the page ships at `www/html/index.html` (the c4zproj packages `www/`
recursively). This is the mechanism finitelabs' drivers on DriverCentral use
for their Requests / Expressions / Settings tabs
([control4-finite-labs-essentials](https://github.com/finitelabs/control4-finite-labs-essentials)
`drivers/network_requests`, `drivers/variable_expressions`;
[control4-influxdb](https://github.com/finitelabs/control4-influxdb)), and
that junohouse/control4-adapter's FINDINGS.md (section 3) describes for
Control4's own agents. Snap One does not document it.

Composer opens the page with `?apihost=<controller>&driverid=<id>&apitoken=<token>`,
and the page talks to the driver through Director's REST API:

| Direction | How |
|---|---|
| page -> driver | `POST https://<apihost>/api/v1/items/<driverid>/commands` with `{command, tParams: {DATA: base64(JSON)}, async: false}`; Director calls the driver's `UIRequest(command, tParams)` |
| driver -> page | `C4:SendDataToUI(command, {json = ...})`; the page loads Director's `/socket.io/socket.io.js`, subscribes with `GET /api/v1/items/<driverid>/datatoui?SubscriptionClient=<engine id>`, and receives each push as a `devicecommand` |

The page also reads the project from Director directly, with the same token:

| What | Endpoint | Shape relied on |
|---|---|---|
| rooms | `GET /api/v1/rooms`, else `GET /api/v1/locations` | items with `typeName: "room"` / `type: 8`, `id`, `name`, `roomHidden` (captured from an EA-3) |
| a room's commands | `GET /api/v1/items/<room>/commands` (then `/api/v1/locations/<room>/commands`, `/api/v1/rooms/<room>/commands`) | `[{command, display, params: [{name, type, values: [{id, name}], minimum, maximum}]}]` |
| a room's sources | `GET /api/v1/agents/ui_configuration` + `GET /api/v1/items?tree=false` for names | `experiences[]: {room_id, type: watch/listen, sources: {source: [{id}]}}` |

If Director cannot be read, rooms come from the driver (`C4:GetDevices`,
which carries each device's `roomId`/`roomName`), sources from the room's
`GET_WATCH_DEVICES`/`GET_LISTEN_DEVICES` UI requests, and a command can be
typed. (The room list was empty on a real controller because the driver used
`C4:GetProjectHierarchy`, which junohouse/control4-adapter found returns
nothing usable on OS 3.3.3.)

Requests are `UI_GET_STATE`, `UI_GET_ROOMS`, `UI_GET_SOURCES`, `UI_LEARN`,
`UI_CANCEL_LEARN`, `UI_SAVE_BUTTON`, `UI_DELETE_BUTTON`, `UI_TEST_BUTTON` and
`UI_RENAME_REMOTE`. Pushes are `STATE_DATA` (everything the page draws;
also sent on every remote press, learn result and connection change),
`ROOMS_DATA`, `SOURCES_DATA`, `SAVED`, `TESTED` and `ERROR`. Values travel as
one JSON string because `SendDataToUI` cannot carry booleans. `UIRequest` also
returns `{"_command", "data"}` as JSON, and the page uses that if the socket
never connects. Nothing listens on a port of its own.

## What needs a real Control4 system to confirm

- **IR_OUT provider.** The `PULSE` + `<pattern>` handling follows Pulse-Eight's
  shipping Control4 drivers (`PRX_CMD.PULSE` in their `p8proxy.lua`); Snap One
  does not document the IR_OUT provider side. `START`/`STOP` style commands for
  held buttons are treated as a single send / ignored.
- **RS_232 provider.** Not documented anywhere I could find. The driver sends
  any `DATA`/`data`/`STRING` parameter it receives on a serial binding and
  logs anything else to **Last Unhandled Proxy Command** - turn on Debug Mode
  and bind a serial device to see what Director actually sends. Received
  serial data is not passed back to a bound device driver (there is no
  documented way to do it); it is available to programming. Serial settings
  come from the bound driver's XML (OS 4.2+) or from Properties.
- **Dynamic IR/serial bindings.** `AddDynamicBinding` with class `IR_OUT` /
  `RS_232` is the documented API (used here in OnDriverInit so Director can
  restore connections), but switching a bound port has not been tried in
  Composer.
- **mDNS listening.** Both listeners are best guesses: whether Director lets a
  driver bind UDP port 5353 next to the controller's own mDNS stack, and
  whether multicast reaches that socket, can only be seen on a controller
  (watch **Discovery Status**). Whether the controller resolves
  `zrp6-XXXX.local` names is also unknown.
- **Director REST from the tab.** Whether the token Composer hands the tab
  may read `/api/v1/rooms`, `/api/v1/items/<room>/commands` and
  `/api/v1/agents/ui_configuration` (finitelabs' tabs only use their own
  item's endpoints). The room list shape is from a real EA-3 capture; the
  command list shape is from open-source clients' parsers, not a capture.
  Both have driver-side fallbacks.
- **Remote Buttons tab.** The `<tabs>` element and the REST/socket.io bridge
  are copied from finitelabs' shipping drivers and tested here against a
  stand-in Director, not a real one. Unconfirmed: whether Director's REST
  reply to a command carries `UIRequest`'s return value (the page does not
  depend on it), and which Composer Pro versions show tabs (finitelabs require
  OS 3.3.0, so this driver now does too). If the tab does not show, the same
  editing is available through the properties and actions.
