/* main.c — STM32G474RE (NUCLEO-G474RE) as an OpenOCD remote_bitbang JTAG adapter,
 * for reading the RTI ZRP-6's uPSD3434EV over its 14-pin FlashLINK header.
 *
 * Bare metal: no HAL, no CMSIS, no Arduino. Runs on the reset-default HSI16
 * clock (16 MHz) so there is no PLL to get wrong.
 *
 * Protocol (openocd/doc/manual/jtag/drivers/remote_bitbang.txt):
 *   '0'..'7'         write, bit2=TCK bit1=TMS bit0=TDI
 *   'R'              read, reply '0' or '1' = TDO
 *   'r','s','t','u'  reset (trst,srst) = (0,0)(0,1)(1,0)(1,1)
 *   'B','b'          blink on/off  -> drives LD2
 *   'Q'              quit
 *
 * Plus three diagnostics, outside the OpenOCD protocol:
 *   'P','p','n'      TDO pull-up / pull-down / none  — tells floating from driven
 *   'I'              read back all four pins as one hex char
 *                    bit3=TCK bit2=TMS bit1=TDI bit0=TDO, sampled at the pin
 *
 * Two extra probe pins, for characterising target signals that are not JTAG:
 *   'x','X','y'      probes to input with no pull / pull-up / pull-down
 *   'S'              sample both probes for ~1 s, reply one hex char:
 *                    bit0 P0 saw low, bit1 P0 saw high,
 *                    bit2 P1 saw low, bit3 P1 saw high.
 *                    So 1 = stuck low, 2 = stuck high, 3 = toggling.
 *   'G','g'          drive P0 push-pull high / low (releases or asserts a
 *                    target reset wired to header pin 8)
 *
 *   P0   PB6   (D10) ->  header pin  8   RST
 *   P1   PA7   (D11) <-  header pin  6   TSTAT = uPSD PC3
 *
 * Wiring — all four on the Arduino header, none of them SWJ pins
 * (PA13/PA14/PA15/PB3/PB4 default to JTAG at reset and are deliberately avoided),
 * none of them the VCP (PA2/PA3), the LED (PA5) or the user button (PC13):
 *
 *   TCK  PB10  (D6)  ->  header pin 11
 *   TMS  PA8   (D7)  ->  header pin  9
 *   TDI  PA9   (D8)  ->  header pin  5
 *   TDO  PC7   (D9)  <-  header pin 13     input
 *   GND              <-> header pin 3, 10 or 12
 *
 * The G474 runs at 3.3 V, correct for the uPSD3434EV (3.0-3.6 V).
 * Do NOT connect header pin 7 (VCC) — it is an output from the target.
 */

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC        0x40021000u
#define RCC_AHB2ENR  REG(RCC + 0x4C)
#define RCC_APB1ENR2 REG(RCC + 0x5C)

#define GPIOA      0x48000000u
#define GPIOB      0x48000400u
#define GPIOC      0x48000800u
#define MODER(p)   REG((p) + 0x00)
#define OSPEEDR(p) REG((p) + 0x08)
#define PUPDR(p)   REG((p) + 0x0C)
#define IDR(p)     REG((p) + 0x10)
#define BSRR(p)    REG((p) + 0x18)
#define AFRL(p)    REG((p) + 0x20)

#define LPUART1    0x40008000u
#define LP_CR1     REG(LPUART1 + 0x00)
#define LP_BRR     REG(LPUART1 + 0x0C)
#define LP_ISR     REG(LPUART1 + 0x1C)
#define LP_RDR     REG(LPUART1 + 0x24)
#define LP_TDR     REG(LPUART1 + 0x28)

#define ISR_RXNE (1u << 5)
#define ISR_TXE  (1u << 7)

/* JTAG pin assignment */
#define TCK_PORT GPIOB
#define TCK_BIT  10
#define TMS_PORT GPIOA
#define TMS_BIT  8
#define TDI_PORT GPIOA
#define TDI_BIT  9
#define TDO_PORT GPIOC
#define TDO_BIT  7
#define LED_PORT GPIOA
#define LED_BIT  5

/* Probe pins -- not JTAG, used only for diagnosing the target */
#define P0_PORT  GPIOB
#define P0_BIT   6
#define P1_PORT  GPIOA
#define P1_BIT   7

/* BRR for LPUART = (256 * f_ck) / baud, f_ck = PCLK1 = HSI16 = 16 MHz.
 * 921600 -> 4444 (actual 921692, 0.01% error). Change both here and in
 * tools/rbb_bridge.py --baud if you want something else. */
#define LP_BAUD_BRR 4444u

static inline void pin_out(uint32_t port, uint32_t bit, int high)
{
    BSRR(port) = high ? (1u << bit) : (1u << (bit + 16));
}

static inline int pin_in(uint32_t port, uint32_t bit)
{
    return (IDR(port) >> bit) & 1u;
}

static void gpio_mode(uint32_t port, uint32_t bit, uint32_t mode)
{
    MODER(port) = (MODER(port) & ~(3u << (bit * 2))) | (mode << (bit * 2));
}

