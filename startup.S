/*
 * startup.S - Jaguar 68000 startup and interrupt stub
 *
 * Loaded at $4000 (RAM) by the emulator / skunkboard / BJL loader.
 * Init sequence and exception handling follow the canonical Jaguar
 * pattern (proven on BigPEmu and hardware): set GPU/DSP
 * endianness, park the stack, install a catch-all exception handler,
 * clear .bss, call main().
 *
 * The vblank ISR stub lives at 68k vector 64 ($100) - Tom delivers
 * all interrupts there.
 */

	.text
	.globl	_start
	.globl	vblank_stub
	.extern	main
	.extern	vblank_handler

_start:
	move.w	#0x2700,%sr		| interrupts off while we set up
	move.l	#0x00070007,0xF0210C	| G_END: GPU big-endian
	move.l	#0x00050005,0xF1A10C	| D_END: DSP big-endian
	lea	0x200000,%sp		| stack at top of 2MB DRAM
	move.w	#0xFFFF,0xF0004E	| VI: suppress vertical interrupts

	| install catch-all handler in vectors 2..255 so a wild exception
	| lands somewhere observable instead of jumping through zero
	lea	exc_catch(%pc),%a1
	lea	0x8,%a0
	move.w	#256-2-1,%d0
0:	move.l	%a1,(%a0)+
	dbra	%d0,0b

	| clear .bss
	lea	__bss_start,%a0
	lea	__bss_end,%a1
1:	cmp.l	%a1,%a0
	bhs.s	2f
	clr.l	(%a0)+
	bra.s	1b
2:
#if defined(HANGDIAG) || defined(BOOTMARK)
| ONE instruction, the earliest possible place, the smallest perturbation this
| fault can be probed with: RED border means _start completed and main() was
| entered.  Border still BLACK => the image never began executing at all.
	move.w	#0xF800,0xF00058	| RED: startup done, entering main()
#endif
	jsr	main
3:	bra.s	3b			| main should never return

/*
 * Catch-all 68k exception handler. Writes magic + SSP + the top 8
 * longs of the exception frame (faulting PC is in there) to the crash
 * scratch area at $820, then spins. The BigPEmu verify script dumps
 * $800-$8FF, so a crash is diagnosable from a headless test run.
 */
