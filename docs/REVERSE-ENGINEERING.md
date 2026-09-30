# Rebuilding an RTI ZRP-6 from a blank chip

The ZRP-6 is an RTI control processor built around an ST uPSD3434EV — a Turbo
8032 with a programmable logic block welded to it. This is the story of getting
one working again after a JTAG full-chip erase destroyed both the firmware and
the PLD configuration that made the firmware addressable at all.

Every number here was measured on the bench. Where an earlier conclusion turned
out to be wrong, it is marked, because the wrong turns are most of the value.

---

## The hole we started in

A full-chip erase on a uPSD does not merely blank the Flash. The PLD
configuration — the fuse map that decides which chip select answers at which
address — goes with it. Without it the 8032 has no memory map: no code space,
no RAM, no peripherals. The part powers up, fetches from nothing, and sits
there.

So the first job was not firmware. It was working out the *address decode* of a
board nobody has schematics for, and expressing it as PLD equations.

## The bug that hid everything: SRAM over the reset vector

The first reconstructed PLD looked right and the board stayed dead.

The PSD gives memory a **priority order**: SRAM (level 1) beats secondary Flash
(level 2) beats main Flash (level 3). Our `rs0` equation placed SRAM across
`0x0000`–`0x1FFF` *unqualified*, so it also covered the reset vector — and being
higher priority, it won. The 8032 was faithfully executing uninitialised RAM.

The fix is to qualify by access type. `PSEN`, `RD`, `WR` and `ALE` are PLD
inputs, so a select can distinguish a code fetch from a data cycle:

```abel
rs0   = (address >= ^h0000) & (address <= ^h1FFF) & (_psen);   " data only
fs0   = (address >= ^h0000) & (address <= ^h7FFF) & (!_psen);  " code only
```

**Lesson:** on a uPSD, never let a higher-priority region overlap the reset
vector in code space.

## Latched address needs two registers, not one

With the map fixed, the W5100 Ethernet controller still returned its own address
back at us — every read gave the low byte of the address it was asked for. That
signature ("address echo") means nothing drove the bus.

The cause was Table 121 of the datasheet: **Latched Address Output needs a
logic 1 in the Control register AND the Direction register.** We had written
`0x2003` and not `0x2007`, so `A8`–`A14` never left the chip. Every peripheral
access was aliasing into the same 256 bytes.

## The gate that took the longest: P3.1

The 16C554 quad UART would not answer. Not at either chip-select polarity, not
with any port pin forced, not anywhere in the 512-byte window it was supposed to
occupy. Reads came back as pure address echo for weeks.

It needed **two** things at once, which is why every single-variable test failed:

1. the **v5** PLD, with `ECSNET` **active-low** — a `74LCX138`'s `/G2A` wants a
   low to enable, and the v6 design drove it high;
2. **P3.1 held low** for the duration of the access.

The second came from RTI's own firmware, which clears P3.1 before touching a
UART register and sets it after. That had been in the disassembly the whole
time and was dismissed, because a note in this repo asserted P3.1 was the
address-latch enable. It is not — the W5100 reads identically with P3.1 either
way. That note was written during a period when the chip selects were swapped,
so its supporting evidence was a false positive.

When both conditions were finally true at once:

```
0x3000:  00 00 01 00 00 60 B4 FF
         RBR IER IIR LCR MCR LSR MSR SCR
```

Every register at its documented 16550 power-on value. And on a second read,
`MSR` had changed from `B4` to `B0` — reading it cleared the delta bit. A
floating bus cannot do that.

## Two compiler traps that looked like hardware faults

Both of these produced confident, wrong hardware diagnoses.

**A `__naked` function eats the caller's register.** SDCC emits no prologue for
a `__naked` mcs51 function and does not record which registers its inline
assembly uses, so the allocator hands the *caller* the same one. A delay routine
doing `mov r7, dpl` / `djnz r7` was called from a loop whose counter SDCC had put
in `r7`. The counter was zeroed every pass, the loop never ended, and the
firmware hung before writing the W5100's MAC address. Symptom: no network, no
clue why. Fix: `push`/`pop` every register a naked function touches.

