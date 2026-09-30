# Field updates without JTAG

Goal: put new firmware on the box over the network, with no JTAG probe, and
with no sequence of events that can leave it unbootable.

**Status (2026-09-29): working on the box, PLD v8.** The PLD, the
bootloader and the application's upload path are on the bench unit, and every
test below passes on hardware: installs, power cuts in the middle of an
install at four different points, and automatic rollback of an image that
never confirms itself. Rules saved on the box survive an update. Since v8 the
web page has a Flash sector of its own and is updated separately, which gave
the firmware back ~13 KB of code space.

## Using it

```bash
firmware/build.sh                            # build/zrp6.bin and build/zrp6-ui.bin
python3 tools/update/push.py <box address>   # both; logs in as admin/admin
ZRP6_USER=joe ZRP6_PASS=... python3 tools/update/push.py <box address>
curl -u admin:admin --data-binary @build/zrp6.bin http://<box>/update
```

The box's address comes from DHCP (docs/MQTT.md, "Network"): `zrp6/net/ip`,
or the router's list under `zrp6-XXXX`. Uploads need a login — HTTP Basic auth
with any box user; without one the answer is `401`.

or open the box's web page, go to **Firmware**, and choose either file.

* **Firmware** (`build/zrp6.bin`) — the box restarts, installs the image
  with the status LED red, and answers again about 30 seconds after the
  upload ends.
* **The web page** (`build/zrp6-ui.bin`) — written straight into its sector
  and live at once; reload the page. There is no backup copy of the page: if
  an upload of it is interrupted, the box serves a small built-in page that can
  install it again. The firmware and MQTT carry on either way.

Rules saved on the box are kept through both.

A new firmware image is **on trial** until the first MQTT client logs in to
it — a browser tab, Control4, or the push tool checking in afterwards. If none
does, the box restarts into the previous image on the fourth boot. Nobody has
to cause those boots: an image still on trial after 10 minutes restarts
itself, and one that hangs is restarted by the hardware watchdog within about
two seconds (a main loop that stops calling in for 30 s counts as hung). So a
bad image goes back to the previous one within about half an hour on its own,
power untouched. Measured 2026-09-29 with a 60 s trial build: boot 1, 2, 3,
then back to the previous image at 191 s; a deliberate hang reset the box in
2.0 s, a deliberate stall in 32.9 s.
`zrp6/firmware/state` says which: `running`, `on trial, boot N of 3`, or
`no bootloader` on v5. `zrp6/firmware/ui` says whether a page is installed.

## Why a PLD change is needed

For the CPU to write a Flash sector, the PLD has to select that sector on a
**data** cycle, and a CPU cannot fetch instructions from the same Flash array
while it writes it. On v5 main Flash answers code fetches only, and the only
writable Flash is the secondary array. So the running program cannot write
itself, and nothing else runs.

v7 changes the decode so that:

* a **bootloader** in the secondary Flash answers code fetches at
  `0x0000-0x1FFF`. It owns the reset vector and is written only by JTAG;
* every main Flash sector can be reached for reading and writing through a
  **data window** at XDATA `0x8000-0xFFFF`, chosen by the PSD page register.
  Page 0 is the normal state, where the window is the W5100;
* code fetches never depend on the page, so the application can open a window
  to read and keep running.

The datasheet's limits shape all of this: each Flash sector select can have at
most **three product terms**, and each external chip select only **one**. The
bootloader window is a whole 8 KB because anything smaller would split fs0's
code range into more terms than it can have.

## Memory map (v10)

| Code (PSEN) | | Data (RD/WR) | page | |
|---|---|---|---|---|
| `0000-1FFF` | csboot2, bootloader | `0000-1FFF` | any | SRAM |
| `2000-7FFF` | the running slot's common sector | `2000-20FF` | any | csiop |
| `8000-FFFF` | the running slot's bank 0 or 1 | `3000-31FF` | any | 16C554 + panel latch |
| | | `4000-7FFF` | any | csboot0/1, the settings store |
| | | `8000-FFFF` | 0 | W5100 |
| | | `8000-FFFF` | 1..8 | fs0..fs7 |

The page register (csiop `E0h`) carries three things: bits 0-3 are the data
window page above, **bit 6 is the code bank** and **bit 7 the firmware slot**.
In PSDsoft, pgr6 and pgr7 are "logic" bits named `bank` and `slot`; they
qualify only the code terms, so the data window never depends on them.

