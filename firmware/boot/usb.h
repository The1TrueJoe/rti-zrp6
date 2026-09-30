/* usb.h — the ZRP-6 as a USB HID device, for flashing without JTAG.
 *
 * One driver, compiled twice: into the bootloader (rescue mode, when the box
 * cannot run its firmware or is asked not to) and into the application (so a
 * host tool can find a running box, read its address and send it to rescue).
 * The host sees the same device either way; docs/USB.md has the protocol.
 *
 * It is polled — no interrupts, as everywhere else in this firmware — and
 * talks HID on endpoint 0 only: a command is a 64-byte Output report
 * (SET_REPORT), the reply a 64-byte Feature report (GET_REPORT). That is what
 * RTI's own firmware did on this board, and it needs no driver on macOS,
 * Windows or Linux (hidapi).
 *
 * The register sequences follow RTI's bootloader, read out of the board's
 * secondary Flash (research/captures/zrp6_secondary_flash.bin, 0x66CE-0x6C99):
 * they are known to work on this silicon, whose data-toggle behaviour the
 * datasheet's errata (34.4, 34.6) leave uncertain.
 */
#ifndef ZRP6_USB_H
#define ZRP6_USB_H
#include <stdint.h>

/* The bootloader builds with USB_RESCUE; in the application these live in a
 * code bank. */
#ifdef USB_RESCUE
#define USB_FN
#else
#define USB_FN BANKED
#endif

/* RTI's own ZRP-6 IDs, which the box shipped with (user decision
 * 2026-09-30): for boxes run in installs we control, not for distribution.
 * The strings stay ours, so the box does not claim to be RTI's firmware. */
#define USB_VID      0x13BD     /* Remote Technologies */
#define USB_PID      0x1019     /* ZRP-6 */

#define USB_FRAME    64

/* The cable: P3.4 reads high while a host powers VBUS (RTI's firmware tests
 * it the same way, three times, before starting USB). */
#define USB_CABLE()  (P3_4)

extern __xdata uint8_t usb_cmd[USB_FRAME];     /* the last command */
extern __xdata uint8_t usb_reply[USB_FRAME];   /* what GET_REPORT returns */
extern __xdata uint8_t usb_pending;            /* usb_cmd holds a new command */
extern __xdata uint8_t usb_configured;

/* Start the PLL's 48 MHz clock and the SIE. `product` picks the string the
 * host shows (0 application, 1 rescue). */
void usb_start(uint8_t product) USB_FN;

/* Stop answering: the SIE is disabled, the clock off. */
void usb_stop(void) USB_FN;

/* Service the bus: enumeration, SET_REPORT, GET_REPORT. Cheap when idle.
 * After it sets usb_pending, the caller handles usb_cmd, fills usb_reply and
 * clears usb_pending; a GET_REPORT meanwhile answers with the old reply. */
void usb_poll(void) USB_FN;
#ifdef BENCH
void usb_debug(__xdata uint8_t *o) USB_FN;      /* 13 bytes */
#endif

/* ---- the protocol: docs/USB.md ------------------------------------------
 * A command is usb_cmd: 'Z', '6', then [0] op, [1] sequence number and the
 * arguments, offsets below counted after the two magic bytes. A frame
 * without them is ignored: with RTI's IDs, Integration Designer may talk
 * its own protocol to this device, whose opcodes 0x02 and 0x03 would
 * otherwise read as ERASE and WRITE. The reply is usb_reply: [0] op, [1] the
 * same sequence, [2] rc (0, an OTA_E_* code, or UC_E_BAD), then data. The
 * host polls GET_REPORT until the sequence number comes back. */
#define UC_MAGIC0   'Z'
#define UC_MAGIC1   '6'
#define UC_PROTO      2
#define UC_INFO       1     /* reply: see usbcmd.c, info() */
#define UC_ERASE      2     /* the slot being written */
#define UC_WRITE      3     /* [2..4] image offset, [5] n <= 56, [6..] data */
#define UC_HEADER     4     /* [2] offset into the 64-byte header, [3] n <= 58, [4..] */
#define UC_FINISH     5     /* check the CRC, commit the header */
#define UC_UI_ERASE   6
#define UC_UI_WRITE   7     /* [2..3] offset, [4] n <= 57, [5..] data */
#define UC_UI_FINISH  8     /* [2..17] the page's 16-byte header */
#define UC_READ       9     /* [2] data page (0xFF: XDATA), [3..4] address, [5] n <= 61 */
#define UC_REBOOT    10
#define UC_RESCUE    11     /* application: restart into rescue mode */
#define UC_HOLD      12     /* rescue: stay (ends the power-up window) */
#define UC_E_BAD   0xFF

/* Handle usb_cmd into usb_reply and clear usb_pending; 0 if the frame was
 * not ours (no magic) and was ignored. May set usb_reboot. */
uint8_t usb_command(void) USB_FN;
extern __xdata uint8_t usb_reboot;
#ifdef USB_RESCUE
extern __xdata uint8_t usb_keep;        /* the slot rescue writes around */
#else
/* The application's main-loop hook: starts USB while a cable is in, serves it. */
void usb_service(void) USB_FN;
#endif

/* Rescue requests survive a watchdog reset in SRAM below both programs'
 * variables (their XDATA starts at 0x0100). */
#define USB_RESCUE_FLAG   0x00F0u        /* two bytes: 'R', ~'R' */
#define USB_RESCUE_MAGIC  0x52

#endif
