; bank.s — the banked-call trampoline and the bootloader stub. Common code.
;
; SDCC calls a __banked function as
;     mov r0,#lo  mov r1,#hi  mov r2,#(addr >> 16)   lcall __sdcc_banked_call
; (or ljmp for a tail call) with the first argument in DPL/DPH/B/A and the
; rest on the stack, and a __banked function returns with
; ljmp __sdcc_banked_ret, its result in DPL/DPH/B/A. SDCC counts exactly one
; byte between the return address and the callee's arguments (the mcs51
; port's banked_overhead): here it is the page register as it was at the call.
; The caller has already saved every live register (SDCC is caller-saves), so
; R0-R7 of the current bank are free; A, B and DPTR are not.
;
; Bank n lives at linear address n*0x10000 + 0x8000, so r2 is 0 or 1; bit 6
; of the page register selects it. Bits 0-3 (the data window) and 7 (the
; slot) are carried through untouched, and restored from the live register
; on return, not from the saved byte, so a window the callee left open stays
; open. No interrupts are enabled in this firmware; if one ever is, its
; handler must be in common code and must not call a banked function.
;
; Every bank switch flushes the prefetch queue and branch cache (BUSCON bits
; 7-6 off and on): they are not flushed by a page change and can hold
; instructions fetched from the other bank (uPSD34xx datasheet, Important
; notes 34.12, "Incorrect code execution when code banks are switched").
;
; Stack: the callee runs 3 bytes above the caller (return address + saved
; page) instead of 2. The trampoline peaks at 5 for the instant before the
; `ret` that enters the callee.
	.module bank
	.globl	__sdcc_banked_call
	.globl	__sdcc_banked_ret
	.globl	_boot_api
	.globl	_boot_page
PSD_PAGE = 0x20E0
BUSCON = 0x9D

	.area	XSEG	(XDATA)
_boot_page:
	.ds	1

	.area	HOME	(CODE)
__sdcc_banked_call:
	mov	r3,dpl
	mov	r4,dph
	mov	r5,a
	mov	dptr,#PSD_PAGE
	movx	a,@dptr
	push	acc			; the banked_overhead byte
	anl	a,#0xBF
	xch	a,r2			; a = bank (0/1), r2 = page without bit 6
	rr	a
	rr	a			; 1 -> 0x40
	orl	a,r2
	movx	@dptr,a			; switch: common code, safe
	anl	BUSCON,#0x3F		; flush PFQ/BC (34.12)
	orl	BUSCON,#0xC0
	mov	a,r0
	push	acc
	mov	a,r1
	push	acc
	mov	dpl,r3
	mov	dph,r4
	mov	a,r5
	ret				; into the callee

__sdcc_banked_ret:
	mov	r3,dpl
	mov	r4,dph
	mov	r5,a
	pop	acc			; the page at the call
	anl	a,#0x40
	mov	r2,a
	mov	dptr,#PSD_PAGE
	movx	a,@dptr
	anl	a,#0xBF
	orl	a,r2
	movx	@dptr,a			; the caller's bank, today's window
	anl	BUSCON,#0x3F		; flush PFQ/BC (34.12)
	orl	BUSCON,#0xC0
	mov	dpl,r3
	mov	dph,r4
	mov	a,r5
	ret

; void boot_api(__xdata struct ota_req *r) — lcall the bootloader API with
; DPTR -> r. The bootloader leaves bits 6-7 of the page register alone; they
; are kept in XDATA (not on the stack: the bootloader's 47 bytes are already
; the deepest thing on its path) and put back anyway, which costs nothing.
; Must be in common code. A normal (not __naked) function to SDCC,
; so the caller saves its registers; _bp (0x08) and the bit bank (0x20) are
; this program's and are saved here.
_boot_api:
	push	0x08
	push	0x20
	mov	r6,dpl
	mov	r7,dph
	mov	dptr,#PSD_PAGE
	movx	a,@dptr
	anl	a,#0xC0
	mov	dptr,#_boot_page
	movx	@dptr,a
	mov	dpl,r6
	mov	dph,r7
	lcall	0x0080
	mov	dptr,#_boot_page
	movx	a,@dptr
	mov	r7,a
	mov	dptr,#PSD_PAGE
	movx	a,@dptr
	anl	a,#0x3F
	orl	a,r7
	movx	@dptr,a
	pop	0x20
	pop	0x08
	ret