**A peripheral write sourced from XDATA delivers `0x00`.** `XB(periph) = req[i]`
compiles to a `MOVX` read of the source immediately followed by a `MOVX` write to
the peripheral, and the peripheral latches zero. The identical write with an
*immediate constant* lands perfectly. That is why `FCR` and `MCR` always
configured and `LCR`, `DLL` and `DLM` never did — and it produced a wonderfully
convincing false diagnosis of "the data bus only carries D5 and D6".

The fix is to stage values in internal RAM first. There is a companion trap:
SDCC will **elide a `volatile` read** that follows a write to the same address,
so a readback confirms whatever you just wrote. Read through a pointer it cannot
value-track.

## What the pins actually are

Determined by driving each candidate and watching, not by reading datasheets.

| | |
|---|---|
| **IR ports 1–6** | csiop Port A bits 0–5, active **high** |
| **IR 7 / HIGH OUT** | Port A bit 6, active **low**, high-current |
| **IR indicator LEDs 1–6** | write-only latch at `0x3020`–`0x303F`, bits 0–5 |
| **Status LED** | same latch: bit **7 green**, bit **6 red** |
| **Relays 1–3** | Port C bits 2,3,4, active **high** (these are the ones that click) |
| **12 V triggers 1–3** | `P1.5`–`P1.7`, active **low** |
| **Contact inputs 1–3** | `P3.0`, Port C Data In bit 7, `P1.4` |
| **SIGNAL IN** | `P3.5` — idles **low**, pulses high |
| **16C554 UART** | `0x3000`, four channels on an 8-byte stride |
| **W5100 Ethernet** | `0x8000`–`0xFFFF` via `ECSPER` on PD1 |

Three of those contradict the disassembly-derived notes this project started
with, and the bench won each time:

- The relay/trigger **polarity was inverted** in the notes. The register reset
  values settle it independently: `Data Out C` resets to `0x00` and `P1` to
  `0xFF`, which is exactly the state that leaves every relay released. A design
  whose reset state energised all six relays would be perverse.
- The **status LEDs are not "mode bits"**. RTI sets latch bits 6–7 to `11` and
  this project copied that, which simply lit both permanently.
- **Port A was documented as "never driven, probably parked"**. It is the IR
  output port. RTI's own init writes Port A direction `0x7F` — seven bits for
  seven IR ports — and that line had been sitting in the disassembly unread.

## Measuring instead of assuming

Two measurements are worth copying to any similar project.

**The MCU clock, over the network.** The firmware spins a known number of `DJNZ`
pairs and replies; the host times the round trip and subtracts a baseline. That
gives **8.95 M machine-cycles/s** with no scope.

**The IR carrier, optically.** The same trick applied to the carrier loop gave a
figure that was *wrong by 9%* — it measures the loop, not the light. A Global
Caché IR Learner watching the actual emitter settled it: 38 kHz is `dly(15)`,
where the network-timed method had claimed `dly(20)`. **A hardware receiver
measuring emitted light is the ground truth**; anything timed through a software
loop is an estimate.

The same discipline matters for detection. Before attributing any capture to the
board, run a silent control — 60 s idle, expect zero captures. Without it an
ambient source is indistinguishable from success, and a `grep` that accidentally
matched a log banner briefly "proved" exactly the wrong conclusion.

## Reading the RTI remote

The RM-433 receiver drives SIGNAL IN with **demodulated baseband** — RTI's
handset IR really is 40 kHz, but the receiver strips the carrier before driving
the wire. The proof is arithmetic: captures tick at ~7 µs, a 40 kHz carrier
would appear as 1–2 tick runs, and the shortest run ever recorded is 46.

