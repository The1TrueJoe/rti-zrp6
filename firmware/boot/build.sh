#!/bin/bash
# Build the bootloader into build/boot.bin: an 8 KB image of secondary Flash
# sector 2 (csboot2), which the PLD answers on code fetches at 0x0000-0x1FFF.
#
#   firmware/boot/build.sh
#
# The same flags as the application, for the same reasons (firmware/build.sh),
# and for one more: --stack-auto keeps every local on the stack, which is what
# lets the application call into this without either side's variables landing
# on the other's. The link map is checked afterwards for exactly that.
set -e
cd "$(dirname "$0")"
B=../../build/boot
mkdir -p "$B"

CFLAGS="-mmcs51 --model-large --stack-auto --code-loc 0x0100 --code-size 0x1F00
        --xram-loc 0x0100 --xram-size 0x1E00 --iram-size 256 --opt-code-size"

sdas8051 -plosgff "$B/vectors.rel" vectors.s
# USB rescue mode (rescue.c, usb.c, usbcmd.c) runs only when the application
# does not, so it may own RAM; core and hal, which the application calls
# into, must not (check.py).
for m in core hal usb usbcmd rescue; do
    sdcc $CFLAGS -DUSB_RESCUE -c "$m.c" -o "$B/$m.rel"
done
sdcc $CFLAGS -DUSB_RESCUE main.c "$B/core.rel" "$B/hal.rel" "$B/usb.rel" "$B/usbcmd.rel" \
    "$B/rescue.rel" "$B/vectors.rel" -o "$B/boot.ihx"
makebin -s 8192 < "$B/boot.ihx" > "$B/../boot.bin"

python3 check.py "$B/boot.map" "$B/boot.ihx"
