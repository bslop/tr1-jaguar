; dsp_ovl_ent.das - JERRY OVERLAY: rigid-body entity vertex transform.
;
; WHY.  The 68000 rebuilt every animated entity's vertices every frame
; (build_ent_pickup spins, build_ent_door swings), on the chip the user wants
; reduced to boot code.  The maths is the same rotate-about-Y + translate Jerry
; already runs for Lara, so it belongs here.
;
; LOADED, NOT LINKED.  Jerry's code window is 4192 bytes and the resident
; kernel uses 2864, so this image is copied into `ovl_area` by CMD=3 once at
; level start and CALLED by CMD=4 per frame.  It is assembled at OVLBASE, which
; the Makefile computes from the built dsp_pose.bin size - it is NOT position
; independent (JRISC jumps are absolute).
;
; ☠️ AUDIO_PUMP DOES NOT RUN while this executes.  Keep the total job small
; (tens of verts, not hundreds) or the DAC underruns - the symptom is crackle,
; not a hang.
;
; CONTRACT
;   mailbox[6] = job list address (DRAM), mailbox[7] = job count
;   (mailbox base = D_PARAMS[0])
; ☠️ NOT D_PARAMS[16..] - the pose kick's angle copy overwrites that window.
; each job is 8 longs:
;   0 src   - vertex source, 3 LONGS per vert (x,y,z sign-extended).  LONGS,
;             not packed s16: Jerry's byte/word reads of DRAM are unreliable
;             (the same quirk that forces the angle bytes into SRAM), and the
;             source tables are tiny and uploaded once.
;   1 n     - vertex count, PLUS bit16 = SWAP (exchange the two rotated
;             components before the origins are added).  A door hinged along Z
;             needs mspan on Z and thin on X, and that swap happens AFTER the
;             rotation, so it cannot be baked into the source verts.
;   2 c     - cos, pre-scaled >>2 so it FITS s16 (16384 = 1.0)
;   3 s     - sin, same scale
; ☠️ JRISC `imult` is 16x16->32 SIGNED - it multiplies only the LOW 16 BITS.
; Passing a 16.16 cosine here multiplies garbage and the model renders as a
; spike to infinity (seen on silicon 2026-08-09).  The pose kernel dodges this
; the same way: COS(yaw)>>2 into s16, then >>14 instead of >>16.
;   4 rx0   - room-local X origin
;   5 y     - world Y of the model origin
;   6 rz0   - room-local Z origin
;   7 dst   - destination (DRAM), 2 longs per vert, geotex vertex layout:
;             long0 = (rx<<16)|(wy&$FFFF),  long1 = (rz<<16)|255
; The resident loader hands the RETURN ADDRESS in r29 (this image is assembled
; separately and must not depend on where main_loop landed).

	.org	OVLBASE

D_PARAMS	.equ	$F1C240
MAGIC_OVL_DONE	.equ	$0D5BD0E0	; must match main.c

ovl_ent:
	movei	#D_PARAMS,r4
	load	(r4),r0			; the DRAM mailbox base (params[0])
	nop
	nop
	addq	#24,r0			; mailbox[6] = job list
	load	(r0),r5			; r5 = job cursor
	addq	#4,r0			; mailbox[7] = job count
	load	(r0),r6			; r6 = jobs remaining
	nop
	cmpq	#0,r6
	movei	#ovl_ret,r22
	jump	EQ,(r22)
	nop
	nop

job_loop:
	move	r5,r0
	load	(r0),r7			; src
	addq	#4,r0
	load	(r0),r8			; n | swap<<16
	addq	#4,r0
	load	(r0),r11		; c
	addq	#4,r0
	load	(r0),r12		; s
	addq	#4,r0
	load	(r0),r13		; rx0
	addq	#4,r0
	load	(r0),r18		; y
	addq	#4,r0
	load	(r0),r15		; rz0
	addq	#4,r0
	load	(r0),r10		; dst
	nop
	nop
	move	r8,r9
	shrq	#16,r9			; r9 = swap flag
	movei	#$FFFF,r2
	and	r2,r8			; r8 = vertex count
	cmpq	#0,r8
	movei	#job_next,r22
	jump	EQ,(r22)
	nop
	nop

vert_loop:
	move	r7,r0
	load	(r0),r20		; mx
	addq	#4,r0
	load	(r0),r21		; my
	addq	#4,r0
	load	(r0),r23		; mz
	addq	#4,r0
	move	r0,r7			; advance source
	nop
	; rx = ((mx*c + mz*s) >> 16) + rx0
	move	r20,r0
	imult	r11,r0
	move	r23,r1
	imult	r12,r1
	nop
	add	r1,r0
	sharq	#14,r0			; c/s are >>2-scaled (see header)
					; r0 = first rotated component
	; rz = ((-mx*s + mz*c) >> 16) + rz0
	move	r20,r1
	neg	r1
	imult	r12,r1
	move	r23,r2
	imult	r11,r2
	nop
	add	r2,r1
	sharq	#14,r1			; r1 = second rotated component
	; SWAP the two before the origins: which one becomes X depends on the
	; door's orientation, and the origins are per-AXIS, not per-component.
	cmpq	#0,r9
	jr	EQ,no_swap
	nop
	move	r0,r3
	move	r1,r0
	move	r3,r1
no_swap:
	add	r13,r0			; + X origin
	add	r15,r1			; + Z origin
	; wy = y + my
	move	r21,r2
	add	r18,r2
	; pack exactly as the geotex vertex layout expects
	shlq	#16,r0
	movei	#$FFFF,r3
	and	r3,r2
	or	r2,r0
	store	r0,(r10)
	addq	#4,r10
	shlq	#16,r1
	movei	#255,r2
	or	r2,r1
	store	r1,(r10)
	addq	#4,r10
	subq	#1,r8
	movei	#vert_loop,r22
	jump	NE,(r22)
	nop
	nop

job_next:
	movei	#32,r0			; 8 longs per job
	add	r0,r5
	subq	#1,r6
	movei	#job_loop,r22
	jump	NE,(r22)
	nop
	nop

ovl_ret:
	; DRAIN: read back the last written long so the posted writes land before
	; the 68k is told the job is done (same protocol as the pose path).
	subq	#4,r10
	load	(r10),r0
	nop
	nop
	or	r0,r0
	movei	#MAGIC_OVL_DONE,r0
	movei	#D_PARAMS,r22
	load	(r22),r22		; the DRAM mailbox
	nop
	nop
	store	r0,(r22)
	jump	T,(r29)			; back to the resident dispatch loop
	nop
	nop
