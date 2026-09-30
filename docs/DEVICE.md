# The RTI ZRP-6: everything known about the hardware

A reference for the box itself: what is on the board, where every signal
goes, how the memory is mapped, what the chip gets wrong, and how to get code
onto it. It collects what the project measured between 2026-09-09 and
2026-09-30. The story of how each fact was found, including the wrong turns,
is in [REVERSE-ENGINEERING.md](REVERSE-ENGINEERING.md). The firmware that
runs on it now is described in [ARCHITECTURE.md](ARCHITECTURE.md),
[MQTT.md](MQTT.md), [UPDATING.md](UPDATING.md) and [USB.md](USB.md).

Unless a line says otherwise, a fact here was **measured on the bench box**.
"RTI's code" means it was read from RTI's own firmware, disassembled from a
dump of the original Flash. That dump is not in this repository. "Inferred"
means reasoned, not measured.

## At a glance

| | |
|---|---|
| Processor | ST **uPSD3434EV-40U6**: Turbo 8032 core plus a PSD module, TQFP80, 3.3 V |
| Clock | **36 MHz** crystal; the core runs about 9 M instructions/s (4 clocks per cycle) |
| Main Flash | 256 KB, eight 32 KB sectors (fs0-fs7) |
| Secondary Flash | 32 KB, four 8 KB sectors (csboot0-3) |
| SRAM | 256 B internal (IRAM, the stack) and 8 KB PSD SRAM (XDATA) |
| Ethernet | WIZnet **W5100**, parallel bus, 4 sockets |
| Serial | Exar **16C554** quad UART at 14.7456 MHz: 2x RS-232, 2x RS-485 |
| IR | 6 zone outputs and 1 high-current output, all on PSD Port A |
| Relays | 3, dry contact, PSD Port C |
| Triggers | 3 x 12 V outputs, P1.5-P1.7 |
| Contacts | 3 voltage-sense inputs |
| Remote input | SIGNAL IN for an RTI RM-433 receiver, P3.5 |
| USB | the uPSD's own full-speed device port; cable sense on P3.4 |
| Front panel | 6 IR indicators and a red/green status LED, one write-only latch |
| JTAG | the uPSD's ISP port: Flash, and the PLD through JAM STAPL only |

## Parts on the board

| Part | What it does |
|---|---|
| ST uPSD3434EV | the processor, the Flash, and the programmable logic that decodes every chip select and maps Flash pages |
| WIZnet W5100 | Ethernet with TCP/IP in hardware; MAC and PHY built in; 16 KB of socket buffers |
| Exar 16C554DCQ | four 16550-compatible UARTs |
| SP3074E | RS-485 full-duplex (4-wire) transceiver: the RTI COM port |
| SP3072E | RS-485 half-duplex (2-wire) transceiver: the expansion port |
| RS-232 driver | for serial ports 1 and 2 (part not identified) |
| 74LCX138 | 3-to-8 decoder: fans one PSD chip select out to the UARTs and the panel latch |
| 74HC4002 | dual 4-input NOR: combines interrupt lines (inferred from the part) |
| 74x573 | address latch for A0-A7 on the multiplexed bus |

The uPSD can drive external chip selects on Port D only (datasheet Table
120), which on the TQFP80 means two pins, PD1 and PD2. So one of them selects
the W5100 directly, and the other enables the '138 for everything else.

## Clocks and time

| | |
|---|---|
| fOSC | 36 MHz. Timer 0 at fOSC/12 counts **3.00 MHz**: measured by reading the firmware's tick counter over MQTT 30 s apart (45.8 overflows/s) |
| Timer 0 overflow | **21.8 ms** (16-bit). Old comments said 6.5 ms and every timeout ran 3.3x long; `board.h` has `TICKS_S()` / `TICKS_MS()` |
| USB clock | the uPSD's PLL: CCON1 = **0x61** (PLLM 6, PLLD 1) makes exactly 48 MHz from 36 MHz. PLLEN, then about 200 µs, then UPLLCE |
| 16C554 | 14.7456 MHz; divisor 96 (0x60) is 9600 baud |
| IR carrier | a busy-wait loop; `dly(17)` is **38.0 kHz**, measured with a Global Caché IR learner. Timing it over the network over-estimated by about 9% |
| Watchdog | WDKEY (SFR 0xAE) and WDRST (0xA6). Anything but 0x55 in WDKEY enables it. It resets the MCU **and the PSD module**, which makes it the clean way to reboot. It fires about 1.9 s after the last feed at WDRST = 0 |