static void init(void)
{
    RCC_AHB2ENR  |= (1u << 0) | (1u << 1) | (1u << 2);   /* GPIOA, GPIOB, GPIOC */
    RCC_APB1ENR2 |= (1u << 0);                            /* LPUART1 */

    /* PA2 = LPUART1_TX, PA3 = LPUART1_RX, both AF12 */
    gpio_mode(GPIOA, 2, 2);
    gpio_mode(GPIOA, 3, 2);
    AFRL(GPIOA) = (AFRL(GPIOA) & ~(0xFu << 8)  & ~(0xFu << 12))
                | (12u << 8) | (12u << 12);

    /* JTAG outputs, driven low; TDO an input */
    gpio_mode(TCK_PORT, TCK_BIT, 1);
    gpio_mode(TMS_PORT, TMS_BIT, 1);
    gpio_mode(TDI_PORT, TDI_BIT, 1);
    gpio_mode(LED_PORT, LED_BIT, 1);
    gpio_mode(TDO_PORT, TDO_BIT, 0);
    PUPDR(TDO_PORT) &= ~(3u << (TDO_BIT * 2));            /* no pull on TDO */
    OSPEEDR(TCK_PORT) |= (3u << (TCK_BIT * 2));           /* fast edges on TCK */
    pin_out(TCK_PORT, TCK_BIT, 0);
    pin_out(TMS_PORT, TMS_BIT, 0);
    pin_out(TDI_PORT, TDI_BIT, 0);
    pin_out(LED_PORT, LED_BIT, 0);

    /* Probes start as plain inputs: never drive the target until asked. */
    gpio_mode(P0_PORT, P0_BIT, 0);
    gpio_mode(P1_PORT, P1_BIT, 0);
    PUPDR(P0_PORT) &= ~(3u << (P0_BIT * 2));
    PUPDR(P1_PORT) &= ~(3u << (P1_BIT * 2));

    LP_CR1 = 0;
    LP_BRR = LP_BAUD_BRR;
    LP_CR1 = (1u << 3) | (1u << 2) | (1u << 0);           /* TE | RE | UE */
}

static void put(uint8_t c)
{
    while (!(LP_ISR & ISR_TXE)) { }
    LP_TDR = c;
}

int main(void)
{
    init();
    for (;;) {
        if (!(LP_ISR & ISR_RXNE))
            continue;
        uint8_t c = (uint8_t)LP_RDR;

        if (c >= '0' && c <= '7') {
            uint8_t v = c - '0';
            pin_out(TDI_PORT, TDI_BIT, v & 1);
            pin_out(TMS_PORT, TMS_BIT, v & 2);
            pin_out(TCK_PORT, TCK_BIT, v & 4);
        } else if (c == 'R') {
            put(pin_in(TDO_PORT, TDO_BIT) ? '1' : '0');
        } else if (c >= 'r' && c <= 'u') {
            /* TRST/SRST not wired: pin 2 is unsupported by PSDsoft and pin 8
             * (RST) would reset the ZRP-6. Accepted and ignored. */
        } else if (c == 'B') {
            pin_out(LED_PORT, LED_BIT, 1);
        } else if (c == 'b') {
            pin_out(LED_PORT, LED_BIT, 0);
        } else if (c == 'P' || c == 'p' || c == 'n') {
            /* 01 = pull-up, 10 = pull-down, 00 = none */
            uint32_t pull = (c == 'P') ? 1u : (c == 'p') ? 2u : 0u;
            PUPDR(TDO_PORT) = (PUPDR(TDO_PORT) & ~(3u << (TDO_BIT * 2)))
                            | (pull << (TDO_BIT * 2));
        } else if (c == 'I') {
            uint32_t v = (pin_in(TCK_PORT, TCK_BIT) << 3)
                       | (pin_in(TMS_PORT, TMS_BIT) << 2)
                       | (pin_in(TDI_PORT, TDI_BIT) << 1)
                       | (pin_in(TDO_PORT, TDO_BIT));
            put(v < 10 ? (uint8_t)('0' + v) : (uint8_t)('A' + v - 10));
        } else if (c == 'x' || c == 'X' || c == 'y') {
            uint32_t pull = (c == 'X') ? 1u : (c == 'y') ? 2u : 0u;
            gpio_mode(P0_PORT, P0_BIT, 0);
            gpio_mode(P1_PORT, P1_BIT, 0);
            PUPDR(P0_PORT) = (PUPDR(P0_PORT) & ~(3u << (P0_BIT * 2)))
                           | (pull << (P0_BIT * 2));
            PUPDR(P1_PORT) = (PUPDR(P1_PORT) & ~(3u << (P1_BIT * 2)))
                           | (pull << (P1_BIT * 2));
        } else if (c == 'S') {
            uint32_t seen = 0;
            for (uint32_t i = 0; i < 2000000u; i++)
                seen |= (pin_in(P0_PORT, P0_BIT) ? 2u : 1u)
                      | (pin_in(P1_PORT, P1_BIT) ? 8u : 4u);
            put(seen < 10 ? (uint8_t)('0' + seen) : (uint8_t)('A' + seen - 10));
        } else if (c == 'G' || c == 'g') {
            pin_out(P0_PORT, P0_BIT, c == 'G');
            gpio_mode(P0_PORT, P0_BIT, 1);
        }
        /* 'Q' and anything else: nothing to do */
    }
}
