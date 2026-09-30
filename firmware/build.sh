#!/bin/bash
# Build the ZRP-6 firmware, UI included, into a raw 64 KB main-Flash image.
#
#   firmware/build.sh            -> build/zrp6.bin
#   firmware/build.sh --no-ui    skip the UI (needs no node)
#
# ZRP6_LAYOUT picks the memory map the image is for, and it must match the PLD
# on the box (tools/flash/flash.sh checks):
#
#   v8  (default) the bootloader owns code 0x0000-0x1FFF (firmware/boot), and
#       the image is one contiguous run of code from 0x2000 to 0xFFFF with a
#       header the bootloader checks. The web page is NOT in it: it has a
#       Flash sector of its own (fs7) and a file of its own,
#       build/zrp6-ui.bin — docs/UPDATING.md and boot/ota.h. Both install
#       over the network. (v7, which carried the page inside the image, is
#       gone: it left 0.5 KB of code space.)
#   v5  the old PLD, no bootloader: code at 0x0000-0x7FFF in fs0, the UI and
#       the HIGH modules in fs1. JTAG only; POST /update checks an image but
#       writes nothing. Kept buildable for a box still on v5.
#
# Three rules apply when editing any of these modules, each of which cost a
# night when ignored and each of which produced a convincing but false hardware
# diagnosis. They are in ../docs/REVERSE-ENGINEERING.md, and in short:
#   1. a __naked function must push/pop every register it touches;
#   2. stage peripheral write data in IRAM — an XDATA source delivers 0x00;
#   3. read back through xrd(), not XB() — SDCC elides a volatile read that
#      follows a write to the same address.
set -e
cd "$(dirname "$0")"
B=../build
mkdir -p "$B"

# --model-large --stack-auto, and neither half of that is the obvious choice.
#
# In the default small model SDCC gives every non-leaf function a private,
# non-overlaid block in the 8032's ~100 usable bytes of directly-addressable
# IRAM. These sixteen modules asked for 741 bytes of it and the link failed
# with eight separate "could not get N consecutive bytes" errors. --stack-auto
# makes functions reentrant so those live on the stack instead: 741 bytes of
# IRAM demand becomes 30. --model-large then puts anything not marked __xdata
# into XDATA rather than IRAM, so forgetting an annotation costs space in the
# 8 KB rather than breaking the link in the 100 bytes.
#
# --xstack was measured too and rejected: it moves the stack to XDATA, which
# nothing here needs, and costs 6.3 KB of code out of a 32 KB sector.
#
# The cost of the large model is speed, and exactly one thing cares: the IR
# carrier loop. Its counters are pinned to __data in ir.c so that loop is
# unchanged — but the carrier is calibrated by measurement, not arithmetic, so
# it is re-checked against a receiver after any change here.
CFLAGS="-mmcs51 --model-large --stack-auto --code-loc 0x0000 --code-size 0x10000
        --xram-loc 0x0100 --xram-size 0x1F00 --iram-size 256
        --opt-code-size --peep-asm"

# Where code lives. Sector 0 (0x0000-0x7FFF) is always mapped and holds
# everything the box needs to boot and be reachable. The modules in HIGH run
# from the top of main Flash page 0 (0xE000-0xFFFF) — the same page as the UI,
# which only fills the bottom of it. The page register is 0 from reset and
# nothing changes it, so that code is always mapped too, and is called like any
# other function: no bank switching. Only modules the box does not need to
# boot go there, so a mistake up there cannot take the network down.
# --code-size is the whole 64 KB so the linker accepts the placement; the
# layout check below is what enforces the real limits.
LAYOUT="${ZRP6_LAYOUT:-v10}"
BUILD_ID="${ZRP6_BUILD_ID:-$(date -u +%Y-%m-%dT%H:%MZ)}"
if [ "$LAYOUT" != v10 ]; then
    echo "this tree builds the v10 layout only; the v8 firmware is in research/v8-snapshot" >&2
    exit 1
