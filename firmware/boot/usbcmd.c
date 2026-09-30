/* usbcmd.c — the USB commands (usb.h, docs/USB.md), for both builds.
 *
 * The bootloader (USB_RESCUE) runs them itself, around the slot it keeps;
 * the application hands the Flash operations to the bootloader through the
 * same call as a network upload (ota.c), so the trial rule applies there: an
 * unconfirmed image refuses to overwrite the other slot.
 */
#include "../src/board.h"
#include "ota.h"
#include "usb.h"
#ifdef USB_RESCUE
#include "hal.h"
#include "core.h"
#else
#include "../src/ota_app.h"
#include "../src/net.h"
#include "../src/config.h"
#include "../src/io.h"
#endif

__xdata uint8_t usb_reboot;
static __xdata uint8_t hdr[OTA_HDR_SIZE];
static __xdata struct ota_req rq;

#ifdef USB_RESCUE
__xdata uint8_t usb_keep;

static uint8_t rd(uint8_t page, uint16_t a)
{
    if (page == 0xFF) return *(volatile __xdata uint8_t *)a;
    return hal_rd(page, a);
}

static uint8_t flash(uint8_t op)
{
    /* Nothing is running to be proved, so no trial rule: the slot the
     * bootloader would boot is kept, the other one written. */
    if (op == OTA_OP_ERASE) return slot_erase((uint8_t)(usb_keep ^ 1));
    rq.op = op;
    ota_op(&rq);
    return rq.rc;
}
#else
static uint8_t rd(uint8_t page, uint16_t a)
{
    if (page == 0xFF) return *(volatile __xdata uint8_t *)a;
    return ota_peek(page, a);
}

static uint8_t flash(uint8_t op)
{
    return ota_call(op, (uint32_t)rq.hi << 16 | rq.off, rq.len, rq.buf);
}
#endif

/* INFO: [3..6] "ZRP6", [7] protocol, [8] 1 in rescue, [9] bootloader
 * version, [10] the running (or kept) slot; the application adds [11..16]
 * MAC, [17..20] IP, [21..63] its build. The slot headers and the page's are
 * the host's to READ and judge (tools/usb/zrp6usb.py). */
static void info(__xdata uint8_t *r)
{
    r[3] = 'Z'; r[4] = 'R'; r[5] = 'P'; r[6] = '6';
    r[7] = UC_PROTO;
    r[9] = OTA_BOOT_VERSION;
#ifdef USB_RESCUE
    r[8] = 1;
    r[10] = usb_keep;
#else
    {
        __code const char *b = BUILD_ID;
        __xdata const uint8_t *m = cfg_get()->mac;
        const uint8_t *ip = net_ip();
        uint8_t i;
        r[10] = RUN_SLOT();
        for (i = 0; i < 6; i++) r[11 + i] = m[i];
        for (i = 0; i < 4; i++) r[17 + i] = ip[i];
        for (i = 0; i < 42 && b[i]; i++) r[21 + i] = b[i];
    }
#endif
}

uint8_t usb_command(void) USB_FN
{
    __xdata uint8_t *c = usb_cmd + 2, *r = usb_reply;
    uint8_t rc = 0, i, n;
    uint16_t a;

    if (usb_cmd[0] != UC_MAGIC0 || usb_cmd[1] != UC_MAGIC1) {
        usb_pending = 0;                /* not ours: RTI's tools, say */
        return 0;
    }
    for (i = 3; i < USB_FRAME; i++) r[i] = 0;
    rq.hi = 0;
    rq.off = 0;
    rq.len = 0;
    rq.buf = hdr;
    switch (c[0]) {
    case UC_INFO:
        info(r);
        break;
    case UC_ERASE:
        rc = flash(OTA_OP_ERASE);
        break;
    case UC_WRITE:
        rq.off = c[2] | (uint16_t)c[3] << 8;
        rq.hi = c[4];
        rq.len = c[5];
        rq.buf = c + 6;
        rc = c[5] > 56 ? UC_E_BAD : flash(OTA_OP_WRITE);
        break;
    case UC_HEADER:
        n = c[3];
        if (n > 58 || c[2] + n > OTA_HDR_SIZE) { rc = UC_E_BAD; break; }
        for (i = 0; i < n; i++) hdr[c[2] + i] = c[4 + i];
        break;
    case UC_FINISH:
        rc = flash(OTA_OP_FINISH);
        break;
    case UC_UI_ERASE:
        rc = flash(OTA_OP_UI_ERASE);
        break;
    case UC_UI_WRITE:
        rq.off = c[2] | (uint16_t)c[3] << 8;
        rq.len = c[4];
        rq.buf = c + 5;
        rc = c[4] > 57 ? UC_E_BAD : flash(OTA_OP_UI_WRITE);
        break;
    case UC_UI_FINISH:
        rq.buf = c + 2;
        rc = flash(OTA_OP_UI_FINISH);
        break;
    case UC_READ:
        n = c[5];
        if (n > 61) { rc = UC_E_BAD; break; }
        a = c[3] | (uint16_t)c[4] << 8;
        for (i = 0; i < n; i++) r[3 + i] = rd(c[2], (uint16_t)(a + i));
        break;
    case UC_REBOOT:
        usb_reboot = 1;
        break;
#ifndef USB_RESCUE
    case UC_RESCUE:
        *(volatile __xdata uint8_t *)USB_RESCUE_FLAG = USB_RESCUE_MAGIC;
        *(volatile __xdata uint8_t *)(USB_RESCUE_FLAG + 1) = (uint8_t)~USB_RESCUE_MAGIC;
        usb_reboot = 1;
        break;
#endif
    case UC_HOLD:
        break;
    default:
        rc = UC_E_BAD;
    }
#ifdef USB_RESCUE
    hal_page(OTA_PAGE_NORMAL);
#endif
    r[0] = c[0];
    r[1] = c[1];
    r[2] = rc;
    usb_pending = 0;
    return 1;
}

#ifndef USB_RESCUE
/* The main loop's hook. USB runs only while a cable is in, so a box that
 * never sees one never starts the PLL; the cable is read three times, as
 * RTI's firmware does. A REBOOT waits a moment for the host to fetch the
 * reply. */
static __xdata uint8_t running;

void usb_service(void) USB_FN
{
    uint8_t cable = USB_CABLE() && USB_CABLE() && USB_CABLE();
    if (cable != running) {
        if (cable) usb_start(0);
        else usb_stop();
        running = cable;
    }
    if (!running) return;
    usb_poll();
    if (usb_pending) usb_command();
    if (usb_reboot) {
        uint16_t t = ticks();
        while ((uint16_t)(ticks() - t) < TICKS_MS(300))
            usb_poll();
        ota_reboot();
    }
}
#endif
