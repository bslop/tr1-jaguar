# PACING CAMPAIGN — collect/flip robustness

## STEP 3 — SILICON VERDICT (2026-07-22 evening): fps100 WAS LYING; TOM IS THE WALL

THE ORACLE: fps-blocks are 60 renders each, so BLOCK CADENCE IN WALL TIME
(timestamped `jcp -c | awk strftime`) is an uncheatable fps meter.
Measured: blocks every 11-12s at spawn while fps100 printed 2500.

**fps100 INFLATES ~4.7x: its pftt window opens at pfA — AFTER game logic —
so the whole logic phase is invisible to it. TRUE fps: spawn idle 5.3,
route ~4-5. THE ENTIRE CAMPAIGN FPS LEDGER (21.6 idle / 19.6 play / 26.7
record) IS fps100-INFLATED FICTION.** pp_per (loop-top deltas) was honest
all along: 5912 hl = 11.5 fields ✓ cross-checked against wall clock; NO 2x
timestamp inflation in PPNOW at these scales. fpsT (loop-top window)
added; trust it, never fps100.

PACEPROBE rev2 A/B (DIAG_XBRSP_PP2 vs _PPM_ + SYNCPOLL variant, silicon):
- pp_coll ~= renders (collect fires every frame; denominator settled).
- **pp_span ~5800-6300 hl (~110-120ms) from kick to mailbox-done IN EVERY
  BUILD — Tom's render span is the frame floor** (matches the old 91.5%
  GPUbusy decomposition). Non-diet hides it under 3800 hl of concurrent
  68k logic (wait ~950 hl); diet shortens logic to ~2200 and just exposes
  ~3800 hl of naked waiting.
- Diet+SYNCPOLL (busy-poll wait): 13-14s blocks — NO recovery. STOP-poison
  hypothesis DEAD; the wait mode is irrelevant. (SYNCPOLL slightly worse:
  its per-iteration g_syncspins++ DRAM RMW is real bus noise.)
- Worst-case singles on route: pp_wmax ~7000 hl (13 fields), pp_smax
  ~19000 hl (0.6s) — the felt stalls.

VERDICTS:
1. **M68DIET: SHELVED, exonerated.** It does exactly what it claims
   (logic 3800->~2200 hl) against a bottleneck that isn't logic. Re-enter
   when Tom's span drops under ~3800 hl — then it lifts the fps cap from
   ~7 to ~19.
2. The old "cadence lock / phase dependency" narrative was chasing
   fps100 artifacts + the (real, now fixed) lost-wakeup races.
3. **NEXT CAMPAIGN: TOM'S 110ms.** Kernel-side, 320x240 mandate intact:
   phrase-mode shade pass (banked -17%), blit-transfer 46% of GPU busy,
   jump_refill ~18% (JRISC scheduling). Honest baseline to beat: 5.3 fps
   spawn (fpsT).

Builds staged: DIAG_XBRSP_PP2 / DIAG_XBRSPM_PP2 / DIAG_XBRSPM_PP2_SP;
fpsT in all future PROFILE builds.

---

## STEP 1 — ROOT CAUSE FOUND + FIX BUILT (2026-07-22, code review; silicon CONFIRMED for the races — see step 3 for the bigger picture)

TWO textbook lost-wakeup races, both in the STOP-wake path — together they
ARE the phase mechanism the brief hypothesized (no emu ever sees them
because jagemu delivers the wake exactly; only silicon's real interrupt
latching hits the windows):

RACE 1 (the cadence lock) — gpu.c gpu_sync was open-coded
check-then-cpu_stop_sleep(): a CPUINT raised BETWEEN the mailbox read and
the STOP is taken+acked BEFORE the STOP executes, so the STOP then sleeps
into the NEXT VBL with MAGIC_DONE already set (up to a full field lost).
SELF-LOCKING: a VBL-quantized wake makes the next kick VBL-aligned; a
near-constant render time lands the finish in the same window every frame.
Explains the whole M68DIET autopsy: maxvbl pinned, frame time insensitive
to a +5% kernel, and PHT_pad's ±7 fps jitter (the busy-pad randomizes the
arrival phase, escaping the window some frames). Same race existed in
video_flip's pending_fb wait (self-healing there — its waker is periodic —
but worth one field).