| | |
|---|---|
| half-bit | ~60 ticks ≈ 417 µs |
| long symbol | ~120 ticks ≈ 835 µs (exactly 2×) |
| frame gap | ~22 ms |
| duty skew | HIGH runs ~7 ticks short, LOW ~6 long — threshold each polarity apart |

It is **not Manchester**. Decoding it that way yields plausible hex that is
nonsense: an alternating signal comes out as long runs of a single bit. The data
is **pulse-distance** — carried in the spacing between the long symbols. Every
frame opens `[16, 16, 14, 12, 14]` (preamble plus system ID) and the tail is the
button.

Twelve buttons produced twelve tails, 1:1, each repeating identically up to
twelve times. **The codes are static, not rolling**, which matches RTI's
documented library of 256 fixed RF trigger codes.

One more trap: discard the first gap of a capture. It records only where
sampling started inside the preamble, and keeping it made 12 buttons look like
25 distinct frames.

## Recovering a secured part

Late on, programming began failing with `Full chip erase failed`, repeatedly,
on fresh power, with the CPU held in reset. JTAG was fine throughout.

`jam -v -aERASE` printed the answer: **`Part is secured`**. `PROGRAM`'s own
erase path fails silently on a secured device; the standalone `ERASE` action
handles it and cleared the part in 90 seconds, after which programming was
normal.

Two supporting details: drive the target reset line **high** rather than leaving
it floating, or ISP entry intermittently fails with
`ISC_ENABLE failed: readback 0xfe`. And `jam -aBLANKCHECK` is non-destructive
and is the trustworthy way to ask a device what state it is in — a hand-rolled
OpenOCD sector read reported all zeros while the device was perfectly healthy,
which briefly looked like a wiped chip.

---

## Reference

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the running system fits together
- [MQTT.md](MQTT.md) — the full topic tree
- [PSDSOFT.md](PSDSOFT.md) — building the PLD image in PSDsoft Express

## D4

The board has a hardware fault, and finding it took most of a night down several
wrong turns, so the evidence is written out here in full.

Serving the 17 KB interface out of Flash produced a page that was *almost*
right. One byte in seven thousand came back wrong, at a different offset every
time. Small transfers had hidden it for weeks: at that rate a 30-byte status
reply is correct 98.5% of the time and every hand test passes.

The wrong bytes were not random. Of nine isolated errors, nine were the same:

```
0xfe -> 0xee    0xdd -> 0xcd    0x77 -> 0x67
0xb7 -> 0xa7    0xbf -> 0xaf    0xff -> 0xef
0x5b -> 0x4b    0x5f -> 0x4f    0xfb -> 0xeb
```

Every one is bit 4, and every one is 1 going to 0. **D4 of the external data
bus occasionally fails to reach a valid high.** It is a hardware fault — most
likely a joint or a via on the AD4 net between the uPSD, the '573 latch and the
W5100 — and no firmware makes it go away.

### What was ruled out, and how

Four wrong diagnoses came before the right one, and each was published
confidently before being falsified:

* **"It is the MOVC-then-MOVX pair."** Staging Flash through SRAM first changed
  nothing. Staging through IRAM, which is the fix for the identical-looking
  hazard in `bus.c`, also changed nothing.
* **"It is the burst length."** Forcing the transfer into 1 KB, 2 KB and
  window-limited chunks left the rate flat.
* **"It is PSD Turbo mode."** Disabling it changed the rate from 1 in 6800 to
  1 in 7800, which is noise.
* **"It is Flash."** It is not. A command was added that checksums a code range
  in a tight `MOVC` loop and returns the result, so the arithmetic happens on
  chip with no network in the path (`K`, in `proto.c`). Thirty consecutive
  checksums of 4 KB of paged Flash agreed exactly — 123 KB read, zero errors.
  Reading is sound; writing to the W5100 is not.

