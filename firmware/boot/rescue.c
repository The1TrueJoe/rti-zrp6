/* rescue.c — the bootloader's USB mode: flashing with no JTAG and no network.
 *
 * Entered from main.c:
 *   RS_WINDOW    a USB cable is in at power-up: answer for a few seconds, and
 *                stay if the host sends anything (a tool waiting for the box
 *                sends HOLD as soon as it sees it);
 *   RS_ASKED     the application was told to (UC_RESCUE) and restarted: stay
 *                while the cable is in, up to a minute without a command;
 *   RS_NOTHING   no slot holds an image: stay for good, cable or not.
 * It ends with UC_REBOOT (a watchdog reset, so the image just written is
 * judged by a clean boot), or when the cable comes out — except with nothing
 * to run.
 *
 * The slot the bootloader would boot is kept and the other one written, so a
 * rescue can never make things worse than they were: if the new image fails
 * its three trial boots, the kept one runs again. The LED blinks red the
 * whole time.
 *
 * Timer 0 counts the seconds (21.8 ms overflows at fOSC/12, as in the
 * application, which sets it up again from scratch).
 */
#include "../src/board.h"
#include "ota.h"
#include "hal.h"
#include "core.h"
#include "usb.h"
#include "rescue.h"

__sfr __at(0xAE) WDKEY;
__sfr __at(0xA6) WDRST;

static void reset(void)
{
    usb_stop();
    WDKEY = 0x00;                   /* anything but 55h enables it */
    WDRST = 0xFF;
    for (;;)
        ;
}

void rescue(uint8_t why)
{
    uint16_t t = 0, quiet = 0;
    uint8_t held = 0, gone = 0;

    usb_keep = boot_pick();
    if (usb_keep == 0xFF) usb_keep = 1;         /* nothing to keep: write A */
    XB(CS_PAGE) = usb_keep ? OTA_PG_SLOT : 0;   /* ota_op's "running" slot */
    usb_reboot = 0;
    usb_start(1);
    TMOD = (TMOD & 0xF0) | 0x01;
    TH0 = 0;
    TL0 = 0;
    TF0 = 0;
    TR0 = 1;

    for (;;) {
        usb_poll();
        if (usb_pending && usb_command()) {     /* only our frames hold it */
            held = 1;
            quiet = 0;
        }
        if (!TF0) continue;
        TF0 = 0;
        t++;
        hal_led((uint8_t)((t >> 4) & 1));
        if (usb_reboot && ++usb_reboot > 14)    /* ~300 ms to fetch the reply */
            reset();
        gone = USB_CABLE() ? 0 : (uint8_t)(gone < 255 ? gone + 1 : 255);
        if (why == RS_NOTHING) continue;
        if (gone > TICKS_PER_S) break;          /* cable out for a second */
        if (held) continue;
        if (++quiet > (why == RS_WINDOW ? 3 * TICKS_PER_S : 60 * TICKS_PER_S))
            break;
    }
    TR0 = 0;
    TF0 = 0;
    TMOD = 0;
    usb_stop();
    XB(CS_PAGE) = 0;
}
