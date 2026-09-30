# ZRP-6 firmware

C for the uPSD3434EV's Turbo 8032, built with SDCC. It owns the hardware, is
its own MQTT broker (1883, and over WebSocket on 80), and serves the interface
in `ui/` out of Flash. Every client logs in (users.c); the address comes from
DHCP (net.c, the only UDP). docs/MQTT.md is the interface.

```bash
./build.sh                  # -> ../build/code.bin, ../build/ui.bin
python3 test/test_rti.py    # the RTI decoder, on this computer
```

## Memory map it expects

This firmware only works against the **v5** PLD image. The map, and why each
qualification is there, is in [../docs/REVERSE-ENGINEERING.md](../docs/REVERSE-ENGINEERING.md).

| | |
|---|---|
| `0x0000`–`0x1FFF` | SRAM (data cycles only — it must not shadow the reset vector) |
| `0x2000`–`0x20FF` | csiop |
| `0x3000`–`0x31FF` | `ECSNET`, active low — 16C554 and the LED latch |
| `0x8000`–`0xFFFF` | `ECSPER` — W5100 |

## Rules for editing this

Each of these cost a night when ignored, and each produced a convincing but
false hardware diagnosis. They are not style preferences.

1. **A `__naked` function must `push`/`pop` every register it touches.** SDCC
   does not track inline-assembly register use and will hand the caller the
   same register.
2. **Stage peripheral write data in IRAM.** `XB(periph) = xdata_var` makes the
   peripheral latch `0x00`; the same write with an immediate constant works.
3. **Read back through `xrd()`, not `XB()`.** SDCC elides a `volatile` read that
   follows a write to the same address, so a readback confirms whatever you
   just wrote.
4. **Watch the stack.** Everything runs on 175 bytes of IRAM (`--stack-auto`)
   and running out *reboots* the box, which looks like a bug in whatever
   command was running. `grep 'Stack starts' ../build/zrp6.mem` after every
   build, never add a `__data` array, and subscribe to `zrp6/debug/stackfree`
   — the firmware measures its own margin and publishes it.
5. **Read the `.asm` of anything on the boot path.** SDCC miscompiled a simple
   IRAM loop into one that wiped the register banks; a boot-path bug leaves no
   way to see the box except JTAG. `sdcc <flags> -S src/x.c`.
6. **Never read the UART when idle.** Every 16C554 access flashes the front
   panel. Serial is read only when its interrupt line says a byte is waiting.

## Port A idles at 0x40

`PA0`–`PA5` are IR ports 1–6, active high. `PA6` is HIGH OUT and is active
**low**, so parking Port A at `0x00` drives the high-current output
continuously. The idle value is `0x40`.
