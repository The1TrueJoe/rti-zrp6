/* bank.h — code banks and firmware slots (bootloader 3, PLD v10).
 *
 * Code 0x2000-0x7FFF is common; 0x8000-0xFFFF is bank 0 or bank 1, chosen by
 * bit 6 of the PSD page register. Bits 0-3 are the data window and bit 7 the
 * firmware slot, which the bootloader sets and nothing here ever changes.
 * docs/UPDATING.md has the map.
 *
 * A function in a bank that is called from common code or from the other
 * bank is declared BANKED, and SDCC then calls it through the trampoline in
 * bank.s, which switches bit 6 and costs the caller one byte of stack.
 * layout.py checks the link for any call that crosses a bank without it.
 */
#ifndef ZRP6_BANK_H
#define ZRP6_BANK_H

#ifdef __SDCC
#define BANKED __banked
#else
#define BANKED
#endif

#define PAGE_KEEP 0xC0          /* bank + slot: a window never touches them */

/* Open data-window page n / put the W5100 back, keeping the bank and slot.
 * Every write to the page register in the application goes through these:
 * a plain `PSD_PAGE = n` would switch the code bank under the running code,
 * or the slot under everything. */
#define PAGE_WINDOW(n) (PSD_PAGE = (uint8_t)((PSD_PAGE & PAGE_KEEP) | (n)))
#define PAGE_NORMAL()  (PSD_PAGE &= PAGE_KEEP)

/* The slot this firmware is running from: 0 = A, 1 = B. */
#define RUN_SLOT()     ((uint8_t)(PSD_PAGE >> 7))

#endif