RACE 2 (accomplice) — startup.S vblank_stub acked with a blanket
#$0303 INT1 write, clearing a GPU-done that latched DURING the ISR:
eaten unseen, next STOP sleeps to the following VBL.

FIX (surgical, no new machinery):
- cpu68k.S: new cpu_stop_unless(addr, val) — masks IPL7, checks, then
  STOP #$2000 ATOMICALLY unmask+sleeps. A latched-but-masked Tom interrupt
  survives the mask and wakes the STOP the instant it drops — window closed.
- gpu_sync + video_flip converted to it (SYNCPOLL variant untouched).
- vblank_stub: acks ONLY the sources observed at entry (pending<<8);
  a latch set during the ISR re-fires right after RTE.

VERIFY LADDER PASSED (emu):
- Reproduction gate: pristine tree + bins_statics + documented flags
  rebuilds PLAY_XBRS.cof BYTE-IDENTICAL (910c765c) — config validated.
- PLAY_XBRSP (fix): 650f silicon-fidelity spawn PIXEL-IDENTICAL to base;
  2100f driven soak (run forward from f700) — 0 illegal, all cores live,
  mid-run frame clean. 68k instret +0.03% (mask overhead, noise).
- PLAY_XBRSPM (fix + M68DIET rehab attempt): 650f clean render (pixel
  diff vs base = anim phase from faster 68k, expected), 0 illegal.

STAGED (probes/): PLAY_XBRSP c4f42fe3 | DIAG_XBRSP 7988a4b6 (telemetry
twin: +NOGD PROFILE NOPROFGPU QUIETFPS) | PLAY_XBRSPM 4ba8e7fb (+M68DIET)
| DIAG_XBRSPM 85793280. Live tree bins = bins_statics again (the stray
11:42 mystery bins stashed to session scratch bins_live_1142).

SILICON PROTOCOL (pending rig): 1) DIAG_XBRSP telemetry run — judge
block-to-block variance (±0.5 target) + throughput vs PLAY_XBRS baseline;
2) if pacing holds, PLAY_XBRSPM — the -57% 68k diet should now convert to
real fps instead of the lock; 3) user play-feel gate. THEN: LEMITDIET,
governor churn recheck (may share the mechanism).

---

## ORIGINAL BRIEF (2026-07-22)

THE DOOR: M68DIET's autopsy proved the PIPELINE collect/flip machinery
has a hidden PHASE dependency on 68k timing. Evidence (all silicon,
2026-07-22): diet build = cadence LOCK (7.6-7.7 fps, maxvbl pinned 9,
hl_clear x7, spind x3.8, frame time INSENSITIVE to a +5% faster
kernel); diet+busy-pad = throughput restored (19-27, record 26.7) but
pacing ERRATIC (+-7 fps block swings vs +-0.5 healthy; user perceives
"really slow"). Neither jagemu (counters identical to 6 digits) nor
any GPU/DSP/blitter metric sees the mechanism — it lives in the
68k<->VBL<->collect<->flip interaction on silicon.
BEHIND THIS DOOR: M68DIET's real -57% 68k logic, LEMITDIET (~3%,
built+verified, staged), and every future 68k diet.
HYPOTHESIS SPACE (discriminators staged in probes/): the collect's
gpu_sync STOP-sleep wakes on VBL — a fast 68k reaches the collect at a
different VBL phase; if Tom finishes mid-field, the collect sleeps to
the NEXT VBL (quantization), and the FLIP then lands at a phase that
delays presentation/rotation; certain 68k durations resonate into
worst-case alignment every frame (the lock), others jitter (the pad).
CANDIDATE FIXES (design, then emu+silicon):
1. Phase-free collect: replace the VBL-wake sleep in the collect with a
   short busy-poll ONLY when the previous frame's sync waited < 1 field
   (adaptive: poll when Tom is nearly done, sleep when long) — bounds
   the quantization without the SYNCPOLL bus-noise cost (poll window is
   tiny).
2. Flip decoupling: flip immediately at collect regardless of VBL phase
   (triple buffering already tolerates this — audit pending_fb).
3. GPU-done mailbox interrupt (Tom pokes an interrupt on MAGIC_DONE) —
   the "right" fix; needs an ISR hook; biggest change.
