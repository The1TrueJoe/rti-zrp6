#!/bin/bash
# zrp6_jtag_verify.sh -- read every Flash sector of the ZRP-6 over JTAG and
# compare against the reference images in captures/.
#
#   tools/zrp6_jtag_verify.sh [--quick]
#
# --quick reads 16 bytes per sector instead of the whole thing (seconds rather
# than a couple of minutes).  Read-only: the Tcl it sources contains no erase
# or program opcode.
set -u
cd "$(dirname "$0")/.."

DEV="${ZRP6_JTAG_DEV:-/dev/cu.usbmodem1203}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

python3 tools/rbb_bridge.py --dev "$DEV" --once -q >/dev/null 2>&1 &
sleep 2

if [ "$QUICK" = 1 ]; then
    openocd -f tools/openocd/zrp6_run.cfg \
        -c "source [pwd]/tools/jtag_read_sector.tcl" -c "isc_enable" \
        -c 'foreach s {0 1 2 3 4 5 6 7 8 9 10 11} { echo "S$s: [read_sector $s 16]" }' \
        -c "isc_disable" -c shutdown 2>&1 | grep -E '^S[0-9]+:|Error'
    exit 0
fi

openocd -f tools/openocd/zrp6_run.cfg \
    -c "source [pwd]/tools/jtag_read_sector.tcl" -c "isc_enable" \
    -c "set f [open $OUT/m0 w]; puts \$f [read_sector 0 32768]; close \$f" \
    -c "set f [open $OUT/m1 w]; puts \$f [read_sector 1 32768]; close \$f" \
    -c "set f [open $OUT/b w]; foreach s {8 9 10 11} { puts \$f [read_sector \$s 8192] }; close \$f" \
    -c "isc_disable" -c shutdown 2>&1 | grep -E 'Error' || true

python3 - "$OUT" <<'PY'
import hashlib, pathlib, sys
out = pathlib.Path(sys.argv[1])
def rd(p): return bytes(int(x, 16) for x in (out / p).read_text().split())
checks = [
    ("main sector 0 (boot/common)", "m0", "captures/zrp6_secondary_flash.bin"),
    ("main sector 1 (bank 1)",      "m1", "captures/zrp6_bank01_from_rfw.bin"),
    ("2nd Flash  (boot sectors 0-3)", "b", "captures/zrp6_secondary_flash.bin"),
]
bad = 0
for name, f, ref in checks:
    d = rd(f); r = open(ref, "rb").read()
    ok = hashlib.sha256(d).digest() == hashlib.sha256(r).digest()
    bad += 0 if ok else 1
    print(f"{'OK  ' if ok else 'FAIL'} {name}: {len(d)} bytes, sha256 {hashlib.sha256(d).hexdigest()[:16]}")
    if not ok:
        diffs = [i for i, (a, b) in enumerate(zip(d, r)) if a != b]
        print(f"     {len(diffs)} differing bytes, first: "
              + ", ".join(f"{i:#07x} got {d[i]:#04x} want {r[i]:#04x}" for i in diffs[:5]))
sys.exit(1 if bad else 0)
PY
