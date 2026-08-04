# TRAPEZOID CAMPAIGN — walk-loop restructure (opened 2026-07-21, user: "Do it.")

GOAL: replace per-scanline blitter programming (~4800 setups+launches/frame,
the 45% walk slice) with ONE multi-row blit per straight-edge run, using the
Blitter's outer-loop iterators (A1_STEP/A1_FSTEP per-row texture DDA,
A2_STEP dest row step). Target: mid-scene 9.4 fps -> mid-teens.

## Current state (all silicon-verified 2026-07-21)
- Kernel: gpu_geotex.gas (jas assembler, 3656/3680 bytes with STAGEDIET).
- Frame: PIPELINE + STAGEDIET play build = 19-20 fps idle / 9.4 mid-scene /
  ~6 junctions. Tom is the critical path (68k fully hidden at idle).
- Walk loop: per scanline — edge DDA step, span clamp to CLIPX0/X1,
  program A2_PIXEL/A1_PIXEL/A1_FPIXEL(+INC)/B_COUNT, launch BCMD_TEX,
  software-pipelined bwait. RUNC ($F03F40) already counts scanline runs to
  the next "chain event" (edge change) — the run boundaries needed for
  trapezoid batching are HALF-COMPUTED already.
- SHADEPASS rect-shade v4b: per-face union-rect OR blit at pkt_done (must
  keep working or be consciously re-integrated).

## The hard design problem (solve BEFORE writing kernel bytes)
A multi-row blit has ONE B_COUNT inner width for all rows and INTEGER
A2_STEP: real trapezoids have fractionally-stepping left edges AND per-row
width change. Options to evaluate (emu-first):
1. Split runs where floor(xl) changes AND width changes: exact pixels,
   shorter runs — measure the average run length actually achievable from
   real scenes (instrument the current kernel's RUNC in jagemu FIRST; if
   avg run < ~3 rows the whole campaign is moot — MEASURE BEFORE BUILDING).
2. Constant-width union runs with <=1px edge slop (visual wobble risk —
   screenshot-diff to judge).
3. Hybrid: trapezoid path only for runs >= N rows, per-scanline fallback
   otherwise (keeps correctness trivially; the fallback already exists).
   RECOMMENDED SHAPE: opt-in `-d TRAPEZOID=1`, hybrid, fallback intact.
- A2_STEP is at (r15+13)=$F02234 (A2 file NOT parallel to A1 — the v4b
  register-map trap). UPDA1|UPDA2 under DSTA2 semantics: see
  COBWEB_REQ_rectshade_and_calibration.md + calib/p_topphrase_upda.s.
  Texture per-row DDA: A1_STEP=$F0221C?? NO — verify every A1/A2 register
  offset against the working shx_bw block and cobweb docs; do not trust
  folklore (two register-map bugs already killed one campaign day).

## Byte budget
Kernel has 24 bytes free. The trapezoid path needs ~100-200B. Sources:
another jopt pass after edits (jopt gpu_geotex.gas --gpu
--allow-input-hazards -d <ALL defines>), hand slot-fills, or making
TRAPEZOID and (temporarily) SHADEPASS mutually exclusive for the first
proof (get the fps datum, then reconcile). Ceiling raise = SRAM var moves =
minefield (see the SRAM map notes in the ramps memory).

## Hard-won rules (violate none)
- jas hazard errors are real bugs; `or rX,rX` consumes, `move` does not;
  `.align 4`; every -d must be passed explicitly; delay slots: single nop.
- Makefile does NOT track -d changes: make clean between flag configs.
- Blitter ownership: NOTHING may program blitter regs while a kernel is in
  flight (PIPELINE rules in main.c comments).
- Verify ladder (in order): (1) TRAPEZOID=0 build byte-compares or 0-px
  screenshot-diffs vs baseline; (2) TRAPEZOID=1 emu screenshot parity at
  spawn + room-12 spawn variant (mrt_spawn.h patch recipe in
  NEXT_WORK_SPEC.md) + driven-input gameplay; (3) jsim fps directional;
  (4) silicon via QUIETFPS fps100 console A/B (ab_run.sh pattern) — jsim
  UNDER-predicts wins (68k too fast, contention unmodeled): silicon is the
  judge. (5) User play-feel gate before it enters the play build.
- Emulator: ~/Documents/Git/cobweb/sim/target/release/jagemu
  (run/screenshot/serve/ctl/--pc-histogram/--watch). Build:
  make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 AUTOSTART=1
  NOGDONLY=1 STAGEDIET=1 PIPELINE=1 PIPESTAGE=2 [TRAPEZOID=1]
  (+ NOGD=1 PROFILE=1 NOPROFGPU=1 QUIETFPS=1 for telemetry builds; bins:
  scratchpad bins_planes for STAGEDIET builds, bins_ship otherwise).
