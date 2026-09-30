; vectors.s — what must sit at fixed addresses in the bootloader.
;
;   0x0000        reset: the bootloader's own startup
;   0x0003-0x0073 every interrupt vector, forwarded to the same offset in
;                 the application (0x2003-0x2073), so an application that
;                 enables an interrupt gets its own handler
;   0x0080        the entry the application calls (ota.h, OTA_API_ENTRY)

	.module	vectors
	.globl	__sdcc_gsinit_startup
	.globl	_ota_op

	.area	VECTORS (ABS,CODE)
	.org	0x0000
	ljmp	__sdcc_gsinit_startup
	.org	0x0003
	ljmp	0x2003
	.org	0x000B
	ljmp	0x200B
	.org	0x0013
	ljmp	0x2013
	.org	0x001B
	ljmp	0x201B
	.org	0x0023
	ljmp	0x2023
	.org	0x002B
	ljmp	0x202B
	.org	0x0033
	ljmp	0x2033
	.org	0x003B
	ljmp	0x203B
	.org	0x0043
	ljmp	0x2043
	.org	0x004B
	ljmp	0x204B
	.org	0x0053
	ljmp	0x2053
	.org	0x005B
	ljmp	0x205B
	.org	0x0063
	ljmp	0x2063
	.org	0x006B
	ljmp	0x206B
	.org	0x0073
	ljmp	0x2073
	.org	0x0080
	ljmp	_ota_op
