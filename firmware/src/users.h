/* users.h — who may log in: MQTT CONNECT, and HTTP Basic for uploads.
 *
 * Up to eight users, every one allowed everything. Only a salted SHA-1 of each
 * password is kept, in the store's last slot (store.h). A box that has never
 * saved a user has exactly one: admin / admin.
 *
 * The name and password go in user_name/user_pass (with their lengths) and
 * the functions take no pointers: under SDCC's large model every generic
 * pointer access is a library call, and this was 3 KB the first time.
 */
#ifndef ZRP6_USERS_H
#define ZRP6_USERS_H
#include <stdint.h>
#include "bank.h"

#define USERS_MAX   8
#define USER_NAME   15
#define USER_PASS   32

extern __xdata uint8_t user_name[USER_NAME + 1];
extern __xdata uint8_t user_pass[USER_PASS];
extern __xdata uint8_t user_nl, user_pl;

/* Copy a name and password in; 0 if either is too long or empty. */
uint8_t users_take(const uint8_t *name, uint16_t nl, const uint8_t *pw, uint16_t pl) BANKED;

/* 1 if user_name/user_pass is a user. One SHA-1 block: milliseconds. */
uint8_t users_check(void) BANKED;

/* Results of users_build() */
#define US_SAVED    0
#define US_DELETED  1
#define US_INVALID  2
#define US_FULL     3
#define US_LAST     4

/* The record that adds, changes (user_pl > 0) or deletes (user_pl == 0)
 * user_name, built into rec for store_commit(STORE_USERS_SLOT, ...), with its
 * length in *len. US_SAVED or US_DELETED if there is something to commit. */
uint8_t users_build(__xdata uint8_t *rec, __xdata uint16_t *len) BANKED;

/* "admin,joe" into out, NUL-terminated; returns the length. */
uint8_t users_list(__xdata char *out) BANKED;

#endif
