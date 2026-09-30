/* main.c — the ZRP-6 bootloader.
 *
 * Lives in secondary Flash sector 2 (csboot2), answering code fetches at
 * 0x0000-0x1FFF, and is written only over JTAG. On every reset it decides
 * what runs (core.c) and jumps to the application at 0x2000. vectors.s puts
 * the reset vector, the interrupt forwarders and the call entry for the
 * application in place. docs/UPDATING.md is the design.
 */
#include "../src/board.h"
#include "ota.h"
#include "hal.h"
#include "core.h"
#include "usb.h"
#include "rescue.h"

void hal_init(void);

void main(void)
{
    uint8_t s;

    hal_init();
    /* USB rescue (rescue.c): asked for by the application before its reset,
     * or offered for a few seconds when a cable is in at power-up. Either
     * way it comes before boot_decide(), so it spends no trial boot. */
    if (XB(USB_RESCUE_FLAG) == USB_RESCUE_MAGIC
        && XB(USB_RESCUE_FLAG + 1) == (uint8_t)~USB_RESCUE_MAGIC) {
        XB(USB_RESCUE_FLAG) = 0;
        rescue(RS_ASKED);
    } else if (USB_CABLE() && USB_CABLE() && USB_CABLE()) {
        rescue(RS_WINDOW);
    }
    s = boot_decide();
    if (s != 0xFF) {
        /* The slot bit makes the PLD answer code fetches from that slot's
         * sectors; bank 0 and the W5100 window to start with. Written
         * whole, not through hal_page(), which keeps bits 6-7: a jump to
         * 0x0000 without a reset would otherwise carry the old slot over. */
        XB(CS_PAGE) = (uint8_t)(OTA_PAGE_NORMAL | (s ? OTA_PG_SLOT : 0));
        BUSCON &= 0x3F;             /* flush prefetch queue + branch cache */
        BUSCON |= 0xC0;             /* after a code-page change (34.12) */
        hal_led(0);
        __asm
            ljmp    0x2000
        __endasm;
    }

    /* Nothing to run: wait in rescue mode for an image over USB, however
     * long it takes. It returns only through a reset. */
    rescue(RS_NOTHING);
}
