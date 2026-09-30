/* core.h — the bootloader's decisions; see core.c. */
#ifndef ZRP6_BOOT_CORE_H
#define ZRP6_BOOT_CORE_H
#include "ota.h"

uint32_t crc32_byte(uint32_t crc, uint8_t b);
uint8_t  slot_valid(uint8_t s);
uint16_t slot_gen(uint8_t s);
uint8_t  slot_tries(uint8_t s);
uint32_t slot_crc(uint8_t s);
uint32_t slot_body_crc(uint8_t s);
uint8_t  slot_erase(uint8_t s);
uint8_t  boot_pick(void);               /* the newest usable slot, or 0xFF */
uint8_t  boot_decide(void);             /* the slot to run, or 0xFF */
void     ota_op(__xdata struct ota_req *r);

#endif
