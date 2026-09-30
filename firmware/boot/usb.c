/* usb.c — the uPSD34xx USB SIE as a HID device on endpoint 0. See usb.h.
 *
 * A control transfer, as this SIE sees it:
 *   U_SETUP      USTA.U_SETUP and UIF2.OUT0F; the 8 bytes are read through
 *              USCI/USCV. Nothing is armed on endpoint 0 until firmware
 *              answers, so the host is NAKed meanwhile.
 *   data IN    the IN FIFO is loaded and USIZE written; U_TOGGLE is flipped by
 *              firmware before each packet (cleared on U_SETUP, so the first
 *              data packet goes as DATA1).
 *   data OUT   OUT0F without U_SETUP: USIZE bytes in the OUT FIFO.
 *   status     after IN data, the OUT FIFO is armed (U_TOGGLE set, USIZE 0) for
 *              the host's empty DATA1; after OUT data or none, an empty IN
 *              packet is sent.
 * SET_ADDRESS takes effect only when its status packet has gone, as USB
 * requires: UADDR is written from the IN-complete flag.
 *
 * Every descriptor is shorter than 64 bytes, so every IN data stage is one
 * packet; nothing here splits a transfer.
 */
#include "../src/board.h"
#include "usb.h"

__sfr __at(0xE2) UADDR;
__sfr __at(0xE3) UPAIR;
__sfr __at(0xE4) UIE0;
__sfr __at(0xE5) UIE1;
__sfr __at(0xE6) UIE2;
__sfr __at(0xE7) UIE3;
__sfr __at(0xE8) UIF0;
__sfr __at(0xE9) UIF1;
__sfr __at(0xEA) UIF2;
__sfr __at(0xEB) UIF3;
__sfr __at(0xEC) UCTL;
__sfr __at(0xED) USTA;
__sfr __at(0xEF) USEL;
__sfr __at(0xF1) UCON;
__sfr __at(0xF2) USIZE;
__sfr __at(0xF3) UBASEH;
__sfr __at(0xF4) UBASEL;
__sfr __at(0xF5) USCI;
__sfr __at(0xF6) USCV;
__sfr __at(0xFA) CCON1;
__sbit __at(0xE8) RESUMF;       /* UIF0 is bit-addressable */
__sbit __at(0xEA) SUSPENDF;
__sbit __at(0xEB) RSTF;
__sbit __at(0xED) OUTF;
__sbit __at(0xEE) INF;

#define U_USBEN     0x04          /* UCTL */
#define U_VISIBLE   0x02
#define U_SETUP     0x04          /* USTA */
#define U_ENABLE    0x08          /* UCON */
#define U_STALL     0x04
#define U_TOGGLE    0x02
#define U_BSY       0x01
#define U_OUT_DIR   0x80          /* USEL */

/* The FIFO window. 0x3200 is where RTI put it; in PLD v10 nothing else
 * decodes 0x2100-0x2FFF or 0x3200-0x3FFF (research/captures/zrp6_v10.abl),
 * and the datasheet warns that an overlap can damage the part. */
#define FIFO      ((volatile __xdata uint8_t *)0x3200)

__xdata uint8_t usb_cmd[USB_FRAME];
__xdata uint8_t usb_reply[USB_FRAME];
__xdata uint8_t usb_pending;
__xdata uint8_t usb_configured;
static __xdata uint8_t setup[8];
static __xdata uint8_t addr_pending;
static __xdata uint8_t out_report;     /* a SET_REPORT data stage is due */
static __xdata uint8_t product;

/* ---- descriptors -------------------------------------------------------- */
static __code const uint8_t dev_desc[18] = {
    18, 1, 0x10, 0x01,          /* USB 1.1 */
    0, 0, 0, 64,                /* class in the interface; EP0 64 bytes */
    USB_VID & 0xFF, USB_VID >> 8, USB_PID & 0xFF, USB_PID >> 8,
    0x00, 0x01,                 /* bcdDevice 1.00 */
    1, 2, 0,                    /* manufacturer, product, no serial */
    1
};

/* HID: 64-byte Output (commands), 64-byte Feature (replies), and the 1-byte
 * Input the class requires an interrupt endpoint for, never sent. */
