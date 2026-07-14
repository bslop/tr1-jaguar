; dsp_hello.das - Jerry DSP "M0": prove the DSP loads, runs, and reports.
; Param block at DSP SRAM end: D_PARAMS+0 = mailbox ptr (DRAM, 68k writes).
; Writes MAGIC to mailbox[0] then halts. rmac .dsp mode, origin $F1B000.
	.dsp
	.org	$F1B000

D_PARAMS	.equ	$F1BF00
MAGIC_DSP	.equ	$0D5B0001

start:
	movei	#D_PARAMS,r1
	load	(r1),r30		; mailbox ptr (DRAM)
	nop
	nop
	movei	#MAGIC_DSP,r0
	store	r0,(r30)		; mailbox[0] = alive!
	nop
	nop
halt:
	nop
	jr	T,halt
	nop
	nop