SILICON PROTOCOL: PHT_pad (erratic baseline) + candidate builds; judge
on BLOCK-TO-BLOCK VARIANCE (+-0.5 target) AND throughput AND user feel.
The discriminator kit (PHT_pad/a1/a2/a3/syncpoll) is in probes/.
DEPENDS ON: nothing. UNBLOCKS: M68DIET rehab, LEMITDIET, governor
(its churn defect may share this mechanism — check after).

---

## STEP 4 — TOM COMPUTE CAMPAIGN opened (2026-07-22 late)

Honest play baseline (fpsT, wall-verified): spawn 5.0-5.5, route 4.3-5.1,
dips 2.8. Tom = 2.6M cycles/render at 0.5 IPC, 1.3M instret ~= 8K
scanline-spans x ~170 instr.

- PHRASESHADE (pixel->phrase DSTEN shade blit): built, works, pixel-diff
  2161px (edge-bleed class). Emu +61%; **silicon 0%** — jagemu's known
  blitter-concurrency overcharge, now repro'd end-to-end (COBWEB_BUG
  round 4 + shared trap 14). Flag kept, DEFAULT OFF. Tom is COMPUTE-bound.
- FACEDU (cache per-face du/dv, kill the 2 divides/scanline): **DEAD ON
  ARRIVAL** — kernel comment at sp_have records the identical idea
  REVERTED: projection is perspective, du/dv genuinely varies per row,
  cached gradient = sheared textures. The divides are load-bearing.
  (Possible future: 68k-side per-face affine-safe flag — 68k time is free
  under Tom per step 3 — gating a cache path for low-perspective faces.)
- **DIVHIDE (next, designed): hide both div latencies via resequencing.**
  Zero new bytes, ~11% of Tom target (stall_div 59K cyc/render + serial
  dv tail). Key moves: div dest OFF r0 (dividend+dest in r28 for du, r27
  for dv — both dead post-numerator on the off==0 fast path) frees r0 for
  A1 packing; order = issue du -> clamps+off+A1_PIXEL/FPIXEL builds ->
  issue dv -> bwait (spin on r5, NOT r0 — r0 WAW vs div is the classic
  trap; r5 free with TRAPEZOID=0) -> stores -> sign-fix du from r28 ->
  INC/FINC packs -> sign-fix dv from r27 -> A2/B_COUNT/launch. Gate
  under -d DIVHIDE=1; flags-off byte-identity mandatory; emu pixel-diff
  gate (compute timing IS silicon-calibrated), then fpsT block cadence.

### Step 4 results round 2 (2026-07-22 night)

- **DIVHIDE: built, correct, NULL (82 vs 82 syncs/600vbl).** Post-mortem:
  stall_div is only ~59K cyc/render (2%) — resequencing moves instructions
  but executes the same 1.3M; only true stalls were recoverable. Lesson:
  size the prize from the STALL COUNTERS, not from instruction counts.
  Flag kept (default off). Cost of the exercise, banked as knowledge:
  **TRM bug-13 WAW is the div-hide killer** — imul32's r27/r28 scratch
  writes raced the ~32-tick divide and the quotient re-landed on top
  (8838px of left-band garbage); fixed by an inline 16x16 mult pair in
  rd_off1 (write-free wrt r27/r28, and -10B). 29px residual = garbage-vs-
  garbage on denom==0 single-column spans (baseline equally arbitrary).
- **Honest stall ledger per render (emu, silicon-calibrated compute):
  jump_refill 463K (18%) >> stall_alu 141K > stall_flags 111K >
  mem_ext 215K > stall_div 59K.** Instruction count (1.3M x 2.0 cyc) is
  the wall; the two biggest levers are FEWER instructions and FEWER
  taken jumps.
- **NEXT: BANKDIET — the GPU's ALTERNATE REGISTER BANK is 100% unused**
  (regs1 all zero every dump; kernel has zero moveta/movefa; sim
  implements both). 32 free registers reachable in ~1 cycle. Plan:
  park hot constants ($FFFF0000/$FFFF masks, BLIT_GUARD, jump targets)
  + hot SRAM state (RUNC, uv-DDA AU..BVSL 8 slots, GDU/RSAVE stashes)
  in bank 1; hot loop uses movefa/moveta (2B/1cyc) instead of movei
  (6B/2cyc) and SRAM load/store pairs (2B/2cyc+latency each way + bus).
  Each movei->movefa ALSO frees 4 bytes — self-funding for the
  jump_refill branch-straightening that follows. Init: one-time moveta
  block at kernel entry; uv_edge/chain writes convert on cold paths.
  Verify: TRM rules for moveta/movefa latency (check Lat in timing.rs),
  no GPU-side interrupts use bank 1 in this kernel (none exist).

