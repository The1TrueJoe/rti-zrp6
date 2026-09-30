# Tools

Everything that puts firmware, the web page, the bootloader or a PLD image on
a box. None of it is needed to run the system.

## The front door

```bash
python3 tools/zrp6flash.py --status        # what is connected, what it would do
python3 tools/zrp6flash.py                 # install build/: firmware + web page
python3 tools/zrp6flash.py --bootloader    # the bootloader and PLD too
```

It finds what can reach the box and takes the quickest route:

| connected | firmware + page | bootloader + PLD |
|---|---|---|
| USB (+ anything) | USB, about 15 s | with JTAG: `flash.sh --quick`, about 7 min |
| network, no USB | network, about 20 s | with JTAG: `flash.sh`, about 45 min |
| JTAG only | `flash.sh`, about 45 min | `flash.sh`, about 45 min |

If the running firmware refuses an upload because its own image is still on
trial, it retries from the bootloader's USB rescue mode. `--via usb|net|jtag`
forces a route; `--host IP` names the box. The bootloader cannot be read back,
so it is never replaced unless `--bootloader` says so.

| | |
|---|---|
| `zrp6flash.py` | the front door, above |
| `usb/zrp6usb.py` | USB: info, flash, rescue, wait, backup-store (docs/USB.md) |
| `update/push.py` | the network upload (docs/UPDATING.md) |
| `flash/flash.sh` | JTAG: patch, program, power-cycle, verify; `--quick` leaves main Flash to USB |
| `flash/jam_rbb/` | ported Altera Jam STAPL player: the only thing that can write the PLD |
| `flash/jam_patch_flash.py` | swaps the firmware payload inside a PSDsoft `.jam` |
| `flash/rbb_bridge.py` | presents the probe to OpenOCD as TCP |
| `nucleo-jtag/` | bare-metal STM32G474 firmware: a `remote_bitbang` JTAG probe |
| `flash/bench_power.py` | switches the bench supply (TP-Link Kasa) |
| `pc-remote.sh`, `vm/` | drive PSDsoft on Windows |

## JTAG

`flash.sh` reads the settings store (MAC, users, rules) out over JTAG before
the JAM erases the whole chip, and writes it back with the bootloader. After a
flash that died past the erase, the store is no longer on the chip:
`ZRP6_STORE_FILE=research/store-backups/<file>` supplies a saved copy.
`--quick` needs the USB cable: the box comes out of JTAG with no firmware,
in rescue mode, and the script finishes over USB. It refuses without the
cable (`ZRP6_QUICK_FORCE=1` to go on anyway).

`.jam` is required for anything touching the PLD: SVF cannot program the fuse
array. `jam_patch_flash.py` exists so firmware can be swapped without opening
PSDsoft — it rewrites the main-Flash payload in place (each byte stored as
`(byte << 2) | 3` in a 10-bit group, LSB group first) and reproduces the file
byte-for-byte when fed the original data.

The CRC warning it produces is expected; the patcher does not recompute it.

## If programming fails

`Full chip erase failed` means **the part is secured**. `PROGRAM` fails silently
on a secured device; the standalone `ERASE` action handles it:

```bash
cd build && ../tools/flash/jam_rbb/jam -aERASE   image.jam   # ~90 s
           ../tools/flash/jam_rbb/jam -aPROGRAM image.jam   # ~8 min
```

Also drive the target reset line **high** rather than leaving it floating, or
ISP entry intermittently fails with `ISC_ENABLE failed: readback 0xfe`. Send
`G` to the Nucleo (P0 = PB6/D10 → header pin 8). `jam -aBLANKCHECK` is
non-destructive and is the honest way to ask the device what state it is in.
