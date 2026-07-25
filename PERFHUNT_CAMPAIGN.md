# PERFHUNT CAMPAIGN — general performance hunt (opened 2026-07-22, agent)

DIRECTIVE (user): "start looking for places to find even more performance to
make things smoother and faster." Emulator-only session (rig owned by the
calibration work — no jcp / /dev/video* / skunk). jagemu freshly calibrated
to near-silicon parity; its numbers are decision-grade for the first time.

BASELINE (given, parity-jsim, 1200 fr spawn, PLAY_XB config = MULTIROOM
GEOMDIRECT SHADEPASS JERRYPOSE AUTOSTART NOGDONLY STAGEDIET PIPELINE
PIPESTAGE=2 HOPDIAL HOPBOOT=1 XCULL=1 BEXIT=1, bins_planes, kernel
3668/3680): blit-busy 52.8% (busy != paid — use blit_wait), jump_refill
17.2%, stall_flags 6.0%, stall_alu 5.9%, mem_external 4.8%, stall_div 2.0%,
stall_load 1.2%, IPC 0.50.

PLAN
1. Profile deepening: spawn + r12 junction (MRT_SPAWN_ROOM=12 — the index
   is 12, NOT 15) + driven gameplay. Per-scene stall/blit_wait splits,
   fresh 68k --pc-histogram, frame-time VARIANCE (worst-frame spikes).
2. Rank candidates by measured pool x feasibility. Gate: net >= 3% of the
   relevant regime's frame time in parity-jsim, or die at the gate.
3. Implement survivors behind opt-in flags; verify (flags-off byte
   identity, flags-on pixel parity spawn+r12+driven, jsim fps A/B);
   stage telemetry twins + PLAY candidate in scratchpad.
Byte budget: kernel 3668/3680 (12 spare).

BINDING GOTCHAS (predecessors): make clean between flag configs;
enable-by-omission (never FLAG=0 on the make line); jas: `.align 4`,
`or rX,rX` consumes / `move` does not, indexed addressing r14/r15-only,
jr range ±15 words; SHADEPASS=0 + RAMP bins = polluted-U garble (fps valid,
look not); blitter ownership under PIPELINE; bins_planes for STAGEDIET
builds, bins_ship restored at session end.

--- LOG ---

## STEP 1 — DEEP PROFILE (2026-07-22, parity-jsim rebuilt from cobweb 66bb59a)

INSTRUMENT NOTES (methodology, reusable):
- The installed jagemu binary was STALE vs the committed source (missing the
  blit_launch/blit_transfer/blit_wait split) — rebuilt with cargo from the
  clean tree; decomposition numbers shifted slightly vs the brief's baseline
  (blit busy 53%->44%, mem_external 4.8->8.6%): same calibration lineage,
  finer counters. All numbers below = rebuilt binary.
- --pc-histogram and --watch are MUTUALLY EXCLUSIVE in `jagemu run` (the
  histogram branch returns first).
- The whole-window GPU decomposition is POLLUTED by two idle modes:
  (a) the kernel's `halt:` nop/jr spin after alldone (charged ~50%
  jump_refill), (b) truly-stopped time (G_CTRL=0 between sync and re-kick).
  Separated via serve/ctl GPU-PC SAMPLING (600 samples @ 1/frame, label map
  from jas -r + .jo symbol table parse — jas --elf-obj fails on MOVEI relocs,
  the .jo parse works; ph_scene.py + ph_analyze.py in scratchpad).
- MRT_SPAWN_ROOM patch builds shift BSS addresses (~272B code delta from
  constant folding): displist.7 = 0x173058 (spawn build) vs 0x172F48 (r12
  build). nm the exact .elf or the watch silently logs nothing.
- ctl breakpoint caller-census: `continue` does NOT step past a breakpoint
  (re-stops at same pc, frame frozen) — `step 4` first, then continue.

### Per-scene decomposition (600-field steady windows, PLAY_XB, silicon fid.)
scene   GPUbusy  blit  b_wait  jrefill  mem_ext  s_alu  s_flags | 68k-serial
spawn    91.5%   45.8%  6.8%   14.3%     8.9%    5.8%   4.6%   | 20.7% of wall
r12      91.2%   43.4%  6.5%   15.1%    11.1%    5.8%   4.3%   | 21.3%
drive    88.9%   41.8%  7.2%   17.6%    11.4%    5.5%   4.5%   | 33.0%  (!)
(68k-serial = GPU truly-stopped + halt-spin share from PC sampling.)
GPU PC sample shares (of GPU time): bwait spin 20-25% (blitter-bound spans,
paid wait — matches blit_wait+overlap), halt 13-25%, span-setup row loop
(sp_run..sp_tail) ~25-30%, imul32 4-7%, staging 6-8%, vc pre-pass ~4%.

