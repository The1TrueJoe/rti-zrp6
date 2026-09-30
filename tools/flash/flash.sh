#!/bin/bash
# Program a built firmware image over JTAG, power-cycle, and prove it came up.
#
#   tools/flash/flash.sh [--quick] [settle_seconds]
#
# --quick (v10, bootloader with USB rescue): JTAG writes only the boot sectors
# (bootloader + the settings store) and the PLD, not main Flash: about 10
# minutes instead of 45. The box then has no image, so its bootloader waits in
# USB rescue mode, and the web page and firmware go in over USB
# (tools/usb/zrp6usb.py, docs/USB.md). Needs the USB cable to this machine.
#
# Takes the sector images firmware/build.sh leaves in build/, so
# the usual sequence is:
#
#   firmware/build.sh && tools/flash/flash.sh
#
# Only the two main-Flash sectors are replaced; the PLD fuse array inside the
# JAM is untouched, which is what makes this possible without PSDsoft. Changing
# the memory map still means re-fitting on the Windows machine — see
# docs/REVERSE-ENGINEERING.md.
#
# This is the JTAG path, and it is the one that cannot be locked out. The
# field-update path over the network is tools/update/ and docs/UPDATING.md.
set -eu
cd "$(dirname "$0")/../.."

QUICK=0
if [ "${1:-}" = "--quick" ]; then QUICK=1; shift; fi
SETTLE="${1:-10}"
# v10: two firmware slots of three sectors, code banking, bootloader 3
# (docs/UPDATING.md), fitted 2026-09-30. JTAG writes slot A (fs0-fs2), the web
# page (fs7), the bootloader (csboot2) and the settings store (csboot0/1),
# which is READ BACK FIRST over JTAG and written back as it was: the JAM's
# PROGRAM erases the whole chip, and the store holds the MAC, the users and
# the rules. For a box still on v8: ZRP6_JAM=research/captures/zrp6_v8.jam
# with a v8 build (research/v8-snapshot). Never v6: it drives the peripheral
# chip select into the '138 Active-High, which disables the decoder for the
# 16C554 and the front-panel latch and enables it for almost every OTHER bus
# cycle. v8 keeps v5's Active-Lo. For a box still on v5:
# ZRP6_JAM=research/captures/zrp6_v5.jam and ZRP6_LAYOUT=v5.
JAM="${ZRP6_JAM:-research/captures/zrp6_v10.jam}"
DEV="${ZRP6_JTAG_DEV:-/dev/cu.usbmodem1203}"
KASA="${KASA_PY:-$HOME/.zrp6/kasaenv/bin/python}"   # outside /tmp: a venv there lost its packages overnight
OUT=build/_run.jam

[ -f build/LAYOUT ] || { echo "nothing built — run firmware/build.sh first"; exit 1; }

# The image and the PLD must agree on the memory map (firmware/build.sh,
# ZRP6_LAYOUT). A v8 image under the v5 PLD has no code at the reset vector;
# a v5 image under v8 is shadowed by the bootloader. Either way: no boot.
case "$JAM" in *v10*) PLD=v10 ;; *v8*) PLD=v8 ;; *v7*) PLD=v7 ;; *) PLD=v5 ;; esac
LAYOUT="$(cat build/LAYOUT 2>/dev/null || echo unknown)"
if [ "$LAYOUT" != "$PLD" ]; then
    echo "build/ is a $LAYOUT image but $JAM is the $PLD PLD — rebuild with ZRP6_LAYOUT=$PLD"
    exit 1
fi

bridge() {
    pkill -f rbb_bridge.py 2>/dev/null || true
    sleep 1
    python3 tools/flash/rbb_bridge.py --dev "$DEV" --once -q >/dev/null 2>&1 &
    sleep 2
}

BOOTARG=()
if [ "$PLD" = v10 ]; then
    [ -f build/boot.bin ] || { echo "no build/boot.bin — run firmware/boot/build.sh first"; exit 1; }
