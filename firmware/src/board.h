/* board.h — what is wired where on an RTI ZRP-6.
 *
 * Every assignment here was measured on the bench, not taken from a datasheet
 * or a disassembly; several contradict both. docs/REVERSE-ENGINEERING.md has
 * the working and the wrong turns.
 */
#ifndef ZRP6_BOARD_H
#define ZRP6_BOARD_H

/* The csiop base must be declared before the uPSD header, which then names
 * every PSD register for us -- including the VM register bits that decide
 * whether each Flash is reachable as CODE, as XDATA, or both. */
#define PSD_CSIOP 0x2000
#include <mcs51/uPSD33xx.h>
#include <stdint.h>
#include "bank.h"

/* SDCC's uPSD header names the SFRs, including the five that RTI's own init
 * zeroes: P1SFS0/P1SFS1, P3SFS, P4SFS0/P4SFS1 — the port special-function
 * selects. Writing 0 to them puts Ports 1, 3 and 4 into plain I/O, which is
 * what this board wants. BUSCON (0x9D) and P4 (0xC0) come from there too. */

#define XB(a)  (*(__xdata volatile uint8_t *)(uint16_t)(a))

/* ---- time --------------------------------------------------------------- */
/* Timer 0 counts at fOSC/12 = 3.00 MHz (36 MHz crystal) and ticks() counts its
 * 16-bit overflows: 21.8 ms each, 45.8 a second. Measured 2026-09-29 by
 * reading the tick counter over MQTT 30 s apart. For a year the comments said
 * ~6.5 ms, and every timeout written against that ran 3.3 times long. */
#define TIMER_HZ       3001460ul
#define TICKS_PER_S    46u
#define TICKS_S(s)     ((uint16_t)((s) * 46u))
#define TICKS_MS(ms)   ((uint16_t)(((uint32_t)(ms) * 46u + 999u) / 1000u))
#define W(off) XB(0x8000u + (off))          /* W5100, by chip offset */

/* ---- memory map, as the v5 PLD decodes it ------------------------------ */
/*   0x0000-0x1FFF  SRAM      data only — must not shadow the reset vector
 *   0x2000-0x20FF  csiop
 *   0x3000-0x31FF  ECSNET    active low — 16C554 and the LED latch
 *   0x8000-0xFFFF  ECSPER    W5100                                        */

/* ---- csiop (Table 106) --------------------------------------------------
 * uPSD33xx.h declares these as objects (PSD_DATAIN_A and friends). The raw
 * addresses are kept because the protocol layer takes an address from the
 * wire and this board's map is documented in those terms. */
#define CS_DIN_A   0x2000
#define CS_DIN_B   0x2001
#define CS_CTL_A   0x2002
#define CS_CTL_B   0x2003
#define CS_DOUT_A  0x2004
#define CS_DOUT_B  0x2005
#define CS_DIR_A   0x2006
#define CS_DIR_B   0x2007
#define CS_DIN_C   0x2010
#define CS_DIN_D   0x2011
#define CS_DOUT_C  0x2012
#define CS_DOUT_D  0x2013
#define CS_DIR_C   0x2014
#define CS_DIR_D   0x2015
#define CS_DRIVE_D 0x2017
#define CS_MCELLAB 0x2020
#define CS_MCELLBC 0x2021
#define CS_MASKAB  0x2022
#define CS_MASKBC  0x2023
#define CS_PAGE    0x20E0
#define CS_VM      0x20E2

/* ---- infrared ----------------------------------------------------------- */
/* Ports 1-6 are Port A bits 0-5, ACTIVE HIGH. Port 7 is PA6 — the HIGH OUT
 * high-current output — and is ACTIVE LOW, so parking Port A at 0x00 drives it
 * continuously. 0x40 is the true idle. */
#define IR_PORT_DIR   0x7F
#define IR_IDLE       0x40
#define IR_HIGHOUT    0x40

/* ---- front panel -------------------------------------------------------- */
/* One write-only latch, reachable anywhere in 0x3020-0x303F behind the '138.
 * Bits 0-5 are IR indicators 1-6, active high. Bit 7 is the GREEN half of the
 * status LED and bit 6 the RED half — measured on the v5 PLD, 2026-09-23.
 *
 * For a week this file said bits 6-7 were "mode bits" that had to stay set
 * because clearing them hung the CPU. That was measured on the v6 PLD, which
 * disables the '138 for the very cycles addressed to it; on v5 both bits clear
 * with the box running normally. They are LEDs. */