exc_catch:
	move.l	#0xEEEE0000,0x820
	move.l	%sp,0x824
	move.l	(%sp),0x828
	move.l	4(%sp),0x82C
	move.l	8(%sp),0x830
	move.l	12(%sp),0x834
	move.l	16(%sp),0x838
	move.l	20(%sp),0x83C
	move.l	24(%sp),0x840
	move.l	28(%sp),0x844

	| ── crash beacon ──────────────────────────────────────────────
	| Hardware has no memory readback over the GameDrive USB link,
	| so make the crash photographable: red border, plus the first
	| six scratch longs ($820..$834: magic, SSP, then the exception
	| frame - SR/PC live in there for every 68000 frame format)
	| painted into ALL THREE framebuffers as rows of 32 bit-blocks,
	| MSB left, white = 1, ~10px per bit. Row 0 is a fixed A5A5A5A5
	| calibration pattern (transitions across the FULL width - a
	| left-heavy pattern let the decoder's grid drift on the right
	| half); row 1 then reads EEEE0000 as a checksum. Decode a
	| capture with tools/decode_beacon.py. The painter itself is
	| beacon_paint below - also C-callable (gpu.c's watchdog paints
	| GPU wedges with a white border; callers halt afterward, so
	| clobbered registers don't matter).
	move.w	#0xF800,0xF00058	| BG: red border = 68k exception
	| STOP TOM FIRST (2026-07-31): the 68k has crashed but the GPU keeps
	| running and re-rendering into the framebuffer, erasing the beacon rows
	| as fast as we paint them - the screen showed the object alive but
	| perfectly black, with no white blocks anywhere. Clear G_CTRL (GPUGO)
	| before painting so what we write stays written.
	clr.l	0xF02114		| G_CTRL = 0: halt the GPU
	jsr	beacon_paint
	| KEEP THE DISPLAY ALIVE (2026-07-31). beacon_paint writes the exception
	| frame into the FRAMEBUFFERS, but after a crash the vblank ISR is dead,
	| so nothing repairs the OP list, the object stops being fetched, and the
	| screen shows only BG - a flat red field with the data invisible in
	| memory that is no longer displayed. That is why the beacon has never
	| been readable. Spin REPAIRING the scaled object ourselves, exactly as
	| the ISR would, so the painted rows stay on screen indefinitely.
	| front_fb is what the OP was last showing; op_fix0/fs_ph* are the
	| C-owned phrase values. All registers are free - we never return.
4:	move.l	op_fix0,%d0
	move.l	%d0,op_list
	move.l	fs_ph1,%d0
	move.l	%d0,op_list+4
	move.l	fs_ph2,%d0
	move.l	%d0,op_list+8
	move.l	fs_ph3,%d0
	move.l	%d0,op_list+12
	clr.l	op_list+16
	move.l	fs_ph5,%d0
	move.l	%d0,op_list+20
	bra.s	4b

	.globl	beacon_paint
beacon_paint:
	lea	crash_fbs,%a5
	moveq	#2,%d5			| three buffers
bfb:	move.l	(%a5)+,%a4		| a4 = framebuffer base
	lea	0x820,%a3		| a3 = scratch longs
	moveq	#0,%d4			| d4 = row block 0..6
brow:	move.l	#0xA5A5A5A5,%d3		| row 0: calibration pattern
	tst.l	%d4
	beq.s	bgot
	move.l	(%a3)+,%d3		| rows 1..6: the scratch longs
bgot:	move.l	%d4,%d0
#ifdef FB8BEACON
	| FB8 (2026-07-31): this painter was written for the ORIGINAL RGB16
	| framebuffer - 640 bytes per scanline, 16-bit pixel writes. The game
	| builds with -DFB8: 8-bit paletted, so a 320px scanline is 320 BYTES.
	| With the old stride every row landed twice as far down as intended
	| (7 rows * 10 lines * 2 = 140 lines) - past the end of a 120-line
	| framebuffer. The beacon has never been visible for that reason.
	| Byte writes, 320-byte stride, 3200 bytes per row block. Colour values
	| are palette INDICES; the decoder calibrates its threshold from the
	| A5A5A5A5 row, so any two distinguishable indices work.
	mulu.w	#3200,%d0		| 10 scanlines * 320 bytes per row
#else
	mulu.w	#6400,%d0		| 10 scanlines * 640 bytes per row
#endif
#ifdef FB8BEACON
	lea	2(%a4,%d0.l),%a2	| +2px inset (1 byte per pixel)
	lea	640(%a2),%a2		| +2 scanlines of gap (320B each)
#else
	lea	4(%a4,%d0.l),%a2	| +2px inset
	lea	1280(%a2),%a2		| +2 scanlines of gap
#endif
	moveq	#31,%d6			| d6 = bit, MSB first
#ifdef FB8BEACON
bbit:	move.w	#0x0000,%d1		| dark block = 0 (palette index 0)
	btst	%d6,%d3
	beq.s	bcol
	move.w	#0xFFFF,%d1		| bright block = 1 (index 255)
bcol:	moveq	#31,%d0
	sub.l	%d6,%d0
	mulu.w	#10,%d0			| x = (31 - bit) * 10px * 1B
#else
bbit:	move.w	#0x2104,%d1		| dark block = 0
	btst	%d6,%d3
	beq.s	bcol
	move.w	#0xFFFF,%d1		| white block = 1
bcol:	moveq	#31,%d0
	sub.l	%d6,%d0
	mulu.w	#20,%d0			| x = (31 - bit) * 10px * 2B
#endif
	lea	(%a2,%d0.l),%a1
	moveq	#6,%d2			| 7 scanlines tall
#ifdef FB8BEACON
blin:	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)+
	move.b	%d1,(%a1)		| 9px wide, 1px gutter
	lea	(320-8)(%a1),%a1
#else
blin:	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)+
	move.w	%d1,(%a1)		| 9px wide, 1px gutter
	lea	(640-16)(%a1),%a1
#endif
	dbra	%d2,blin
	dbra	%d6,bbit
	addq.l	#1,%d4
	cmp.l	#7,%d4
	blt.s	brow
	dbra	%d5,bfb
	rts

/*
 * Vertical interrupt. Fires at VI = vdb-2, just before the display
 * starts, so the handler can rebuild the OP list (the OP destroys it
 * every frame) before the object's YPOS line is reached.
 */