fi
# v10 (bootloader 3, docs/UPDATING.md): code is common at 0x2000-0x7FFF plus
# two 32 KB banks at 0x8000-0xFFFF, switched by page-register bit 6 through
# the trampoline in src/bank.s. Bank n is linked at n*0x10000 + 0x8000.
# Common holds what everything calls and what sits on the deepest stack paths
# (the MQTT publish chain); a banked call costs one byte of stack.
CFLAGS="${CFLAGS/--code-loc 0x0000 --code-size 0x10000/--code-loc 0x2000 --code-size 0x6000}"
CFLAGS="$CFLAGS -DZRP6_BOOT -DBUILD_ID=\"$BUILD_ID\" ${ZRP6_EXTRA_CFLAGS:-}"
COMMON="delay bus io w5100 ws broker cfgacc"
BANK0="app rules ir uart rti capture store flash config"
BANK1="net ota users sha1 http usb usbcmd"   # usb*: boot/, shared with rescue
UI_MAX=40000                            # layout.py checks the real limit

# mqtt.c is deliberately absent. It is the outbound bridge to a house broker,
# and every socket is a listener, so it is kept compiling by hand
# (build.sh --check-bridge) rather than carried in the image.
if [ "$1" = "--check-bridge" ]; then
    sdcc $CFLAGS -DZRP6_BRIDGE -c src/mqtt.c -o "$B/mqtt.rel" && echo "bridge: compiles"
    exit
fi

src() { [ -f "src/$1.c" ] && echo "src/$1.c" || echo "boot/$1.c"; }
for m in $COMMON; do sdcc $CFLAGS -c "$(src $m)" -o "$B/$m.rel"; done
for m in $BANK0;  do sdcc $CFLAGS --codeseg BANK0 -c "$(src $m)" -o "$B/$m.rel"; done
for m in $BANK1;  do sdcc $CFLAGS --codeseg BANK1 -c "$(src $m)" -o "$B/$m.rel"; done
sdas8051 -plosgff "$B/bank.rel" src/bank.s
RELS=$(for m in $COMMON $BANK0 $BANK1 bank; do echo "$B/$m.rel"; done)
# sdld's memory summary (-M) refuses any code above 0xFFFF whatever -C says,
# and then writes no image. So: one link for the .mem (the IRAM and stack
# report), which fails on that check, and the real one from the same .lk
# without -M/-C.
rm -f "$B/zrp6.ihx" "$B/zrp6.mem"
sdcc $CFLAGS -Wl-bBANK0=0x8000 -Wl-bBANK1=0x18000 src/main.c $RELS -o "$B/zrp6.ihx" \
    > "$B/link1.log" 2>&1 || true
grep -q "Stack starts" "$B/zrp6.mem" || { cat "$B/link1.log"; exit 1; }
grep -v -e '^-M$' -e '^-C ' "$B/zrp6.lk" > "$B/zrp6_nom.lk"
sdld -f "$B/zrp6_nom.lk" > "$B/link2.log" 2>&1 || { cat "$B/link2.log"; exit 1; }
echo "common: $COMMON main" > "$B/GROUPS"
echo "bank0: $BANK0" >> "$B/GROUPS"
echo "bank1: $BANK1" >> "$B/GROUPS"

# ---- the UI ---------------------------------------------------------------
if [ "$1" = "--no-ui" ] || [ ! -d ui/node_modules ]; then
    [ "$1" = "--no-ui" ] || echo "ui: node_modules missing, building without it"
    : > "$B/ui.blob"
else
    (cd ui && npm run --silent build >/dev/null)
    python3 ui/pack.py ui/dist "$B/ui.blob" "$UI_MAX"
fi

# ---- lay out and check the image ------------------------------------------
python3 layout.py --v10 "$B"
echo "$LAYOUT" > "$B/LAYOUT"

printf 'image: build/zrp6.bin  '
ls -l "$B/zrp6.bin" | awk '{print $5" bytes"}'
grep -E "^Stack|CODE|XSEG|ISEG" "$B/main.mem" 2>/dev/null | head -6 || true
