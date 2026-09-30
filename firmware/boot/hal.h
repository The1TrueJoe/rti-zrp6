/* hal.h — the four things the bootloader needs from the hardware.
 *
 * hal.c implements them on the box; test/sim.c implements them over arrays
 * and cuts the power in the middle of them. `page` is a data-window page
 * (ota.h): 1-8 are main Flash sectors fs0-fs7, 0 is the W5100. `off` is the
 * offset within the 32 KB sector.
 */
#ifndef ZRP6_BOOT_HAL_H
#define ZRP6_BOOT_HAL_H
#include <stdint.h>

uint8_t hal_rd(uint8_t page, uint16_t off);
uint8_t hal_prog(uint8_t page, uint16_t off, uint8_t v);   /* 0 = ok; 0xFF is a no-op */
uint8_t hal_erase(uint8_t page);                           /* 0 = ok */
void    hal_page(uint8_t page);

/* The slot the application is running: the page register's slot bit, read
 * before anything here moves the page. */
uint8_t hal_running(void);

#define HAL_LED_BUSY  1         /* red: installing or restoring */
#define HAL_LED_FAULT 2         /* nothing to run */
void    hal_led(uint8_t state);

#endif
