/* sim.c — the bootloader's decisions under power cuts, on the host.
 *
 * core.c is compiled unchanged against a HAL made of arrays. The HAL counts
 * Flash operations and can cut the power on any one of them: an erase stops
 * with the sector half-erased, a program with only some of the byte's bits
 * cleared. Then the "box" boots again, possibly cut again, and every scenario
 * checks where it ends up.
 *
 *   cc -O2 -I.. -o sim sim.c ../core.c && ./sim
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota.h"
#include "hal.h"
#include "core.h"

#define SECTORS 8
static uint8_t flash[SECTORS][OTA_SECTOR];
static long budget = -1;            /* ops until the cut; -1 = never */
static long ops;
static jmp_buf cut;
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static uint8_t running;             /* the slot the "application" runs */

static uint8_t *cell(uint8_t page, uint16_t off)
{
    if (page < 1 || page > SECTORS || off >= OTA_SECTOR) {
        fprintf(stderr, "bad access page %u off %04x\n", page, off);
        exit(2);
    }
    return &flash[page - 1][off];
}

static void tick(void (*partial)(void *), void *arg)
{
    ops++;
    if (budget >= 0 && budget-- == 0) {
        partial(arg);
        longjmp(cut, 1);
    }
}

uint8_t hal_rd(uint8_t page, uint16_t off) { return *cell(page, off); }
void hal_page(uint8_t page) { (void)page; }
void hal_led(uint8_t s) { (void)s; }
uint8_t hal_running(void) { return running; }

struct pw { uint8_t *c; uint8_t v; };
static void prog_partial(void *a)
{
    struct pw *p = a;
    *p->c &= (uint8_t)(p->v | (uint8_t)rnd());   /* some of the 0 bits land */
}

uint8_t hal_prog(uint8_t page, uint16_t off, uint8_t v)
{
    struct pw p;
    if (v == 0xFF) return 0;
    p.c = cell(page, off);
    p.v = v;
    tick(prog_partial, &p);
    *p.c &= v;                      /* Flash can only clear bits */
    return 0;
}

static void erase_partial(void *a)
{
    uint8_t *s = a;
    long i;
    for (i = 0; i < OTA_SECTOR; i++)
        if (rnd() & 1) s[i] = 0xFF;
}

uint8_t hal_erase(uint8_t page)
{
    uint8_t *s = cell(page, 0);
    tick(erase_partial, s);
    memset(s, 0xFF, OTA_SECTOR);
    return 0;
}

/* ---- images ------------------------------------------------------------ */
#define N OTA_IMAGE_SIZE
static uint8_t img[4][N];               /* A, B, C, D: files as layout.py writes */

static uint32_t crc_of(const uint8_t *p, long n)
{
    uint32_t c = 0xFFFFFFFFul;
    long i;
    for (i = 0; i < n; i++) c = crc32_byte(c, p[i]);
    return ~c;
}

static void make_image(uint8_t *m, unsigned seed)
{
    long i;
    uint32_t c;
    rng = seed;
    for (i = 0; i < (long)N; i++) m[i] = (rnd() % 5) ? (uint8_t)rnd() : 0xFF;
    memset(m, 0xFF, OTA_HDR_SIZE);
    m[0] = 'Z'; m[1] = 'R'; m[2] = 'P'; m[3] = 'B';
    c = crc_of(m + OTA_HDR_SIZE, N - OTA_HDR_SIZE);
    m[4] = (uint8_t)c; m[5] = (uint8_t)(c >> 8); m[6] = (uint8_t)(c >> 16); m[7] = (uint8_t)(c >> 24);
    m[OTA_H_FORMAT] = OTA_FORMAT;
}

/* What JTAG writes: an image in slot s with a generation, confirmed. */
static void put(uint8_t s, const uint8_t *m, uint16_t gen, int confirmed)
{
    int p;
    for (p = 0; p < OTA_PARTS; p++) memcpy(flash[OTA_PAGE(s, p) - 1], m + (long)p * OTA_SECTOR, OTA_SECTOR);
    flash[OTA_PAGE(s, 0) - 1][OTA_H_GEN] = (uint8_t)gen;
    flash[OTA_PAGE(s, 0) - 1][OTA_H_GEN + 1] = (uint8_t)(gen >> 8);
    if (confirmed) flash[OTA_PAGE(s, 0) - 1][OTA_H_CONFIRMED] = 0;
}

