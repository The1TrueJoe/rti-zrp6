#include "board.h"
#include "bus.h"
#include "io.h"
#include "sha1.h"
#include "store.h"
#include "users.h"

/* The record, in the store's user slot:
 *   [0]    'U' when there is one; erased (0xFF) on a box that never saved
 *   [1]    how many users
 *   [2..3] CRC-16 of the entries
 *   [4..]  entries of 40 bytes: name (16, NUL padded), salt (4), and
 *          SHA-1(salt, name, ':', password) (20)
 * With no record, the one user is admin/admin. A record whose CRC fails lets
 * nobody in rather than falling back to admin/admin: a corrupt list must not
 * turn into the factory password. */
#define HDR     4
#define ENT     40
#define E_SALT  16
#define E_HASH  20

__xdata uint8_t user_name[USER_NAME + 1];
__xdata uint8_t user_pass[USER_PASS];
__xdata uint8_t user_nl, user_pl;

static __xdata uint8_t  digest[20];
static __xdata uint8_t  ent[ENT];
static __xdata uint16_t base;
static __xdata uint8_t  count, i, k;

static __code const uint8_t ADMIN[6] = "admin";
/* admin/admin as a stored entry, salt 0 — what a box that never saved a user
 * has, made real when the first other user is added. Generated with
 * sha1(b'\0\0\0\0admin:admin'). */
static __code const uint8_t ADMIN_ENT[ENT] = {
    0x61, 0x64, 0x6D, 0x69, 0x6E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,
    0xBB, 0x7B, 0xE9, 0x95, 0x42, 0x0C, 0x3E, 0x50, 0xC5, 0xF0,
    0x2A, 0x13, 0x47, 0x66, 0x93, 0x1A, 0x1E, 0xAB, 0x13, 0x9F };
static __code const uint8_t COLON = ':';

uint8_t users_take(const uint8_t *name, uint16_t nl, const uint8_t *pw, uint16_t pl) BANKED
{
    if (!nl || nl > USER_NAME || pl > USER_PASS) return 0;
    user_nl = (uint8_t)nl;
    user_pl = (uint8_t)pl;
    for (i = 0; i < user_nl; i++) user_name[i] = name[i];
    user_name[i] = 0;
    for (i = 0; i < user_pl; i++) user_pass[i] = pw[i];
    return 1;
}


/* digest = SHA-1(salt, user_name, ':', user_pass) */
static void hash(__xdata const uint8_t *salt)
{
    sha1_init();
    sha1_update(salt, 4);
    sha1_update(user_name, user_nl);
    sha1_update(&COLON, 1);
    sha1_update(user_pass, user_pl);
    sha1_final(digest);
}

/* 0: no record (the admin/admin box); 1: valid, `count` entries at `base`;
 * 2: corrupt. */
static uint8_t state(void)
{
    uint16_t crc, n;
    uint8_t t;
    base = store_slot(STORE_USERS_SLOT);
    if (xrd3(base) != 'U') return 0;
    for (t = 0; t < 3; t++) {
        count = xrd3(base + 1);
        if (!count || count > USERS_MAX) continue;
        crc = 0xFFFF;                           /* crc16(), through fb3 */
        for (n = 0; n < (uint16_t)count * ENT; n++) {
            crc ^= (uint16_t)xrd3(base + HDR + n) << 8;
            for (k = 0; k < 8; k++)
                crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
        if (crc == (xrd3(base + 2) | ((uint16_t)xrd3(base + 3) << 8))) return 1;
    }
    return 2;
}

/* Entry k into ent; 1 if it is user_name. */
static uint8_t load(uint8_t e)
{
    uint16_t a = base + HDR + (uint16_t)e * ENT;
    for (i = 0; i < ENT; i++) ent[i] = xrd3(a + i);
    for (i = 0; i < user_nl; i++) if (ent[i] != user_name[i]) return 0;
    return user_nl == 16 || ent[user_nl] == 0;
}

static uint8_t is_admin(void)
{
    for (i = 0; i < 6; i++) if (user_name[i] != ADMIN[i]) return 0;
    return 1;
}

uint8_t users_check(void) BANKED
{
    uint8_t e;
    if (!user_pl) return 0;
    switch (state()) {
    case 0:
        if (!is_admin() || user_pl != 5) return 0;
        for (i = 0; i < 5; i++) if (user_pass[i] != ADMIN[i]) return 0;
        return 1;
    case 1:
        for (e = 0; e < count; e++) {
            if (!load(e)) continue;
            hash(ent + E_SALT);
            for (i = 0; i < 20; i++) if (digest[i] != ent[E_SALT + 4 + i]) return 0;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static uint8_t valid_name(void)
{
    uint8_t c;
    for (i = 0; i < user_nl; i++) {
        c = user_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* user_name/user_pass as an entry at e, salted from the clock: the salt only
 * has to differ between users and between saves. */
static void put(__xdata uint8_t *e)
{
    uint16_t t = ticks();
    for (i = 0; i < 16; i++) e[i] = i < user_nl ? user_name[i] : 0;
    e[E_SALT] = (uint8_t)t;
    e[E_SALT + 1] = (uint8_t)(t >> 8);
    e[E_SALT + 2] = TL0;
    e[E_SALT + 3] = TH0;
    hash(e + E_SALT);
    for (i = 0; i < 20; i++) e[E_SALT + 4 + i] = digest[i];
}

uint8_t users_build(__xdata uint8_t *rec, __xdata uint16_t *len) BANKED
{
    __xdata uint8_t *e;
    uint8_t out = 0, found = 0, st, n, j;
    uint16_t crc;

    if (!user_nl || !valid_name()) return US_INVALID;
    st = state();
    n = st == 1 ? count : 0;
    if (st == 0) {
        if (is_admin()) {
            found = 1;
        } else {                                /* admin/admin, made real */
            for (i = 0; i < ENT; i++) rec[HDR + i] = ADMIN_ENT[i];
            out = 1;
        }
    }
    for (j = 0; j < n; j++) {
        if (load(j)) { found = 1; continue; }
        e = rec + HDR + (uint16_t)out * ENT;
        for (i = 0; i < ENT; i++) e[i] = ent[i];
        out++;
    }
    if (user_pl) {
        if (out == USERS_MAX) return US_FULL;
        put(rec + HDR + (uint16_t)out * ENT);
        out++;
    } else if (!found) {
        return US_INVALID;                      /* no such user */
    } else if (!out) {
        return US_LAST;                         /* would lock everyone out */
    }
    crc = crc16(rec + HDR, (uint16_t)out * ENT);
    rec[0] = 'U';
    rec[1] = out;
    rec[2] = (uint8_t)crc;
    rec[3] = (uint8_t)(crc >> 8);
    *len = HDR + (uint16_t)out * ENT;
    return user_pl ? US_SAVED : US_DELETED;
}

uint8_t users_list(__xdata char *out) BANKED
{
    uint8_t o = 0, e;
    switch (state()) {
    case 0:
        for (i = 0; i < 5; i++) out[o++] = (char)ADMIN[i];
        break;
    case 1:
        for (e = 0; e < count; e++) {
            (void)load(e);
            if (o) out[o++] = ',';
            for (i = 0; i < 16 && ent[i]; i++) out[o++] = (char)ent[i];
        }
        break;
    }
    out[o] = 0;
    return o;
}