fi
if [ "$PLD" = v8 ]; then
    # The bootloader rides along on every v8 flash: it is part of the JAM, and
    # a JTAG flash is also the only way it ever changes.
    [ -f build/boot.bin ] || { echo "no build/boot.bin — run firmware/boot/build.sh first"; exit 1; }
    BOOTARG=(--boot build/boot.bin)
fi
# The main-Flash sectors, in the order the JAM carries them: v8 has fs0, fs1
# and fs7 (the web page); v5 has fs0 and fs1.
if [ "$PLD" = v10 ]; then
    SECTORS=(build/sector0.bin build/sector1.bin build/sector2.bin build/sector7.bin)
elif [ "$PLD" = v8 ]; then
    SECTORS=(build/sector0.bin build/sector1.bin build/sector7.bin)
else
    SECTORS=(build/code.bin build/ui.bin)
fi
# --quick: the JAM programs and verifies as many main sectors as
# gNumOfMainSector says; with 0 it still erases the whole chip and writes the
# boot sectors and the PLD. The boot sectors are selected from their own table
# (ISC_ADDR_SELECT_BOOT), not counted after the main ones, so nothing moves.
quick_jam() {
    [ "$QUICK" = 1 ] || return 0
    python3 - "$OUT" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, newline='').read()          # CRLF, as PSDsoft wrote it
s, n = re.subn(r'(INTEGER gNumOfMainSector\[1\] =\s*)\d+;', r'\g<1>0;', s)
if n != 1:
    sys.exit('--quick: gNumOfMainSector not found once in the JAM')
open(p, 'w', newline='').write(s)
print('== quick: main Flash left to USB')
PY
}
if [ "$QUICK" = 1 ]; then
    [ "$PLD" = v10 ] || { echo "--quick needs the v10 PLD and its USB bootloader"; exit 1; }
    python3 -c "import hid; import sys; sys.exit(0 if hid.enumerate(0x13BD, 0x1019) or hid.enumerate(0x1209, 0x0001) else 1)" \
        || [ "${ZRP6_QUICK_FORCE:-0}" = 1 ] \
        || { echo "--quick: no ZRP-6 on USB, and without the cable the box would be left with no firmware (ZRP6_QUICK_FORCE=1 to go on)"; exit 1; }
fi

patch_jam() {
    echo "== patching ${SECTORS[*]} into $JAM${BOOTARG[@]+ (with ${BOOTARG[1]})}"
    python3 tools/flash/jam_patch_flash.py ${BOOTARG[@]+"${BOOTARG[@]}"} "$JAM" "$OUT" "${SECTORS[@]}"
}
[ "$PLD" = v10 ] || patch_jam

"$KASA" tools/flash/bench_power.py on >/dev/null 2>&1 || true
sleep 4
for try in 1 2 3; do
    bridge
    if openocd -f tools/flash/openocd/zrp6_run.cfg -c shutdown 2>&1 | grep -q "0x18140041"; then
        echo "== TAP alive"; break
    fi
    echo "== TAP silent (attempt $try)"
    pkill -f rbb_bridge.py 2>/dev/null || true
    "$KASA" tools/flash/bench_power.py cycle 5 >/dev/null 2>&1 || true
    sleep 5
done

# v10: the store (csboot0, csboot1 — JTAG sector codes 8 and 9) is read out
# before the chip is erased and goes back in with the bootloader. Refuse to
# go on if neither copy has the store's marker: that is a read that failed,
# not a store, and programming would lose the MAC, users and rules. A new box
# with an erased store: ZRP6_NEW_STORE=1.
# After a flash that erased the chip and then failed, the store is no longer on
# the chip to read: ZRP6_STORE_FILE= a saved copy (research/store-backups/),
# whose first 16 KB are the two store sectors.
if [ "$PLD" = v10 ] && [ -n "${ZRP6_STORE_FILE:-}" ]; then
    echo "== store from $ZRP6_STORE_FILE"
    python3 - "$ZRP6_STORE_FILE" <<'PY' || exit 1
import sys
d = open(sys.argv[1], 'rb').read()[:16384]
if len(d) != 16384 or not any(d[b + 0xF0] == 0xA5 and d[b + 0xF3] == 0x5A for b in (0, 8192)):
    sys.exit('not a saved store: no marker in either copy')