### Render cadence + VARIANCE (displist watch)
All three scenes: ~58-60 renders/600 fields (jsim ~6 fps; silicon runs
~3x faster at idle — jsim is a RELATIVE instrument, reconfirmed).
Frame-time fields: spawn mean 10.39 {10:35, 11:22}; r12 10.00 (all 10s!);
drive mean 10.14 {6..12}. NO silicon-style 250-400ms spikes reproduce in
the emulator: worst/median = 1.2. Variance work must be gated on silicon;
in-emu the smoothness lever = the driven-play 68k-serial term (33%).

### 68k steady-state mix (driven window, PC samples of wall time)
STOP-sleep 71% | lara_finish 9.8% | main+0x1500 (painter-order O(n^2)
selection sort over ALL 38 rooms, dist recomputed per inner iter) 6.3% |
__mulsi3 4.7% + __udivsi3 1.8% | main misc ~4%.
__mulsi3 caller census (step-over breakpoint, 300 hits): 94% from
portal_rect() corner projection (8x 32x16 mul + 2 div per corner, 4
corners/portal — the visibility rect chain); rest Lara move trig.
lara_finish anatomy: per-mesh centroid vert-sum (byte reads + 3 __divsi3
per mesh = 48 divs/render) + face re-emit on sort-order change.

### CANDIDATES RANKED (pool x feasibility)
A. 68K-DIET package (C-only, no kernel bytes, driven-play pool ~12-15% of
   wall = ~1/3 of the 68k-serial segment): A1 portal_rect muls.w/mulu.w
   split-multiply helper (exact, no precision loss); A2 painter-sort on the
   admitted-candidate subset only (rdepth<=3 && prv!=0, ~5-10 rooms vs 38)
   + hoisted dist[]; A3 lara_finish centroid pass: word reads + reciprocal
   multiply (kills 48 divs). GATE: expect >=3% driven-regime frame time.
B. ROWDIET kernel micro-diet (row loop, ~27 cyc/row x ~5-8K rows/render
   ~= 3-6% of GPU render): hoist G_DIVCTRL integer/16.16 toggles out of the
   row loop (2 movei+moveq+store pairs per ROW -> per RUN boundaries);
   CLIPX0/X1 kept in r4/r21 across the run (both dead in-run at
   TRAPEZOID=0); off==0 fast path falls THROUGH to sp_fill (slow path moved
   out of line). Bytes ~neutral. GATE: >=3% GPU render cycles.
C. bwait spin (20-25% of GPU) — NO-GO: needs faster blits; phrase-mode,
   trapezoid, overdraw all previously closed on silicon/step-0.
D. per-scanline chain-check diet — ALREADY DONE (RUNC batching amortizes
   chain checks per run); confirmed by PC profile (yloop/chains < 2%).
E. stall_flags/alu scheduling (~10% combined) — hand-scheduling risk high,
   pool diffuse; only as opportunistic slot-fills during B.

## STEP 2 — M68DIET (candidate A) BUILT + MEASURED (2026-07-22)

Implementation (main.c + Makefile, `make ... M68DIET=1`, opt-in, flags-off
build verified BYTE-IDENTICAL to PH_base/PLAY_XB aa528dda):
- A1: mul32x16() (muls.w hi + mulu.w lo + sign fix, exact for |b|<=32767) —
  portal_rect's 8 rotation multiplies via PMUL() macro (legacy expansion
  token-identical). The x190 focal multiplies were ALREADY gcc shift-add
  inlined (checked in disasm) — only __divsi3 remains there, kept.
- A2: painter sort over the candidate subset (rdepth<=3 && prv!=0) with
  hoisted cdist[]; admit loop bound = M68D_ORD_N macro (legacy expands to
  the original `roomCount` token). Comparator + ascending scan order
  preserved => identical relative order of admitted rooms. Makefile guards:
  M68DIET x ROOMCAP, M68DIET x NOVISCULL mutually exclusive.
- A3: lara_finish centroids: big-endian s16 WORD reads (blob even-aligned)
  + per-mesh reciprocal multiply (kills the 48 __divsi3/render; exact-to-
  1-unit rounding difference documented, only depth ties can reorder).

Verify: emu pixel parity frame-700 — spawn env 0 px (row-1 HB strip +
nothing); r12 164 px ALL inside Lara's documented idle-sway bbox
(x133-186 y94-200), environment 0 px. No illegal ops. Driven 600-frame
window stable.

MEASURED (parity-jsim, 600-field windows):
- 68k instret: spawn 3.49M -> 1.53M (-56%); driven 4.17M -> 2.52M (-40%).
- 68k awake share of wall: spawn 19.3% -> 9.7%; driven 29.0% -> 19.2%.
  __mulsi3 gone from samples (was 4.7-5.5%), main sort bucket 12.3 -> 6.7%.
- Frame time (render cadence): driven mean gap 10.14 -> 9.92 fields
  (-2.2%); spawn 10.39 -> 10.39 (0%). 68k-serial share 33.0 -> 32.4%.
