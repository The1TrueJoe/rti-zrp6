/* flash.h — in-application programming of the secondary Flash.
 *
 * Secondary Flash is the only memory this board can write from the CPU:
 * csboot0 sits at XDATA 0x4000-0x5FFF and csboot1 at 0x6000-0x7FFF, both on
 * data cycles. The firmware itself is in MAIN Flash, which the CPU cannot
 * write, so nothing here can damage the running program.
 */
#ifndef ZRP6_FLASH_H
#define ZRP6_FLASH_H
#include <stdint.h>

#define FLASH_A 0x4000          /* csboot0 — 8 KB sector */
#define FLASH_B 0x6000          /* csboot1 — 8 KB sector */

/* 0 on success. Both poll DQ6 until the operation finishes, give up on DQ5 or
 * a timeout, and leave the array in read mode. */
uint8_t flash_erase(uint16_t sector_base);
uint8_t flash_program(uint16_t addr, uint8_t value);

#endif