A fifth claim was wrong in the other direction. Verifying every W5100 register
write reported 6673 failures — on a board sitting idle with no traffic at all.
`Sn_MR` and `Sn_PORT` simply do not read back between a close and an open. That
number was the instrumentation, not the hardware, and it is why the verified
writes are now scoped to registers written on an open socket.

### The part that mattered

A corrupt payload byte spoils one byte. A corrupt *pointer* spoils everything
after it, and looks nothing like corruption from the outside.

Roughly one page load in ten arrived with every byte past some offset being the
stream shifted by exactly one ring — correct length, correct TCP checksums,
wrong data. An on-chip check confirmed the transmit ring held the right bytes at
the moment of every send, 1080 chunks out of 1080, which ruled out the payload
path entirely. Recording `TX_WR` per chunk found it:

```
[ 1] sent=  1024 TX_WR= 55162
[ 2] sent=  2048 TX_WR= 51834   <- first bad byte is at 2048
```

`0xDB7A` was read back as `0xCA7A`. `tcp_commit()` added the chunk length to the
corrupted pointer, wrote it back, and every later segment carried the ring one
wrap behind.

`rd16()`'s read-twice guard was no defence, and that is the useful lesson: the
line fails as a function of what was on the bus a moment earlier rather than at
random, so two adjacent reads of the same address fail **identically** and agree
on the wrong value. Agreement is not evidence unless the reads are independent.

`sock_status()` is the same hazard in one byte: ESTABLISHED is `0x17`, and
`0x17` with D4 low is `0x07`. Acting on a single misread tore down live
connections — the box would answer two requests and then go quiet on that port.

### What the firmware does about it

Three things, all in `w5100.c`:

* **Payload** — every byte written into a W5100 buffer is read back and
  rewritten if it disagrees. Measured over 2.65 MB: 301 bytes caught, none
  unrecoverable.
* **Pointers** — `rd16_safe()` reads three times with a read of an unrelated
  register in between, so each attempt sees a different preceding bus state.
  Used for `TX_WR` and `RX_RD`, and to verify pointer writes.
* **Status** — `sock_status()` is confirmed the same way before any decision
  hangs on it.

Holding `TX_WR` in firmware instead of reading it back was tried and is worse:
the value still has to be read once when the socket opens, and a bad read there
poisons a whole connection rather than one chunk of it.

### Where it stands

Control is unaffected. MQTT packets are small enough to never wrap the ring, and
relay, trigger, serial and IR commands round-trip correctly with their state
acknowledgements over raw MQTT and over WebSocket alike.

Serving the interface is at 97-98% of page loads byte-exact. A failed load is a
refresh away, but it should not need one, and **the fix is a soldering iron, not
a compiler.** AD4 is pin 41 on the uPSD3434 in the reconstructed pinout; inspect
and reflow that net where it runs to the '573 and to the W5100.

### It reaches inside the W5100 too

Found 2026-09-25 while making 64 KB firmware uploads reliable
(docs/UPDATING.md). A trace of the receive loop, frozen at the first chunk
that repeated data from one ring-length earlier, showed the W5100's **own**
`Sn_RX_RD` at `0xE7E9` right after the firmware had released `0xF7E9` through
the verified pointer write. That is bit 4 of the high byte, gone inside the
chip. `Sn_RX_RSR`, which the chip derives from its pointers, then read
`0x1000`, impossible for a 2 KB ring. So a register that was written and
verified can still end up wrong in the chip.

The fix for long transfers is to believe nothing the chip derives until its
pointer matches ours. Before each chunk the upload checks the chip's RX_RD
against its own copy and rewrites it if they differ, and it rejects any
RX_RSR above 2048. Uploads went from about one in two intact to twelve in
twelve.

### And into the PSD's own Flash