GATE READING (honest): jsim frame-time -2.2% driven — BELOW the 3% gate
in-model. The pool is real (half the 68k's instructions) but PIPELINE
overlap + the flip's field quantization absorb it in jsim, and jsim's Tom
is ~3x slower relative to silicon (jsim ~6 fps where silicon idles at
19-20), so the 68k-serial slice is proportionally ~3x LARGER on silicon.
Same class as the collision diet (emu ~nothing -> silicon 6.2->11.5 fps,
bus-price precedent) and jsim cannot price 68k DRAM byte-loop costs
(1.73x silicon ratio, documented). DISPOSITION: NOT promoted on jsim
numbers alone — silicon A/B twins staged (below); play-candidate carries
it for the user's silicon session to decide.

## STEP 3 — ROWDIET (candidate B) BUILT + MEASURED (2026-07-22)

Implementation (gpu_geotex.gas + Makefile, `make ... ROWDIET=1`, opt-in):
- G_DIVCTRL integer/16.16 toggles hoisted OUT of the per-row span loop:
  integer mode set once at run entry (the borrow section), 16.16 restored
  once at run exit (where it also hides the r14/r15 restore load latency).
  Safe because the ONLY in-run divides are the span du/dv; uv_edge /
  chain_step 16.16 divides happen at chain events, outside the run.
- CLIPX0/CLIPX1 loaded into r4/r21 at run entry; per-row clamps lose
  2x (movei+load+2nop). r4/r21 are dead in-run at TRAPEZOID=0 (chain
  guards re-init per event; LEFTP/BATCHC are TRAPEZOID-only; imul32
  preserves r4-r25). Makefile guard: ROWDIET x TRAPEZOID exclusive.
- off==0 FAST PATH now FALLS THROUGH to sp_fill; the left-clamp multiply
  path moved out of line (rd_off1, after pkt_done) — one taken jump
  (+refill) removed from every hot row.
- Kernel: ROWDIET=0 BYTE-IDENTICAL 3668; ROWDIET=1 = 3664/3680
  (4B SMALLER — 16 spare). jas hazard-clean.

Verify: r12 frame-700 pixel diff vs base = **0 px RAW** (env+Lara);
spawn diff = 138 px all inside Lara idle-sway bbox (cadence phase),
env 0 px. Driven 1100-frame run+turn: stable, coherent, 0 illegal.

MEASURED (parity-jsim, 600-field windows):
- spawn idle: renders 58 -> 60 (+3.4%), frame-time mean 10.39 -> 10.00
  fields (-3.75%), GPU instret/render -4.3%. **PASSES the 3% gate.**
- r12 driven: renders 60 -> 61 (+1.7%), instret/render -2.3% (driven
  content diverges between builds — spawn is the clean datum).
- Sampling caveat logged: the halt-share estimator phase-locks when the
  cadence hits exactly 10 fields (kick/flip are vblank-synchronized), so
  halt% is untrustworthy at locked cadence; gate metrics = renders +
  instret/render + gap mean.

## STEP 4 — COMBINED CANDIDATE + STAGED ARTIFACTS (2026-07-22)

PLAY_PH = PLAY_XB + M68DIET=1 + ROWDIET=1:
- spawn: renders 58 -> 60, gap 10.39 -> 10.00 (-3.75%); 68k instret -55%;
  GPU instret/render -3.4%.
- driven r12: renders 60 -> 61, gap 10.14 -> 9.82 (-3.2%); 68k instret
  -39%; worst-gap unchanged (12) — in-emu variance was already small;
  the smoothness case rides on the silicon 68k-serial share.
- Parity: spawn env 0 px (diffs only in Lara sway bbox + HB row).

Staged in session scratchpad (NOT flashed — rig untouched per mission):
- **PLAY_PH.cof (09e407c3)** play candidate: MULTIROOM GEOMDIRECT
  SHADEPASS JERRYPOSE AUTOSTART NOGDONLY STAGEDIET PIPELINE PIPESTAGE=2
  HOPDIAL HOPBOOT=1 XCULL BEXIT M68DIET ROWDIET, ship spawn header,
  bins_planes. PLAY_PH_r12.cof (f0bb0950) = ROOM=12 variant. +.elf's.
- **Telemetry twins** (play flags + NOGD PROFILE NOPROFGPU QUIETFPS):
  PHT_ref.cof (03027ee2 — bit-identical to the BUCKET session's XB_on.cof,
  lineage verified) / PHT_on.cof (0c1fd27d, +M68DIET+ROWDIET). fps100
  console A/B per ab_run.sh pattern. NOGD twins sit black in emu without
  a console host — expected.
- A/B + parity builds: PH_base_spawn/r12 (base = aa528dda = PLAY_XB),
  PH_m68d_*, PH_rd_* (+.elf/.map); profiles ph_scene2_*.json,
  ph_prof/ph2_* whole-run profiles + 68k histograms; tools ph_scene.py,
  ph_analyze.py, ph_kernel_syms.json (kernel label map from .jo parse);
  screenshots *_700.png, ph_rd_drive1100.png.

Tree at session end: ship bins restored (all 10 files byte-equal to
bins_ship), mrt_spawn.h = ship, default `make MULTIROOM=1 JERRYPOSE=1`
green + boots to title in emu, flags-off PLAY_XB config rebuilt and
byte-identical to aa528dda AFTER all edits. All new code opt-in
(M68DIET / ROWDIET). Kernel flags-off 3668; PLAY_PH kernel 3664/3680.

## OPEN RISKS / NEXT STEPS
- SILICON GATES PENDING for both flags (PHT_ref vs PHT_on, then user
  play-feel on PLAY_PH). M68DIET expects a LARGER win on silicon than in
  jsim (bus-price class, 68k-serial 33% of driven wall in-model);
  ROWDIET expects jsim-or-better (jsim historically under-predicts
  kernel wins). If silicon disappoints, both retract cleanly (opt-in).
- A3 centroid rounding: <=1-unit vs C division — a rare Lara limb-order
  flip on exact depth ties is possible; watch for limb flicker in the
  play-feel gate.
- Driven-play remaining 68k-serial (~32% of wall in-model even after
  M68DIET): lara_finish face EMIT (order-flip re-emits, ~10% wall) is
  the next 68k pool; a sort-order hysteresis could cut it but risks
  visible limb-order lag — unfunded.
- bwait spin (20-25% of GPU time) remains the biggest single GPU pool
  and remains closed (phrase/trapezoid/overdraw all previously
  no-go'd). Any future attack must make blits cheaper, not fewer.
- The whole-run GPU decomposition (jump_refill 15-17% etc.) includes the
  halt-spin idle loop (~50% jump_refill by construction) — do not quote
  those numbers as render-side stalls without the PC-sampling split.

## SILICON GATE RESULTS (2026-07-22, supervisor)
- Combined PHT_on: 7.8 idle vs ref 20.5 = -62% CATASTROPHIC.
- Bisect: ROWDIET-only 21.6-21.8 idle (NEW RECORD, +5-6% over ref, SHIPS);
  M68DIET-only 7.6-7.7 = CONVICTED, quarantined TOXIC pending autopsy.
- **M68DIET = the largest jsim-vs-silicon divergence on record: jsim
  frame -2.2% / 68k instret -56% vs silicon -62% WALL. Byte-exact repro
  pair for the parity campaign: PHT_ref.cof vs PHT_m68.cof (scratchpad)
  + single-flag builds. Autopsy suspects: reciprocal-divide convergence
  or split-multiply memory traffic; unverified.**
- Shipped play candidate: PLAY_XBR.cof = PLAY_XB + ROWDIET.

## M68DIET AUTOPSY (2026-07-22, smoothness-campaign agent) — MECHANISM FOUND-BY-ELIMINATION + SILICON DISCRIMINATOR KIT STAGED

### Verdict up front
Every "direct cost" suspect named at conviction time is REFUTED with
evidence.  The three sub-changes are individually sound 68000 code and
all of their own frame segments measured EQUAL-OR-FASTER on the silicon
run that convicted them.  The regression is an EMERGENT WAIT/CADENCE
pathology: the frame locks to ~8 fields with the 68k asleep in gpu_sync,
and — the decisive clue — frame time in the toxic state is INSENSITIVE
to Tom's own speed (PHT_on = M68DIET + the +5% ROWDIET kernel scored
7.8 ~= PHT_m68's 7.65).  A five-build silicon discriminator kit is
staged; each hypothesis below predicts a different outcome pattern.

### Evidence chain (all from the conviction-night pht_*.log console
telemetry + disassembly + emu forensics; logs in scratchpad)
1. The QUIETFPS hl ledger (per-240-render dumps, ref vs m68):
   hl_logic 1,064,050 -> 458,197 (x0.43 — the A1+A2 diet WORKED);
   hlr_pose 418,259 -> 417,390 (x1.00) and hl_mdep 534,523 -> 540,674
   (x1.01 — A3's lara_finish cost UNCHANGED, and the emit cadence
   unchanged: the A3-rounding-flips-sort-order theory is DEAD);
   hl_clear 95,012 -> 669,378 (x7.0 — the PIPELINE collect: gpu_sync
   wait + flip + safe_vc + clear); hl_tom 28,218 -> 7,406; hl_flip
   140,317 -> 9,534.  spind (gpu_sync interrupt-wakes) 1.7 -> 6.3 per
   render; maxvbl 4 -> 9 EVERY block (locked long cadence, not spikes).
   NOTE the ledger's absolute units are untrustworthy (the known
   frame_count*525+VC double-count plus internal inconsistencies —
   segments sum to more than the fps100 wall on the ref side too); the
   RATIOS above are build-relative and solid.
