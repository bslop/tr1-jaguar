/*
 * divmod68k.S - 68000-safe integer helpers
 *
 * The system libgcc (m68k-linux-gnu) is built for 68020: its
 * __divsi3/__modsi3/__umodsi3 call __udivsi3 with bsr.l (0x61FF +
 * 32-bit displacement), which a real 68000 decodes as bsr.s -1 and
 * jumps to an odd address (found the hard way: address error at
 * __modsi3+0xd on BigPEmu). Algorithms below are libgcc's own
 * (lb1sf68), re-assembled for -m68000 with bsr.w linkage. Defining
 * the symbols here keeps the linker from pulling libgcc's copies.
 */

	.text

	.globl	__mulsi3
__mulsi3:
	move.w	4(%sp),%d0	| a_hi
	mulu.w	10(%sp),%d0	| a_hi * b_lo
	move.w	6(%sp),%d1	| a_lo
	mulu.w	8(%sp),%d1	| a_lo * b_hi
	add.w	%d1,%d0
	swap	%d0
	clr.w	%d0
	move.w	6(%sp),%d1
	mulu.w	10(%sp),%d1	| a_lo * b_lo
	add.l	%d1,%d0
	rts

	.globl	__udivsi3
__udivsi3:
	move.l	%d2,-(%sp)
	move.l	12(%sp),%d1	| divisor
	move.l	8(%sp),%d0	| dividend
	cmp.l	#0x10000,%d1
	bcc.s	3f
	| divisor fits 16 bits: two chained divu.w
	move.l	%d0,%d2
	clr.w	%d2
	swap	%d2
	divu.w	%d1,%d2		| high word / divisor
	move.w	%d2,%d0
	swap	%d0
	move.w	10(%sp),%d2	| remainder:dividend_lo
	divu.w	%d1,%d2
	move.w	%d2,%d0
	bra.s	6f
3:	| big divisor: shift both down, then correct
	move.l	%d1,%d2
4:	lsr.l	#1,%d1
	lsr.l	#1,%d0
	cmp.l	#0x10000,%d1
	bcc.s	4b
	divu.w	%d1,%d0
	and.l	#0xFFFF,%d0
	move.l	%d2,%d1
	mulu.w	%d0,%d1
	swap	%d2
	mulu.w	%d0,%d2
	swap	%d2
	tst.w	%d2
	bne.s	5f
	add.l	%d2,%d1
	bcs.s	5f
	cmp.l	8(%sp),%d1
	bls.s	6f
5:	subq.l	#1,%d0
6:	move.l	(%sp)+,%d2
	rts

	.globl	__divsi3
__divsi3:
	move.l	%d2,-(%sp)
	moveq	#1,%d2
	move.l	12(%sp),%d1
	bpl.s	1f
	neg.l	%d1
	neg.b	%d2
1:	move.l	8(%sp),%d0
	bpl.s	2f
	neg.l	%d0
	neg.b	%d2
2:	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	bsr.w	__udivsi3
	addq.l	#8,%sp
	tst.b	%d2
	bpl.s	3f
	neg.l	%d0
3:	move.l	(%sp)+,%d2
	rts

	.globl	__umodsi3
__umodsi3:
	move.l	8(%sp),%d1
	move.l	4(%sp),%d0
	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	bsr.w	__udivsi3
	addq.l	#8,%sp
	move.l	8(%sp),%d1
	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	bsr.w	__mulsi3
	addq.l	#8,%sp
	move.l	4(%sp),%d1
	sub.l	%d0,%d1
	move.l	%d1,%d0
	rts

	.globl	__modsi3
__modsi3:
	move.l	8(%sp),%d1
	move.l	4(%sp),%d0
	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	bsr.w	__divsi3
	addq.l	#8,%sp
	move.l	8(%sp),%d1
	move.l	%d1,-(%sp)
	move.l	%d0,-(%sp)
	bsr.w	__mulsi3
	addq.l	#8,%sp
	move.l	4(%sp),%d1
	sub.l	%d0,%d1
	move.l	%d1,%d0
	rts

	.section .note.GNU-stack,"",%progbits