Found 2026-09-28. A build that changed only the RTI decoder made every rule
save fail — yet the box answered "saved". Read over JTAG, the store's sectors
returned `27 67 27 67`: the secondary Flash array in error mode, DQ5 set. The
sector-erase command had never been accepted, the status read steady, so the
poll said "done", and the programs that followed tried to turn 0 bits back
into 1s. The same source with a few bytes added elsewhere worked, and shifting
only the Flash code did not help: it depended on where the code landed, which
decides what the bus carried just before each command write. That is this
fault again, on the uPSD's internal Flash.

Two changes, both kept: the MOVX read and write strobes are at their longest
(`BUSCON = 0xFF`, 7 clocks instead of 6), and every Flash erase and program in
the application is read back and retried (`flash.c`), so the store can no
longer report a save that did not happen. The failing build then passed the
whole rules test, power cycle included.

### The stale page was the close, not the pointers

The page loads that began with an earlier connection's bytes (about one in ten
on v7) turned out to be how sockets were closed. `tcp_disconnect()` sent FIN
and then CLOSE at once, and the socket teardown force-closed anything it found
in a closing state. A client that had not yet acknowledged the page's last
segment (macOS delays ACKs up to 200 ms) left that segment queued in the
W5100, which sent it as the first bytes of the next connection on the socket.
Now a close is FIN only, the chip finishes the handshake, and a socket in a
closing state gets a second before it is forced. 200 page loads of 200 clean,
where one in ten had been bad.

## The front panel blinked, and it was not the firmware

For two sessions the front panel blinked — IR indicator LEDs and the two-colour
status LED, no pattern, no rate — and two confident explanations came and went.

**First wrong answer: bus traffic from the main loop.** Plausible, partly true,
and not the cause.

**Second wrong answer: the latch's mode bits.** The rewrite had changed the
panel init from `0xC0` to `0x00`, and `0xC0` did turn out to matter: bits 6-7
of that latch are mode bits RTI sets to `11`, and writing `0x00` there at run
time hangs the 8032 outright (the W5100 keeps answering ping while UDP goes
silent). Restoring them fixed the hang. The panel went on blinking.

**What it actually was** came from a USB camera pointed at the panel in a dark
room, and a script that takes eight frames a quarter-second apart and reports
each light as steady or blinking. On the old, "stable" firmware, the panel was
blinking too. The one thing that had changed on the bench since the
restructure was `service/zrp6d.py` — the UDP bridge, left running for a week,
polling the box twice a second. Stopped: dead steady. Its traffic replayed one
call type at a time:

| replayed | panel |
|---|---|
| W5100 status polls | steady |
| SIGNAL IN captures | steady |
| **gated reads of both UARTs** | **IR LEDs flash** |
| nothing | steady |

**Every read of the 16C554 disturbs the front panel.** The new firmware then
did the same thing itself once serial port 1 was configured at boot — its
interrupt line reacts to whatever is on the wire, and each reaction is a UART
read. Neither serial port now starts until something is sent to it, the UART
is read only when its interrupt line says a byte is waiting, and an idle box
never touches it. Camera-verified steady.

The lesson is about method. Two theories were built on observations relayed
by someone looking at the panel and reported as fixed before being checked.
A camera and eight frames settled in ten minutes what reasoning had not in two
evenings — and the answer was outside the firmware entirely.

### Empty serial ports are not quiet

An RS-232 input with nothing plugged into it floats, and a floating input reads
as a continuous break — an endless stream of NULs with the break bit set.
`serial/2/rx` was publishing them several times a second. `uart_read()` now
discards any byte arriving with overrun, parity, framing or break set; the byte
still has to be read to clear the condition, it just is not data. That is
standard practice and should have been there from the start. This board's
channel 1 also raises data-ready with *no* error flags, which is the
fake-receive quirk this unit has always had, and is the second reason ports
are opt-in: port 1 comes up because it is always wired, port 2 starts the first
time something is sent to it.

`MCR` bit 3 (`OUT2`) gates the 16550's interrupt output; without it the INT
pin is tri-stated and P1.0-P1.3 read dead no matter what the UART is doing.
With it set, "has a byte arrived?" is an SFR read that costs no bus cycle.
Measured: sending bytes at the board raises P1.1.

