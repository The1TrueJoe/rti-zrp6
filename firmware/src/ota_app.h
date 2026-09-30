/* ota_app.h — firmware updates, the application's side (ota.c). */
#ifndef ZRP6_OTA_APP_H
#define ZRP6_OTA_APP_H
#include <stdint.h>
#include "bank.h"

#define OTA_BUF       256

#define OTA_ST_NONE   0         /* the running slot has no image header */
#define OTA_ST_OK     1         /* running, proved or never on trial */
#define OTA_ST_TRIAL  2         /* + boots used: on trial, not yet confirmed */

extern __xdata uint8_t ota_buf[OTA_BUF];

uint8_t ota_state(void) BANKED;
uint8_t ota_backup(void) BANKED;            /* 1 if there is an image to fall back to */
void    ota_client_connected(void) BANKED;
uint8_t ota_http(uint8_t s, uint32_t clen, uint8_t ui, __code const char **msg) BANKED;
void    ota_reboot(void) BANKED;

/* For code in ota.c's own bank (usbcmd.c): a byte through the data window,
 * and a bootloader call with the watchdog paused. */
uint8_t ota_peek(uint8_t page, uint16_t off);
uint8_t ota_call(uint8_t op, uint32_t at, uint16_t len, __xdata uint8_t *buf);

/* Set by the upload once the reply is on its way; main.c reboots shortly
 * after, and app.c republishes firmware/state when `ota_changed` is set. */
extern __xdata uint8_t ota_restart;
extern __xdata uint8_t ota_changed;

#endif