2. Disassembly audit of the convicted PH_m68d ELF (m68d_dis.txt /
   base_dis.txt in scratchpad): pure 68000 ISA — NO muls.l/divs.l (gcc
   emitted the intended mulu.w/muls.w pairs, register-resident, ~8 short
   reg ops per PMUL), no traps, no convergence loops (the A3 reciprocal
   is a one-time 65536/n table init, then one mulu/muls pair per mesh),
   no accidental O(n^2) (A2 is strictly fewer iterations), DRAM traffic
   strictly REDUCED (A3 halves the hottest byte-loop's accesses; A2
   kills 703 iters x 2 Manhattan-distance array walks).  nm size diff:
   ONLY main (+350B) and lara_finish (+244B) changed — no global codegen
   restructuring from -DM68DIET.  Stack audit: +256B (cdist[64]) against
   ~300KB headroom ($200000 top, __bss_end 0x1B4150) — no overflow.
3. Emulator (parity-jsim, silicon fidelity, 650/1250-frame snapshots):
   GPU, DSP and Blitter cycle/instret counters are IDENTICAL to the
   fifth digit between PH_base and PH_m68d.  Same renders, 0 illegal
   ops, pixel parity (env 0 px).  The dispatch CONTENT (rooms, clip
   rects, faces, order) is therefore identical — in emu.  jsim 68k
   instret: -1.96M/600 fields at spawn (-56%).
