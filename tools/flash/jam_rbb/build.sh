#!/bin/bash
# Build a Jam STAPL Player that drives the ZRP-6's JTAG adaptor.
#
# PSDsoft can export either SVF or JAM STAPL.  The SVF is, in PSDsoft's own
# words, a "non-adaptive algorithm" -- it cannot program the uPSD's PLD/ACR
# fuse array, because that array only advances to the next location once the
# cell reports programmed, and SVF has no way to poll.  Played back, it writes
# location 0 and then silently stalls, leaving 13949 of 13950 fuses erased
# while the Flash half programs perfectly.
#
# JAM STAPL is the adaptive one.  This builds Altera's reference player with a
# remote_bitbang backend so it can talk to tools/rbb_bridge.py instead of a
# parallel-port ByteBlaster.
#
#   tools/jam_rbb/build.sh                 # -> tools/jam_rbb/jam
#   python3 tools/rbb_bridge.py --dev /dev/cu.usbmodem1203 --once -q &
#   cd /tmp && <repo>/tools/jam_rbb/jam -aPROGRAM zrp6.jam
#
# NOTE: pass the .jam as a RELATIVE path.  The player parses a leading '/' as a
# DOS-style option prefix and rejects absolute paths as "Illegal argument".
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${1:-$here/build}"

if [ ! -d "$work/jam-stapl" ]; then
    mkdir -p "$work"
    git clone --depth 1 https://github.com/margro/jam-stapl.git "$work/jam-stapl"
fi
cd "$work/jam-stapl"
git checkout -- . 2>/dev/null || true
git apply "$here/rbb-backend.patch"

cd source
# PORT=8 selects the LINUX_RBB backend added by the patch.
# -std=gnu89 and -w are needed because this is 2004-vintage C: modern clang
# rejects its implicit ints outright.
cc -O2 -w -std=gnu89 -DPORT=8 -o "$here/jam" \
   jamstub.c jamexec.c jamexp.c jamsym.c jamstack.c jamheap.c jamarray.c \
   jamutil.c jamcomp.c jamnote.c jamcrc.c jamjtag.c
echo "built $here/jam"
