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

#ifdef FLIPASM
| ---------------------------------------------------------------------------
| video_flip_asm — the LOWRES publish path, hand-written (2026-07-27).
|
| WHY ASSEMBLER.  The other half of this protocol is already assembler: the
| LOWRES vblank OP-list repair lives in startup.S because video.c is built by
| jcc68k (~10 instructions per store) and could not beat the OP's object fetch
| at VC 32.  This is the publish half, and its ORDER is load-bearing:
|
|   (1) wait out the previous flip           (pending_fb must be retired)
|   (2) pick a buffer that is neither being drawn into nor on screen
|   (3) precompute the ISR's phrase 0        (ISR only does stores)
|   (4) BLITTER COMPLETION BARRIER
|   (5) publish pending_fb  L A S T
|
| Step 4 before step 5 is the fix this project already paid for once: gpu_sync
| waits for Tom's GPU PROGRAM, not for the Blitter, so the last span can still
| be transferring into `done` when it is published.  HALFRES got that barrier
| free from blit_double's trailing blit_wait; the direct-to-display path did
| not.  Writing it here fixes the order in the instruction stream instead of
| relying on a compiler to keep two volatile stores on either side of a
| non-volatile poll.
|
| front_fb is read EXACTLY ONCE, here, from memory.  It is written by the
| vblank ISR (video.c:308/328) and is NOT declared volatile — so a compiler is
| entitled to cache it.  Nothing does today only because video.c is jcc68k and
| jcc68k keeps everything in memory; that is luck, not a guarantee.
|
| gcc m68k ABI: d0/d1/a0/a1 scratch, d2+/a2+ preserved.
	.globl	video_flip_asm
video_flip_asm:
	movem.l	%d2,-(%sp)

|	(1) wait for the previous flip to retire
#ifdef HANGDIAG
| POLL, never STOP - a STOP nothing wakes cannot report its own timeout.
	suba.l	%a0,%a0
0:	tst.l	pending_fb
	beq.s	1f
	addq.l	#1,%a0
	cmpa.l	#8000000,%a0
	bhs.s	hangdiag_pend
	bra.s	0b
#elif defined(IRQREARM)
| A10: do NOT sleep here waiting for an ISR that may never run.  Poll; every
| ~200k spins re-arm the vertical interrupt; and after three failed re-arms
| give up on the ISR and RETIRE THE FLIP HERE (video_flip_force does the ISR's
| work inline).  Fixing only gpu_sync just moves the hang to this wait.
| SLEEP, do not poll. pending_fb lives in DRAM and only clears when the vblank
| ISR runs ~a field later, so a spin here hammers DRAM for a WHOLE FIELD every
| frame and starves Tom. Measured: that cost HALF the rendered frames.
| STOP releases the bus; the A10 protection is a WAKE COUNT, not a spin - after
| 8 wakes re-arm the interrupt, after 24 give up and retire the flip inline.
	movem.l	%d0/%d1/%a0,-(%sp)
	moveq	#0,%d1			| d1 = wake count
0:	tst.l	pending_fb
	beq.s	7f
	move.l	#0,-(%sp)		| val = 0
	pea	pending_fb		| addr
	jsr	cpu_stop_unless		| atomic check + STOP (releases the bus)
	addq.l	#8,%sp
	addq.l	#1,%d1
	cmp.l	#24,%d1
	blt.s	8f
	moveq	#0,%d1
	jsr	video_flip_force	| the ISR is not coming - do its work here
	bra.s	0b
8:	cmp.l	#8,%d1
	bne.s	0b
	jsr	video_rearm_irq
	bra.s	0b
7:	movem.l	(%sp)+,%d0/%d1/%a0
	bra	1f			| word branch: 1f is past the whole #else arm
#else
0:	tst.l	pending_fb
	beq.s	1f
	move.l	#0,-(%sp)		| val = 0
	pea	pending_fb		| addr
	jsr	cpu_stop_unless		| atomic check + STOP (no lost wakeup)
	addq.l	#8,%sp
	bra.s	0b
#endif

1:	move.l	draw_buf,%d0		| done  = the frame just rendered
	move.l	front_fb,%d1		| shown = what the OP is displaying

|	(2) rotate to the buffer that is neither `done` nor `shown`
	lea	fb0,%a1
	cmp.l	%a1,%d0
	beq.s	2f			| fb0 is being drawn -> try fb1
	cmp.l	%a1,%d1
	bne.s	4f			| fb0 is free -> take it
2:	lea	fb1,%a1
	cmp.l	%a1,%d0
	beq.s	3f
	cmp.l	%a1,%d1
	bne.s	4f
3:	lea	fb2,%a1			| both busy -> the third buffer
4:	move.l	%a1,draw_buf

|	(3) pend_fix0 = (done << 8) | (op_link >> 8)
	move.l	%d0,%d2
	lsl.l	#8,%d2
	move.l	op_link,%d1
	lsr.l	#8,%d1
	or.l	%d1,%d2
	move.l	%d2,pend_fix0

|	(4) COMPLETION BARRIER — I/O poll, not a DRAM poll; ~nil when idle
#ifdef HANGDIAG
| HANGDIAG: this barrier is UNBOUNDED.  If the Blitter never reports idle the
| 68k spins here forever and the screen stays black with no other symptom —
| indistinguishable from every other hang in the boot path.  Bound it, and on
| timeout paint the border RED and halt, so a photograph names the culprit.
	suba.l	%a0,%a0			| a0 = iteration counter
5:	addq.l	#1,%a0
	cmpa.l	#8000000,%a0
	bhs.s	hangdiag_blit
	move.l	0xF02238,%d1		| B_CMD (read = status)
	btst	#0,%d1			| BLIT_IDLE
	beq.s	5b
#else
5:	move.l	0xF02238,%d1		| B_CMD (read = status)
	btst	#0,%d1			| BLIT_IDLE
	beq.s	5b
#endif

|	(5) publish LAST
	move.l	%d0,pending_fb

	movem.l	(%sp)+,%d2
	rts

#ifdef HANGDIAG
hangdiag_blit:
	move.w	#0xF800,0xF00058	| RED border: the Blitter never went idle
9:	bra.s	9b
hangdiag_pend:
	move.w	#0x001F,0xF00058	| BLUE border: pending_fb never retired
8:	bra.s	8b
#endif
#endif /* FLIPASM */

	.section .note.GNU-stack,"",%progbits