| slot | sectors | window pages | holds |
|---|---|---|---|
| A | fs0 fs1 fs2 | 1 2 3 | an image: common, bank 0, bank 1 |
| B | fs3 fs4 fs5 | 4 5 6 | the same |
| — | fs6 | 7 | spare |
| UI | fs7 | 8 | the web page: a 16-byte header (`ZUI2`, CRC-32, length) and the gzipped blob |

## The image

`build/zrp6.bin`, 96 KB, is laid out exactly like a slot:

| offset | contents |
|---|---|
| `0x00000` | header: `ZRPB`, CRC-32 of `0x40-0x17FFF`, format 2; the rest 0xFF (the bootloader's) |
| `0x00040` | the page served when fs7 has none (`src/noui.html`) |
| `0x01C00` | the mDNS announcement template (`layout.py`) |
| `0x02000-0x07FFF` | common code |
| `0x08000-0x0FFFF` | bank 0, which runs at 0x8000 |
| `0x10000-0x17FFF` | bank 1, which runs at 0x8000 |

The first 8 KB are never fetched as code (the bootloader answers there), so
they hold data the application reads through the window. `firmware/layout.py
--v10` builds the image, the JTAG sectors for slot A and `build/zrp6-ui.bin`,
and fails the build if any code area leaves its window or **any call crosses
into a bank without the trampoline** (below).

## Why this cannot brick

* **The bootloader is never written by software**, and the PLD
  write-protects csboot2.
* **An update never touches the running slot.** It is written into the other
  one; only a restart changes which slot runs, and the old one is still there
  whole.
* **An image counts only once it is complete.** The bootloader programs the
  new header's generation, then the rest of it, and the magic last, after the
  body's CRC has been checked. A slot either holds a verified image or does
  not exist.
* **Nothing is ever copied.** The state is a handful of status bytes in each
  header, each one programmed once, so a power cut can leave at most one byte
  half-set — and a byte that is not 0xFF counts as set.
  `firmware/boot/test/sim.c` cuts the power during uploads, during finishing,
  and at every write of the trial and the rollback (391 checks).
* **When all of that is not enough, there is USB.** The bootloader answers
  USB by itself (docs/USB.md): with no image, on request, or for 3 s after
  power-up with a cable in. A box whose firmware or network settings are
  beyond saving takes a new image from a laptop, with no JTAG.

## What the bootloader does at every reset

```
rescue asked for (XDATA 0x00F0), or a USB cable in:  USB rescue mode first
pick the usable slot (valid, not failed) with the newest generation
  confirmed:                    run it
  fewer than 3 tries used:      set the next try byte, run it
  otherwise:                    set its failed byte, pick again (the other slot)
nothing usable: run any valid slot, confirmed first; none at all: USB rescue
set the page register's slot bit, flush the prefetch queue, jump to 0x2000
```

The application confirms its image when the first MQTT client logs in, and an
image still unconfirmed after 10 minutes restarts itself, so a bad image is
back on the previous one within about half an hour with nobody at the box.

## How the application writes Flash

It cannot, so it asks the bootloader. `lcall 0x0080` with DPTR pointing at a
`struct ota_req` in XDATA (`firmware/boot/ota.h`) runs one of:

| op | does |
|---|---|
| ERASE | erase the slot that is not running (refused while the running one is on trial) |
| WRITE | program up to 256 bytes at an image offset (24-bit; never the header) |
| FINISH | check the header and the body's CRC, then write generation, header, magic |
| CONFIRM | mark the running image as proved |
| UI_ERASE | erase the page sector |
| UI_WRITE | program up to 256 bytes of it (never its header) |
| UI_FINISH | check the page header and CRC, then write the header |

The bootloader finds the running slot from the page register's slot bit, and
every page-register write it makes keeps bits 6-7: only the data window moves.
That matters. The CPU returns from the call, and takes any interrupt during
it, from whichever slot bit 7 selects. The first bootloader 3 cleared the
register and returned with it at 0, and so returned into slot A. From slot B
that was an erase of the code it was returning into, and the box quietly
restarted on B. Updates then worked only from slot A: the first one always
succeeded and the next always failed. Found on the bench 2026-09-30; the stub
(`boot_api` in `src/bank.s`) still restores bits 6-7 too. The
call runs on the application's stack: at most 47 bytes (bootloader 3, return
address included), measured on the compiled binary in the s51 simulator
(`firmware/boot/test/abi.py`). It uses no RAM of its own; its only IRAM is
SDCC's frame pointer and bit bank, which the stub saves.

## Code banks