/* Slot s holds image m, whatever its status bytes say. */
static int is(uint8_t s, const uint8_t *m)
{
    int p;
    if (memcmp(flash[OTA_PAGE(s, 0) - 1], m, OTA_H_GEN)) return 0;
    for (p = 0; p < OTA_PARTS; p++)
        if (memcmp(flash[OTA_PAGE(s, p) - 1] + (p ? 0 : OTA_HDR_SIZE),
                   m + (long)p * OTA_SECTOR + (p ? 0 : OTA_HDR_SIZE),
                   OTA_SECTOR - (p ? 0 : OTA_HDR_SIZE))) return 0;
    return 1;
}

static void wipe(void) { memset(flash, 0xFF, sizeof flash); }
static uint8_t hdr(uint8_t s, uint8_t o) { return flash[OTA_PAGE(s, 0) - 1][o]; }

/* ---- what the application does ----------------------------------------- */
static uint8_t call(uint8_t op, uint32_t at, uint16_t len, uint8_t *buf)
{
    struct ota_req r;
    r.op = op; r.rc = 0xEE; r.off = (uint16_t)at; r.hi = (uint8_t)(at >> 16);
    r.len = len; r.buf = buf;
    ota_op(&r);
    return r.rc;
}

/* Boot: the bootloader picks, and the application then runs that slot. */
static int boot(void)
{
    int s = boot_decide();
    if (s != 0xFF) running = (uint8_t)s;
    return s;
}

/* Upload m the way src/ota.c does: erase, the body in 256-byte pieces, the
 * header last. Returns the FINISH result. */
static uint8_t upload(const uint8_t *m)
{
    static uint8_t buf[256];
    long o;
    uint8_t rc = call(OTA_OP_ERASE, 0, 0, 0);
    if (rc) return rc;
    for (o = OTA_HDR_SIZE; o < (long)N; o += 256) {
        long n = N - o < 256 ? N - o : 256;
        memcpy(buf, m + o, n);
        if (call(OTA_OP_WRITE, (uint32_t)o, (uint16_t)n, buf)) return 0xE1;
    }
    memcpy(buf, m, OTA_HDR_SIZE);
    return call(OTA_OP_FINISH, 0, 0, buf);
}

/* Boot with the power cut after `first` operations, then again after a
 * random number, up to `cuts` times; then a clean boot. */
static int boot_with_cuts(long first, int cuts)
{
    long b = first;
    int k;
    for (k = 0; k < cuts; k++) {
        budget = b;
        if (!setjmp(cut)) {
            int r = boot();
            budget = -1;
            return r;
        }
        b = (long)(rnd() % 4);
    }
    budget = -1;
    return boot();
}

/* ---- checks ------------------------------------------------------------ */
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t saved[SECTORS][OTA_SECTOR];
static uint8_t saved_run;
static void snap(void) { memcpy(saved, flash, sizeof flash); saved_run = running; }
static void restore(void) { memcpy(flash, saved, sizeof flash); running = saved_run; }

