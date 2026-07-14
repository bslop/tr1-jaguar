/*
 * gdbios.S - Jaguar GameDrive BIOS bindings (68000), GNU as port.
 *
 * Ported from JagGD's gdbios_bindings.s (madmac syntax, underscore
 * symbols) with ONE deliberate deviation: every SPI wait is BOUNDED.
 * The stock bindings spin forever on ASIC_SPI_STATUS bits, which on
 * a console (or BigPEmu) WITHOUT a GameDrive floats open-bus and can
 * hang the machine at the first probe. Here every wait counts down
 * and a timeout fails the whole call cleanly (-1, gdb_base cleared),
 * so gd_install() doubles as safe detection: 0 = GameDrive present
 * and BIOS installed, negative = absent/old firmware -> caller falls
 * back (embedded music). On real hardware the handshake completes in
 * microseconds; the bounds are ~milliseconds.
 *
 * Only the needed subset is ported: install + the file calls
 * the SD music loader uses. Args are longs on the stack (gcc m68k),
 * results in d0; the dispatchers mirror upstream's register ABI into
 * the installed BIOS jump table at gdb_base (header: version word,
 * function-count word, then long entries at index*4).
 */

	.equ	SPI_STATUS, 0xF16002
	.equ	SPI_DATA,   0xF16004
	.equ	SPI_DATAB,  0xF16005

	.equ	ST_PKT,     0x10        /* packet start            */
	.equ	ST_SEL,     0x01        /* slave select            */
	.equ	ST_LATCH,   0x20        /* latch full              */
	.equ	B_HAVE,     3           /* have-data bit           */

	.equ	FN_INIT,     1
	.equ	FN_CARDIN,   9
	.equ	FN_FOPEN,   10
	.equ	FN_FCLOSE,  11
	.equ	FN_FREAD,   13
	.equ	FN_FSIZE,   16

	/* Real-hardware handshakes complete in us..low ms; these bounds
	 * are still ~100x margin, and they cap the no-GameDrive boot
	 * delay (one wait runs to exhaustion during the failed probe)
	 * at ~a quarter second. */
	.equ	SPI_BOUND,  0x18000     /* outer waits, ~0.2s worst */
	.equ	XCHG_BOUND, 0x8000      /* per-byte busy waits      */

	.data
	.balign	4
gdb_base:
	.long	0

	.text

/* ── bounded helpers (d1 = 0 ok / -1 timeout; d2 scratch) ────────── */

/* wait_data: HAVE_DATA low -> ack (PKT|SEL) -> HAVE_DATA high */
gd_waitdata:
	move.l	#SPI_BOUND,%d2
0:	move.w	SPI_STATUS,%d1
	btst	#B_HAVE,%d1
	beq.s	1f
	subq.l	#1,%d2
	bne.s	0b
	bra.s	gd_wd_to
1:	move.w	#ST_PKT+ST_SEL,SPI_STATUS       | ('|' is the comment char)
	move.l	#SPI_BOUND,%d2
2:	move.w	SPI_STATUS,%d1
	btst	#B_HAVE,%d1
	bne.s	3f
	subq.l	#1,%d2
	bne.s	2b
gd_wd_to:
	moveq	#-1,%d1
	rts
3:	moveq	#0,%d1
	rts

/* xchg: send d0.w a byte at a time, receive the reply word in d0 */
gd_xchg:
	move.w	%d0,SPI_DATA
	move.l	#XCHG_BOUND,%d2
0:	tst.w	SPI_STATUS
	bpl.s	1f
	subq.l	#1,%d2
	bne.s	0b
	bra.s	gd_wd_to
1:	move.b	SPI_DATAB,%d0
	ror.w	#8,%d0
	move.w	%d0,SPI_DATA
	move.l	#XCHG_BOUND,%d2
2:	tst.w	SPI_STATUS
	bpl.s	3f
	subq.l	#1,%d2
	bne.s	2b
	bra.s	gd_wd_to
3:	move.b	SPI_DATAB,%d0
	moveq	#0,%d1
	rts

/* ── int gd_install(void *buf4k) ─────────────────────────────────── */
	.globl	gd_install
gd_install:
	movem.l	%d2-%d4/%a2,-(%sp)      | arg lands at 20(%sp)

	/* firmware version probe (upstream GD_HWVersion, bounded) */
	move.w	#ST_PKT,SPI_STATUS
	bsr	gd_waitdata
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	#12,%d0                 | hw-version command
	bsr	gd_xchg
	tst.l	%d1
	bmi	gd_inst_fail
	moveq	#0,%d0                  | zero param-size word
	bsr	gd_xchg
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	#ST_PKT,SPI_STATUS
	bsr	gd_waitdata
	tst.l	%d1
	bmi	gd_inst_fail
	bsr	gd_xchg                 | high word: FIRMWARE
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	%d0,%d3
	bsr	gd_xchg                 | low word: ASIC (unused)
	tst.l	%d1
	bmi	gd_inst_fail
	clr.w	SPI_STATUS
	move.w	#500,%d0