4. Silicon accounting: with 68k logic 2x faster and every serial 68k
   segment equal, a sane frame model says frame = max(logic, Tom) + tail
   = UNCHANGED or better.  Instead the render-wait span (pfA->flip,
   frame_count-anchored, unimpeachable) went 2.95 -> 7.87 fields.
5. ROWDIET cross-check (the killer): ROWDIET alone = 21.6-21.8 (Tom
   measurably faster than ref's 20.5); M68DIET+ROWDIET = 7.8 vs M68DIET
   alone 7.6-7.7.  In the toxic state the frame does NOT respond to a
   faster Tom => the frame is NOT render-bound there; it is parked on a
   field-quantized WAIT (~8 fields) somewhere in the collect chain
   (gpu_sync STOP-wakes / video_flip pending_fb / wait_safe_vc).

### What could and could not cause it (ranked hypotheses)
- H1 TIMELINE/PHASE: the diet shortens the 68k's busy window inside the
  PIPELINE overlap (~1.2 fields less awake time) and the frame re-phases
  against its field-quantized wait points, settling in a stable bad
  attractor (all frames ~8 fields, maxvbl=9 every block).  Mechanically
  vague but consistent with EVERY observation, including emu blindness
  (jsim's Tom is ~3x slower relatively — frame ~10 fields — so phase
  effects of a 1-field logic change drown).
- H2 STOP/interrupt-wake machinery: more STOP-sleep entries/wakes per
  frame (spind x3.8) interacting badly with the CPUINT/VID ack chain on
  real silicon (jagemu models wakes functionally, not their bus/ack
  timing).  SYNCPOLL twin discriminates.
- H3 piece-specific silicon side effect not visible in emu (the original
  "reciprocal convergence / split-mul traffic" suspects are refuted as
  DIRECT costs, but a piece could still be the trigger of H1/H2 via its
  specific timing).  A1/A2/A3 twins discriminate.
- REFUTED OUTRIGHT: pathological instruction mixes (no muls.l etc.),
  divide convergence loops (none exist), added DRAM table traffic
  (reduced), O(n^2) (reduced), A3 sort-order flip re-emits (hlr_pose
  identical), stack overflow, Tom kernel differences (byte-identical),
  displist/rect content differences (emu-identical), DSP work change
  (emu-identical to 6 digits).

### DISCRIMINATOR KIT (staged in scratchpad — flash order + predictions)
All twins = PHT_ref flags (play + NOGD PROFILE NOPROFGPU QUIETFPS,
bins_planes, ship spawn header); ref datum PHT_ref.cof 20.5, toxin
PHT_m68.cof 7.6-7.7.  Rebuilt-and-verified byte-identical through the
new sub-flag plumbing (PHT_m68 md5 de565eeb reproduced).
1. **PHT_pad.cof** (M68DIET + M68PAD=7000): full diet + calibrated
   busy-pad at the end of the logic segment (5 instr + 1 DRAM read per
   iter; emu-verified +2.02M instr/650fr vs diet's -1.96M — restores the
   removed 68k timeline within ~3%).  RECOVERS to ~20 => H1 confirmed
   (the diet code is innocent; the 68k's busy DURATION was load-bearing
   — then the fix is keeping SOME pad or restructuring the collect).
   STAYS ~7.6 => H1 dead, go to piece bisect.
2. **PHT_a1.cof / PHT_a2.cof / PHT_a3.cof**: one piece each (new
   Makefile vars M68A1/M68A2/M68A3; M68DIET still = all three).  Emu
   parity: a1 = 0 px RAW vs base; a2/a3 diffs only inside Lara idle-sway
   mask, env 0 px.  Whichever scores ~7.6 carries the toxin trigger.
3. **PHT_syncpoll.cof** (M68DIET + SYNCPOLL=1): gpu_sync busy-polls
   instead of STOP-sleeping.  Different fps than PHT_m68 (in either
   direction) => the STOP/wake machinery is implicated (H2).
Sub-flag refactor verify: flags-off byte-identity PASS (aa528dda =
PLAY_XB); M68DIET=1 byte-identity PASS (333d67c6 = PH_m68d); play-build
piece parities as above.  M68PAD is compile-time (make M68PAD=N).

### Disposition
M68DIET stays QUARANTINED (no play candidate carries it).  Nothing to
salvage blind: each piece is already the silicon-safe idiom of its
intent, and per the evidence the pieces' direct costs are not the
problem — re-landing any of them is gated on the discriminator kit
verdict (user silicon session, ~10 min of flashes).

## LARA EMIT DIET — LEMITDIET=1 BUILT + EMU-VERIFIED, SILICON-GATED (2026-07-22, smoothness agent)

Pool validation first (emu, serve/ctl driven turning at spawn): the mesh
order changed in 61% of 5-field windows — the face re-emit fires on MOST
driven renders, so the pool is real.  Change-shape measurements that set
the design: first-diff position mean 2.1/15 (head churn — prefix-skip
alone worth only ~13%) but full changed-window median 12/15 (turning
churns most of the order — window-emit worth ~20%, its floor).

Implementation (`make ... LEMITDIET=1`, C + cpu68k.S only, ZERO kernel
bytes; flags-off byte-identical — PLAY_XBR md5 6624d359 reproduced after
all edits; the asm helpers are #ifdef'd because unconditional symbols
shift addresses — burned once, caught by the identity check):
1. PAYLOAD-ONLY RE-EMIT: record slots are fixed-size, so the STAGEDIET
   12B plane prefixes live at ORDER-INDEPENDENT offsets and are constant
   -> written once at lemit_init, never re-emitted (-33% of emit writes).
2. PRE-GROUPED BANKS (7.7KB BSS, built once): per-mesh packed payloads;
   re-emit is lemit_qcopy/lemit_tcopy movem.l bursts (6-long quad / 4-long
   +word tri per iteration, dst advances by record stride) — no per-face
   mq_list gather, ~1 fetch word per 3 data words vs ~2 fetch words per
   data word in the gcc loop.
3. CHANGED-WINDOW EMIT: only mesh positions first-diff..last-diff are
   copied; prefix offsets unchanged by equality, suffix offsets unchanged
   because the window is a permutation of the same mesh set.

Verify (emu, silicon fidelity):
- Blob BYTE-IDENTITY: face region (12,156B) peeked from LEMITDIET vs
  legacy builds at spawn idle = identical; after 480 driven fields of
  turning (order = a heavy permutation) the region still exactly matches
  a full python reconstruction from the banks + live order.
- Pixel parity frame-700: spawn AND r12 junction — environment 0 px,
  diffs only inside Lara's idle-sway mask.  Driven r12 run+turn: stable,
  coherent, 0 illegal ops.
- Driven A/B (identical 600-field scripted window, r12): 68k instret
  5.314M -> 4.977M (-6.3% of all driven 68k work); GPU instret +1.3%
  (more renders completed in the same wall window); DSP identical.

HONEST HAND MODEL (silicon): the emit runs in the SERIAL post-collect
segment (Tom idle), so savings are full-price wall.  68k ~33% of driven
wall in-model (larger on silicon), removed work is DRAM-copy class
(1.73x bytewise silicon ratio): modeled net ~2.5-3.5% of driven frame —
AT the 3% gate, not above it.  CAUTION FLAGS: (a) the M68DIET conviction
proves 68k-timeline changes can behave nonlinearly on silicon; (b) the
autopsy found A3's serial-segment diet (hlr_pose) moved NOTHING on
silicon despite killing 48 divides — serial-segment models are suspect
until the autopsy discriminators run.  DISPOSITION: staged, NOT resident.
Silicon A/B = PHT_led.cof (569846c3) vs the existing PHT_ref.cof; play
candidate PLAY_XBRL.cof (= PLAY_XBR + LEMITDIET, 23e989a5).

## FRAME GOVERNOR — GOVERNOR=1 BUILT + EMU-VERIFIED (2026-07-22, smoothness agent)

Opt-in variance tool (`make ... GOVERNOR=1`, requires HOPDIAL): loop-top
frame_count delta measures each frame's VBL span with zero PROFILE
dependency; span > GOV_HI fields => g_hopcap clamps to 1 (+hop cache
invalidate); GOV_K consecutive spans <= GOV_LO restore the dial cap.
Hysteresis = LO<HI + K-count; post-restore worst case is one over-budget
probe frame per K calm frames (bounded 1/K duty), and persistently heavy
views keep the clamp engaged.  The OPTION+L/R dial now edits the restore
target (g_gov_user) so governor and dial do not fight.  Telemetry: govt
(trip count) joins the QUIETFPS block prints.  Defaults GOV_HI=6 GOV_LO=4
GOV_K=20 are SILICON-scaled (idle ~3 fields); emu cadence sits ~10 fields
so in-emu exercises used GOV_HI=11 GOV_LO=10 GOV_K=8.

Emu verdict (r12 junction, HOPBOOT=4, identical 900-field driven script,
governor-armed vs inert-control GOV_HI=255 — same binary layout, same
instrumentation):
- renders completed 85 -> 97 (+14% throughput, mean gap 10.68 -> 9.40);
- heavy-tail gaps >=12 fields: 26% -> 8.3% of frames; p90 12 -> 11;
- 7 trips, all with clean restores (no oscillation lock);
- max single gap unchanged (17): a REACTIVE governor cannot dodge the
  first heavy frame, only the pile-up after it — judge it on the tail
  fraction, which is the felt "stall" statistic;
- steady-view parity: spawn frame-700 armed vs inert = env 0 px.
NOTE the mission's worst/median ratio metric MOVED THE WRONG WAY
(1.55 -> 1.70) because the MEDIAN improved more than the max — recording
that honestly; tail-fraction + p90 + mean all improved and are the
better variance statistics here.
RESIDENT INTERACTION: at HOPBOOT=1 (current resident) the governor is
inert by construction (nothing to clamp).  Its use case is running the
richer hop-2+ draw distance with hop-1 worst-case behavior.  Staged for
silicon: PHT_hop2_ref.cof (167ab95f, HOPBOOT=2 no governor) vs
PHT_hop2_gov.cof (dc4f3639, HOPBOOT=2 + governor, silicon defaults);
play candidate PLAY_XBR_G2.cof (ef3b58e3, PLAY_XBR flags at HOPBOOT=2 +
GOVERNOR).  Judge on silicon fps100 variance across a walk + play feel.

## SESSION LEDGER (2026-07-22 smoothness agent)
- Tree: flags-off PLAY_XBR config byte-identical (6624d359) after ALL
  edits; ship bins restored (10/10 byte-equal, spawn header ship);
  default `make MULTIROOM=1 JERRYPOSE=1` green, boots to title in emu,
  0 illegal.  Kernel untouched this session: flags-off 3668, ROWDIET
  3664/3680 (16 spare) — no new kernel bytes spent.
- New opt-in flags: M68A1/M68A2/M68A3 (M68DIET sub-bisect), M68PAD=N
  (timeline discriminator), LEMITDIET (emit diet), GOVERNOR (+GOV_HI/
  GOV_LO/GOV_K).  All enable-by-omission safe; Makefile guards:
  GOVERNOR requires HOPDIAL; M68DIET/M68A2 x ROOMCAP/NOVISCULL kept.
- Staged artifacts (session scratchpad): autopsy kit PHT_a1/a2/a3,
  PHT_pad, PHT_syncpoll (+ PH_a*_spawn play builds + parity screenshots);
  emit diet PHT_led + PLAY_XBRL (+ PH_led_spawn/r12 with .elf, driven
  captures sm_*); governor PHT_hop2_ref/PHT_hop2_gov + PLAY_XBR_G2
  (+ GOV_* emu exercise builds); r12 ref PH_xbr_r12.
- SILICON QUEUE (suggested flash order, ~10 builds x ~1 min each):
  1) PHT_pad (the single most informative flash — H1 test),
  2) PHT_a1/a2/a3, 3) PHT_syncpoll, 4) PHT_led vs PHT_ref,
  5) PHT_hop2_ref vs PHT_hop2_gov, then play-feel on PLAY_XBRL /
  PLAY_XBR_G2 as gates pass.