int main(void)
{
    long n, step;
    int r, k;
    const uint8_t *A = img[0], *B = img[1], *C = img[2];

    make_image(img[0], 1); make_image(img[1], 2); make_image(img[2], 3);

    /* The CRC is zlib's. */
    CHECK(crc_of((const uint8_t *)"123456789", 9) == 0xCBF43926ul, "crc32 is not zlib's");

    /* 1. A in slot A, as JTAG leaves it: it runs, nothing is written. */
    wipe(); put(0, A, 1, 1);
    n = ops; r = boot();
    CHECK(r == 0 && ops == n, "plain boot: slot %d, %ld writes", r, ops - n);

    /* 2. No image anywhere: the bootloader says so. */
    wipe();
    CHECK(boot() == 0xFF, "empty Flash booted");

    /* 3. Upload B while A runs: B lands in slot B with the next generation;
     *    a cut anywhere in the upload leaves A running. */
    wipe(); put(0, A, 1, 1); boot();
    CHECK(upload(B) == OTA_OK, "upload failed");
    CHECK(is(1, B) && slot_valid(1) && slot_gen(1) == 2, "slot B: image %d valid %d gen %u",
          is(1, B), slot_valid(1), slot_gen(1));
    CHECK(is(0, A), "the upload touched the running slot");
    for (step = 1; step < 110000; step += 997) {
        wipe(); put(0, A, 1, 1); boot();
        budget = step;
        if (!setjmp(cut)) upload(B);
        budget = -1;
        r = boot();
        CHECK(r == 0 || (r == 1 && is(1, B)), "upload cut at %ld: booted %d", step, r);
        CHECK(is(0, A), "upload cut at %ld: slot A changed", step);
    }

    /* 4. A bad upload is refused and leaves no image in slot B. */
    wipe(); put(0, A, 1, 1); boot();
    {
        static uint8_t bad[N];
        memcpy(bad, B, sizeof bad);
        bad[0x12345] ^= 0x01;                           /* body no longer matches */
        CHECK(upload(bad) == OTA_E_CRC, "corrupt image accepted");
        CHECK(!slot_valid(1), "corrupt image left a valid slot");
        bad[0x12345] ^= 0x01; bad[3] = '6';            /* an old-format image */
        CHECK(upload(bad) == OTA_E_HEADER, "foreign image accepted");
        CHECK(boot() == 0, "bad upload disturbed the boot");
        CHECK(call(OTA_OP_WRITE, 0x10, 16, bad) == OTA_E_RANGE, "header writable directly");
        CHECK(call(OTA_OP_WRITE, N - 8, 16, bad) == OTA_E_RANGE, "write past the image");
    }

    /* 5. B on trial, never confirmed: three boots on B, the fourth goes back
     *    to A and B is marked failed; it stays A. */
    wipe(); put(0, A, 1, 1); boot(); upload(B); snap();
    for (k = 1; k <= OTA_MAX_TRIES; k++) {
        r = boot();
        CHECK(r == 1 && slot_tries(1) == k, "trial boot %d: slot %d tries %u", k, r, slot_tries(1));
    }
    r = boot();
    CHECK(r == 0 && hdr(1, OTA_H_FAILED) != 0xFF, "no rollback after %d boots: slot %d", OTA_MAX_TRIES, r);
    for (k = 0; k < 5; k++) CHECK(boot() == 0, "rolled-back image did not stay");
    CHECK(is(0, A) && is(1, B), "a rollback changed an image");

    /* 6. Every boot of the trial and the rollback, cut at every operation
     *    (a boot writes at most two bytes), and several cuts in a row. */
    restore();
    for (k = 0; k < 6; k++) {
        uint8_t mid[SECTORS][OTA_SECTOR];
        uint8_t mid_run = running;
        memcpy(mid, flash, sizeof flash);
        for (step = 0; step < 3; step++) {
            memcpy(flash, mid, sizeof flash); running = mid_run;
            r = boot_with_cuts(step, 1 + (int)step);
            CHECK(r == 0 || r == 1, "boot %d cut at %ld: nothing booted", k, step);
        }
        memcpy(flash, mid, sizeof flash); running = mid_run;
        boot();
    }
    CHECK(boot() == 0, "cuts during the trial: did not end on A");

    /* 7. B confirmed during its first boot: it never rolls back, and the next
     *    update (C) goes into slot A, a generation on. */
    wipe(); put(0, A, 1, 1); boot(); upload(B);
    CHECK(boot() == 1, "B did not boot");
    CHECK(call(OTA_OP_CONFIRM, 0, 0, 0) == OTA_OK && hdr(1, OTA_H_CONFIRMED) == 0, "confirm");
    for (k = 0; k < 10; k++) CHECK(boot() == 1, "confirmed image rolled back");
    CHECK(upload(C) == OTA_OK && is(0, C) && slot_gen(0) == 3, "C into slot A: gen %u", slot_gen(0));
    CHECK(boot() == 0, "C did not boot");
    CHECK(boot() == 0 && boot() == 0 && boot() == 1, "C never confirmed: not back to B");

    /* 8. An update while the running image is still on trial is refused:
     *    the only proven image is the one it would overwrite. */
    wipe(); put(0, A, 1, 1); boot(); upload(B); boot();       /* B on trial */
    CHECK(upload(C) == OTA_E_TRIAL && is(0, A), "update over the proven image while on trial");

    /* 9. Confirming an image that already is: nothing written. */
    wipe(); put(0, A, 1, 1); boot();
    n = ops; call(OTA_OP_CONFIRM, 0, 0, 0);
    CHECK(ops == n, "confirm of a confirmed image wrote Flash");

    /* 10. Only one image, and it has failed: it still runs. */
    wipe(); put(1, B, 5, 0); flash[OTA_PAGE(1, 0) - 1][OTA_H_FAILED] = 0;
    CHECK(boot() == 1, "the only image, failed, did not run");

    /* 11. Generations wrap: 0xFFFF then 0x0000 is newer. */
    wipe(); put(0, A, 0xFFFF, 1); put(1, B, 0x0000, 1);
    CHECK(boot() == 1, "wrapped generation not newer");

    /* 12. A half-finished FINISH (cut while writing the header): the slot is
     *     not an image until the magic, which goes last. */
    wipe(); put(0, A, 1, 1); boot();
    call(OTA_OP_ERASE, 0, 0, 0);
    {
        static uint8_t buf[256];
        long o;
        for (o = OTA_HDR_SIZE; o < (long)N; o += 256) { memcpy(buf, B + o, 256); call(OTA_OP_WRITE, (uint32_t)o, 256, buf); }
        memcpy(buf, B, OTA_HDR_SIZE);
        snap();
        for (step = 0; step < 20; step++) {
            restore();
            budget = step;
            if (!setjmp(cut)) call(OTA_OP_FINISH, 0, 0, buf);
            budget = -1;
            r = boot();
            CHECK(r == 0 || (r == 1 && slot_valid(1)), "finish cut at %ld: booted %d", step, r);
        }
    }

    /* 13. The web page sector: written and checked on its own, and nothing
     *     else is touched. */
    wipe(); put(0, A, 1, 1); put(1, B, 2, 1); boot();
    snap();
    {
        static uint8_t ui[OTA_UI_SIZE], hd[16], buf[256];
        long len = 20000, o;
        uint32_t c;
        rng = 7;
        for (o = 0; o < len; o++) ui[OTA_UI_HDR + o] = (uint8_t)rnd();
        c = crc_of(ui + OTA_UI_HDR, len);
        memset(hd, 0xFF, 16);
        hd[0] = 'Z'; hd[1] = 'U'; hd[2] = 'I'; hd[3] = '2';
        hd[4] = (uint8_t)c; hd[5] = (uint8_t)(c >> 8); hd[6] = (uint8_t)(c >> 16); hd[7] = (uint8_t)(c >> 24);
        hd[8] = (uint8_t)len; hd[9] = (uint8_t)(len >> 8);
        CHECK(call(OTA_OP_UI_ERASE, 0, 0, 0) == OTA_OK, "ui erase");
        for (o = OTA_UI_HDR; o < OTA_UI_HDR + len; o += 256) {
            long m = OTA_UI_HDR + len - o < 256 ? OTA_UI_HDR + len - o : 256;
            memcpy(buf, ui + o, m);
            CHECK(call(OTA_OP_UI_WRITE, (uint32_t)o, (uint16_t)m, buf) == OTA_OK, "ui write at %ld", o);
        }
        CHECK(call(OTA_OP_UI_WRITE, 4, 4, buf) == OTA_E_RANGE, "ui header writable directly");
        CHECK(call(OTA_OP_UI_WRITE, 0x7FF0, 32, buf) == OTA_E_RANGE, "ui write past the sector");
        hd[4] ^= 1;
        CHECK(call(OTA_OP_UI_FINISH, 0, 0, hd) == OTA_E_CRC, "bad ui crc accepted");
        CHECK(flash[7][0] == 0xFF, "a refused ui got its magic");
        hd[4] ^= 1;
        CHECK(call(OTA_OP_UI_FINISH, 0, 0, hd) == OTA_OK, "ui finish");
        CHECK(!memcmp(flash[7], hd, 16) && !memcmp(flash[7] + 16, ui + 16, len), "ui sector contents");
        CHECK(!memcmp(flash, saved, 7 * OTA_SECTOR), "ui writes touched another sector");
        hd[0] = 'X';
        CHECK(call(OTA_OP_UI_FINISH, 0, 0, hd) == OTA_E_HEADER, "foreign ui accepted");
        CHECK(boot() == 1, "boot after a ui update");
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