### Step 4 round 3 — BANKDIET landed, campaign arithmetic (2026-07-22 close)

BANKDIET=1 (alternate-bank residents) steps 1+2 SHIPPED: masks
$FFFF0000/$FFFF, BLIT_GUARD, sp_run jump target, RUNC countdown all moved
to bank-1 (alt r1-r5); init block at kernel entry; hot row path uses
movefa/moveta (2B/1cyc) for what were moveis (6B/2cyc) + an SRAM RMW.
Gates: PIXEL-IDENTICAL, flags-off byte-identical (c4f42fe3), kernel
3664->3656 (24B freed net of init). Measured: GPU IPC +1.7% (instret
+1.66% at equal cycles, frame 650); sync-count flat at 82 (metric
quantizes ~1.2%/step). Real but small — as the arithmetic predicts:
~10-15 cycles shaved of ~340/row.

**CAMPAIGN ARITHMETIC (the honest wall):** 170 instr x 2.0 cyc per
1-scanline span x 8K spans = Tom's 110ms. The GPU spends ~340 cycles
programming a blit that draws a ~20px row in ~40. Shaving (BANKDIET
expansion: uv-DDA 16 instr/row -> bank regs; branch straightening for the
18% jump_refill) realistically buys -20-30% total -> ~6.5-7 fps true.
**Saturn-class 15 fps needs 3x = architectural change, two candidates:**
1. ROW-BATCH REVISITED: v4b rect-shade + tz A1_STEP row-stepping prove
   multi-row hardware blits work; TRAPEZOID died on its per-row gate
   cost, not the concept. A cheap batch criterion (e.g. run-length from
   ROWDIET's existing chain-event runs + accepting <=1px edge error on
   interior rows, or integer-slope-only batching) amortizes the 340-cycle
   setup over k rows. Kill criterion first: RUNHIST the run-length
   distribution (probe exists) — if median run >= 4, a 2-3x is live.
2. JERRY CO-DERIVATION: Jerry has ~12% idle + his DRAM traffic measured
   NULL vs Tom (calib lddramj). Precompute per-row du/dv (+ maybe uL/vL)
   tables for frame N's faces on Jerry while Tom rasters N-1 (pipeline
   exists); Tom's row loop shrinks to table-reads + blit programming.
   JERRYX's old null was co-TRANSFORM (Tom never waited); this feeds the
   actual hot loop.
Both are fresh-session campaigns; specs here, kill criteria first.

### RUNBATCH kill-check MEASURED (2026-07-22, RUNHIST v1+v2 in emu)

- Chain-run histogram (v1): median 4, **89% of rows in runs >= 4** — the
  geometry comes in slabs.
