| cpu68k.S — the game's only privileged-instruction sites, hoisted out of
| inline asm so jcc68k (which cannot express __asm__ — it silently DROPPED
| these until we caught it, see COBWEB_REQ_jcc68k_adoption.md) compiles
| every C translation unit. GNU as syntax, assembled like startup.S.

	.text

	.globl	cpu_irq_on
cpu_irq_on:				| enable interrupts: SR = $2000
	move.w	#0x2000,%sr
	rts

	.globl	cpu_stop_sleep
cpu_stop_sleep:				| sleep until any interrupt (STOP
	stop	#0x2000			| releases the bus to Tom/Jerry)
	rts

	.globl	cpu_stop_unless
cpu_stop_unless:			| (volatile u32 *addr, u32 val) -> 1 if
					| *addr==val (no sleep), else STOP once, 0.
					| The mask makes check->STOP atomic: an
					| interrupt raised after the check stays
					| LATCHED in INT1 (masking doesn't clear
					| Tom latches) and wakes the STOP the
					| moment it drops the mask — no lost
					| wakeup, no sleeping into the next VBL
					| with the condition already true.
	move.w	#0x2700,%sr		| mask (Tom IRQs are level 2; 7=NMI only)
	move.l	4(%sp),%a0
	move.l	8(%sp),%d0
	cmp.l	(%a0),%d0
	beq.s	1f
	stop	#0x2000			| atomic unmask+sleep
	moveq	#0,%d0
	rts
1:	move.w	#0x2000,%sr		| unmask (game runs supervisor, IPL0)
	moveq	#1,%d0
	rts

#ifdef LEMITDIET
| LARA EMIT DIET burst copies (LEMITDIET, PERFHUNT_CAMPAIGN.md 2026-07-22).
| movem.l streams the face payloads: ~1 fetch word per 3 data words vs the
| gcc per-face gather loop's ~2 fetch words per data word.  Payload-only
| (the constant 12B STAGEDIET plane prefixes are written once at init and
| never touched again), dst advances by the full record stride.
| gcc m68k ABI: args on stack, d0/d1/a0/a1 scratch, d2+ preserved.

	.globl	lemit_qcopy
lemit_qcopy:				| (dst, src, n>=1, stride): 24B payloads
	movem.l	%d2-%d7,-(%sp)		| 24B saved -> args at 28(%sp)
	move.l	28(%sp),%a1		| dst (record slots)
	move.l	32(%sp),%a0		| src (packed bank)
	move.l	36(%sp),%d1		| n
	move.l	40(%sp),%d0		| stride (QREC)
	subq.l	#1,%d1
0:	movem.l	(%a0)+,%d2-%d7		| 6 longs = one quad payload
	movem.l	%d2-%d7,(%a1)
	adda.l	%d0,%a1
	dbra	%d1,0b
	movem.l	(%sp)+,%d2-%d7
	rts

	.globl	lemit_tcopy
lemit_tcopy:				| (dst, src, n>=1, stride): 18B payloads
	movem.l	%d2-%d6,-(%sp)		| 20B saved -> args at 24(%sp)
	move.l	24(%sp),%a1
	move.l	28(%sp),%a0
	move.l	32(%sp),%d1
	move.l	36(%sp),%d0		| stride (TREC)
	subq.l	#1,%d1
0:	movem.l	(%a0)+,%d2-%d5		| 4 longs
	move.w	(%a0)+,%d6		| + 1 word = 18B tri payload
	movem.l	%d2-%d5,(%a1)
	move.w	%d6,16(%a1)
	adda.l	%d0,%a1
	dbra	%d1,0b
	movem.l	(%sp)+,%d2-%d6
	rts
#endif /* LEMITDIET */

	.section .note.GNU-stack,"",%progbits