#define LED_LATCH     0x3028
#define LED_IR_MASK   0x3F
#define LED_GREEN     0x80
#define LED_RED       0x40

/* ---- relays, triggers, contacts ---------------------------------------- */
/* Relays 1-3 are Port C bits 2,3,4 ACTIVE HIGH — they are the three that
 * click. Triggers 1-3 are P1.5-P1.7 ACTIVE LOW. The rest state is DOUT_C=0x00
 * and P1 bits set, which is what those registers reset to: relays released. */
#define RELAY_MASK    0x1C
#define TRIGGER_MASK  0xE0
#define P1_INPUTS     0x1F          /* P1.0-P1.3 UART IRQs, P1.4 contact 3 */

/* ---- 16C554 ------------------------------------------------------------- */
/* Four channels on an 8-byte stride. Every access needs P3.1 LOW, exactly as
 * RTI's driver does; with it high the whole window reads as address echo. */
#define UART_BASE(ch) (0x3000 + ((ch) * 8))
#define UART_DLL 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7
#define P3_GATE_LOW   0xFD          /* clear ONLY P3.1 */
#define P3_IDLE       0xFF

/* ---- signal in ---------------------------------------------------------- */
/* The RM-433 drives P3.5 with demodulated baseband — no 40 kHz carrier. It
 * idles LOW and pulses HIGH, inverted from a normal IR demodulator. */
#define SIGIN_MASK    0x20

/* ---- W5100 -------------------------------------------------------------- */
#define WR_MR     0x0000
#define WR_GAR    0x0001
#define WR_SUBR   0x0005
#define WR_SHAR   0x0009
#define WR_SIPR   0x000F
#define WR_RTR    0x0017
#define WR_RCR    0x0019
#define WR_RMSR   0x001A
#define WR_TMSR   0x001B
/* Four independent sockets, 2 KB of TX and RX each with RMSR/TMSR = 0x55, and
 * all four are MQTT clients:
 *
 *   0, 1  listen on 80 — a browser needs the page AND a WebSocket at once
 *   2, 3  listen on 1883 for anything speaking MQTT natively
 *
 * There is no UDP. The box speaks MQTT and nothing else on the network;
 * recovery from a bad image is JTAG. Every TCP socket sniffs its first byte
 * (see ws.h), so the port split is a convention rather than a restriction. */
#define SOCK(n)      (0x0400 + ((n) * 0x0100))
#define S_MR(n)      (SOCK(n) + 0x00)
#define S_CR(n)      (SOCK(n) + 0x01)
#define S_IR(n)      (SOCK(n) + 0x02)
#define S_SR(n)      (SOCK(n) + 0x03)
#define S_PORT(n)    (SOCK(n) + 0x04)
#define S_DIPR(n)    (SOCK(n) + 0x0C)
#define S_DPORT(n)   (SOCK(n) + 0x10)
#define S_TX_FSR(n)  (SOCK(n) + 0x20)
#define S_TX_RD(n)   (SOCK(n) + 0x22)
#define S_TX_WR(n)   (SOCK(n) + 0x24)
#define S_RX_RSR(n)  (SOCK(n) + 0x26)
#define S_RX_RD(n)   (SOCK(n) + 0x28)
#define TXBASE(n)    (0x4000 + ((n) * 0x0800))
#define RXBASE(n)    (0x6000 + ((n) * 0x0800))
#define BUFMASK      0x07FF
#define TX_RING      0x0800         /* 2 KB per socket, and the reason tcp_commit
                                     * waits for it to empty — see w5100.c */

#define MODE_TCP     0x01
#define MODE_UDP     0x02
#define SOCK_CLOSED  0x00
#define SOCK_INIT    0x13
#define SOCK_ESTAB   0x17
#define SOCK_UDP     0x22
#define CR_OPEN      0x01
#define CR_LISTEN    0x02
#define CR_CONNECT   0x04
#define CR_DISCON    0x08
#define CR_CLOSE     0x10
#define CR_SEND      0x20
#define CR_RECV      0x40


/* Joining a house broker (mqtt.c) needs an outbound socket and there is not a
 * spare one, so it takes the native-MQTT listener: a box that has joined a
 * broker does not also need to be one. Nothing enables this yet. */
#define SOCK_BRIDGE  2

#endif
