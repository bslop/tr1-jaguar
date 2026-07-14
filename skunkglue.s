; skunkglue.s — C-callable wrappers for the Skunkboard console library
; (skunk.s, vendored from tursilion/skunk_jcp).  gcc m68k-aout prefixes C
; symbols with an underscore; skunk.s exports bare labels, so each wrapper
; moves the C stack argument into the register the library expects and
; tail-jumps.  The library functions save/restore every register they touch,
; so these are safe from any C call site (not ISRs — skunklib is not
; interrupt-safe).
;
; DEBUG BUILDS ONLY (make SKUNK=1): console writes poll the EZ-Host across
; the cart bus and time out (~ms each) when no `jcp -c` listener is attached.

	.extern	skunkRESET
	.extern	skunkNOP
	.extern	skunkCONSOLEWRITE
	.extern	skunkCONSOLECLOSE
	.extern	skunkConsoleUp

	.text

	.globl	_skunk_init
_skunk_init:
	jsr	skunkRESET
	jsr	skunkNOP		; 2x NOP: sync both EZ-Host buffers with
	jsr	skunkNOP		; the PC console (per SkunkLibrary.txt)
	rts

	.globl	_skunk_puts
_skunk_puts:
	move.l	4(sp),a0		; C arg: NUL-terminated string
	jmp	skunkCONSOLEWRITE	; tail call; its rts returns to C caller

	.globl	_skunk_close
_skunk_close:
	jmp	skunkCONSOLECLOSE

	.globl	_skunk_up
_skunk_up:
	move.l	skunkConsoleUp, d0	; nonzero = console alive
	rts
