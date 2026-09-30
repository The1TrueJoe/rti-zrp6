# Jam STAPL Player with a remote_bitbang backend

Programs a uPSD34xx (and any other PSD that PSDsoft supports) over an ordinary
bit-banged JTAG adaptor, instead of ST's FlashLINK or RLink cable.

## Why this is needed

PSDsoft Express exports two ATE formats. Its own dialog describes them:

- **SVF** — *"Due to the nature of the SVF language, this option generates
  **non-adaptive** algorithm using the four standard JTAG pins."*
- **JAM STAPL** — *"generates adaptive algorithm using the four standard JTAG
  pins."*

That distinction is not cosmetic. **The SVF cannot program the PLD/ACR fuse
array.** `ISC_PGM_S_INC` advances to the next fuse location only once the
current cell reports programmed; SVF is a flat vector list with no way to poll
and retry. Played back, it programs fuse location 0, never advances, and
writes all remaining values on top of that one location.

The failure is silent and very convincing, because the *Flash* half of the same
SVF programs perfectly and verifies byte-for-byte. On a uPSD3434EV the result
was 1 of 13950 fuse locations programmed, a board that would not boot, and no
error from the playback beyond the ordinary TDO-check noise.

Do not try to hand-drive the fuse programming either: the SVF's scan order is
the *physical* array order (a regular repeating pattern) while the `.obj`'s
PLD bytes are in *logical* order. They are not the same sequence, so writing
`.obj` bytes in order puts correct-looking values into the wrong cells.

## What the patch does

`rbb-backend.patch` applies to [margro/jam-stapl](https://github.com/margro/jam-stapl)
(commit `e8dceb1`) and adds:

- **`PORT == LINUX_RBB` (8)** — a backend speaking OpenOCD's `remote_bitbang`
  protocol over TCP to `tools/rbb_bridge.py`. Writes are buffered and flushed
  only when TDO must be sampled; one round trip per TCK over a serial link is
  unusably slow.
- Two build fixes for 2004-era C under modern clang: `malloc.h` → `stdlib.h` on
  macOS, and `((const) x)` → `((const int) x)` in `jamdefs.h`, a K&R cast whose
  implicit `int` clang 16 no longer infers.

## Use

```bash
tools/jam_rbb/build.sh                                   # -> tools/jam_rbb/jam
python3 tools/rbb_bridge.py --dev /dev/cu.usbmodem1203 --once -q &
cd /tmp && /path/to/repo/tools/jam_rbb/jam -aPROGRAM zrp6.jam
```

Actions in a PSDsoft-generated `.jam`: `BLANKCHECK`, `ERASE`, `PROGRAM`,
`PROGRAM_ONLY`, `VERIFY`. `VERIFY` is read-only and is the safe way to check
the link before committing to a program.

**Pass the `.jam` as a relative path.** The player treats a leading `/` as a
DOS-style option prefix and rejects absolute paths as `Illegal argument`.

## What a good run looks like

```
Device 1
Part is not blank
Begin erase operation
Full chip erase succeeded
Part is now blank
Program blank bit
Program device 1 main sector 0 succeeded
Program device 1 main sector 1 succeeded
Verify device 1 main sector 0 succeeded
Verify device 1 main sector 1 succeeded
Program device 1 PLD/ACR succeeded
Program device 1 USERCODE succeeded
Program verify operation succeeded
Exit code = 0... Success
```

`Program device 1 PLD/ACR succeeded` is the line the SVF path can never reach.