vblank_stub:
	move.l	2(%sp),0x848		| interrupted PC -> scratch: the
					| diagnostic ISR paints it, and a
					| hung main loop shows up as a
					| stable PC (poor man's profiler)
#ifdef OPMOVEM
	movem.l	%d0-%d5/%a0-%a1,-(%sp)	| d3-d5 too: the movem block copy needs 6
#else
	movem.l	%d0-%d2/%a0-%a1,-(%sp)
#endif
#ifdef A10BG
	| A10 probe: the ISR was ENTERED. Overwrites video_init's BLUE with RED.
	| One instruction, so it cannot shift the race the way a console attach
	| does. Visible only when nothing is painting - i.e. in the failing case.
	move.w	#0xF800,0xF00058	| RED
#endif
	move.w	0xF000E0,%d2		| INT1 read = pending sources (d2: survives
					| the C handler; d1 is gcc scratch)
	btst	#0,%d2			| video pending?
	beq.s	1f
#if defined(LOWRES) && !defined(HALFRES)
	| ---- FAST SCALED-OBJECT REPAIR (deadline-critical) -------------------
	| The OP consumes the TYPE-1 scaled object as it draws (phrase 0's data
	| ptr / ypos / height, and the scale phrase's REMAINDER) and re-fetches
	| it at VC 32 — only a few half-lines after this interrupt. video.c is
	| compiled by jcc68k, which folds no constant offsets and spills every
	| local, so the same repair written in C costs ~79 instructions and has
	| been measured overshooting that fetch. Here it is 11, before the C
	| handler is even entered. d0/d1 are already saved above.
	| The C handler re-does this harmlessly (it sees pending_fb == 0), but
	| by then the deadline is already met.
	move.l	pending_fb,%d0
	beq.s	2f
	move.l	pend_fix0,%d1		| flip: adopt the queued buffer now
	move.l	%d1,op_fix0
#ifdef OPMOVEM
	move.l	%d1,op_shadow		| block-copy source must see the flip too
#endif
	move.l	%d0,front_fb
	clr.l	pending_fb
#ifdef OPMOVEM
	| A10 BLOCK REPAIR: six `move.l abs,abs` cost 5 words of INSTRUCTION
	| FETCH each (~30 words) before any data moves, and video.c:145 records
	| that repair finishing at VC 33-35 - PAST the OP's VC 32 re-fetch -
	| once render-time bus starvation cuts the 68k to ~10-20% bus service.
	| Under starvation the fetches dominate, so collapse them to TWO
	| instructions reading a contiguous shadow. Restores all 6 longs
	| including [4] REMAINDER and [5] scale, so the #else tail is skipped.
2:	movem.l	op_shadow,%d0-%d5
	movem.l	%d0-%d5,op_list
#else
2:	move.l	op_fix0,op_list		| [0] data ptr | link
	move.l	fs_ph1,op_list+4	| [1] height | ypos | TYPE=1
	move.l	fs_ph2,op_list+8	| [2] iwidth
	move.l	fs_ph3,op_list+12	| [3] dwidth | pitch | depth | xpos
#ifdef OPPLAIN
	clr.l	op_list+16		| [4] STOP (high long)
	moveq	#4,%d1			| [5] STOP (type 4) — plain-object probe
	move.l	%d1,op_list+20
#else
	clr.l	op_list+16		| [4] REMAINDER
	| [5] VSCALE|HSCALE.  Was the literal #0x4020 (2.0x).  That silently
	| OVERRODE the C-side scale every field, so VRES60 (4.0x) rendered
	| perfectly in the emulator and black-screened silicon - the emulator
	| screenshot is taken from the framebuffer, the ISR only matters on the
	| real display path.  Read it from fs_ph5 so C owns the value.
	move.l	fs_ph5,op_list+20
#endif
#endif	| OPMOVEM
#endif
	jsr	vblank_handler		| only run the frame flip on VIDEO ints
1:
	| ack ONLY the latches observed at entry. The old blanket #$0303 ack
	| also cleared a GPU-done that latched DURING this ISR — eaten unseen,
	| leaving the next STOP to sleep into the following VBL (lost wakeup,
	| the PACING cadence-lock accomplice). An unobserved latch now stays
	| pending and re-fires right after RTE. (GPU-done ints just wake the
	| 68k out of STOP in gpu_sync — no handler work needed.)
	lsl.w	#8,%d2			| pending -> clear bits (INT1 write 8..12)
	andi.w	#0x1F00,%d2
	ori.w	#0x0003,%d2		| keep video+GPU enabled
	move.w	%d2,0xF000E0
	move.w	#0x0000,0xF000E2	| INT2: resume
#ifdef OPMOVEM
	movem.l	(%sp)+,%d0-%d5/%a0-%a1	| MUST match the entry push (d3-d5 too)
#else
	movem.l	(%sp)+,%d0-%d2/%a0-%a1
#endif
	rte

	.section .note.GNU-stack,"",%progbits
