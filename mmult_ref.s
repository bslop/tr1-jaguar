	.gpu
	.org	$F03000
start:
	; matrix [10,20,30] as high-16 of 32-bit words at F03100,+4,+8 (by-row stride 4)
	movei	#$F03100,r1
	movei	#$000A0000,r0
	store	r0,(r1)
	movei	#$F03104,r1
	movei	#$00140000,r0
	store	r0,(r1)
	movei	#$F03108,r1
	movei	#$001E0000,r0
	store	r0,(r1)
	; MTXC = 3 (width 3, by-row)
	movei	#$F02104,r1
	moveq	#3,r0
	store	r0,(r1)
	; MTXA = $100 (byte offset into local SRAM)
	movei	#$F02108,r1
	movei	#$100,r0
	store	r0,(r1)
	; switch to register bank 1 via FLAGS reg (REGPAGE bit14, IMASK clear)
	movei	#$F02100,r1
	movei	#$00004000,r0
	store	r0,(r1)
	nop
	nop
	; bank 1: vector [3,4,5] -> r2=(4<<16)|3, r3=5
	movei	#$00040003,r2
	moveq	#5,r3
	; MMULT r2,r4 -> r4 = 3*10 + 4*20 + 5*30 = 260
	mmult	r2,r4
	nop
	nop
	; store result to DRAM 0x100000 while still in bank 1
	movei	#$00100000,r5
	store	r4,(r5)
	nop
	nop
halt:
	jr	T,halt
	nop
