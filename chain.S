/*
 * chain.S - hand the machine from vidrom to the game, so the boot video costs
 *           the game NOTHING.
 *
 * WHY THIS EXISTS (user, 2026-08-11): "the boot video demo should be completely
 * out of memory by the time the game starts. This is why we have a loading
 * screen."  Measured: BOOTVID adds ZERO BSS - g_vidscratch and g_arena are
 * already shared exactly as intended - but ~4,900 bytes of DECODER CODE, and
 * code cannot be freed on a machine with no MMU.  vidrom already exists as a
 * standalone boot-to-video executable; all that was missing was the handover.
 *
 * ☠️ THE WHOLE PROBLEM IS THAT WE OVERWRITE OURSELVES.  vidrom links at $4000
 * and so does the game, so the moment the first chunk lands the loader is gone.
 * Everything the handover needs therefore has to be BELOW $4000 first:
 *
 *      $0000..$0400   68000 vector table (startup.S wrote it)
 *      $0800..$1800   GameDrive BIOS, re-installed here by the caller
 *      $1C00..        THIS BLOB, memcpy'd down and jumped to
 *      $4000..        the game image, landing on top of vidrom
 *
 * $4000 is the conventional homebrew load address precisely because the space
 * under it is loader scratch, so this is the region that is safe to own.
 *
 * ☠️ POSITION INDEPENDENT, and it has to be: it is memcpy'd to an address the
 * linker never sees.  Every data reference is PC-relative (the filename) or in
 * a register (the BIOS base).  There is no writable data - a .data reference
 * would resolve to the vidrom copy at $4000 and be shot out from under us mid
 * transfer.
 *
 * ABI in:  d0 = gdb_base (the installed BIOS base; gdbios.S's dispatch is
 *               `move.l gdb_base,%a6; jsr (4*fn)(%a6)`, so we hold it in a6
 *               and call the same table directly)
 * Never returns: it jmps to the game's _start at $4000.  On any failure it
 * stops with the border set so a capture names the stage that failed.
 */

	.equ	FN_FOPEN,   10
	.equ	FN_FCLOSE,  11
	.equ	FN_FREAD,   13
	.equ	FN_FSIZE,   16

	.equ	LOAD_ADDR,  0x4000
	.equ	CHUNK,      16384          /* <=24KB and a 512 multiple: the GD
	                                      wants 512-sector, 4-aligned reads */
	.equ	BG,         0xF00058       /* border colour = failure stage    */

	.text
	.globl	chain_blob
	.globl	chain_blob_end

chain_blob:
	move.w	#0x2700,%sr             | interrupts OFF for the whole handover:
	                                | the video ISR would otherwise fire into
	                                | code we are in the middle of erasing
	move.l	%d0,%a6                 | a6 = BIOS base for every call below

	lea	cf_name(%pc),%a0
	moveq	#1,%d0                  | GD_FOPEN_READ|OPEN_EXISTING
	jsr	(4*FN_FOPEN)(%a6)
	tst.w	%d0
	bmi.s	cf_fail_open
	move.w	%d0,%d7                 | d7 = handle

	moveq	#0,%d0
	move.w	%d7,%d0
	jsr	(4*FN_FSIZE)(%a6)
	move.l	%d0,%d6                 | d6 = bytes remaining
	tst.l	%d6
	beq.s	cf_fail_size
	bmi.s	cf_fail_size

	movea.l	#LOAD_ADDR,%a5          | a5 = write pointer

cf_loop:
	tst.l	%d6
	beq.s	cf_done
	move.l	%d6,%d1                 | d1 = this chunk
	cmpi.l	#CHUNK,%d1
	bls.s	cf_have
	move.l	#CHUNK,%d1
cf_have:
	moveq	#0,%d0                  | flags 0 (GD_FREAD_CPU) in the high word,
	move.w	%d7,%d0                 | handle in the low word
	movea.l	%a5,%a0                 | destination
	movem.l	%d1/%d6/%d7/%a5/%a6,-(%sp)
	jsr	(4*FN_FREAD)(%a6)
	movem.l	(%sp)+,%d1/%d6/%d7/%a5/%a6
	tst.w	%d0                     | gd_fread returns 0 on SUCCESS
	bne.s	cf_fail_read
	adda.l	%d1,%a5
	sub.l	%d1,%d6
	bra.s	cf_loop

cf_done:
	moveq	#0,%d0
	move.w	%d7,%d0
	jsr	(4*FN_FCLOSE)(%a6)
	/* Straight into the game's _start.  It sets its own stack, rebuilds the
	   vectors and clears its own BSS, so nothing here needs unwinding - and
	   nothing here may be left on the stack, because that stack is about to
	   belong to somebody else. */
	movea.l	#LOAD_ADDR,%a0
	jmp	(%a0)

	/* Failure stops with a distinct border so one capture names the stage.
	   Stopping is deliberate: a half-written image must never be entered. */
cf_fail_open:
	move.w	#0x00E0,%d0             | red    - file not found / open refused
	bra.s	cf_stop
cf_fail_size:
	move.w	#0x001C,%d0             | green  - opened but size is 0/negative
	bra.s	cf_stop
cf_fail_read:
	move.w	#0x0003,%d0             | blue   - a chunk failed mid-transfer
cf_stop:
	movea.l	#BG,%a0
	move.w	%d0,(%a0)
9:	bra.s	9b

	.balign	2
cf_name:
	.asciz	"OPENLARA.BIN"

	.balign	4
chain_blob_end:

	.section .note.GNU-stack,"",%progbits
