/* ZRP-6 firmware.
 *
 * The box is self-contained: it serves its own interface, is its own MQTT
 * broker, and needs nothing on the network to be useful. Point a browser at
 * its address and the UI comes out of Flash; point mosquitto_sub at 1883 and
 * the same state is there.
 *
 *   http://<ip>/        the interface, one gzipped file out of Flash page 0
 *   ws://<ip>/          MQTT over WebSocket, which is how that interface talks
 *   <ip>:1883           MQTT, for anything that speaks it natively
 *
 * MQTT is the only thing on the network. There is no UDP side channel: it
 * polled the UARTs and made the front panel blink, and a box that is meant to
 * be driven over MQTT should not have a second, undocumented way in. A bad
 * image is recovered over JTAG (tools/flash/).
 *
 * See ../README.md for the three rules that matter when editing any of this.
 */
#include "board.h"
#include "config.h"
#include "../boot/usb.h"
#include "io.h"
#include "w5100.h"
#include "ws.h"
#include "broker.h"
#include "app.h"
#include "ota_app.h"
#include "net.h"
#include "store.h"

/* A new image that nobody has logged in to within this long restarts, and so
 * counts its three boots towards a rollback with nobody at the power switch
 * (docs/UPDATING.md). -DTRIAL_S=60 for a quicker test build. */
#ifndef TRIAL_S
#define TRIAL_S 600
#endif


/* Runs before crt0 clears memory. The address latch and Port B's Control AND
 * Direction registers must be right before the first MOVX or every external
 * access aliases into the same 256 bytes. */
uint8_t __sdcc_external_startup(void)
{
    io_init();
    return 0;
}

/* ---- the stack gauge -----------------------------------------------------
 * Everything lives on 175 bytes of IRAM stack (--stack-auto), and running out
 * does not crash cleanly — it reboots, and the first two times that happened
 * it was diagnosed as a bug in whatever command happened to be running. So the
 * free stack is painted with a pattern at boot, and the untouched part is
 * measured every few seconds and published as <base>/debug/stackfree
 * whenever it shrinks. It only ever goes down; the number after exercising
 * every command is the real margin. */
#define STACK_PAINT 0xA5

/* The address is the loop variable, on purpose. The first version counted
 * with a separate integer, `for (a = SP + 8; a <= 0xFF; a++) *(a) = 0xA5`, and
 * SDCC rewrote it as a countdown and dropped the base: it painted from the top
 * of the stack region DOWN THROUGH THE REGISTER BANKS to address 0, its own
 * return address included, and the box never reached the network. Inline
 * assembly would say exactly what is meant, but --peep-asm crashes SDCC's
 * peephole optimizer on it. With the pointer as the induction variable there
 * is nothing to reverse — and the generated code was read before it was
 * flashed. */
static void stack_paint(void)
{
    __idata uint8_t *p = (__idata uint8_t *)(uint8_t)(SP + 8);
    do {
        *p = STACK_PAINT;
        p++;
    } while ((uint8_t)p != 0);          /* stops when it wraps past 0xFF */
}

/* Bytes at the top of IRAM still holding the paint: the stack's real margin.
 * C this time, because --peep-asm crashes SDCC's peephole optimizer on a naked
 * routine that returns a value — and the generated code was read before it
 * was trusted, after what happened to stack_paint(). */
static uint8_t stack_free(void)
{
    __idata uint8_t *p = (__idata uint8_t *)0xFF;
    uint8_t n = 0;
    while (*p == 0xA5 && (uint8_t)p > SP) {
        n++;
        p--;
    }
    return n;
}

/* The power-cycle reset (docs/MQTT.md): the fourth power-up after three cut
 * short within 15 s. Users back to admin/admin and the network to DHCP;
 * rules, ports and firmware are kept. The status LED flashes red and green
 * for three seconds to say it happened. */
static void factory_reset(void)
{
    uint16_t t;
    uint8_t i;
    (void)store_commit(STORE_USERS_SLOT, 0, 0);
    cfg_edit()->net = NET_DHCP;
    (void)cfg_save();
    for (i = 0; i < 12; i++) {
        leds_status((i & 1) ? LED_GREEN : LED_RED);
        t = ticks();
        while ((uint16_t)(ticks() - t) < TICKS_MS(250))
            ;
    }
    leds_status(LED_RED);
}

void main(void)
{
    uint16_t checked, restart_at = 0;
    uint8_t low = 0xFF, f;

    stack_paint();

    ticks_init();
    cfg_load();
    if (cfg_boot_mark() >= 3) factory_reset();
    wdt_start();
    net_start();                        /* DHCP: up to ~10 s */
    broker_init();
    app_init();
    leds_status(LED_GREEN);             /* up: on the network and serving */
    checked = ticks();

    for (;;) {
        heartbeat();
        broker_poll();
        app_poll();
        ws_ota_run();
        net_poll();
        usb_service();                  /* only while a USB cable is in */

        /* A new address: every connection was to the old one. */
        if (net_changed) {
            net_changed = 0;
            broker_dropall();
            app_net();
        }

        /* An image was just staged (ota.c). Give the reply a moment to leave,
         * then reset into the bootloader, which installs it. */
        if (ota_restart == 1) {
            ota_restart = 2;
            restart_at = ticks();
        } else if (ota_restart && (uint16_t)(ticks() - restart_at) > TICKS_MS(500)) {
            ota_reboot();
        }

        /* A W5100 reset wipes its MAC and IP and nothing announces it: the
         * sockets just go quiet. Once every ~3 s, check it still holds its
         * address and set it up again if not. */
        if ((uint16_t)(ticks() - checked) > TICKS_S(3)) {
            checked = ticks();
            if (!w5100_ip_ok(net_ip())) {
                leds_status(LED_RED);   /* the network chip was reset */
                net_reapply();
                broker_dropall();
                leds_status(LED_GREEN);
            }
            if (uptime() > TRIAL_S && ota_state() >= OTA_ST_TRIAL)
                ota_reboot();           /* unconfirmed: towards a rollback */
            if (uptime() >= 15) cfg_boot_ok();      /* not a reset gesture */
            /* A new static address stands once someone logs in on it; with
             * nobody in 10 minutes it was probably wrong: back to DHCP. */
            if (cfg_get()->net == NET_TRIAL) {
                if (broker_clients()) {
                    cfg_edit()->net = NET_STATIC;
                    (void)cfg_save();
                } else if (uptime() > TRIAL_S) {
                    cfg_edit()->net = NET_DHCP;
                    (void)cfg_save();
                    ota_reboot();
                }
            }
            f = stack_free();
            if (f < low) { low = f; app_stack(f); }
        }
    }
}