static __code const uint8_t report_desc[] = {
    0x06, 0x00, 0xFF,           /* Usage Page (vendor 0xFF00) */
    0x09, 0x01,                 /* Usage 1 */
    0xA1, 0x01,                 /* Collection (Application) */
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,   /* 0..255, 8 bits */
    0x95, 0x40, 0x09, 0x02, 0x91, 0x02,         /* 64 x Output */
    0x95, 0x40, 0x09, 0x03, 0xB1, 0x02,         /* 64 x Feature */
    0x95, 0x01, 0x09, 0x04, 0x81, 0x02,         /* 1 x Input */
    0xC0
};

static __code const uint8_t cfg_desc[34] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,             /* 1 interface, 100 mA */
    9, 4, 0, 0, 1, 3, 0, 0, 0,                  /* HID, 1 endpoint */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof report_desc, 0,
    7, 5, 0x81, 3, 1, 0, 255                    /* EP1 IN, interrupt */
};

static __code const uint8_t zero2[2] = { 0, 0 };
static __code const uint8_t str0[] = { 4, 3, 0x09, 0x04 };
static __code const uint8_t str1[] = { 10, 3, 'z',0,'r',0,'p',0,'6',0 };
static __code const uint8_t str2[] = { 12, 3, 'Z',0,'R',0,'P',0,'-',0,'6',0 };
static __code const uint8_t str3[] = { 26, 3, 'Z',0,'R',0,'P',0,'-',0,'6',0,
                                       ' ',0,'r',0,'e',0,'s',0,'c',0,'u',0,'e',0 };

/* ---- endpoint 0 --------------------------------------------------------- */
static void wait_idle(void)
{
    uint16_t n = 0;
    while ((UCON & U_BSY) && --n)
        ;
}

/* An empty IN packet: the status stage of a transfer with no IN data. */
static void ack(void)
{
    USEL = 0;
    UCON ^= U_TOGGLE;
    wait_idle();
    USIZE = 0;
}

static void stall(void)
{
    USEL = 0;
    UCON |= U_ENABLE | U_STALL;
    UCON &= ~U_TOGGLE;
    USIZE = 0;
}

/* After IN data: take the host's empty DATA1 OUT, the status stage. */
static void arm_status_out(void)
{
    USEL = U_OUT_DIR;
    UCON |= U_ENABLE;
    UCON |= U_TOGGLE;
    USIZE = 0;
}

/* One IN data packet from code or XDATA (a generic pointer), at most wLength. */
static void send(const uint8_t *p, uint8_t n)
{
    uint8_t i;
    if (setup[7] == 0 && setup[6] < n) n = setup[6];
    USEL = 0;
    UCON |= U_ENABLE;
    wait_idle();
    UCON ^= U_TOGGLE;
    for (i = 0; i < n; i++)
        FIFO[i] = p[i];
    USIZE = n;
}

static void sie_reset(void)
{
    UCTL &= ~U_USBEN;
    UCTL |= U_USBEN;
    USTA = 0;
    UADDR = 0;
    UIF1 = 0;
    UIF2 = 0;
    UIF3 = 0;
    UIF0 = 0;
    /* The flags these enable are what usb_poll() reads; the CPU interrupt
     * itself (IEA.0) stays off. */
    UIE0 = 0x0C;
    UIE1 = 0x03;
    UIE2 = 0x01;
    UIE3 = 0;
    UPAIR = 0;
    USEL = U_OUT_DIR;
    UCON = U_ENABLE | U_BSY;
    USEL = 0;
    UCON = U_ENABLE;
    USEL = 1;
    UCON = U_ENABLE;
    addr_pending = 0;
    out_report = 0;
    usb_configured = 0;
}

static void standard(void)
{
    const uint8_t *d = 0;
    uint8_t n = 0;
    switch (setup[1]) {
    case 0:                                     /* GET_STATUS */
        send(zero2, 2);
        return;
    case 5:                                     /* SET_ADDRESS */
        addr_pending = 1;
        ack();
        return;
    case 6:                                     /* GET_DESCRIPTOR */
        switch (setup[3]) {
        case 1: d = dev_desc; n = sizeof dev_desc; break;
        case 2: d = cfg_desc; n = sizeof cfg_desc; break;
        case 0x21: d = cfg_desc + 18; n = 9; break;
        case 0x22: d = report_desc; n = sizeof report_desc; break;
        case 3:
            if (setup[2] == 0) { d = str0; n = sizeof str0; }
            else if (setup[2] == 1) { d = str1; n = sizeof str1; }
            else if (setup[2] == 2) {
                if (product) { d = str3; n = sizeof str3; }
                else { d = str2; n = sizeof str2; }
            }
            break;
        }
        if (!n) break;
        USEL = 0;
        UCON &= ~U_TOGGLE;
        send(d, n);
        return;
    case 8:                                     /* GET_CONFIGURATION */
        send(&usb_configured, 1);
        return;
    case 9:                                     /* SET_CONFIGURATION */
        usb_configured = setup[2];
        ack();
        return;
    case 11:                                    /* SET_INTERFACE */
        ack();
        return;
    }
    stall();
}