## Nothing closes a socket that vanished

The broker accepted a keepalive in CONNECT and never enforced it. A client
killed without sending a FIN leaves its socket ESTABLISHED for ever, and there
are three client sockets: the third one to do that takes the box off the
network. It happened during testing, to me.

There was no clock to fix it with, so there is one now. Timer 0 free-runs as a
16-bit counter and `ticks()` counts its overflows, no interrupt involved. A
client silent for 1.5x a 60-second keepalive is dropped and its socket goes
back to listening, which is what MQTT says a server should do and what a box
behind a TV needs it to do.

**The tick was never 6.5 ms.** For a year the comments said so, and every
timeout was written against it. Measured 2026-09-29 by reading the tick
counter over MQTT (`debug/peek 0100`) four times ten seconds apart: 45.8 ticks
a second, so Timer 0 counts at 3.00 MHz — fOSC/12 from a 36 MHz crystal, while
the core runs 9 M cycles/s. Every timeout had been running 3.3 times long: a
send to a dead peer blocked for 13 s rather than 4, a closing socket was
forced after 3.3 s rather than 1 (which is why the socket test needed a 4.5 s
settle), and a remote button pressed again within 2 s was taken for the held
one. `board.h` now has `TICKS_S()` / `TICKS_MS()` from the measured rate, and
the watchdog (which counts machine cycles) resets about 1.9 s after it is last
fed.

## The stack is 175 bytes, and MQTT publish needs all of it

After the panel fix the box hung again — not at boot, not when idle, but on the
first MQTT PUBLISH after a SUBSCRIBE. Every time. The same path had worked for
hours.

What had changed was 16 bytes. The `V` recovery command had been rewritten to
stage its data in a `__data` array instead of XDATA (correctly — XDATA-sourced
peripheral writes deliver `0x00`), and that array came out of the 8032's IRAM.
The linker's report went from **175 bytes of stack to 157**.

The build is `--stack-auto`, so every call frame lives on that stack, and
PUBLISH is the deepest chain in the firmware:

```
broker_poll → handle → app_command (reentrant) → ack_relay → broker_publish
  → fanout → send_publish → ws_send → tcp_send → ring_put → ring_byte → xrd
```

SUBSCRIBE, which kept working, is shallower — no `app_command` frame. Eighteen
bytes was the difference between that path fitting and it running into the
register banks. Putting the IRAM back (the `V` command now stages through the
one byte `bus.c` already owns) restored 175 and the hang went with it.

Two rules that come out of this:

* **Read `Stack starts at` in `build/zrp6.mem` on every build.** 175 is known
  to work; anything meaningfully below it is a regression before it is
  anything else. It is one grep and it would have named this bug in a second.
* **`__data` is the scarcest memory on the part.** A 16-byte IRAM buffer is
  nearly a tenth of the stack. Stage through a single byte and loop.

And one that is about method rather than memory: this was found by comparing
the linker report between the last working build and the failing one, after an
evening of theories about what the failing path *did*. The number was sitting
in a file the whole time.


## An IR command rebooted the box

With the panel fixed, `ir/<n>/send` reset the connection and wiped the retained
state — a reboot. Relays and triggers were fine. IR is the deepest call chain
in the firmware: broker, packet handler, command callback, the IR handler and
its locals, then the whole publish path for its acknowledgement on top. With
175 bytes of stack for everything, it ran off the end.

Acknowledgements are now queued. A handler touches the hardware and queues its
`/state`; the main loop publishes it a moment later from the top, where the
stack is shallow. And the margin is measured rather than inferred: at boot the
free stack is painted with a pattern, and every few seconds the untouched part
is counted and published as `zrp6/debug/stackfree`. After exercising every
command: **26 bytes**.

### The gauge's first version wiped the register banks