## The processor's pins

8032 ports:

| Pin | Use | Notes |
|---|---|---|
| P1.0-P1.3 | 16C554 interrupt lines | keep high (inputs); an enabled UART interrupt moves exactly its bit |
| P1.4 | contact 3 | high = open |
| P1.5-P1.7 | triggers 1-3 | **active low**; the reset value (1) is off |
| P3.0 | contact 1 | high = open. Also the 8032's RXD, unused as such |
| P3.1 | 16C554 gate | must be **low** during every 16C554 access, as RTI's driver does; with it high the window reads as address echo. It does *not* gate the address latch (an early belief, disproved) |
| P3.4 | USB cable sense (VBUS) | high while a host powers the cable. RTI's firmware reads it three times before entering USB mode (RTI's code, 0x427F); on the box it reads 1 with a cable in |
| P3.5 | SIGNAL IN | the RM-433 receiver. Idles **low**, pulses high (see below) |
| P3.6, P3.7 | /WR, /RD | the bus |
| P4.1 | W5100 hardware reset | clearing it drops the box off the network at once and wipes the W5100's MAC and IP. RTI's init pulses it first thing (RTI's code, 0x1AC6) |

All of P1, P3 and P4 are set to plain I/O by zeroing P1SFS0/1, P3SFS and
P4SFS0/1, exactly as RTI's init does.

PSD ports (through csiop, at XDATA 0x2000):

| Port | Use | Setup |
|---|---|---|
| A | IR outputs: PA0-PA5 = IR 1-6 **active high**; PA6 = IR 7, the HIGH OUT, **active low** | Control A 0x00, Direction A 0x7F, idle Data Out A = **0x40** (0x00 would drive HIGH OUT continuously). RTI's init writes the same (RTI's code, 0x1AF5) |
| B | PB0-PB6: latched address A8-A14 | **Control B 0x7F and Direction B 0x7F**: datasheet Table 121 needs both, or A8-A14 never leave the chip and every external address above 0xFF aliases down |
| C | PC2-PC4 = relays 1-3, **active high**; PC7 = contact 2 (high = open) | Direction C 0x7F; relays released at reset |
| D | PD1 = W5100 /CS (`ecsper`); PD2 = 74LCX138 enable (`ecsnet`) | both **active low** in the PLD; Direction D 0x03, Drive D 0x04 |

PB0-PB6 are TQFP80 pins 80, 78, 76, 74, 73, 71 and 67. AD4, the bus line with
the fault described below, is pin 41 (reconstructed pinout).

## Memory

### The PSD module