static void on_setup(void)
{
    uint8_t i;
    USTA &= ~U_SETUP;
    for (i = 0; i < 8; i++) {
        USCI = i;
        setup[i] = USCV;
    }
    USTA &= ~U_SETUP;
    UIF2 &= ~1;
    USEL = 0;
    UCON &= ~U_STALL;
    out_report = 0;

    if (setup[0] == 0x21 && setup[1] == 0x09) {         /* SET_REPORT */
        out_report = 1;
        USEL = U_OUT_DIR;
        USIZE = 0;
    } else if (setup[0] == 0xA1 && setup[1] == 0x01) {  /* GET_REPORT */
        send(usb_reply, USB_FRAME);
    } else if (setup[0] == 0x21 && setup[1] == 0x0A) {  /* SET_IDLE */
        ack();
    } else if ((setup[0] & 0x60) == 0) {
        standard();
    } else {
        stall();
    }
}

/* OUT data on endpoint 0: a report, or the empty status packet after IN
 * data, which needs nothing but re-arming. */
static void on_out(void)
{
    uint8_t i, n;
    USEL = U_OUT_DIR;
    n = USIZE;
    if (out_report && n) {
        for (i = 0; i < USB_FRAME; i++)
            usb_cmd[i] = i < n ? FIFO[i] : 0;
        usb_pending = 1;
    }
    UIF2 &= ~1;
    USIZE = 0;
    if (out_report) {
        /* RTI's status stage for SET_REPORT, as it is: an extra empty packet
         * first when U_TOGGLE reads clear — the datasheet's workaround for a
         * toggle firmware cannot set (errata 34.4) — then the real one. */
        out_report = 0;
        USEL = 0;
        if (!(UCON & U_TOGGLE))
            USIZE = 0;
        ack();
    }
}

static void on_in(void)
{
    if (UIF1 & 1) {
        UIF1 &= ~1;
        if (addr_pending) {
            UADDR = setup[2];
            addr_pending = 0;
        } else {
            arm_status_out();
        }
    }
    if (UIF1 & 2) {                     /* erratum 34.6: clear U_BSY by hand */
        UIF1 &= ~2;
        USEL = 1;
        UCON &= ~U_BSY;
    }
}

void usb_poll(void) USB_FN
{
    uint8_t keep = USEL;
    UCTL |= U_VISIBLE;
    if (RSTF)
        sie_reset();
    if (SUSPENDF && RESUMF) {
        RESUMF = 0;
        SUSPENDF = 0;
    }
    if (OUTF && (UIF2 & 1)) {
        if (USTA & U_SETUP)
            on_setup();
        else
            on_out();
    }
    if (INF)
        on_in();
    UCTL &= ~U_VISIBLE;
    USEL = keep;
}

void usb_start(uint8_t prod) USB_FN
{
    volatile uint16_t n;
    product = prod;
    usb_pending = 0;
    CCON0 &= 0x9F;                      /* PLL and USB clock off */
    CCON1 = 0x61;                       /* PLLM 6, PLLD 1: 36 MHz -> 48 MHz */
    CCON0 |= 0x40;                      /* PLL on; lock takes ~200 us */
    for (n = 0; n < 2000; n++)
        ;
    CCON0 |= 0x20;                      /* USB clock on */
    UBASEH = 0x32;
    UBASEL = 0;
    sie_reset();
}

#ifdef BENCH
/* A bench build's look at the hardware (app.c, debug/usb). */
void usb_debug(__xdata uint8_t *o) USB_FN
{
    o[0] = P3;
    o[1] = CCON0;
    o[2] = CCON1;
    o[3] = UCTL;
    o[4] = UIF0;
    o[5] = UIF1;
    o[6] = UIF2;
    o[7] = USTA;
    o[8] = UADDR;
    o[9] = usb_configured;
    USEL = 0;
    o[10] = UCON;
    USEL = U_OUT_DIR;
    o[11] = UCON;
    o[12] = UBASEH;
}
#endif

void usb_stop(void) USB_FN
{
    UCTL = 0;
    CCON0 &= ~0x20;
}