- Rig: flashing recipe + recovery ritual in project memory (jcp -r twice
  with sleeps, kill orphan jcp by PID first, never pkill by pattern).
  The skunkboard may be shared with other sessions — coordinate via user.

## Step 0 (mandatory): measure the run-length distribution
Instrument or trace RUNC values per frame in jagemu (watchpoint on RUNC or
a debug store) at spawn + room-12. The histogram decides option 1/2/3 and
predicts the ceiling: saved_launches = sum(run-1)/sum(run). Report this
BEFORE kernel design.

---
# STEP 0 RESULTS (2026-07-21, jagemu, PLAY build MULTIROOM+GEOMDIRECT+
# JERRYPOSE+AUTOSTART+NOGDONLY+STAGEDIET+PIPELINE, planes bins)

Two independent instruments, cross-checked:
1. **True RUNC histogram** — kernel probe `-d RUNHIST=1` (emu-only;
   RUNHIST_BUF=$1C0000 32-long DRAM histogram, RUNHIST_BUF[min(nrun,31)]++
   at the run-start store; SHADEPASS=0 to make room; probe COFs
   RUNHIST_spawn.cof / RUNHIST_r12.cof in the session scratchpad).
2. **Full span-stream trace** — `JAGEMU_BLIT_TRACE=1` with LO/HI over the
   three framebuffers; every span launch logged with dst x/y/w + src u/v;
   analyzed offline (scratchpad trap_analyze.py; last 10-20 renders of a
   650-frame run, spawn + room-12 spawn variant 44032/7168/56320@room15).

RUNC (rows to next chain event), 50-vblank steady-state window:
- spawn: runs=5412 rows=41012 **avg 7.58** (hist 1:1428 2:605 3:491 4:325
  5:318 6:263 7:182 8:123 9:146 10:160 11:180 12:82 13:206 14:67 15:76
  16..30:~350 31+:302)
- room-12: runs=4048 rows=14277 **avg 3.53** (1:1824 2:553 3:373 4:173
  5:230 6:217 7:59 8:33 9:107 10:178 11:126 12:55 13:75 14:32 15:13, none
  above 15) — **RETRACTED LATER SAME DAY: the r12 spawn variant renders
  Lara-on-black in-game (rooms never draw), so every r12 number in this
  section measured title+Lara content, NOT the junction. See RESULTS.**
Both clear the >=3-row viability bar. BUT RUNC alone over-states what a
multi-row blit can batch — the Blitter constraints (one B_COUNT width,
integer A2_STEP) split runs further. From the span trace:

- Face-column ceiling (perfect trapezoid hw): spawn avg 18.8 rows (94.7%
  launch cut), r12 avg 9.2 (89.1%).
- **EXACT batching** (const width + const integer xl-step + linear u/v):
  spawn avg 1.93 -> **48.3%** of launches removable; r12 avg 1.96 ->
  **48.9%**. Run-length: 62-67% of segments are 1 row, but the >=2 tail
  carries 65-71% of all rows.
- 1px-slop batching (option 2): 64.5% / 70.7% — only ~16-22pt more than
  exact, for real visual wobble risk.
- **THE DECISIVE SPLIT: d=0 (xl constant AND width constant, i.e. pure
  RECTANGLE runs) carry 59.5% (spawn) / 63.7% (r12) of ALL rows.** General
  d!=0 trapezoids add only ~6% more. Caves geometry is axis-dominated.
- Span width dist (spawn): w1 9.4% | w2-3 22.9% | w4-7 22.6% | w8-15
  16.3% | w16+ 28.8%. (r12 is thinner: 86% below w8.)

**VERDICT: viable. Ceiling ~48% of span launches (and their whole span
setup: clamp+2 divides+7 stores+bwait) removable with EXACT, pixel-
identical batching, ~60-64% of it from rectangle runs alone.**

# DESIGN (chosen: option 3 hybrid, EXACT d=0 rectangle runs only)

Rationale: rectangle runs are ~94% of the exact-batch win at ~40% of the
complexity; exact means the verify ladder can demand pixel parity (no
wobble-judgment escape hatch); hybrid keeps the per-scanline path as the
untouched fallback for k=1 rows. General d!=0 (~6% more rows) deferred.