The PSD module decides, for every bus cycle, which memory or chip select
answers. It uses the address, whether the cycle is a code fetch (/PSEN) or a
data access, and the **page register** (csiop + 0xE0, XDATA 0x20E0). Its
equations are written in PSDsoft and fitted into a fuse array that only JTAG
can program, and only through JAM STAPL (see JTAG below). The current design,
v10, is described in [UPDATING.md](UPDATING.md#memory-map-v10). Its source
files are kept with the project's captures, not in this repository.

Things about the PSD that cost days:

- **Priority.** PSD SRAM is priority level 1 and main Flash level 3. An SRAM
  select covering 0x0000 in *both* code and data space masks the reset
  vector: the core fetches uninitialised SRAM and never runs, while JTAG,
  programming, verify and the fitter all report success. Every select is
  qualified by `_psen` (data) or `!_psen` (code).
- **Product terms.** A Flash sector select has at most three product terms,
  and an external chip select one.
- **Edits to the `.abl` file are discarded.** PSDsoft regenerates it from its
  Design Assistant; equations must be changed there.
- **Page register bits can be logic, not paging.** In the Design Assistant
  each page-register bit is either part of the page number or a named logic
  signal. v10 uses bits 0-3 as the data-window page, bit 6 as the code bank
  and bit 7 as the firmware slot.
- The page register reads back, and the data-window decode uses only bits
  0-3, so bits 6-7 can be kept across window moves.

### Code space (v10)

| Address | Answers | |
|---|---|---|
| 0x0000-0x1FFF | csboot2 | the bootloader; it owns the reset vector and the interrupt vectors (it forwards each to 0x2000 + vector) |
| 0x2000-0x7FFF | fs0 (slot A) or fs3 (slot B) | the running slot's common code; its first 8 KB is data only |
| 0x8000-0xFFFF | fs1/fs2 (A) or fs4/fs5 (B) | the running slot's bank 0 or bank 1, by page bit 6 |

### Data space, XDATA (v10)

| Address | What | |
|---|---|---|
| 0x0000-0x1FFF | PSD SRAM (8 KB) | 0x00F0-0x00F1: the USB rescue request, which survives a watchdog reset; program variables from 0x0100 |
| 0x2000-0x20FF | csiop | PSD ports, page register (0xE0), VM register (0xE2) |
| 0x3000-0x301F | 16C554 | four channels, 8-byte stride, behind the '138 |
| 0x3020-0x303F | front-panel latch | write-only; any address in the range |
| 0x3200-0x323F | USB FIFO window | only while UCTL.VISIBLE is set (set UBASEH 0x32); nothing else decodes here |
| 0x4000-0x5FFF | csboot0 | the settings store, copy A |
| 0x6000-0x7FFF | csboot1 | the settings store, copy B |
| 0x8000-0xFFFF | the data window | page 0 = the W5100; pages 1-8 = fs0-fs7 as data |

Free in XDATA: 0x2100-0x2FFF and 0x3240-0x3FFF. The datasheet warns that a
USB FIFO window overlapping anything else "may damage the device". csboot3,
the fourth 8 KB of secondary Flash, is unused.

### Flash

| Sector | Size | Holds (v10) |
|---|---|---|
| fs0-fs2 | 3 x 32 KB | firmware slot A: common, bank 0, bank 1 |
| fs3-fs5 | 3 x 32 KB | firmware slot B |
| fs6 | 32 KB | spare |
| fs7 | 32 KB | the web page (gzip), with its own header |
| csboot0, csboot1 | 2 x 8 KB | the settings store, two copies with sequence markers |
| csboot2 | 8 KB | the bootloader |
| csboot3 | 8 KB | unused |

- The CPU **cannot fetch code from a Flash array while that array is being
  programmed or erased**: reads return status bits. That is why main Flash
  is written by code in the secondary Flash (the bootloader), and why the
  store in secondary Flash is written by code in main Flash.
- Programming and erasing use JEDEC command sequences (0x555/0xAAA) through
  the data window. DQ6 toggles while busy, and DQ5 set means failed. A 32 KB
  sector erase is about 1 s typical, 10 s at worst. A byte program is about
  14 µs typical, 150 µs at worst.
- An erase or program that the Flash did not accept reads exactly like a
  finished one (steady status bits). So every write is read back, and retried.

## The W5100

- Behind PD1 (`ecsper`) at data-window page 0, XDATA 0x8000 + chip offset.
- Four sockets. With RMSR/TMSR = 0x55, each has 2 KB of TX and 2 KB of RX
  (TX at 0x4000, RX at 0x6000, 0x800 apart). Socket registers are at
  0x0400 + n*0x100.
- The W5100 answers ICMP echo and ARP in hardware, even while the 8032 is
  stuck.
- A 2048-byte write into a 2048-byte ring leaves TX_WR equal to TX_RD, which
  is also how an empty ring looks: the segment goes out with the previous
  chunk's bytes. Stage half a ring at a time.
- Closing (`CLOSE`) a socket with unacknowledged data leaves that segment
  queued, and the next connection on the socket sends it first. Disconnect
  gracefully (`DISCON`) and wait up to about 1 s before forcing a close.
- Every bus access to it is exposed to the D4 fault.

## The 16C554 and the serial ports

| Channel | XDATA | Port | Transceiver |
|---|---|---|---|
| 0 | 0x3000 | RS-232 port 1 | RS-232 |
| 1 | 0x3008 | RS-232 port 2 | RS-232 |
| 2 | 0x3010 | RTI COM | SP3074E, RS-485 4-wire |
| 3 | 0x3018 | Expansion | SP3072E, RS-485 2-wire |

- P3.1 must be **low** for every access. The `ecsnet` chip select must be
  **active low** in the PLD: a PLD revision (v6) drove it active-high, which
  holds the '138 disabled for its own cycles and enabled for nearly every
  other bus cycle. Under v6 the panel showed fragments of unrelated bus
  traffic, and serial was dead in both directions.
- A cold read of channel 0 gives `00 00 01 00 00 60 B4 FF`: every register at
  its 16550 power-on value.
- **Channel 1's register reads are unreliable. Its writes and its data path
  work both ways.** It was declared faulty twice, wrongly both times. Drive
  it blind: configure by writing, pace the transmitter by time rather than
  LSR, and detect received data by its interrupt line. Two traps look like
  proof of a fault. Offset 0 behaves as a latch, so a loopback "passes" with
  no UART involved. And LSR's data-ready bit can sit permanently set.
- MCR bit 3 (OUT2) must be set, or the channel's INT pin is tri-stated.
- **Every access to the 16C554 flickers the front panel.** They share the
  '138 and the bus; proven with a camera, one access type at a time. An idle
  box must never read a UART. The firmware reads only when a P1.0-P1.3
  interrupt line says a byte is waiting, and only once a port has been used.
- Only channels 0 and 1 (RS-232) have been tested with a null modem. The
  RS-485 ports have not been exercised.

## IR outputs

- Port A as above: IR 1-6 active high, IR 7 (HIGH OUT) active low, idle 0x40.
  Verified optically with an IR learner on an emitter in port 1.
- Modulating any other port, macrocell, mask or chip-select address emits
  nothing. An older theory (macrocell B plus a latch at 0x3100) was wrong.
- Driving an emitter does not light its panel LED; firmware does both.
- The ports can also carry one-way serial: bit-banged against Timer 0, with
  RS-232 or TTL polarity (docs/MQTT.md).
- When testing IR, run a silent control first (board idle for about a minute,
  expecting zero captures), so a capture can be blamed on the board.

## Front panel

One write-only latch, anywhere in XDATA 0x3020-0x303F behind the '138:

| Bit | |
|---|---|
| 0-5 | IR indicators 1-6, active high (there is no LED 7) |
| 6 | status LED, **red** half |
| 7 | status LED, **green** half |

RTI writes 11 to bits 6-7, which lights both halves. They were once thought to
be "mode bits" whose clearing hung the CPU, but that was measured under the
v6 PLD and is wrong. RTI's code keeps a shadow of the latch in IRAM 0x1E, with
low-bit scanning cleared by `ANL 0x1E,#0xC0` (RTI's code). Its USB mode lights
red until configured and green once configured.

## Relays, triggers and contacts

| | Where | Polarity | Rest |
|---|---|---|---|
| Relay 1-3 | Port C bits 2, 3, 4 | active high | released (reset value) |
| Trigger 1-3 | P1.5, P1.6, P1.7 | active low | off (reset value) |
| Contact 1 | P3.0 | high = open | |
| Contact 2 | Port C Data In bit 7 | high = open | |
| Contact 3 | P1.4 | high = open | |

The relays were identified by ear: only three of the six outputs click.
Writing P1 must keep P1.0-P1.4 high, because those are inputs.

## SIGNAL IN: the RTI remote receiver

An RTI RM-433 receiver drives P3.5 with **demodulated baseband**:

- idles **low** and pulses high, inverted from a normal IR demodulator, so it
  looks dead until watched for edges;
- no carrier. The handset's IR is 40 kHz, but the RM-433 demodulates before
  the wire;
- symbols are T and 2T, T ≈ 417 µs. Runs are about 7 high and 6 low, so
  threshold each polarity separately. Frame gap about 22 ms;
- **pulse-distance** encoding, not Manchester. Decoding it as Manchester
  gives convincing, meaningless hex;
- frames open `[16, 16, 14, 12, 14]` (preamble and system ID), and the tail
  is the button;
- **codes are static**: 12 buttons gave 12 distinct tails that repeat
  exactly;
- throw away the first gap of a capture. It only says where sampling started
  inside the preamble, and keeping it made 12 buttons look like 25;
- a tight capture loop blocks the 8032, so it must yield.

## USB

The uPSD34xx's own full-speed device: five endpoints, each with a 64-byte IN
and OUT FIFO. Details and the protocol are in [USB.md](USB.md). About the
hardware:

- D+ has a 1.5 kΩ pull-up. Firmware can't switch it, as far as RTI's code
  shows: a host sees the box attached as soon as it is powered, whatever
  runs. A Mac that meets a silent box gives up on the port until the next
  power cycle. After a watchdog restart it re-enumerated on its own.
- **Cable sense on P3.4** (RTI's code, confirmed reading 1 with a cable in).
  No boot delay is needed without one.
- The FIFO window at XDATA **0x3200**, as RTI had it.
- The register sequences came from RTI's own USB driver (RTI's code, 0x66CE-0x6C99; interrupt handler at 0x69D4). Its original identity is 13BD:1019, "Remote Technologies" / "ZRP-6", HID, full speed, bcdDevice 1.00. The firmware now uses the same IDs, with its own strings.
- RTI's report descriptor: vendor page 0xFFA0, a 64-byte Feature report,
  a 64-byte Output report and a 1-byte Input report on an interrupt endpoint
  (bInterval 128 ms), with no report IDs. Integration Designer writes Output
  reports and reads Feature reports through `HidD_GetFeature`.

## Errata that matter on this board

From the uPSD34xx datasheet, section 34, plus what the bench showed:

| | What happens | What this firmware does |
|---|---|---|
| 34.12 | after a code-bank switch, the prefetch queue and branch cache can hold the other bank's instructions | flushes both (BUSCON bits 7-6 off, then on) on every bank switch and in the bootloader's jump to the application |
| 34.2 | with the MCU clock prescaled, a USB reset raises no interrupt | the clock is never prescaled |
| 34.3 | a USB reset does not reset the SIE | the whole SIE is set up again on every USB reset |
| 34.4 | the IN data toggle cannot be written | RTI's workaround, as RTI wrote it: an extra empty packet when TOGGLE reads clear |
| 34.6 | an IN FIFO's busy bit may stay set, and a packet is sent twice | clears BSY by hand on each IN completion |
| 34.1 | a USB interrupt does not wake Idle mode | Idle mode is never used |
| 34.11 | Port 1 is not 5 V tolerant | |

## The bus has a bad bit: D4

Data bit 4 of the external bus sometimes fails to reach a valid high. It
happens on about one access in two thousand, always 1 read as 0, always bit
4. It is almost certainly the AD4 net (pin 41) between the uPSD, the '573
and the W5100. **Reflowing that net is the real fix**, and it has not been
done. Full evidence is in [REVERSE-ENGINEERING.md](REVERSE-ENGINEERING.md#d4).

- It hits W5100 payload, **W5100 pointers inside the chip** (an RX_RD read
  back 0xE7E9 after 0xF7E9 was written), and **the PSD's own Flash
  commands**. In one build, every store erase silently failed, depending on
  code position.
- Two back-to-back reads fail the same way: the fault depends on the previous
  bus state. Independent reads have to be made independent.
- What the firmware does:
  - BUSCON = 0xFF, the longest MOVX strobes (7 clocks);
  - reads that decide something are three reads ORed together (`xrd3`);
  - long transfers read a W5100 pointer once and track it;
  - every Flash write is read back and retried.
- Small transfers hide it: a 30-byte reply was right 98.5% of the time, and
  a 17 KB page is what exposed it.

## Programming it over JTAG

- **Adapter:** an ST Nucleo-G474 running a bare-metal OpenOCD
  `remote_bitbang` probe (tools/nucleo-jtag). `rbb_bridge.py` presents it as
  TCP. Drive the target reset line **high**; left floating, ISP entry
  intermittently fails (`ISC_ENABLE` reads 0xFE).
- **Chain:** one BYPASS TAP (IR 5 bits) nearer TDO, then the uPSD. IDCODE
  **0x18140041**. MCU silicon revision: IDs 0x0451F041 (rev A) and 0x1451F041
  (rev B) per the datasheet. This board's has not been read.
- **ISP opcodes** (measured):

  | Step | IR | DR |
  |---|---|---|
  | ISC_ENABLE | 0x10 | 8 bits 0x81, reads back 0x81 |
  | ISC_ADDR_SELECT | 0x12 | 20 bits: sector code << 16, a bit pattern and not an address |
  | ISC_READ_INC | 0x17 | a 10-bit prime, then 10 bits per byte, data = (dr >> 2) & 0xFF |
  | ISC_DISABLE | 0x1E | 8 bits 0x00 |

  Sector codes 8 and 9 read csboot0 and csboot1: the settings store.
- **ISC_ERASE (IR 0x1A) erases the whole device**, including the bootloader
  and the PLD fuse array. ISC_ADDR_SELECT does not narrow it. Never issue it
  by hand.
- **The security bit is not set** on this box. A secured part fails
  `PROGRAM` with "Full chip erase failed". The JAM `ERASE` action clears it
  in about 90 s, then `PROGRAM` works.
- **The PLD needs JAM STAPL.** SVF can restore Flash but never the fuse
  array: its programming loop (`ISC_PGM_S_INC`, 0x05) advances only when a
  cell reports programmed, which a fixed vector list cannot follow. This
  repo's player is Altera's Jam STAPL Player ported to `remote_bitbang`
  (tools/flash/jam_rbb). `jam -aVERIFY` and `-aBLANKCHECK` are read-only.
- **OpenOCD cannot replay PSDsoft's SVF.** It rejects PIO/PIOMAP and stops
  silently with exit code 0.
- **A JAM `PROGRAM` erases the whole chip first**, settings store included.
  tools/flash/flash.sh reads the store out over JTAG before programming,
  and writes it back with the bootloader.
- **Timing through this adapter:**
  - the full v10 JAM (4 main sectors, 3 boot sectors, PLD) takes about 45
    minutes;
  - `flash.sh --quick` sets the JAM's `gNumOfMainSector` to 0, so only the
    boot sectors and PLD are written, then installs the rest over USB: 6
    minutes 56 seconds, measured.
- **A replugged USB device on the same hub has stalled the adapter
  mid-flash** (2026-09-30). If a flash dies after the erase,
  `ZRP6_STORE_FILE` supplies a saved copy of the store.

## RTI's original firmware

What was learned about the stock box. It's useful when meeting one:

- **Secondary Flash (32 KB) holds RTI's bootloader and USB stack**, as one
  program at 0x0000-0x7FFF. It answers USB by itself, which is why
  Integration Designer can show "Bootloader: x.y" for a box whose
  application is not running. Its main Flash holds the application in
  32 KB banks.
- **Updates are `.rfw` files.** `ZRP6_101.rfw` is firmware 1.01 build 26, from
  Nov 2009. It starts with the magic 0x08675309 ("867-5309") and is checked
  by a standard CRC-32 (zlib's) before the magic. The payload is compressed,
  encrypted or both, and was not cracked.
- **Integration Designer's USB protocol:** 64-byte frames padded with 0xCC.
  Legacy opcodes 0x02 (write start) and 0x03 (read start) take a 16-bit
  big-endian length. Modern opcodes 0x82 and 0x83 take a 32-bit
  little-endian length. Data frames start 0x00 or 0x80. Transfers are
  "banks" of at most 64 KB. `0x83 0x81` is the device-info read.
- **RTI's init, in order:**
  1. pulses P4.1 (W5100 reset);
  2. zeroes P1SFS0/1, P3SFS and P4SFS0/1;
  3. writes Port B Control and Direction = 0x7F (A8-A14);
  4. sets Port A Control 0x00 and Direction 0x7F (IR);
  5. drives P3.1 low around each 16C554 access.

  (All RTI's code.)
- RTI's remotes use VID 13BD too. Integration Designer programs them only
  when they present a PID in 0x1022-0x103B. PIDs 0x1009 and 0x1019 are
  normal HID mode and cannot be programmed. One remote presented different
  PIDs on one port.

## Things still open

- **AD4 (pin 41):** reflow and re-measure. It is the one known hardware
  fault.
- **The RS-485 ports** (RTI COM, Expansion) have not been exercised, and the
  expansion port's driver-enable line has not been identified.
- **Whether a watchdog reset drops the USB D+ pull-up.** A power cycle
  certainly does.
- **The MCU's silicon revision** (A or B). It decides which data-toggle
  errata apply; the firmware follows RTI's code, which works either way.
- **What RTI used P3.1 for** beyond gating the 16C554.
- **Power input** range and consumption have not been measured.