SDCC calls a function in a bank through `__sdcc_banked_call` (`src/bank.s`),
which saves the page register, sets bit 6 and jumps; `__sdcc_banked_ret`
puts the caller's bank back. A function in a bank that is called from common
code or from the other bank is declared `BANKED` (`src/bank.h`), and
`layout.py` fails the build on any plain call that crosses a bank. A banked
call costs one byte of stack. Common holds everything on the deepest stack
paths (the MQTT publish chain) and what everything calls; bank 0 the
application (app, rules, IR, serial, remote decoding, the store); bank 1 the
network extras (DHCP/mDNS, updates, users, SHA-1, HTTP). Every write to the
page register goes through `PAGE_WINDOW()`/`PAGE_NORMAL()`, which keep bits
6 and 7. Room, as of 2026-09-30 with USB: common 1.6 KB, bank 0 12.5 KB,
bank 1 15.5 KB.

Every bank switch also flushes the CPU's prefetch queue and branch cache
(BUSCON bits 7-6 off, then on). They are not flushed by a page change and
can hold instructions from the other bank. That is uPSD34xx erratum 34.12,
"Incorrect code execution when code banks are switched", and the first v10
trampoline did not do it. The bootloader flushes too, after setting the slot
bit and before it jumps to the application.

`POST /update` with the 96 KB image as the body is the upload. `ws.c` only
queues it; the main loop runs it (`ws_ota_run`), where the stack is
shallowest, because the bootloader call adds its 38 bytes on top. The handler
in `firmware/src/ota.c` erases STAGE, streams the body through WRITE a buffer
at a time, holds the header back for FINISH, answers, and restarts the chip
through the watchdog, which resets the PSD module too.

**The D4 bus fault** (docs/REVERSE-ENGINEERING.md) drops bit 4 on roughly one
W5100 access in 2000, and a 64 KB upload is 64K reads. It showed up in three
layers, each measured on the box:

* **Payload bytes.** Each byte is read three times, with a read of an
  unrelated register in between, and the three are ORed: the fault only ever
  turns a 1 into a 0.
* **Our copy of the ring pointer.** Re-reading RX_RD for every chunk gave the
  fault hundreds of chances per upload. It is read once and tracked
  (`tcp_rx_ptr`). Uploads went from 3 in 10 intact to 9 in 10 on v5.
* **The chip's own ring pointer.** On v7, where writing Flash makes each chunk
  slower, uploads failed about one in two. A trace build (`-DOTA_DEBUG`)
  caught it: the W5100's own RX_RD held 0xE7E9 after we had released 0xF7E9,
  which is bit 4 of the high byte. RX_RSR, derived from it, claimed 0x1000
  bytes, impossible for a 2 KB ring. The box read data that had not arrived,
  ran ahead of the chip, and RSR wrapped "negative", so seven 256-byte chunks
  came from the previous ring wrap. Now the chip's pointer must match ours,
  and RSR must be at most 2048, before a count is believed. 12 uploads of 12
  intact.

The image's CRC turns anything that still gets through into a clean refusal
with nothing written, and both the web page and `push.py` resend.

## Rules this adds

* **No interrupts, and no calls into the bootloader, while the rules store is
  being written.** The store and the bootloader share the secondary Flash
  array, and a fetch from it returns status bits during a write.
* **The page register's bits 6-7 are the code bank and slot.** Only the
  trampoline changes bit 6 and only the bootloader bit 7; window code uses
  `PAGE_WINDOW()`/`PAGE_NORMAL()`, and must put the window back to 0 before
  the W5100 is touched again.
* **`flash.sh` refuses an image and a PLD that disagree.** `build/LAYOUT`
  records which layout was built.

## Testing

| test | where | checks |
|---|---|---|
| `firmware/boot/test/sim.c` | host | every decision, under power cuts at every write |
| `firmware/boot/test/abi.py` | s51 | the real binary: call entry, return codes, stack, RAM |
| `firmware/test/test_ota.py` | box, v7/v8/v10 | install, power cuts at 3/8/13/18 s into an install, rollback |
| `firmware/test/test_settings.py` | box, v8 | logins, users, upload auth, IR serial mode, settings kept over a restart, static/DHCP (`--net`) |
| uploads, 12 in a row | box, v7 | 12 of 12 intact once the RX pointer guard was in |
| `test_ws_session.py` after uploads | box, v7 | stack: 28 bytes free, same as before updates existed |
| a saved rule, then an update | box, v7 | the rule is still there afterwards |

An install takes about 30 seconds from the end of the upload to the box
answering again. A JTAG flash is different: PSDsoft's JAM erases the whole
chip first, so it also clears the rules and the update slots.

## PSDsoft: how v7 was made