Mechanism (all inside `.if TRAPEZOID=1`, per-scanline path byte-identical
at TRAPEZOID=0):
- **sp_fill lookahead**: with span geometry final (r29 xl, r23 xr, r7 u0,
  r20 v0, r8 du, r9 dv), step temp copies of the edge DDAs (r16/r18,
  r17/r19) forward: k = consecutive rows (capped by RUNC) whose floor(ax)
  and floor(bx) BOTH stay equal to this row's. k>=2 -> batch.
- **Texture drift gates**: batch only if |BUSL-AUSL| < 2048 and
  |BVSL-AVSL| < 2048 (per-row du/dv drift < 1/32 texel -> < 1 texel of
  right-edge shear over 32 rows; the per-poly-gradient-cache shear lesson
  bounded, not repeated). uL side steps EXACTLY via A1_FSTEP.
- **Batched launch** (replaces k single-row launches):
  A1_STEP/A1_FSTEP = (LVSL,LUSL) - w*(dv,du) packed 16.16 (2x imul32; the
  w*du term cancels the Blitter's own inner-loop pointer advance, so
  per-row texture starts land EXACTLY on uL+i*LUSL — even w=1 garbage-du
  spans are exact, no special case);
  A2_STEP = (1<<16)|((-w)&$FFFF) at (r15+13)=$F02234 (v4b-proven, UPDA2
  re-home semantics silicon-confirmed 2026-07-21 in the rectshade REQ);
  B_COUNT = (k<<16)|w; B_CMD = BCMD_TEX|UPDA1F|UPDA1|UPDA2 = $01800F01.
- **State advance via BATCHC skip (zero new state math)**: r21 (dead
  guardB) = k-1; sp_run entry: if r21>0 { r21--; goto sp_tail } — the
  existing tail advances x-DDAs, u/v-DDAs, y, RUNC per skipped row, so
  loop state stays EXACTLY as the per-scanline path would leave it.
  Left-chain slope base r4 (dead in span section) set at the A-left/B-left
  branch: r4 = r14 (A) / r14+16 (B).
- Register budget: r4, r21 (audited free lines 1374-1660), r5/r24-r27 as
  lookahead temps at sp_fill (uL..vR dead there), r0-r3 scratch as today.
- Byte budget: ~180B; TRAPEZOID=1 requires SHADEPASS=0 for the first
  proof (Makefile guard; base ~3450 -> ~3630 of 3680). Reconcile with
  SHADEPASS after the fps datum, per the campaign brief.
- Expected effect: removes ~48% of span setups+launches at spawn/junction
  scale; walk slice ~27% of frame (silicon D4) + launch share of the
  13.6% fill slice -> model says roughly 12-18% of mid-scene frame time;
  jsim A/B is stage 3, silicon is the judge (stage 4, NOT this session).

---
# RESULTS (2026-07-21, same session — READ THE VERDICT BEFORE SPENDING RIG TIME)

## What was built
- `gpu_geotex.gas`: full TRAPEZOID=1 implementation as designed above
  (BATCHC skip at sp_run, LEFTP at chain pick, drift gates + rectangle-run
  lookahead at sp_fill, batched launch with A1_STEP/FSTEP + A2_STEP +
  B_COUNT O=k + $01800F01, exact state advance via existing sp_tail).
  Kernel: **3674/3680 bytes** (TRAPEZOID=1, SHADEPASS=0). TRAPEZOID adds
  216B over the 3458B SHADEPASS=0 base. Also added: `.if RUNHIST=1` probe
  (+~40B, emu telemetry) and `.if DRIFTLOOSE=1` diagnostic gate.
- Makefile: `-d TRAPEZOID`, `-d RUNHIST`, `-d DRIFTLOOSE` plumbed into
  GEOTEX_DEFS; guards TRAPEZOID×SHADEPASS and TRAPEZOID×RUNHIST.
- Dialect traps hit and fixed: `jr` ±15-word range; **indexed addressing
  is r14/r15-ONLY** (LEFTP became a direct pointer); jas `-d` symbols are
  NOT instruction-operand symbols (only `.if` — DRIFTBIT param failed,
  became DRIFTLOOSE `.if`).

## Verify ladder results
1. **TRAPEZOID=0 parity: PASS, bit-exact.** Full-COF md5 identical to the
   pre-campaign baseline (81ca826a…, kernel 3656B, SHADEPASS=1 play build).
