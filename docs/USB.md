# USB: flashing without JTAG or the network

The ZRP-6's USB port is the field recovery path. Its bootloader answers USB
by itself, so a box whose firmware is broken, whose network settings are
wrong, or whose Flash holds no image at all can still be given new firmware
from a laptop. No driver is needed on macOS, Windows or Linux.

```
python3 tools/usb/zrp6usb.py info
python3 tools/usb/zrp6usb.py flash                  # build/zrp6-ui.bin + build/zrp6.bin
python3 tools/usb/zrp6usb.py wait                   # then power the box, cable in
python3 tools/usb/zrp6usb.py backup-store store.bin
```

## Two things answer

| | the firmware | the bootloader ("rescue mode") |
|---|---|---|
| when | while it runs and the cable is in | see below |
| product string | `ZRP-6` | `ZRP-6 rescue` |
| INFO | build, slot, IP, MAC | the slot it keeps |
| flashing | the slot not running; refuses while the running image is on trial | the slot it would not boot |
| source | `firmware/boot/usb*.c` in code bank 1 | the same files, built with `USB_RESCUE` |

Rescue mode starts when:

- **a cable is in at power-up.** The bootloader listens for 3 s before
  booting. A host tool running `wait` sends HOLD as soon as the device
  appears, and the bootloader stays. With no cable it boots at once: P3.4
  reads the cable (VBUS), and it is read three times, as RTI's firmware did.
- **the firmware is told to** (`zrp6usb.py rescue`, UC_RESCUE). It writes
  `'R', ~'R'` at XDATA 0x00F0, below both programs' variables, and restarts.
  The bootloader then stays while the cable is in, up to a minute without a
  command.
- **there is nothing to run.** No slot holds an image: the bootloader stays
  in rescue mode for good, blinking red, cable or not.

It ends with REBOOT (a watchdog reset, so the new image is judged by a clean
boot), or when the cable is out for a second, except with nothing to run.
Rescue runs before `boot_decide()`, so it spends none of a trial image's
boots.

Which slot rescue writes: the one the bootloader would *not* boot. The
image it would boot is kept. The new image gets the next generation, so it
boots next, on trial; if it fails three boots, the kept one runs again. This
covers the worst field case, a newer image that confirmed itself and then
turned out broken: rescue writes the older slot, and the new image replaces
the broken one as the newest.

If the box does not appear with the cable in, power-cycle it once. The D+
pull-up is hardware, so a Mac that met a silent box (firmware without USB,
or plugged in mid-flash) gives up on the port until a fresh attach. Measured
2026-09-30: after a power cycle the firmware enumerated at once, and it
came back by itself after a watchdog restart.

## The protocol

HID, vendor usage page 0xFF00, no report IDs, endpoint 0 only:

- a command is a 64-byte **Output** report (SET_REPORT);
- the reply is a 64-byte **Feature** report (GET_REPORT);
- a 1-byte Input report is declared because HID requires an interrupt
  endpoint; it is never sent.

This is the shape RTI's own firmware used on this board (research/notes/
USB-PROTOCOL.md). hidapi's `write()` and `get_feature_report()` are the whole
host side.

Command: `'Z', '6', op, seq, arguments…`. Reply: `[0] op, [1] seq, [2] rc,
[3..] data`. Offsets in the table below count from op, after the two magic
bytes. A frame without them is ignored, and in rescue mode it does not hold
the box either. That matters because the box answers on RTI's own IDs, so
Integration Designer may talk RTI's protocol to it. RTI's frames start with
their opcode (0x00, 0x02, 0x03, 0x80, 0x82, 0x83), and their 0x02 and 0x03
are this protocol's ERASE and WRITE. Measured 2026-09-30: RTI's write-start
and read-start frames, sent without the prefix, left slot B untouched. The host polls GET_REPORT until its sequence number comes back;
until then the reply is the previous command's. A Flash erase takes seconds,
and the box does not service USB meanwhile, so the host's GET_REPORT waits
(NAKed) or times out and is retried.