- Stable-floor histogram (v2, the actual batchable criterion — floor(ax)
  AND floor(bx) still across a step): **median streak 0 (79% of steps move
  an edge — fractional slopes; TRAPEZOID's true killer), BUT 46% of rows
  sit in batches k>=4** (axis-aligned geometry tail; 31+ bucket healthy).
- VERDICT: row-batching is worth **-25-30% of Tom**, not 3x alone. Design
  that survives the k=1 dominance: compute k ONCE per chain-run
  (k_a = rows till floor(ax) moves = f(frac,slope) via one divide; k =
  min(ka,kb,run_left)), re-derive per batch — ZERO added cost on k=1 rows
  (fall into the existing span path), ~20 cyc per batch start. Reuses the
  tz multi-row launch machinery (A1_STEP/FSTEP row-stepping, v4b A2_STEP).
  Drift gate once per run (slopes constant).
- Probe kit: RUNHIST=1 build (SHADEPASS=0 measurement config, kernel 3608)
  = v1 chain-runs at RUNHIST_BUF, v2 stable-floor at +$100, streak cell
  +$200.
- **The stacked road to ~10-12 true fps:** RUNBATCH (-25-30%) + BANKDIET
  expansion (-10%) + jump_refill straightening (-8%) + Jerry co-derivation
  (-15-20%) -> Tom ~50ms; then M68DIET re-enters (logic 72ms floor) for
  the final step. 3x is a STACK, not a single door.

### RUNBATCH round 1 (2026-07-22 late-night): built, two nulls + one law

Infrastructure SHIPPED (flag RUNBATCH=1, requires BANKDIET, excl TRAPEZOID/
DIVHIDE/NOFILL): alt-r6 BATCHC skip machinery, alt-r7 gate/klim, alt-r8
LEFT-chain tag (pick-time moveta — deriving from r2 at launch reads pack
garbage: wrong chain, slabs), k-row launch via tz A1_STEP/FSTEP + v4b
A2_STEP, alt-r9 [MEAS] batched-rows counter (readable in emu regs1 dump!).
Measurement config: SHADEPASS=0 XCULL=0 (twins share it; play-fit later).

MEASURED (emu, spawn 650f, baseline 104 syncs/600vbl):
- slope==0 gate: CORRECT (30px = drift class) but <1% of rows batch
  (1743) -> 99 syncs (-5%, gate overhead) -> after tag/slot diet: 90=90
  corridor. Verticals-only is too narrow.
- general klim = $10000/max(4 rates), k<=31: fires on EVERYTHING (235K
  rows) but **VISUALLY BROKEN (54K px slabs), still broken at k<=4 —
  STRUCTURAL**:
- **THE LAW: stale-xl batching is UNSOUND for textured spans. A +-1px
  x-error costs +-du TEXELS of texture; du = 8-16 texels/px on minified
  faces -> unbounded wrong-atlas slabs. The drift gate bounds edge-u
  slopes, NOT du*(x-error). Only EXACT floor-stable batches are
  texture-safe — that is the real reason tz paid for its exact per-row
  lookahead.**

NEXT (fresh session): exact-k at the launch row — k_a=(($10000-frac(ax))/
aslope etc. with sign cases, k=min(ka,kb,RUNC,31), 2 divides per BATCH
(amortized; divider already integer in-run). ~+70B: measurement config
drops BEXIT too; play-config fit comes after validation. The slope-0
subset is already correct and could ship as-is (tiny win). Current tree:
RUNBATCH has the [BISECT] k<=4 cap + [MEAS] alt-r9 counter still in —
remove both when resuming. Flags-off byte-identity intact (all .if-gated).

### RUNBATCH round 2 — EXACT-K CORRECT, cost model mapped (2026-07-22 ~22:30)

EXACT-K SHIPPED AND CORRECT: rb_kcalc subroutine (per-edge rows-until-
floor-moves from live DDA fracs; ceil/floor sign cases; 2 int divides per
batch launch), k=min(ka,kb,RUNC,31), kill-flag on k<2 runs, left-chain
re-derived AT LAUNCH from r16/r17 floors (same compare+condition as the
pick; the per-row pick-arm tag was 17K cyc/render). imul32 target banked
(alt-r12, 23 sites, -92B — perm BANKDIET win). Measurement config kernel
3678/3680.

Gates: **visually indistinguishable, spawn diff 948px (texel drift class),
33.6K rows batched (13% at spawn)** — the batching MATH is now right.

Perf: STILL 92 vs 98 (-6%) through three cost-reduction rounds (kill-flag,
tag removal). Remaining per-row taxes identified:
- BATCHC skip-check at sp_run: ~5 cyc x 4300 rows = 19K cyc/render (~7%!)
- skipped rows still execute the FULL sp_tail (~60 cyc each): the batch
  only saves the SPAN SETUP (~270), not the row walk.

**ENDGAME DESIGN (next session, fresh context): DDA-JUMP true batching.**
At batch launch: advance ALL DDAs by k-1 steps arithmetically (ax/bx +=
(k-1)*slope; AU/AV/BU/BV += (k-1)*USL — imul32 x6 or pointer-walk loop,
EXACT), y += k-1, RUNC -= k-1, then fall to sp_tail ONCE. Deletes: BATCHC,
the sp_run skip-check (the 7% tax), and the per-skipped-row sp_tail
(~60cyc x k-1). Batch flat cost ~180-280 cyc vs (k-1)*65 walked — wins
from k=4, AND live rows get the 7% back everywhere. Bytes: +~70B funded
by deleting skip machinery (-30B) + banking SY/SX_BUF in chain_step
(-16B) + GDU/RSAVE (-16B). Alt-bank ledger: r1-r6 BANKDIET, r7 RUNBATCH
gate, r12 imul32; r8-r11,r13+ free.

### RUNBATCH round 3 — DDA-JUMP built; verdict passes to SILICON (2026-07-22 ~22:45)

DDA-JUMP SHIPPED: batch launch now advances ALL loop state k-1 rows
arithmetically (ax/bx += (k-1)*slope via banked imul32; the 4 uv DDA
pairs via an r14-walking multiply loop; y += k-1; RUNC -= k-1) and falls
into ONE normal sp_tail. BATCHC + the sp_run skip-check DELETED (the 7%
per-row tax is gone from live rows). Threshold k>=4 (flat jump cost ~200
cyc loses at k=2-3). Byte funding: GDU/RSAVE stashes -> bank moveta
(alt-r13/r14, -24B, also kills 4 SRAM round-trips/run) + SY/SX_BUF banked
(14 sites; watch the r1-inside-r14 string-replace trap — bit once).
Kernel 3662/3680. Gates: visually correct (934px texel class), runs clean.

EMU VERDICT — AND WHY IT IS INADMISSIBLE: spawn 90 vs 98, corridor-walk
15 vs 90 ("collapse"). But batching EXPOSES blitter time that single-row
rendering hides under GPU setup, and exposed-blit pricing is EXACTLY
jagemu's documented overcharge (COBWEB_BUG rounds 3-4: invented +61% for
PHRASESHADE the same way, sign flipped). The corridor collapse is
suspected mostly emu-artifact; possibly partly real (wide spans ARE
blit-heavier). **In-kernel silicon testing is the only valid blitter
probe — the ledger's own rule. SILICON A/B REQUIRED.**

STAGED: probes/DIAG_RB_BASE.cof (b9e42608) + DIAG_RB_BATCH.cof (71fb5441)
— measurement config (SHADEPASS=0: TEXTURES LOOK GARBLED, expected!) +
NOGD PROFILE QUIETFPS telemetry. Protocol: flash each, spawn idle ~60s +
one corridor walk, judge fpsT + wall-clock block cadence. If batch wins
on silicon: integrate into the play config (needs SHADEPASS bytes — next
diet) and re-run the drift-gate visual QA on CRT.

### RUNBATCH round 4 — SILICON SESSION (2026-07-22 ~23:00-23:45)

Three silicon-only bugs found and two fixed, live on the rig:
1. **Div early-read (0.14 fps crawl #1... actually the crawl was bug 3;
   this one was latent)**: rb_kcalc read quotients 1 instr after issue.
   Silicon divider has NO dest interlock (emu is value-correct on early
   reads — MODEL GAP, report to cobweb). Fixed: fully div-hidden inline
   kcalc (issue A -> prep B -> issue B -> k-independent setup -> consume),
   npix parked in r7 across the imuls.
2. **r22 clobber**: the min-chain's movei #rb_single,r22 poisoned the
   DDA-jump's imul32 pointer -> jumped to rb_single (the 0.14fps crawl:
   relaunch + no DDA-jump). Fixed: movefa r12,r22 reload.