open('build/_boot_with_store.bin', 'wb').write(d + open('build/boot.bin', 'rb').read())
PY
    BOOTARG=(--boot build/_boot_with_store.bin)
    patch_jam
    quick_jam
    bridge
elif [ "$PLD" = v10 ]; then
    echo "== reading the settings store over JTAG"
    rm -f build/_store_a.txt build/_store_b.txt build/_boot_with_store.bin
    bridge
    openocd -f tools/flash/openocd/zrp6_run.cfg \
        -c "source [pwd]/tools/flash/jtag_read_sector.tcl" -c "isc_enable" \
        -c "set f [open [pwd]/build/_store_a.txt w]; puts \$f [read_sector 8 8192]; close \$f" \
        -c "set f [open [pwd]/build/_store_b.txt w]; puts \$f [read_sector 9 8192]; close \$f" \
        -c "isc_disable" -c shutdown >/dev/null 2>&1 || true
    pkill -f rbb_bridge.py 2>/dev/null || true
    python3 - <<'PY' || { [ "${ZRP6_NEW_STORE:-0}" = 1 ] || exit 1; }
import sys
blob = b''
ok = False
for n in 'ab':
    try:
        d = bytes(int(x, 16) for x in open(f'build/_store_{n}.txt').read().split())
    except FileNotFoundError:
        d = b''
    if len(d) != 8192:
        sys.exit(f'store {n}: read {len(d)} bytes, want 8192')
    if d[0xF0] == 0xA5 and d[0xF3] == 0x5A:
        ok = True
    blob += d
blob += open('build/boot.bin', 'rb').read()
open('build/_boot_with_store.bin', 'wb').write(blob)
if not ok:
    sys.exit('neither store copy has its marker: not programming (ZRP6_NEW_STORE=1 for a new box)')
print('== store read: kept for the flash')
PY
    [ -f build/_boot_with_store.bin ] || python3 -c "
open('build/_boot_with_store.bin','wb').write(b'\xff'*16384 + open('build/boot.bin','rb').read())"
    BOOTARG=(--boot build/_boot_with_store.bin)
    patch_jam
    quick_jam
    bridge
fi

# v10's JAM carries four main sectors and three boot sectors: more than 15
# minutes through the bit-bang bridge.
JAM_LIMIT=900; [ "$PLD" = v10 ] && [ "$QUICK" = 0 ] && JAM_LIMIT=3000
prog() {
    ( cd build && ../tools/flash/jam_rbb/jam -aPROGRAM _run.jam 2>&1 | tee /tmp/jam_last.log ) &
    local pid=$! n=0
    while kill -0 $pid 2>/dev/null; do
        sleep 5; n=$((n + 5))
        if [ $n -ge "$JAM_LIMIT" ]; then echo "== jam timed out"; kill -9 $pid 2>/dev/null; return 1; fi
    done
    wait $pid
}
bridge
if ! prog || grep -qE "failed|failure" /tmp/jam_last.log; then
    echo "== retrying programming once"
    bridge
    if ! prog || grep -qE "failed|failure" /tmp/jam_last.log; then
        echo "PROGRAMMING FAILED"; exit 1
    fi
fi
pkill -f rbb_bridge.py 2>/dev/null || true
sleep 1

"$KASA" tools/flash/bench_power.py cycle 5
if [ "$QUICK" = 1 ]; then
    echo "== installing the page and firmware over USB"
    python3 tools/usb/zrp6usb.py flash --wait build/zrp6-ui.bin build/zrp6.bin
    SETTLE=$((SETTLE + 5))                  # the 3 s USB window, cable in
fi
sleep "$SETTLE"

echo "== verifying over MQTT"
for i in $(seq 1 20); do
    if python3 tools/flash/verify_mqtt.py "${ZRP6_HOST:-10.0.0.77}"; then echo "== UP"; exit 0; fi
    sleep 3
done
echo "== no MQTT answer; trying ICMP"
ping -c 3 -t 4 "${ZRP6_HOST:-10.0.0.77}" || true
exit 1