0:	dbra	%d0,0b                  | let the micro finish up

	cmp.w	#0x111,%d3              | 1.11 = first GDBIOS firmware
	blt	gd_inst_fail

	/* drain a stale latch (FIFO DMA termination) */
1:	move.w	SPI_STATUS,%d0
	and.w	#ST_LATCH,%d0
	beq.s	2f
	tst.w	SPI_DATA
	bra.s	1b
2:
	/* request the GDBIOS block: command 0x80, no params */
	move.w	#ST_PKT,SPI_STATUS
	bsr	gd_waitdata
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	#0x80,%d0
	bsr	gd_xchg
	tst.l	%d1
	bmi	gd_inst_fail
	moveq	#0,%d0
	bsr	gd_xchg
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	#ST_PKT,SPI_STATUS

	bsr	gd_waitdata             | block size arrives first
	tst.l	%d1
	bmi	gd_inst_fail
	bsr	gd_xchg
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	%d0,%d3                 | d3 = bytes remaining
	cmp.w	#4096,%d3               | wire-supplied size must fit the
	bhi	gd_inst_fail            | caller's 4KB buffer (desync guard;
	tst.w	%d3                     | upstream trusts it - we don't)
	beq	gd_inst_fail
	move.w	#ST_PKT,SPI_STATUS

	move.l	20(%sp),%a0             | receive into the caller's 4KB
	move.l	%a0,gdb_base

3:	bsr	gd_waitdata             | per <=512-byte packet
	tst.l	%d1
	bmi	gd_inst_fail
	move.w	%d3,%d0
	cmp.w	#512,%d3
	ble.s	4f
	move.w	#512,%d0
4:	sub.w	%d0,%d3
	subq.w	#1,%d0                  | dbra count
5:	move.w	%d0,SPI_DATA            | clock a byte out of the micro
	move.l	#XCHG_BOUND,%d2
6:	tst.w	SPI_STATUS
	bpl.s	7f
	subq.l	#1,%d2
	bne.s	6b
	bra.s	gd_inst_fail
7:	move.b	SPI_DATAB,(%a0)+
	dbra	%d0,5b
	move.w	#ST_PKT,SPI_STATUS
	tst.w	%d3
	bne.s	3b

	move.l	gdb_base,%a0            | installed image sane?
	cmp.w	#0x100,(%a0)            | MINVERSION
	blt	gd_inst_fail
	clr.w	SPI_STATUS

	/* dispatch GD_Init for firmware-side setup */
	move.l	%a6,-(%sp)
	move.l	gdb_base,%a6
	cmp.w	#FN_INIT,2(%a6)
	blt.s	8f
	jsr	(4*FN_INIT)(%a6)
8:	move.l	(%sp)+,%a6

	moveq	#0,%d0
	movem.l	(%sp)+,%d2-%d4/%a2
	rts

gd_inst_fail:
	clr.l	gdb_base
	clr.w	SPI_STATUS
	moveq	#-1,%d0
	movem.l	(%sp)+,%d2-%d4/%a2
	rts

/* ── table dispatchers (upstream GDFunc, register ABI preserved) ── */
.macro	gdfunc num
	move.l	%a6,-(%sp)
	tst.l	gdb_base
	beq.s	9f
	move.l	gdb_base,%a6
	cmp.w	#\num,2(%a6)
	blt.s	9f
	jsr	(4*\num)(%a6)
	move.l	(%sp)+,%a6
	rts
9:	moveq	#-1,%d0
	move.l	(%sp)+,%a6
	rts
.endm

	.globl	gd_card_in              | int gd_card_in(void)
gd_card_in:
	gdfunc	FN_CARDIN

	.globl	gd_fopen                | int gd_fopen(const char *, unsigned)
gd_fopen:
	move.l	4(%sp),%a0              | filename
	move.w	10(%sp),%d0             | mode (low word of the long)
	gdfunc	FN_FOPEN

	.globl	gd_fclose               | int gd_fclose(unsigned handle)
gd_fclose:
	move.w	6(%sp),%d0
	gdfunc	FN_FCLOSE

	.globl	gd_fread                | int gd_fread(unsigned h, void *, unsigned n, unsigned flags)
gd_fread:
	move.w	18(%sp),%d0             | flags
	swap	%d0
	move.w	6(%sp),%d0              | handle
	move.l	8(%sp),%a0              | buffer
	move.l	12(%sp),%d1             | bytes
	gdfunc	FN_FREAD

	.globl	gd_fsize                | int gd_fsize(unsigned handle)
gd_fsize:
	move.w	6(%sp),%d0
	gdfunc	FN_FSIZE

	.section .note.GNU-stack,"",%progbits