## SMOOTHNESS-CAMPAIGN SILICON RESULTS (2026-07-22, supervisor)
- PHT_pad: throughput RESTORED (19.3-26.7, incl. 26.7 = record) but
  pacing ERRATIC (±7 fps block swings; healthy builds ±0.5) — user felt
  it as "really slow". MECHANISM CONFIRMED+REFINED: M68DIET's 68k-speed
  change disturbs PIPELINE collect/flip PHASE; padded = fast but ragged.
  Durable fix = pacing-robust collect (next campaign; unlocks the whole
  68k-serial pool: M68DIET's real -57% logic + LEMITDIET).
- Governor pair: BOTH arms idle-only captured (no driven data — variance
  verdict OPEN). DEFECT FOUND: gov build idles 17.4 vs ref 20.6 (-15%)
  where it should be inert — suspect trip/restore churn thrashing the
  hop BFS cache. Back to the bench (emu-diagnosable).
- Resident restored: PLAY_XBR. SILICON QUEUE now: (1) collect-pacing
  fix, (2) governor churn fix, (3) LEMITDIET behind the pacing door.

---

## SILICON PHASE BREAKDOWN (2026-07-24) — the 68k is a CO-BOTTLENECK

From the hl_* counters captured during the valid A/B window (NOGD + jcp attached),
PLAY_XB config. hl_* are half-lines (HLP = frame_count*525 + VC, 525/field),
accumulated every render and printed+reset every 4th 60-render block => **divide
by 240**. Validation: the sequential phases sum to 14.0-14.6 fields/frame, which
matches the independently measured fpsT period (13.6-14.8). Interpretation sound.

| phase | hl/frame | % of frame |
|---|---|---|
| **hl_logic (68k game logic)** | **~4440** | **54-59%** |
| hl_mdep (depth + painter sort) | ~2200 | 27-29% (span inside logic) |
| hlr_pose (Jerry pose read) | 1240-1730 | 16-23% |
| hl_flip | 570-1230 | 7.5-16% |
| hl_clear | 350-570 | 4.6-7.8% |
| hl_tom | 85-290 | 1-4% (dispatch..kick ONLY under PIPELINE) |

**hl_logic alone is ~4440 hl = 8.5 fields = a hard ceiling of ~7 fps even if Tom
cost nothing.** We measure 4.06 fps (14.8 fields), so Tom's concurrent span adds
~6 fields on top. Both are large; the frame is roughly max(68k serial, Tom) plus
the non-overlapped tail.

CONSEQUENCE FOR THE ROADMAP: M68DIET was SHELVED "until Tom < ~3800 hl" on the
assumption Tom was the only thing worth cutting. That is wrong — **the 68k caps
us at ~7 fps regardless**, so 68k work is not premature, it is the second half of
any path past 7 fps. And the largest identifiable 68k sub-phase is the depth/
painter sort (hl_mdep, ~29% of frame), which **M68DIET's M68A2 already
implements** (subset painter sort), C-only, zero kernel bytes, currently unshipped.

NEXT (cheap, silicon): A/B `M68DIET=1` (or just `M68A2=1`) against the baseline,
judged on fpsT medians with jcp attached for the whole run. Unlike MMULT this
targets a phase measured on SILICON, not a jsim estimate.

CAVEAT: hl_tom is NOT Tom's render span under PIPELINE (it is dispatch..kick).
Tom's true span needs PACEPROBE (pp_smax/pp_span), which memory records at
5800-6300 hl in earlier builds — comparable to the whole frame, so Tom is still
a wall too. Cutting only one side will stall at the other's floor.

## PACEPROBE SILICON VERDICT (2026-07-24): THE 68k IS THE CRITICAL PATH

PLAY_XB + PACEPROBE, 479 renders over two 4-block windows:

| | hl/render | % of frame |
|---|---|---|
| frame period (pp_per/pp_coll) | **7533** | 100% (14.4 fields) |
| Tom kick->collect (pp_span) | 6147 | 82% |
| **68k BLOCKED on Tom (pp_wait)** | **458** | **6%  <-- discriminator** |
| **68k's own work (per - wait)** | **7075** | **94%** |

**Tom overlaps almost perfectly; the 68k only stalls on him 6% of the frame.
The 68k's own 7075 hl EXCEEDS Tom's 6147 hl span, so the frame is paced by the
68k.** This is the sanity check the MMULT campaign skipped, and it inverts the
long-standing assumption that "Tom is the wall" (true for GPU-busy %, but not
for the CRITICAL PATH under PIPELINE).

CEILING FOR 68k WORK: cutting 68k work down to Tom's span = 7533 -> 6147 hl =
**18.4% faster, ~4.2 -> ~5.1 fps**, after which Tom binds. Only **~930 hl** has
to go to reach that floor, while the shelved levers target far more:
  - painter sort (hl_mdep)      ~2200 hl   -> M68A2 (implemented, unmeasured)
  - lara_finish (hlr_pose)  ~1240-1730 hl  -> LEMITDIET / M68A3 (implemented)
So M68A2 ALONE could plausibly deliver the whole 18%. Both are C-only, zero
kernel bytes.

BEYOND ~5.1 fps requires TOM work as well (his 6147 hl becomes the floor) — the
two must be cut together, which is why single-lever fps promises keep failing.

CAVEAT — pp_smax/pp_pmax are CONTAMINATED: both read ~37000 hl (~1.2s), which
matches the skunk-console print-burst stall almost exactly (every 4th block now
emits ~17 dbg_kv values with PACEPROBE on). Treat the worst-single-span figures
from a NOGD build as instrumentation, NOT as real game stalls. pp_span/pp_wait/
pp_per are averages over 240 renders and are only mildly inflated.
