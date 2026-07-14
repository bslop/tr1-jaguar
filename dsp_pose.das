; dsp_pose.das - Jerry DSP: Lara POSE kernel (the 68k's ~60ms job -> DSP).
; Walks the skeleton (matrix stack, YXZ eulers from SINTAB), transforms all
; mesh-local verts to world, heading-rotates, writes stride-8 vert records
; into lara_blob. imult = signed 16x16 in ONE instruction (all pose operands
; fit 16 bits; matrix .12, heading .14). Hazards: >=2 instrs after load;
; 1 gap after imult before reading result (we use 2 - GPU discipline).
;
; PARAM BLOCK (D_PARAMS, longs, 68k-written):
;  +0 mailbox  +4 skv  +8 skvl(u16[])  +12 sknode(s16[4]/mesh)
;  +16 angles(u8[3]/mesh)  +20 SINTAB(32b 16.16 x256)  +24 out(lara_blob+16)
;  +28 rootx +32 rooty +36 rootz  +40 laC(.14) +44 laS  +48 rx0 +52 rz0
;  +56 base_y  +60 mcount (0 = hello mode: write ALIVE magic and halt)
	.dsp

; ---- AUDIO_PUMP: main-context sample service. Fast path = 4 instrs using
; ONLY r22 (the jump-temp, free at every insertion point). Slow path saves
; r0-r3, mixes ONE sample (DRAM loads + DAC stores are LEGAL here — both
; crash in ISR context), restores. FLAGS ARE CLOBBERED: insert only before
; the loop's own compare. ----
.macro	AUDIO_PUMP
	loadw	(r16),r22		; Timer2 divider counter (r16 from init)
	nop
	nop
	cmp	r17,r22			; changed since last service?
	movei	#pskip\~,r22
	jump	EQ,(r22)		; no tick elapsed -> fast skip
	nop
	nop
	movei	#PUMP_SAVE,r22
	store	r0,(r22)
	addq	#4,r22
	store	r1,(r22)
	addq	#4,r22
	store	r2,(r22)
	addq	#4,r22
	store	r3,(r22)
	loadw	(r16),r1		; current down-counter
	nop
	nop
	move	r17,r0
	sub	r1,r0			; elapsed = last - cur (>=1: fast path
	movei	#$FFFF,r3		;  only enters on change)
	and	r3,r0			; mod 2^16
	moveq	#8,r1			; (r1 dead: cur already consumed)
	cmp	r1,r0			; elapsed vs cap
	jr	MI,pcap\~		; < 8 -> service all of it
	nop
	move	r1,r0			; cap the catch-up burst at 8
pcap\~:
	sub	r0,r17			; last -= serviced (debt carries)
	and	r3,r17			; (r3 still $FFFF)
	movei	#DUE_D,r3
	store	r0,(r3)
pmix\~:
	moveq	#0,r2			; acc
	; ---- voice 0 ----
	movei	#V0_CNT,r0
	load	(r0),r1
	nop
	nop
	cmpq	#0,r1
	movei	#pchk0\~,r22
	jump	EQ,(r22)
	nop
	nop
	subq	#1,r1
	store	r1,(r0)
	movei	#V0_LANE,r0
	load	(r0),r1
	nop
	nop
	cmpq	#0,r1
	movei	#ph0\~,r22
	jump	NE,(r22)
	nop
	nop
	movei	#V0_PTR,r0
	load	(r0),r1
	nop
	nop
	load	(r1),r3
	addq	#4,r1
	store	r1,(r0)
	movei	#V0_CVAL,r0
	store	r3,(r0)
	movei	#V0_LANE,r0
	moveq	#4,r1
	store	r1,(r0)
ph0\~:
	movei	#V0_LANE,r0
	load	(r0),r1
	nop
	nop
	subq	#1,r1
	store	r1,(r0)
	movei	#V0_CVAL,r0
	load	(r0),r1
	nop
	nop
	move	r1,r3
	shlq	#8,r1
	store	r1,(r0)
	sharq	#24,r3
	shlq	#6,r3
	add	r3,r2
	movei	#pv1\~,r22
	jump	T,(r22)
	nop
pchk0\~:
	; voice 0 idle: gapless chain — promote the queued buffer (if any)
	movei	#V0_NCNT,r0
	load	(r0),r1
	nop
	nop
	cmpq	#0,r1
	movei	#pv1\~,r22
	jump	EQ,(r22)
	nop
	nop
	movei	#V0_NPTR,r22
	load	(r22),r3
	nop
	nop
	movei	#V0_PTR,r22
	store	r3,(r22)
	movei	#V0_CNT,r22
	store	r1,(r22)
	movei	#V0_LANE,r22
	moveq	#0,r3
	store	r3,(r22)
	store	r3,(r0)			; clear the queue slot LAST
pv1\~:
	; ---- voice 1 ----
	movei	#V1_CNT,r0
	load	(r0),r1
	nop
	nop
	cmpq	#0,r1
	movei	#pdac\~,r22
	jump	EQ,(r22)
	nop
	nop
	subq	#1,r1
	store	r1,(r0)
	movei	#V1_LANE,r0
	load	(r0),r1
	nop
	nop
	cmpq	#0,r1
	movei	#ph1\~,r22
	jump	NE,(r22)
	nop
	nop
	movei	#V1_PTR,r0
	load	(r0),r1
	nop
	nop
	load	(r1),r3
	addq	#4,r1
	store	r1,(r0)
	movei	#V1_CVAL,r0
	store	r3,(r0)
	movei	#V1_LANE,r0
	moveq	#4,r1
	store	r1,(r0)
ph1\~:
	movei	#V1_LANE,r0
	load	(r0),r1
	nop
	nop
	subq	#1,r1
	store	r1,(r0)
	movei	#V1_CVAL,r0
	load	(r0),r1
	nop
	nop
	move	r1,r3
	shlq	#8,r1
	store	r1,(r0)
	sharq	#24,r3
	shlq	#6,r3
	add	r3,r2
pdac\~:
	movei	#L_I2S,r0
	store	r2,(r0)
	movei	#R_I2S,r0
	store	r2,(r0)
	movei	#DUE_D,r0
	load	(r0),r1
	nop
	nop
	subq	#1,r1
	store	r1,(r0)
	cmpq	#0,r1
	movei	#pmix\~,r0
	jump	NE,(r0)			; more samples due -> mix another
	nop
	nop
pfall\~:
	movei	#PUMP_SAVE,r22
	load	(r22),r0
	addq	#4,r22
	load	(r22),r1
	addq	#4,r22
	load	(r22),r2
	addq	#4,r22
	load	(r22),r3
pskip\~:
.endm

	.org	$F1B000

; DSP SRAM map (8KB $F1B000-$F1CFFF): ALL hot data LOCAL — Jerry's 16-bit bus
; link makes DRAM crunching slow; constants uploaded once, results copied out.
; JRISC internal SRAM = 32-BIT ACCESS ONLY (loadw/loadb return garbage!).
; skv packed s16 -> extracted from longs; nodes expanded s32; angles = longs.
D_PARAMS	.equ	$F1C240
MBLK		.equ	$F1C060		; matrix: 12 longs (R[9] then t[3])
OUT_D		.equ	$F1B700		; posed verts out: 300*8 = 2400B
MSTACK		.equ	$F1C0C0		; matrix stack: 20 x 48 bytes
SKV_D		.equ	$F1C3C0		; mesh-local verts s16-PACKED: 1800B
SINTAB_D	.equ	$F1CB00		; 256*4 sine 16.16
SKNODE_D	.equ	$F1CF00		; nodes s32-EXPANDED: 14*4 longs
MAGIC_ALIVE	.equ	$0D5B0001
MAGIC_DONE	.equ	$0D5BD05E
; room-transform mode constants (MUST track gpu_geotex.gas!)
RX_NEAR		.equ	64		; MUST match gpu_geotex NEAR
RX_CX		.equ	160
	.if LOWRES
RX_CY		.equ	60		; half-height render (MUST track gpu_geotex)
RX_FOCAL_Y	.equ	95		; FOCAL/2 vertical
	.else
RX_CY		.equ	120
RX_FOCAL_Y	.equ	190
	.endif
RX_FOCAL	.equ	190
RX_SENT		.equ	$07FFFFFF
CAMB_D		.equ	$F1C090		; 7-long camera copy (below MBLK)
; ---- RESIDENT AUDIO (2026-07-11) ----
CMD_D		.equ	$F1C338		; command word: 0 idle / 1 pose / 2 roomx
					; (AFTER the angle window +40..+F4!)
V0_CNT		.equ	$F1C340		; voice 0: bytes remaining (0 = idle)
V0_LANE		.equ	$F1C344		;   bytes left in the cached long
V0_PTR		.equ	$F1C348		;   next long fetch address (DRAM)
V0_CVAL		.equ	$F1C34C		;   cached long (top byte = next sample)
V1_CNT		.equ	$F1C350
V1_LANE		.equ	$F1C354
V1_PTR		.equ	$F1C358
V1_CVAL		.equ	$F1C35C
ISR_STACK	.equ	$F1C3B8		; ISR stack TOP (grows DOWN into $F1C360+)
ISR_COUNT	.equ	$F1C328		; ISR fire counter (probe; far from stack)
WAKE_D		.equ	$F1C330		; ISR->main "sample due" flag
PUMP_SAVE	.equ	$F1C360		; 6 longs: pump register save area
V0_NCNT		.equ	$F1C378		; voice 0 QUEUED next-buffer len (0=empty)
V0_NPTR		.equ	$F1C37C		;   queued next-buffer DRAM address
LOOP_COUNT	.equ	$F1C32C		; main-loop heartbeat (probe)
D_FLAGS_R	.equ	$F1A100
JPIT1_R		.equ	$F10000		; Jerry PIT: prescale-1
JPIT2_R		.equ	$F10002		;   divide-1: f = 26.59MHz/((p+1)*(d+1))
J_INT_R		.equ	$F10020		; JINTCTRL: low=enables, read=pending
JINTFLG_R	.equ	$F10022		; pending flags; write hi-byte = clear
SSTAT_R		.equ	$F1A150		; SSTAT (READ side of the SCLK addr!):
					; bit0 = Word Strobe pin state (Jaguar
					; Technical Reference v8). One WS cycle
					; = one stereo frame = 1/11025s.
VC_R		.equ	$F00006		; Tom vertical counter (half-lines)
WSLAST_D	.equ	$F1C380		; probe: last WS bit
FALLC_D		.equ	$F1C388		; probe: falling-edge slow entries
JPIT4R_R	.equ	$F1003C		; Timer2 DIVIDER read-back (down counter)
JPIT3_W		.equ	$F10004		; Timer2 prescale write
JPIT4_W		.equ	$F10006		; Timer2 divide write
DUE_D		.equ	$F1C390		; pump: samples still due this service
VCLAST_D	.equ	$F1C384		; probe: last VC value
SCLK_R		.equ	$F1A150
SMODE_R		.equ	$F1A154
L_I2S		.equ	$F1A148
R_I2S		.equ	$F1A14C

; RESIDENT KERNEL: runs forever. $F1B000 = reset entry (16B), $F1B010 =
; I2S interrupt vector (bank 1). Pose/roomx are subroutines dispatched by
; CMD_D; the 68k NEVER touches D_CTRL/D_PC after init.
start:
	movei	#init,r22
	jump	T,(r22)
	nop
	nop
	nop
	nop				; pad entry to EXACTLY 16 bytes:
					; movei(6)+jump(2)+4*nop(8) = $F1B010
	; ---- $F1B010: I2S interrupt vector (ISR-owned r30 ONLY: r22 is a
	; scoreboard-hazard register — the mailbox reroutes load into it) ----
	movei	#i2s_isr,r30
	jump	T,(r30)
	nop

init:
	; SCLK/SMODE are set by the 68k ONLY (16-bit registers; JRISC 32-bit
	; stores mangle them — the reference kernel never touches them either)
	; bank-1 ISR stack pointer
	movei	#ISR_STACK,r31
	moveta	r31,r31			; SP in both banks (reference idiom)
	movei	#JPIT4R_R,r16		; pump timebase: Timer2 divider COUNTER
	moveq	#0,r17			; (reads endorsed by the ref manual);
					; r17 = last read. No ints enabled ever.
	movei	#JPIT3_W,r0
	movei	#2431,r1
	storew	r1,(r0)			; prescale-1: 26.59M/2432 = 10933Hz =
					; EXACTLY the DAC frame rate (SCLK=37:
					; 64*(37+1)=2432 — same crystal, zero
					; drift; an 11025 tick left +91/s surplus
					; samples colliding with the DAC = buzz)
	movei	#JPIT4_W,r0
	movei	#$FFFF,r1
	storew	r1,(r0)			; divider = free-running 16-bit wrap
	; silence the voices
	movei	#V0_CNT,r0
	moveq	#0,r1
	moveq	#8,r2
init_vz:
	store	r1,(r0)
	addq	#4,r0
	subq	#1,r2
	jr	NE,init_vz
	nop
	; clear CMD
	movei	#CMD_D,r0
	store	r1,(r0)
	; hello handshake FIRST (interrupts still off; r30/r31 are ISR-owned
	; from the moment I2S is enabled)
	movei	#D_PARAMS,r4
	movei	#MAGIC_ALIVE,r0
	movei	#D_PARAMS,r22
	load	(r22),r22
	nop
	nop
	store	r0,(r22)
	; I2S interrupt enable PARKED (2026-07-12): the mixer ISR is scoreboard-
	; unsafe — an in-flight main-loop DRAM load lands in its target register
	; MID-ISR, so save/restore tears it (Jerry died ~2s after the first
	; active mix). Fix = full register segregation: ISR on the alternate
	; bank (REGPAGE), bank-0 stub limited to r30/r16/r17 with r16/r17
	; vacated from pose/roomx (reload camy/camz + rx0/rz0 from SRAM per
	; vert). Everything else (resident loop, vectors at $F1B010, voices,
	; unmute, SCLK 37/SMODE $15) is verified plumbing — re-enable when the
	; segregated ISR lands.
	movei	#D_FLAGS_R,r0
	load	(r0),r1
	nop
	nop
	movei	#$08,r2			; POLLED AUDIO v2: IMASK held; NO ints
	or	r2,r1			; enabled at the DSP at all. Timebase =
	store	r1,(r0)			; Jerry PIT polled via JINTFLG.
	movei	#JPIT1_R,r0
	moveq	#3,r1
	storew	r1,(r0)			; prescale-1 = 3
	movei	#JPIT2_R,r0
	movei	#602,r1
	storew	r1,(r0)			; divide-1 = 602 -> 26.59M/2412 = 11024Hz
	; (JPIT keeps counting harmlessly; J_INT left disabled — the pump's
	;  timebase is SSTAT WS edges, measured 2x11025/s on HW 2026-07-12)

main_loop:
	movei	#LOOP_COUNT,r0
	load	(r0),r1
	addq	#1,r1
	store	r1,(r0)
	AUDIO_PUMP
	movei	#CMD_D,r0
	load	(r0),r1
	nop
	cmpq	#1,r1
	movei	#cmd_pose,r22
	jump	EQ,(r22)
	nop
	nop
	cmpq	#2,r1
	movei	#cmd_roomx,r22
	jump	EQ,(r22)
	nop
	nop
	movei	#main_loop,r22
	jump	T,(r22)
	nop
	nop

cmd_pose:
	movei	#CMD_D,r0
	moveq	#0,r1
	store	r1,(r0)
	movei	#D_PARAMS,r4
	movei	#60,r0
	add	r4,r0
	load	(r0),r6			; mcount
	movei	#do_pose,r22
	jump	T,(r22)
	nop
	nop

cmd_roomx:
	movei	#CMD_D,r0
	moveq	#0,r1
	store	r1,(r0)
	movei	#D_PARAMS,r4
; ================= ROOM TRANSFORM MODE =================
	; copy camera block DRAM -> SRAM (7 longs)
	movei	#4,r0
	add	r4,r0
	load	(r0),r5			; room list ptr
	movei	#8,r0
	add	r4,r0
	load	(r0),r1			; camblk DRAM ptr
	movei	#CAMB_D,r2
	moveq	#7,r3
rx_camcpy:
	load	(r1),r0
	addq	#4,r1
	subq	#1,r3
	nop
	store	r0,(r2)
	addq	#4,r2
	cmpq	#0,r3
	jr	NE,rx_camcpy
	nop
	; load camera regs from SRAM copy
	movei	#CAMB_D,r0
	load	(r0),r11		; cY4
	addq	#4,r0
	load	(r0),r12		; sY4
	addq	#4,r0
	load	(r0),r13		; cP4
	addq	#4,r0
	load	(r0),r14		; sP4
	addq	#4,r0
	load	(r0),r15		; camx
	; camy/camz NOT register-resident: r16/r17 are ISR-owned (scoreboard
	; segregation) — reloaded from CAMB_D at each per-vert use via r22
	load	(r5),r6			; room count
	addq	#4,r5
rx_room:
	cmpq	#0,r6
	movei	#rx_alldone,r22
	jump	EQ,(r22)
	nop
	nop
	; PREEMPT: a pose request aborts the remaining rooms BETWEEN rooms
	; (their flags stay 1 -> Tom self-transforms; no orphaned stores)
	movei	#CMD_D,r0
	load	(r0),r1
	nop
	cmpq	#1,r1
	movei	#rx_alldone,r22
	jump	EQ,(r22)
	nop
	nop
	subq	#1,r6
	load	(r5),r7			; vert ptr (DRAM RVerts: s16 x,y,z + pad, stride 8)
	addq	#4,r5
	load	(r5),r8			; vcount
	addq	#4,r5
	load	(r5),r9			; offX<<8
	addq	#4,r5
	load	(r5),r10		; offZ<<8
	addq	#4,r5
	load	(r5),r18		; cache ptr (flag long at cache-4)
	addq	#4,r5
	movei	#4096,r0		; depth array follows the 512-vert cache
	move	r18,r19
	add	r0,r19			; r19 = per-vert depth cursor
rx_vert:
	cmpq	#0,r8
	movei	#rx_vdone,r22
	jump	EQ,(r22)
	nop
	nop
	subq	#1,r8
	; load s16 x,y,z from DRAM (16-bit link: loadw works on DRAM)
	loadw	(r7),r20
	addq	#2,r7
	loadw	(r7),r21
	addq	#2,r7
	loadw	(r7),r23
	addq	#4,r7			; skip pad -> next vert
	shlq	#16,r20
	sharq	#16,r20			; x
	shlq	#16,r21
	sharq	#16,r21			; y
	shlq	#16,r23
	sharq	#16,r23			; z
	add	r9,r20			; wx
	add	r10,r23			; wz
	sub	r15,r20			; dx
	movei	#CAMB_D+20,r22
	load	(r22),r22		; camy (SRAM: cheap)
	nop
	nop
	sub	r22,r21			; dy
	movei	#CAMB_D+24,r22
	load	(r22),r22		; camz
	nop
	nop
	sub	r22,r23			; dz
	; rx = (dx*cY4 - dz*sY4)>>12 ; rz = (dx*sY4 + dz*cY4)>>12
	move	r20,r24
	imult	r11,r24			; dx*cY4
	move	r23,r25
	imult	r12,r25			; dz*sY4
	move	r20,r26
	imult	r12,r26			; dx*sY4
	move	r23,r27
	imult	r11,r27			; dz*cY4
	sub	r25,r24
	sharq	#12,r24			; rx
	add	r26,r27
	sharq	#12,r27			; rz
	; ry = (dy*cP4 - rz*sP4)>>12 ; rz2 = (dy*sP4 + rz*cP4)>>12
	move	r21,r25
	imult	r13,r25			; dy*cP4
	move	r27,r26
	imult	r14,r26			; rz*sP4
	move	r21,r28
	imult	r14,r28			; dy*sP4
	imult	r13,r27			; rz*cP4 (r27 = rz consumed)
	sub	r26,r25
	sharq	#12,r25			; ry
	add	r28,r27
	sharq	#12,r27			; rz2
	; NEAR-SNAP REVERTED (mirrors gpu_geotex; task 19 postponed)
	movei	#RX_NEAR,r0
	cmp	r0,r27
	movei	#rx_behind,r22
	jump	MI,(r22)
	nop
	nop
	; sx = RX_CX + rx*RX_FOCAL/rz2  (unsigned div + sign dance)
	movei	#RX_FOCAL,r0
	imult	r0,r24			; rx*FOCAL (fits: |rx|<=~20000*190=3.8M)
	nop
	move	r24,r26			; sign keeper
	abs	r24
	div	r27,r24
	movei	#RX_FOCAL_Y,r0
	imult	r0,r25			; ry*FOCAL_Y (95 in LOWRES)
	nop
	move	r25,r28
	abs	r25
	cmpq	#0,r26
	jr	PL,rx_sxp
	nop
	nop
	neg	r24
rx_sxp:
	movei	#RX_CX,r0
	add	r0,r24			; sx
	div	r27,r25			; ry*FOCAL/rz2 (2nd divide)
	store	r24,(r18)		; cache sx (plain: posted writes drain FIFO)
	addq	#4,r18
	cmpq	#0,r28
	jr	PL,rx_syp
	nop
	nop
	neg	r25
rx_syp:
	movei	#RX_CY,r0
	add	r0,r25			; sy
	store	r27,(r19)		; per-vert depth
	addq	#4,r19
	store	r25,(r18)
	addq	#4,r18
	AUDIO_PUMP
	movei	#rx_vert,r22
	jump	T,(r22)
	nop
	nop
rx_behind:
	movei	#RX_SENT,r1
	store	r1,(r19)		; behind: depth = sentinel (face skipped anyway)
	addq	#4,r19
	store	r1,(r18)		; behind sentinel
	addq	#8,r18
	AUDIO_PUMP
	movei	#rx_vert,r22
	jump	T,(r22)
	nop
	nop
rx_vdone:
	; set this room's DONE flag (long at cacheptr - 4 = 2)
	; (r18 has advanced; the flag address came before the verts: reload)
	move	r5,r0			; r5 points past this room's 5 longs
	subq	#4,r0
	load	(r0),r1			; cache ptr (re-read from the list)
	nop
	nop
	subq	#4,r1
	; DRAIN the posted-write queue before raising the flag: one read of the
	; LAST-written depth address forces all prior stores to DRAM first
	subq	#4,r19
	load	(r19),r0
	nop
	nop
	move	r0,r0			; consume (order barrier)
	moveq	#2,r2
	store	r2,(r1)			; flag = 2 (done) — plain store, drains after data
	movei	#rx_room,r22
	jump	T,(r22)
	nop
	nop
rx_alldone:
	movei	#MAGIC_DONE,r0
	movei	#D_PARAMS,r22
	load	(r22),r22
	nop
	nop
	store	r0,(r22)
	movei	#main_loop,r22
	jump	T,(r22)
	nop
	nop

do_pose:
	; load param registers
	moveq	#0,r8			; global vert index (skv extracted by long)
	movei	#D_PARAMS,r9
	movei	#$40,r0
	add	r0,r9			; angles: LOCAL (68k copies 3*mcount bytes)
	movei	#SINTAB_D,r7		; sine table: LOCAL SRAM
	; out: params[6] = DRAM blob DIRECT (was OUT_D SRAM + a 600-long 68k
	; copy-back = the 35% hl_poseread bucket). Posted DRAM writes -> the
	; drain-read before MAGIC_DONE below is what makes this safe.
	movei	#24,r0
	add	r4,r0
	load	(r0),r10
	movei	#40,r0
	add	r4,r0
	load	(r0),r11		; laC (.14)
	movei	#44,r0
	add	r4,r0
	load	(r0),r12		; laS
	; rx0/rz0 NOT register-resident (r16/r17 ISR-owned) — reloaded from
	; the params block at each per-vert use via r22
	movei	#56,r0
	add	r4,r0
	load	(r0),r18		; base_y
	movei	#MSTACK,r19		; stack ptr
	movei	#MBLK,r14		; matrix base (indexed via r14)
	moveq	#0,r5			; mesh index i
	; DEBUG: mcount==255 -> isolated rotation-core test (90deg Y on identity)
	movei	#255,r0
	cmp	r0,r6
	movei	#rot_test,r22
	jump	EQ,(r22)
	nop
	nop

mesh_loop:
	cmpq	#0,r5
	movei	#not_first,r22
	jump	NE,(r22)
	nop
	nop
	; --- mesh 0: identity matrix, translate(root) ---
	movei	#4096,r0
	store	r0,(r14)		; R00
	moveq	#0,r1
	store	r1,(r14+1)
	store	r1,(r14+2)
	store	r1,(r14+3)
	store	r0,(r14+4)		; R11
	store	r1,(r14+5)
	store	r1,(r14+6)
	store	r1,(r14+7)
	store	r0,(r14+8)		; R22
	movei	#28,r0
	add	r4,r0
	load	(r0),r20		; rootx
	addq	#4,r0
	load	(r0),r21		; rooty
	addq	#4,r0
	load	(r0),r23		; rootz
	nop
	store	r20,(r14+9)		; t = root (identity: no rotate yet)
	store	r21,(r14+10)
	store	r23,(r14+11)
	movei	#do_rot,r22
	jump	T,(r22)
	nop
	nop
not_first:
	; --- node record: flags,x,y,z (s16 each) at sknode + (i-1)*8 ---
	movei	#SKNODE_D,r1		; nodes: s32-expanded LOCAL SRAM
	move	r5,r0
	subq	#1,r0
	shlq	#4,r0			; *16 (4 longs per node)
	add	r1,r0
	load	(r0),r2			; flags
	addq	#4,r0
	load	(r0),r20		; nx (already s32)
	addq	#4,r0
	load	(r0),r21		; ny
	addq	#4,r0
	load	(r0),r23		; nz
	; POP?
	btst	#0,r2
	movei	#no_pop,r22
	jump	EQ,(r22)
	nop
	nop
	; m = *--stack  (copy 12 longs stack->M)
	movei	#48,r0
	sub	r0,r19
	move	r19,r1
	moveq	#12,r3
pop_cp:
	load	(r1),r0
	addq	#4,r1
	subq	#1,r3
	nop
	store	r0,(r14)
	addq	#4,r14
	cmpq	#0,r3
	jr	NE,pop_cp
	nop
	movei	#MBLK,r14
no_pop:
	; PUSH?
	btst	#1,r2
	movei	#no_push,r22
	jump	EQ,(r22)
	nop
	nop
	move	r19,r1
	moveq	#12,r3
push_cp:
	load	(r14),r0
	addq	#4,r14
	subq	#1,r3
	nop
	store	r0,(r1)
	addq	#4,r1
	cmpq	#0,r3
	jr	NE,push_cp
	nop
	movei	#MBLK,r14
	movei	#48,r0
	add	r0,r19
no_push:
	; translate(nx,ny,nz): t[j] += (Rj0*x + Rj1*y + Rj2*z)>>12
	moveq	#0,r3			; j = 0 (row offset in longs)
	moveq	#0,r28			; row counter (WAS uninitialized -> t[]
					;  writes landed wrong: badbytes bug)
trans_row:
	move	r3,r0
	shlq	#2,r0
	add	r14,r0			; &R[j*3] as byte addr (j*3 longs = j*12B; r3 counts row*3)
	load	(r0),r24		; Rj0
	addq	#4,r0
	load	(r0),r25		; Rj1
	addq	#4,r0
	load	(r0),r26		; Rj2
	nop
	imult	r20,r24			; Rj0*x
	imult	r21,r25			; Rj1*y
	imult	r23,r26			; Rj2*z
	nop
	add	r25,r24
	add	r26,r24
	sharq	#12,r24
	; t index = 9 + j/3
	move	r3,r0
	shrq	#1,r0			; j/3? r3 steps by 3 -> row = r3/3... see below
	; r3 counts 0,3,6 -> row = r3/3: use r27 row counter instead
	nop
	nop
	move	r28,r0			; r28 = row (0,1,2) maintained below
	addq	#9,r0
	shlq	#2,r0
	add	r14,r0
	load	(r0),r1			; t[row]
	nop
	nop
	add	r24,r1
	store	r1,(r0)
	addq	#3,r3
	addq	#1,r28
	moveq	#9,r0
	cmp	r0,r3
	movei	#trans_row,r22
	jump	MI,(r22)
	nop
	nop
	moveq	#0,r28

do_rot:
	; --- rot_yxz from angles[3] at r9 (u8 each; 0 = skip) ---
	; ANGLES MUST SURVIVE do_rot2 (it clobbers r0-r3,r20,r21,r23,r26,r27):
	; keeping ax/ay/az in r2/r3/r20 made the X and Z rotations use GARBAGE
	; angles after the Y call (the badbytes bug: valid matrix, wrong pose;
	; zero angles skipped the calls, so the zero-angle test passed).
	; r13/r15/r28 are untouched by do_rot2 (r13 reloaded in verts, r28
	; reset by trans_row next mesh).
	load	(r9),r13		; ax (one long per angle byte)
	addq	#4,r9
	load	(r9),r15		; ay
	addq	#4,r9
	load	(r9),r28		; az
	addq	#4,r9
	; Y: rotate cols (0,2) by ay
	move	r15,r0
	cmpq	#0,r0
	movei	#rot_x,r22
	jump	EQ,(r22)
	nop
	nop
	moveq	#0,r24			; ia=0
	moveq	#2,r25			; ib=2
	move	r15,r26			; angle
	movei	#rot_ret1,r29
	movei	#do_rot2,r22
	jump	T,(r22)
	nop
	nop
rot_ret1:
rot_x:
	move	r13,r0
	cmpq	#0,r0
	movei	#rot_z,r22
	jump	EQ,(r22)
	nop
	nop
	moveq	#2,r24			; ia=2
	moveq	#1,r25			; ib=1
	move	r13,r26
	movei	#rot_ret2,r29
	movei	#do_rot2,r22
	jump	T,(r22)
	nop
	nop
rot_ret2:
rot_z:
	move	r28,r0
	cmpq	#0,r0
	movei	#verts,r22
	jump	EQ,(r22)
	nop
	nop
	moveq	#1,r24			; ia=1
	moveq	#0,r25			; ib=0
	move	r28,r26
	movei	#rot_ret3,r29
	movei	#do_rot2,r22
	jump	T,(r22)
	nop
	nop
rot_ret3:
	movei	#verts,r22
	jump	T,(r22)
	nop
	nop

; do_rot2: rotate matrix cols (r24=ia, r25=ib) by angle idx r26. ret via r29.
; c = SINTAB[(a+64)&255]>>4 ; s = SINTAB[a&255]>>4  (.12)
do_rot2:
	move	r26,r0
	addq	#32,r0
	addq	#32,r0			; +64
	movei	#255,r1
	and	r1,r0
	shlq	#2,r0
	add	r7,r0
	load	(r0),r27		; cos 16.16
	move	r26,r0
	movei	#255,r1
	and	r1,r0
	shlq	#2,r0
	add	r7,r0
	load	(r0),r26		; sin 16.16
	sharq	#4,r27			; c .12
	sharq	#4,r26			; s .12
	moveq	#0,r3			; row = 0
rot_row:
	; a = M[row*3+ia], b = M[row*3+ib]
	move	r3,r0
	shlq	#1,r0
	add	r3,r0			; row*3
	add	r24,r0
	shlq	#2,r0
	add	r14,r0			; &a
	load	(r0),r20		; a
	move	r3,r1
	shlq	#1,r1
	add	r3,r1
	add	r25,r1
	shlq	#2,r1
	add	r14,r1			; &b
	load	(r1),r21		; b
	move	r20,r2
	imult	r27,r2			; a*c
	move	r21,r23
	imult	r26,r23			; b*s
	nop
	sub	r23,r2
	sharq	#12,r2			; (a*c - b*s)>>12
	store	r2,(r0)
	move	r21,r2
	imult	r27,r2			; b*c
	move	r20,r23
	imult	r26,r23			; a*s
	nop
	add	r23,r2
	sharq	#12,r2
	store	r2,(r1)
	addq	#1,r3
	cmpq	#3,r3
	movei	#rot_row,r22
	jump	MI,(r22)
	nop
	nop
	jump	T,(r29)
	nop
	nop

verts:
	; export this mesh's world translation T (r14+9..11) for the 68k
	; painter sort: params[5] = DRAM int32[3] array (0 = disabled; the
	; SINTAB slot — the kernel only ever uses its LOCAL sine copy).
	; Kills the 68k's whole vert-summing depth loop (hl_mdepth ~25%).
	movei	#20,r0
	add	r4,r0
	load	(r0),r1			; mtout base
	cmpq	#0,r1
	movei	#no_mt,r22
	jump	EQ,(r22)
	nop
	nop
	move	r5,r0			; idx*12
	shlq	#3,r0
	move	r5,r2
	shlq	#2,r2
	add	r2,r0
	add	r0,r1
	load	(r14+9),r0
	load	(r14+10),r2
	load	(r14+11),r3
	store	r0,(r1)
	addq	#4,r1
	store	r2,(r1)
	addq	#4,r1
	store	r3,(r1)
no_mt:
	; --- per-vert: vlen = skvl[i] (u16) ---
	movei	#8,r0
	add	r4,r0
	load	(r0),r1			; skvl base (DRAM, u16-packed)
	move	r5,r0
	shlq	#1,r0
	add	r1,r0			; byte addr of skvl[i]
	move	r0,r2
	movei	#$FFFFFFFC,r3
	and	r3,r2
	load	(r2),r13		; containing LONG (DRAM 32-bit ok)
	btst	#1,r0
	movei	#vl_lo,r22
	jump	NE,(r22)		; odd u16 -> low half
	nop
	nop
	shrq	#16,r13
vl_lo:
	movei	#$FFFF,r2
	and	r2,r13			; vlen
	nop
	nop
	cmpq	#0,r13
	movei	#mesh_next,r22
	jump	EQ,(r22)
	nop
	nop
vert_loop:
	; component c of vert r8: byteoff = r8*6 + c*2, from SKV_D longs
	move	r8,r0
	shlq	#1,r0
	add	r8,r0
	shlq	#1,r0			; r0 = r8*6
	movei	#SKV_D,r1
	add	r1,r0			; byte addr of lx
	; lx
	move	r0,r2
	movei	#$FFFFFFFC,r3
	and	r3,r2
	load	(r2),r20
	btst	#1,r0
	movei	#lx_hi,r22
	jump	EQ,(r22)
	nop
	nop
	shlq	#16,r20			; low half -> hi for sign-extend
lx_hi:
	sharq	#16,r20			; lx (sign-extended)
	; ly at +2
	addq	#2,r0
	move	r0,r2
	movei	#$FFFFFFFC,r3
	and	r3,r2
	load	(r2),r21
	btst	#1,r0
	movei	#ly_hi,r22
	jump	EQ,(r22)
	nop
	nop
	shlq	#16,r21
ly_hi:
	sharq	#16,r21			; ly
	; lz at +4
	addq	#2,r0
	move	r0,r2
	movei	#$FFFFFFFC,r3
	and	r3,r2
	load	(r2),r23
	btst	#1,r0
	movei	#lz_hi,r22
	jump	EQ,(r22)
	nop
	nop
	shlq	#16,r23
lz_hi:
	sharq	#16,r23			; lz
	addq	#1,r8			; next vert index
	; mx = t0 + (R00*lx + R01*ly + R02*lz)>>12   (rows via r14 indexed)
	load	(r14),r0
	load	(r14+1),r1
	load	(r14+2),r2
	nop
	imult	r20,r0
	imult	r21,r1
	imult	r23,r2
	nop
	add	r1,r0
	add	r2,r0
	sharq	#12,r0
	load	(r14+9),r1
	nop
	nop
	add	r1,r0
	move	r0,r24			; mx
	; my
	load	(r14+3),r0
	load	(r14+4),r1
	load	(r14+5),r2
	nop
	imult	r20,r0
	imult	r21,r1
	imult	r23,r2
	nop
	add	r1,r0
	add	r2,r0
	sharq	#12,r0
	load	(r14+10),r1
	nop
	nop
	add	r1,r0
	move	r0,r25			; my
	; mz
	load	(r14+6),r0
	load	(r14+7),r1
	load	(r14+8),r2
	nop
	imult	r20,r0
	imult	r21,r1
	imult	r23,r2
	nop
	add	r1,r0
	add	r2,r0
	sharq	#12,r0
	load	(r14+11),r1
	nop
	nop
	add	r1,r0
	move	r0,r26			; mz
	; rx = ((mx*laC + mz*laS)>>14) + rx0 ; rz = ((-mx*laS + mz*laC)>>14) + rz0
	move	r24,r0
	imult	r11,r0
	move	r26,r1
	imult	r12,r1
	nop
	add	r1,r0
	sharq	#14,r0
	movei	#48,r22
	add	r4,r22
	load	(r22),r22		; rx0 (SRAM params)
	nop
	nop
	add	r22,r0			; rx
	move	r24,r1
	neg	r1
	imult	r12,r1
	move	r26,r2
	imult	r11,r2
	nop
	add	r2,r1
	sharq	#14,r1
	movei	#52,r22
	add	r4,r22
	load	(r22),r22		; rz0 (SRAM params)
	nop
	nop
	add	r22,r1			; rz
	; wy = base_y + my
	move	r25,r2
	add	r18,r2
	; pack: long0 = (rx<<16)|(wy&FFFF) ; long1 = (rz<<16)|255
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
	AUDIO_PUMP
	subq	#1,r13
	movei	#vert_loop,r22
	jump	NE,(r22)
	nop
	nop
mesh_next:
	addq	#1,r5
	cmp	r6,r5			; i - mcount
	movei	#mesh_loop,r22
	jump	MI,(r22)
	nop
	nop

	; DRAIN the posted-write queue before raising done: one read of the
	; LAST-written vert long forces all prior DRAM stores to complete
	; (same protocol as rx_vdone; without it the 68k can see the done
	; flag before the verts have landed).
	subq	#4,r10
	load	(r10),r0
	nop
	nop
	move	r0,r0			; consume (order barrier)
	movei	#MAGIC_DONE,r0
	movei	#D_PARAMS,r22
	load	(r22),r22
	nop
	nop
	store	r0,(r22)
	movei	#main_loop,r22
	jump	T,(r22)
	nop
	nop

rot_test:
	; identity matrix into MBLK
	movei	#4096,r0
	moveq	#0,r1
	store	r0,(r14)
	store	r1,(r14+1)
	store	r1,(r14+2)
	store	r1,(r14+3)
	store	r0,(r14+4)
	store	r1,(r14+5)
	store	r1,(r14+6)
	store	r1,(r14+7)
	store	r0,(r14+8)
	store	r1,(r14+9)
	store	r1,(r14+10)
	store	r1,(r14+11)
	; rotate cols (0,2) by angle 64 (90deg): expect R00=0 R02=4096 R20=-4096 R22=0
	moveq	#0,r24
	moveq	#2,r25
	movei	#64,r26
	movei	#rt_done,r29
	movei	#do_rot2,r22
	jump	T,(r22)
	nop
	nop
rt_done:
	movei	#MAGIC_DONE,r0
	movei	#D_PARAMS,r22
	load	(r22),r22
	nop
	nop
	store	r0,(r22)
	; ---- I2S ISR (canonical idiom) ----
	; The DSP does NOT bank-switch on interrupt: r27-r30 are SAVED to the
	; stack frame and fully RESTORED (last one in the return delay slot),
	; so pose/roomx keep every register. r31 = ISR stack (hardware).
i2s_isr:
	; FINAL FORM: tick + WAKE + ack. Mixing and DAC writes happen in
	; MAIN-LOOP CONTEXT (the AUDIO_PUMP macro) — ISR-context DAC stores
	; and DRAM loads both crash Jerry (HW-proven, 2026-07-12), which is
	; why the reference engine's ISR is also ack-only.
	movei	#ISR_COUNT,r30
	load	(r30),r17
	nop
	nop
	addq	#1,r17
	store	r17,(r30)
	movei	#WAKE_D,r30
	moveq	#1,r17
	store	r17,(r30)
	; ack + return: EXACT clone of the reference tail (dsp_audio.gas
	; dsp_int) — the only instruction stream proven to clear IMASK.
	; r16 = &D_FLAGS, preloaded in init (their r28 idiom). BigPEmu x-ray
	; 2026-07-12: the previous tail COMPUTED the right ack (r17=$420) but
	; the store never took effect; this ordering does.
	load	(r16),r17
	nop				; POLLED MODE: IMASK stays SET on
	load	(r31),r30		; return — a stray dispatch self-seals
	bset	#10,r17			; ...I2S latch clear...
	addq	#4,r31
	addq	#2,r30
	bset	#3,r17			; force IMASK held
	jump	T,(r30)
	store	r17,(r16)		; ...lands as we leave
