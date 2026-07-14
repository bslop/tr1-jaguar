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
	jsr	beacon_paint
4:	bra.s	4b

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
	mulu.w	#6400,%d0		| 10 scanlines * 640 bytes per row
	lea	4(%a4,%d0.l),%a2	| +2px inset
	lea	1280(%a2),%a2		| +2 scanlines of gap
	moveq	#31,%d6			| d6 = bit, MSB first
bbit:	move.w	#0x2104,%d1		| dark block = 0
	btst	%d6,%d3
	beq.s	bcol
	move.w	#0xFFFF,%d1		| white block = 1
bcol:	moveq	#31,%d0
	sub.l	%d6,%d0
	mulu.w	#20,%d0			| x = (31 - bit) * 10px * 2B
	lea	(%a2,%d0.l),%a1
	moveq	#6,%d2			| 7 scanlines tall
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
	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.w	0xF000E0,%d1		| INT1 read = pending sources
	btst	#0,%d1			| video pending?
	beq.s	1f
	jsr	vblank_handler		| only run the frame flip on VIDEO ints
1:
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	| ack video+GPU latches, keep video+GPU enabled. (GPU-done ints just wake
	| the 68k out of STOP in gpu_sync — no handler work needed.)
	move.w	#0x0303,0xF000E0	| INT1: clear video+GPU latches, enable both
	move.w	#0x0000,0xF000E2	| INT2: resume
	rte

	.section .note.GNU-stack,"",%progbits