Done 2026-09-25 on the XP VM, driven with `tools/vm/vm.sh`; the result is
`research/captures/zrp6_v7.jam`, with the fitted `.abl` and `.frp` beside it.
The project on the VM had been left at v6, so this also put the '138 select
(`ecsnet`, pd2) back to **Active-Lo**. A copy of the project from before is in
`C:\PSDexpress\proj_backup_20260925`. Changes are made in the Design
Assistant, not in the `.abl`, which PSDsoft regenerates and discards:

1. **Page register:** three bits, pgr0-pgr2.
2. **Chip selects.** "code" means a PSEN fetch and "data" an RD/WR access,
   qualified the same way v5 qualifies them:
   * `csboot2`: code `0000-1FFF`, any page. New.
   * `fs0`: code `2000-7FFF`, any page, which is two terms, plus data
     `8000-FFFF` at page 1. It was code `0000-7FFF`.
   * `fs1`: code `8000-FFFF`, any page, plus data `8000-FFFF` at page 2.
   * `fs2`..`fs6`: data `8000-FFFF` at pages 3..7. Their old code-page
     mappings go.
   * `fs7`, `csboot3`: nothing.
   * the W5100 select (`8000-FFFF`, data): add **page 0**.
   * everything else (`rs0`, `csiop`, the '138 select, `csboot0/1`): unchanged.
3. **Memory space at power-up:** main Flash *and* secondary Flash in both
   program and data space (VM = 0x1E).
4. **Sector protection:** protect `csboot2`.
5. **Firmware:** one hex file with the bootloader at 0x0000-0x1FFF and a v7
   image from 0x2000, merged into fs0, fs1 and csboot2 only, so exactly those
   sectors are in the JAM (main 0-1, boot mask `$4`). `jam_patch_flash.py`
   swaps the bytes on every flash, so what was merged only had to land in the
   right sectors.
6. Fit: 20 product terms, and the report's equations match the design.
   Generate the JAM from JTAG-ISP → ATE Files.

### v8 (2026-09-29), on top of v7

* **Page register:** four bits, pgr0-pgr3 — every page compare now includes
  pgr3, which the firmware only ever sets for page 8.
* **`fs7`:** data `8000-FFFF` at page 8. The page sector.
* **Firmware:** a second hex file with the page sector at 0x8000-0xFFFF,
  merged into FS7, so the JAM carries main sectors 0, 1 and 7 (in that order
  in `gMAIN_DATA`) and boot sector 2. `flash.sh` patches all four.
* Fit: 21 product terms. Result: `research/captures/zrp6_v8.jam`, with
  `zrp6_v8.abl`/`.frp`; the project before it is in
  `C:\PSDexpress\proj_backup_v7_20260929`.

The VM's screen had shrunk to 1020x483, too short for the Pin Definitions
Done button; `tools/vm` can now drag windows, and the VirtualBox window itself
was enlarged from the lab PC's desktop session (see the PSDsoft note in the
project memory).

Flashing it:

```bash
firmware/boot/build.sh && firmware/build.sh && tools/flash/flash.sh
```

### v10 (2026-09-30), on top of v8

* **Page register:** pgr6 and pgr7 as *logic* signals `bank` and `slot`.
* **Chip selects**, code terms only (the data windows are unchanged):
  `fs0` `2000-7FFF` & `!slot`; `fs1` `8000-FFFF` & `!slot & !bank`; `fs2`
  `8000-FFFF` & `!slot & bank`; `fs3` `2000-7FFF` & `slot`; `fs4`
  `8000-FFFF` & `slot & !bank`; `fs5` `8000-FFFF` & `slot & bank`. At most
  three product terms each.
* **Firmware:** hex files for fs0-fs2 (slot A), fs7, csboot0/1 (the store) and
  csboot2, so the JAM carries main sectors 0, 1, 2, 7 and boot sectors 0, 1,
  2 (`gBOOT_DATA` is 24 KB, in that order). `flash.sh` reads the store over
  JTAG first and writes it back, so a JTAG flash keeps the MAC, users and
  rules.
* Fit: successful; 57 fuse bits differ from v8. Result:
  `research/captures/zrp6_v10.jam` with `.abl`, `.frp`, `.map`; the project
  before it is in `C:\PSDexpress\proj_backup_v8_20260929`.
* The Design Assistant's Done button is below the VM's 1028x538 screen. It is
  reached by moving the Design Assistant window up with the keyboard:
  `alt+minus`, `m`, then `up` twenty times and `enter` (`tools/vm/vm.sh`).
