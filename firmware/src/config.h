/* config.h — the box's settings, kept in Flash rather than compiled in.
 *
 * There is no factory-unique ID on a uPSD3434 that the CPU can read: the JTAG
 * IDCODE is device-type and identical on every part. So the MAC address has to
 * be STORED, and this is where. A box with no config makes one up on its first
 * boot (config.c) and keeps it.
 *
 * The record lives at the start of the live store copy (store.h), which is
 * written the store's way: a complete new copy into the other sector, marked
 * last, so a power cut during a save leaves the previous settings whole.
 */
#ifndef ZRP6_CONFIG_H
#define ZRP6_CONFIG_H
#include <stdint.h>
#include "bank.h"

#define CFG_MAGIC   0x5A52            /* 'ZR' */
#define CFG_VERSION 2

#define NET_DHCP    0
#define NET_STATIC  1
#define NET_TRIAL   2                 /* static, not yet proved by a login:
                                       * back to DHCP if none in 10 minutes */

/* Baud rates, by index, for the IR ports' serial mode and the RS-232 ports.
 * 0 is never a baud rate: an IR port with mode 0 is an IR port. */
#define BAUD_1200   1
#define BAUD_9600   4
#define BAUD_COUNT  9                 /* 1..9 = 1200 ... 115200 */

/* ir_mode[n]: 0 = IR; otherwise the baud index, | IRM_TTL for idle-high. */
#define IRM_BAUD    0x0F
#define IRM_TTL     0x80

/* ser[n]: baud index in the low nibble, frame format (SER_8N1...) above it. */
#define SER_FORMATS 6                 /* 8N1 8E1 8O1 7E1 7O1 8N2 */

struct cfg {
    uint16_t magic;
    uint8_t  version;
    uint8_t  mac[6];
    uint8_t  net;                     /* NET_DHCP or NET_STATIC */
    uint8_t  ip[4];                   /* the static address, or DHCP's fallback */
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    char     topic[16];               /* base topic, NUL padded */
    uint8_t  ir_mode[7];
    uint8_t  ser[2];
    uint16_t crc;                     /* over everything above */
};

/* Loads the live copy, or installs defaults if there is none — with a new
 * random MAC, saved at once. Never fails: an unprovisioned box still comes up
 * on the network. */
void cfg_load(void) BANKED;
__xdata const struct cfg *cfg_get(void);

/* The copy in RAM, to change before cfg_save(). Changes take effect wherever
 * the code reads cfg_get() next; cfg_save() makes them survive a restart.
 * Blocks for a second or two — call it from the top of the loop. 0 = saved. */
__xdata struct cfg *cfg_edit(void);
uint8_t cfg_save(void) BANKED;

uint16_t cfg_crc(const struct cfg *c);

/* The power-cycle reset. Each boot marks a byte in the config area's spare
 * space; cfg_boot_ok() marks it done once the box has been up 15 s.
 * cfg_boot_mark() returns how many boots in a row before this one were cut
 * short — three means someone is asking for a reset (docs/MQTT.md). */
uint8_t cfg_boot_mark(void) BANKED;
void cfg_boot_ok(void) BANKED;

/* Shared tables: baud rate by index (1-based), and the 16C554's LCR byte and
 * name by frame format. */
extern __code const uint32_t baud_rate[BAUD_COUNT + 1];
extern __code const uint8_t  ser_lcr[SER_FORMATS];
extern __code const char     ser_fmt[SER_FORMATS][4];

#endif