| op | name | arguments | reply data |
|---|---|---|---|
| 1 | INFO | | `"ZRP6"`, protocol 2, rescue 0/1, bootloader version, slot; firmware only: MAC(6), IP(4), build (NUL-padded) |
| 2 | ERASE | | erases the slot being written (3 sectors) |
| 3 | WRITE | offset (3 bytes LE), n ≤ 56, data | image bytes 0x40 and up |
| 4 | HEADER | offset, n ≤ 58, data | stages the 64-byte header |
| 5 | FINISH | | checks the CRC against the header, commits (magic last) |
| 6 | UI_ERASE | | the web page sector |
| 7 | UI_WRITE | offset (2 LE), n ≤ 57, data | page bytes 16 and up |
| 8 | UI_FINISH | the 16-byte page header | checks and commits it |
| 9 | READ | page (0xFF = XDATA), address (2 LE), n ≤ 61 | the bytes |
| 10 | REBOOT | | restarts about 300 ms later |
| 11 | RESCUE | | firmware only: restart into rescue mode |
| 12 | HOLD | | rescue: end the power-up window, stay |

rc is 0, one of the bootloader's OTA_E_* codes (ota.h), or 0xFF for a bad
command. The slot headers and the page header are not decoded on the box.
The host READs them (data pages 1 and 4 for slots A and B, page 8 for the
web page) and judges them itself. The settings store is XDATA 0x4000-0x7FFF
(READ with page 0xFF), which is how `backup-store` works.

VID:PID is **13BD:1019, RTI's own ZRP-6 IDs**, the ones the box shipped
with. That was the user's decision on 2026-09-30, for boxes run in installs
we control. The IDs belong to Remote Technologies: firmware that goes to
anyone else should move to a pid.codes PID first. The bootloader cannot
change after deployment, so that choice is made at the last JTAG flash. The
manufacturer and product strings are this firmware's (`zrp6`, `ZRP-6`, `ZRP-6
rescue`), not RTI's. A stock RTI ZRP-6 answers to the same IDs, but not to
this protocol, and `zrp6usb.py info` says so. The first USB bootloader used
the pid.codes test ID 1209:0001 without the prefix; the tool still accepts it.

## The hardware, and where it came from

The uPSD34xx datasheet (§25, and the errata in §34) describes the SIE. The
exact register sequences are taken from **RTI's own bootloader**, which
worked on this board with Integration Designer. It was disassembled from the
secondary Flash dump (`research/captures/zrp6_secondary_flash.bin`, as one
32 KB program at 0x0000; `s51 ... dc`):

| RTI address | what |
|---|---|
| 0x66CE | start: PLL off, CCON1 = 0x61, FIFO base 0x3200, SIE reset |
| 0x66E5 | SIE reset (also the USB-reset handler) |
| 0x6731 | next IN data packet / arm the status OUT |
| 0x67D4 | stall endpoint 0 |
| 0x67E2 | read the SETUP packet through USCI/USCV |
| 0x6800 | GET_DESCRIPTOR (device at 0x1965, report at 0x19A4) |
| 0x68D7 | standard requests |
| 0x69B7 | empty IN packet (status stage) |
| 0x69D4 | the USB interrupt handler |
| 0x6BE0 | HID SET_REPORT / GET_REPORT |
| 0x6C9A | USB mode: PLL on, clock on, stay while P3.4 is high |
| 0x4288 | the main loop enters USB mode when P3.4 is high three times |

What matters:

- **Clock**: fOSC is 36 MHz. PLLM 6 and PLLD 1 (CCON1 = 0x61) give exactly
  48 MHz. Enable PLLEN, wait about 200 µs, then UPLLCE (CCON0 bits 6, 5).
- **FIFO window** at XDATA 0x3200 (UBASEH 0x32). Nothing else decodes
  0x3200-0x3FFF in PLD v10, and the datasheet warns that an overlap can
  damage the part. VISIBLE is set only while the FIFO is being touched.
- **Data toggle**: firmware flips UCON.TOGGLE before each IN packet (cleared
  first for descriptors). The status OUT is armed with TOGGLE set. SET_REPORT's
  status stage sends an extra empty packet when TOGGLE reads clear; that is
  RTI's code, and the workaround for errata 34.4.
- **SET_ADDRESS** is written to UADDR from the IN-complete flag, after its
  status stage.
- **USB reset** does not reset the SIE (34.3): the whole SIE is set up again.
- **IN BSY** is cleared by hand on endpoint 1 (34.6).
- The flags in UIF0-3 are polled. The USB interrupt (IEA.0) stays off: its
  vector forwards to the application, and nothing in this firmware takes
  interrupts.

## Space

The bootloader's sector is 8 KB. USB rescue took about 2.3 KB, which fitted
only after the CRC-32 table went from a byte-wide one (1 KB) to a nibble-wide
one (64 bytes). That CRC runs only when an image is committed, never at
boot. firmware/boot/check.py still holds the rule that matters: core.c and
hal.c, which the application calls into, own no RAM. Rescue code keeps its
variables in XDATA, and runs only when the application does not.
