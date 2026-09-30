/* rescue.h — the bootloader's USB mode; see rescue.c. */
#ifndef ZRP6_BOOT_RESCUE_H
#define ZRP6_BOOT_RESCUE_H
#include <stdint.h>

#define RS_WINDOW   0       /* a cable at power-up: a few seconds, unless held */
#define RS_ASKED    1       /* the application asked (UC_RESCUE) */
#define RS_NOTHING  2       /* no image anywhere: for good */

void rescue(uint8_t why);

#endif