3. **Load-across-taken-jump erratum (the black-screen wedge)**: the rj_uv
   loop loaded the slope and jumped to imul32 which consumes it first
   instr — garbage on silicon (emu fine). Fixed with the kernel's own
   or-settle idiom (3 sites incl LUSL/LVSL loads).

**SILICON RESULTS after fixes**: spawn idle fpsT 4.9-5.5 vs baseline
4.6-5.3 (**batch +4%-ish REAL — where the emu predicted -8%; the emu's
anti-batching blitter bias is now silicon-confirmed**). maxvbl 5 steady.
Walking: 3.8 fpsT (comparable to baseline) for ~90s, then **GPU-side
death, black screen, no 68k beacon, console silent — WALKING-ONLY,
SILICON-ONLY** (emu soaked 2100 walking frames clean). Suspect class:
another unsettled hazard on a path only moving-camera geometry hits
(portal-clipped rooms / rd_off1-adjacent batches / animated Lara faces).
NEXT: GPU breadcrumb probe — paint per-stage markers from the batch path
into the fb (beacon_paint style) so the black screen names its stage;
or bisect RBNOUV (survived spawn; walking untested) vs full.
Console restored to PLAY_XBRSP. Builds: DIAG_RB_BASE / DIAG_RB_BATCH
(settled) / DIAG_RB_NOUV staged in probes/.

### RUNBATCH round 5 close (2026-07-23 ~00:00): racy silicon death, strategy pivot