2. **TRAPEZOID=1 emu parity (spawn, 650 frames, silicon fidelity):**
   - Coverage invariant: per-render painted-pixel totals IDENTICAL to the
     TRAPEZOID=0 reference wherever renders align (e.g. 93,429 px both;
     title renders exactly equal at 15716/15090/16316 px). No holes, no
     bleed. Batches real: kmax=17.
   - Screenshot diff: 1755/76800 px total; after masking Lara+fps-digit
     regions (the two builds run at different speeds, so vblank-matched
     frames catch different idle poses), **static-scene diff = 308 px
     (0.40%)** = the gated texture drift (batched rows reuse row-0 du/dv).
     DRIFTLOOSE (1 texel/row) diag: 3.71% static diff — visibly worse,
     as predicted.
   - Driven-input gameplay (serve/ctl, run+turn through the cave): stable,
     coherent geometry, no hangs (tz_walk1/2.png in scratchpad).
   - Lara-only views: 0-pixel diff (r12 builds, see caveat below).
3. **jsim fps A/B: NET NEGATIVE-to-NEUTRAL.** Renders completed in the
   identical 650-vblank window (render-count proxy; fps-bar not present in
   these builds): ref 32 | TRAPEZOID tight-gate 29 | loose-gate 31.
   In-game launch cut: **tight gate -6.3%** (5600→5260/render, avg_k
   1.07), **loose gate -15.4%** (→4735, avg_k 1.18). The per-span
   lookahead+gate cost (~30-45 cyc × every span) eats the savings.

## WHY the step-0 ceiling did not materialize (the honest post-mortem)
- The 48%/60% step-0 numbers came from span-stream columns that MERGE
  vertically-stacked faces sharing an atlas base (y continues +1, same
  SRC base, same xl/width across aligned wall/floor tiles). The kernel
  can only batch INSIDE one face AND inside one RUNC chain-run. Per-face+
  per-run realizable batching at spawn ≈ avg_k 1.07 (tight) / 1.18
  (loose) vs the 1.93 column prediction.
- Gates are NOT the main limiter: loosening 2048→65536 only moved the cut
  6.3%→15.4% while tripling visual drift.
- **Step-0 r12 numbers are INVALID**: the r12 spawn-header variant (room15
  /44032/7168/56320) renders Lara-on-black in-game in THIS session's
  rebuilds (both SHADEPASS=0 and =1) — the RUNC r12 histogram (avg 3.53)
  and trace stats measured title+Lara content, not the junction. The
  overdraw-era PLAY_PS_r12.cof DOES render the junction (why it works and
  fresh builds don't is UNRESOLVED — same coords, same bins; suspect the
  original patch differed in some way not recorded). Honest junction
  datum from PLAY_PS_r12 trace: d=0 column ceiling 40.4% (avg 1.68) —
  BELOW the spawn ceiling, and subject to the same ~0.3x per-face/per-run
  realization factor => junction expectation ~10-15% launch cut.

## VERDICT
Mechanism verified correct end-to-end (exact coverage, parity, stability),
but **the content does not have enough per-face constant-width runs: the
realizable avg batch is ~1.1-1.2 rows against the >=3 viability bar.**
jsim says the tight build is a net LOSS and the loose build a wash with
visible texture drift. Per the campaign's own stop rule this is a
NO-GO for silicon in its current shape. Possible salvages (user call,
none cheap): (a) restrict lookahead to spans wider than N px so the
per-span cost lands only where launches are expensive; (b) d=constant
(not just 0) generalization (+~6% rows, more bytes); (c) revisit after
S-BUFFER/near-to-far work changes the span population. jsim's known
silicon gaps (busy-B_CMD writer hold, 68k bus interference — both scale
with launch count) could make silicon kinder than jsim to a launch-cut
change, but not plausibly enough to flip -6% into mid-teens fps.

## Staged artifacts (session scratchpad, NOT flashed — silicon untouched)
- TRAP_base.cof (baseline play build, SHADEPASS=1, bit-exact vs tree)
- TRAP_ref0.cof / TRAP_tz1.cof (SHADEPASS=0 A/B pair, spawn header)
- TRAP_tz1_loose.cof (DRIFTLOOSE diagnostic)
- TRAP_ref0_r12.cof / TRAP_tz1_r12.cof (r12 header — rooms don't render,
  see caveat), TRAP_base_r12.cof, RUNHIST_spawn.cof / RUNHIST_r12.cof
- traces + trap_analyze.py + screenshots (tz_*.png, r12_*.png)

## Tree state at session end
- Ship bins restored (mrt.bin md5 40ba1535…, spawn header = ship).
- TRAPEZOID/RUNHIST/DRIFTLOOSE all opt-in; TRAPEZOID=0 verified bit-exact;
  `make MULTIROOM=1 JERRYPOSE=1` builds green.
- Play builds (STAGEDIET=1) still need bins_planes from the scratchpad.
