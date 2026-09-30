/* ota.h — the contract between the bootloader and the application.
 *
 * Shared by firmware/boot (the bootloader), firmware/src (the application) and
 * firmware/boot/test (the host-side simulation), so it holds only constants,
 * the image layout and the call interface. docs/UPDATING.md is the design.
 *
 * Bootloader 3, PLD v10: two firmware SLOTS, A and B, of three 32 KB sectors
 * each, and the PLD runs whichever one the page register's top bit selects.
 * An update is written into the slot that is not running and the box
 * restarts into it; rolling back is just booting the other slot again. There
 * is no copying, so there is nothing to interrupt.
 *
 *   sectors   data pages   what
 *   fs0-fs2   1 2 3        slot A: common code, code bank 0, code bank 1
 *   fs3-fs5   4 5 6        slot B: the same
 *   fs6       7            spare
 *   fs7       8            the web page
 *
 * Code space, as the PLD decodes it for the running slot:
 *   0x0000-0x1FFF  the bootloader (secondary Flash, csboot2)
 *   0x2000-0x7FFF  the slot's common sector (its first 8 KB is data only)
 *   0x8000-0xFFFF  the slot's bank 0 or bank 1, by page register bit 6
 *
 * Every sector is also reachable as data through the window at XDATA
 * 0x8000-0xFFFF, selected by the page register's low four bits (page 0 is the
 * W5100). Bits 6 and 7 never affect the data window, so the application must
 * keep them as they are whenever it moves the window.
 */
#ifndef ZRP6_OTA_H
#define ZRP6_OTA_H
#include <stdint.h>

#ifndef __SDCC
#define __xdata
#define __code
#endif

#define OTA_BOOT_VERSION  3

/* ---- memory ------------------------------------------------------------ */
#define OTA_PAGE_REG      0x20E0u       /* csiop + E0h */
#define OTA_WINDOW        0x8000u       /* data window base */
#define OTA_SECTOR        0x8000u       /* 32 KB */

#define OTA_PG_WINDOW     0x0F          /* page register: the data window */
#define OTA_PG_BANK       0x40          /* the code bank at 0x8000-0xFFFF */
#define OTA_PG_SLOT       0x80          /* the firmware slot: 0 = A, 1 = B */

#define OTA_PAGE_NORMAL   0             /* window = W5100 */
#define OTA_PAGE_SPARE    7             /* fs6 */
#define OTA_PAGE_UI       8             /* fs7 */
#define OTA_SLOTS         2
#define OTA_PARTS         3             /* sectors per slot */
/* The data page of part p (0 common, 1 bank 0, 2 bank 1) of slot s. */
#define OTA_PAGE(s, p)    ((uint8_t)(1 + (s) * OTA_PARTS + (p)))

#define OTA_APP_ENTRY     0x2000u       /* the application's reset vector */
#define OTA_API_ENTRY     0x0080u       /* ota_call() lands here */

/* ---- the image --------------------------------------------------------- */
/* 96 KB, laid out exactly as a slot's three sectors:
 *
 *   0x00000-0x0003F  header
 *   0x00040-0x01FFF  data the application reads through the window
 *                    (never fetched as code: the bootloader owns 0x0000-0x1FFF)
 *   0x02000-0x07FFF  common code
 *   0x08000-0x0FFFF  bank 0, run at 0x8000
 *   0x10000-0x17FFF  bank 1, run at 0x8000
 */
#define OTA_IMAGE_SIZE    0x18000ul
#define OTA_HDR_SIZE      0x40
#define OTA_DATA_AT       0x0040u

/* Header, little-endian. The file carries magic, CRC and format; everything
 * from OTA_H_GEN on is 0xFF in the file and belongs to the bootloader. The
 * magic is programmed LAST, after the body's CRC has been checked and the
 * generation written, so a header with the magic in place means a whole,
 * verified image. The status bytes are then programmed one at a time as the
 * image boots; a byte that is not 0xFF counts as set, so a power cut half
 * way through programming one still leaves a decision. */
#define OTA_H_MAGIC       0x00          /* "ZRPB" */
#define OTA_H_CRC         0x04          /* CRC-32 of 0x40-0x17FFF, as zlib */
#define OTA_H_FORMAT      0x08          /* OTA_FORMAT */
#define OTA_H_GEN         0x10          /* 2 bytes: newer images count higher */
#define OTA_H_CONFIRMED   0x20          /* set: the image proved itself */
#define OTA_H_TRIES       0x21          /* 3 bytes, one set per unconfirmed boot */
#define OTA_H_FAILED      0x24          /* set: gave up on it; boot the other */
#define OTA_FORMAT        2
#define OTA_MAX_TRIES     3             /* boots allowed without a confirm */

/* ---- the web page sector (fs7) ----------------------------------------- */
/* The gzipped UI, served through the data window at page 8, never as code.
 * Updated on its own and with no backup: a page that is not there is
 * replaced by a small built-in one that can upload it again (http.c).
 *
 *   0x0000  "ZUI2", programmed last
 *   0x0004  CRC-32 of the blob, as zlib
 *   0x0008  blob length, 2 bytes
 *   0x0010  the gzipped blob                                               */
#define OTA_UI_SIZE       0x8000u
#define OTA_UI_HDR        0x10
#define OTA_U_MAGIC       0x00
#define OTA_U_CRC         0x04
#define OTA_U_LEN         0x08
#define OTA_UI_MAX        (OTA_UI_SIZE - OTA_UI_HDR)

/* ---- calls from the application ---------------------------------------- */
/* The application calls the bootloader to WRITE Flash, because while main
 * Flash is being programmed a fetch from it returns status bits, not code: the
 * writer has to run from the secondary Flash. Reads need no call.
 *
 * The call is `lcall OTA_API_ENTRY` with DPTR pointing at a struct ota_req in
 * XDATA. It uses the caller's stack and no RAM of its own, may clobber every
 * register, and changes only the page register's data-window bits: the slot
 * and bank bits (6-7) are the caller's, and it returns into the code it was
 * called from — and takes interrupts meanwhile — in the running slot. It
 * returns with the window at OTA_PAGE_NORMAL.
 * Firmware ops always act on the slot that is NOT running, which the
 * bootloader reads from the page register's slot bit at the call. */
#define OTA_OP_ERASE      1     /* erase the other slot */
#define OTA_OP_WRITE      2     /* program len bytes of buf at image offset (hi:off) */
#define OTA_OP_FINISH     3     /* buf = the 64-byte header: check CRC, then commit */
#define OTA_OP_CONFIRM    4     /* the running image is healthy */
#define OTA_OP_UI_ERASE   5     /* erase the web page sector */
#define OTA_OP_UI_WRITE   6     /* program len bytes of buf at page offset off */
#define OTA_OP_UI_FINISH  7     /* buf = 16-byte page header: check CRC, commit */

#define OTA_OK            0
#define OTA_E_FLASH       1     /* erase or program failed */
#define OTA_E_RANGE       2     /* offset/length outside the image body */
#define OTA_E_HEADER      3     /* not a ZRP-6 image of this format */
#define OTA_E_CRC         4     /* the body does not match the header */
#define OTA_E_OP          5
#define OTA_E_TRIAL       6     /* the running image is on trial: confirm it
                                 * first, or there would be no proven image
                                 * left to fall back to */

struct ota_req {
    uint8_t  op;
    uint8_t  rc;
    uint16_t off;               /* image offset, low 16 bits */
    uint16_t len;
    __xdata uint8_t *buf;
    uint8_t  hi;                /* image offset, bits 16-23 */
};

#endif
