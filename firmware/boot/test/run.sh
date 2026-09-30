#!/bin/bash
# The bootloader's tests: the decisions under power cuts (host, ~2 minutes)
# and the compiled binary's call interface (s51).
#
#   firmware/boot/test/run.sh
set -e
cd "$(dirname "$0")/.."
B=../../build/boot
./build.sh
cc -O2 -Wall -Wextra -I. -o "$B/sim" test/sim.c core.c
echo "== power-cut simulation"
"$B/sim" | tail -4
echo "== call interface on the compiled binary (s51)"
python3 test/abi.py "$B/boot.ihx" | tail -9