```c
for (a = SP + 8; a <= 0xFF; a++) *((__idata uint8_t *)(uint8_t)a) = 0xA5;
```

SDCC 4.6 turned that into a countdown and dropped the base address: it painted
from the top of the stack region down to address 0, through the register banks
and its own return address. The box never reached the network. The fix makes
the pointer the loop variable, so there is no separate counter to reverse — and
the generated assembly was read before it was flashed, which is now a rule for
anything on the boot path. (Writing it as inline assembly instead crashed
SDCC's peephole optimizer, which `--peep-asm` runs over `__asm` blocks too.)

## The remote decoder was the wrong algorithm

The decoder moved onto the box during the rewrite had been written from
assumptions, not from the measurements: it captured 80 pulses (a frame is at
least 120) and decoded bit pairs (the encoding is counts of short symbols
between long ones). It was replaced with a port of the decoder that had been
validated on real presses, made self-calibrating — short and long are judged
from each capture's own statistics, per polarity, because capture ticks are
iterations of a polling loop whose speed depends on the compiler.

It is tested on the host (`firmware/test/test_rti.py`) against captures built
from the twelve codes recorded off the real remote, checked against two frames
captured verbatim: 20,000 presses entering during the first frame all decode,
across loop speeds from 0.4x to 2x with jitter, and in no scenario does it ever
return a wrong code — a partial frame yields nothing. It has not yet seen a
real remote on the new firmware.



## The wrong PLD, for a week

This is the one that explains most of the panel trouble above, and it is
embarrassing in exactly the way that makes it worth writing down.

There are two PLD images, `zrp6_v5.jam` and `zrp6_v6.jam`, that differ in five
fuse bytes. v6 drives the peripheral chip select into the 74LCX138 **Active-
High**. The '138 is then *disabled* for the cycles addressed to it and
*enabled* for almost every other bus cycle. Behind it sit the four 16C554 UARTs
and the front-panel latch. This was found and written down on 2026-09-16 — and
then `flash.sh`, carried over in the restructure, defaulted to v6. Every flash
for the next week put the wrong PLD on the box.

Under v6:

* the panel latch ignored every write addressed to it — the IR indicators
  never followed `debug/latch`, which read as "the latch is not what drives
  them";
* it caught fragments of whatever else crossed the bus instead — indicators
  stuck lit in arbitrary patterns, flickering with MQTT traffic, and flashing
  wildly during a remote capture, which writes a value on every pulse edge;
* serial transmit and receive were both dead.

On v5, measured 2026-09-23: the indicators follow the latch exactly (IR 1,
IR 6, all, none), the panel stays steady while remote buttons are pressed, and
serial works in both directions (the bench null modem is on port 2).

What this means for the sections above, which were written in the middle of
it:

* **"The front panel blinked, and it was not the firmware"** — the observations
  are real: the old UDP bridge's UART polling did make the panel flash. But
  that is *because* every bus cycle strobed the panel under v6. How much of it
  survives on v5 is unmeasured.
* **The mode bits.** The claim that bits 6-7 of the latch must stay `11`
  because writing `0x00` hung the CPU was measured on v6 and is not evidence of
  anything. An earlier session, on v5, found they are the red and green status
  LEDs. Re-tested 2026-09-23 on v5: `0x80` lit green, `0x40` lit red, and the
  box ran normally with both cleared. They are LEDs and nothing else. The
  firmware now shows red while booting or re-initialising the W5100 and green
  once it is running.
* **"Open: what drives the IR indicator LEDs"** — answered, and removed: the
  latch does, through a correctly-decoded '138.

The general lesson is about provenance. Every finding about the panel and the
UARTs from that week was taken on a board in a known-bad configuration, and
nothing recorded which PLD was on the box when each measurement was made.
`flash.sh` now defaults to v5 and says why, and the choice of JAM is the first
thing to check when anything behind the '138 misbehaves.