Wedge-autopsy build (gpu_sync timeout now prints wedgepc/wedgecmd/wedgectl
— KEEP, zero-cost diagnostic) died at LEVEL START with NO wedge print:
the 68k never saw a sync timeout -> it is stuck in an unbounded wait
downstream (video_flip pending? trampled vectors?) of a suspected
runaway-blit DRAM trample. Death timing varies wildly (level start /
90s of walking / spawn survives) -> RACY trigger, not geometric. Screen
pure black incl border = no 68k exception beacon.

**STRATEGY PIVOT (next session): fix the SIMULATOR first.** Two silicon
gaps are now proven and documented (COBWEB_BUG round 5): div-dest
early-read poison + load-consumed-across-taken-jump. Teach jsim's
Silicon fidelity to POISON those values (not value-correct them) and the
racy batch bug should reproduce in emulation where iteration is free and
state is inspectable — instead of 5-minute rig cycles at midnight.
cobweb tree is local (~/Documents/Git/cobweb, timing.rs Pend machinery
already re-lands values; poisoning is a small delta). Then fix the
kernel bug it exposes, silicon-confirm once.

Scoreboard for the night: batch +4% real at spawn (silicon), 3 silicon
bugs fixed, 2 sim model gaps proven, 1 racy bug outstanding. Console
restored to PLAY_XBRSP.

### Round 6 close (2026-07-23 ~00:35): poison tool built, calibration soup found

JAGEMU_POISON prototyped in the local cobweb tree (COBWEB_BUG round 6):
div-dest partial-quotient poisoning works, but exposed that jsim's div
ready_at runs ~5-11 cycles late vs silicon (calibrated projection divides
read "early" per the sim yet work on hardware) — the tool can't hunt the
racy RUNBATCH bug until a quotient-CORRECTNESS-vs-shadow silicon probe
(K=3..18) calibrates the true latency curve. THAT PROBE is the next
session's first build (calib/probes.s style, ~30 min of rig time).
RUNBATCH racy-death hunt paths, in order: (1) div-latency probe ->
recalibrate -> poison-hunt in emu; (2) RBNOUV walking bisect on rig;
(3) GPU breadcrumb stage markers. Console holds PLAY_XBRSP (good build).

---

## GOVERNOR IS MIS-TUNED BY ~4x — THE fps100 LIE POISONED ITS THRESHOLDS (2026-07-24)

Silicon measurement of the PLAY_XB baseline (14 fpsT blocks) gives the loop-top
frame period directly (fpsT = 6000*60/pftt2):

  12.0 12.5 13.2 13.7 13.8 14.6 14.7 14.8 15.1 15.1 15.4 16.7 17.4 17.6 fields
  min 12.0 / median 14.8 / max 17.6

GOVERNOR ships with `GOV_HI=6` (trip when a frame span EXCEEDS this) and
`GOV_LO=4` (calm when at/below). Against real frame periods:
- **EVERY frame exceeds GOV_HI=6** -> the governor trips on the first frame.
- **NO frame is ever <= GOV_LO=4** -> `g_gov_calm` can never reach GOV_K.
=> **`g_hopcap` is clamped to 1 permanently, with no path back.** Fewer rooms
drawn forever. That is why GOVERNOR was never validated on silicon.

Root cause: the thresholds were picked when fps100 (~20 fps, ~3 fields/frame)
was believed. TRUE fps is ~4 (fpsT), i.e. ~15 fields/frame — the SAME ~4.7x
error the pacing campaign already found in fps100, propagated into a control
loop. Anything else tuned against fps100 numbers should be re-checked.

Retuned for the true cadence and staged as `probes/GOV_retuned.cof`:
`GOVERNOR=1 GOV_HI=22 GOV_LO=16 GOV_K=20` (trip above normal-worst ~18; calm at
or below ~16 so recovery is reachable). GOVERNOR is C-only — kernel stays 3668,
byte-identical to the baseline.

Target being attacked: maxvbl render-span spikes of 13-15 fields (215-250ms)
against a typical 4-6, with spind (gpu_sync spins) spiking on the same blocks.
Prior PACEPROBE data attributes the worst single Tom kick->collect span at
~19000 hl (~0.6s) vs a typical 5800-6300 hl — i.e. **Tom doing ~3x the work on
spike frames**, consistent with hop-depth/room-count blowups that g_hopcap caps.
