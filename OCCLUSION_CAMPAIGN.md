# OCCLUSION CAMPAIGN — face-granular coverage culling (brief, 2026-07-21)

PREDECESSOR LESSONS (binding):
- TRAPEZOID post-mortem (TRAPEZOID_CAMPAIGN.md): per-SPAN added cost
  (~30 cyc lookahead x ~5400 spans) destroyed a 48%-ceiling win →
  silicon -15-25%. ANY design here must put its test at FACE (or
  coarser) granularity, and step 0 must charge the test cost honestly.
- Overdraw measured (NEXT_WORK_SPEC.md): 1.56x spawn / 1.16x junction —
  the win pool is the occluded fraction of walk+fill: bounded, real,
  view-dependent.
- Analyzer humility: the trapezoid's 48% ceiling was an artifact
  (vertical face-merging). This campaign's step-0 simulator must model
  the EXACT kernel-visible quantities (per-face bbox vs coverage state
  at that face's paint time, in actual paint order).

DESIGN CANDIDATE: near-to-far paint order + coarse coverage grid
- Grid: 20x15 tiles of 16x16 px = 300 bits ≈ 40B (SRAM scraps exist;
  else DRAM with the 0.64 contention charge in the model).
- As each face's spans draw, OR its covered tiles (conservative: only
  tiles FULLY covered by the span row-range/width — needs care).
- Before staging a face (post-STAGEDIET-cull site): screen-bbox from
  the vertex cache (already projected!) → if all overlapped tiles are
  covered → skip face entirely (stage+walk+fill saved).
- ORDERING: requires near-to-far room order (flip the painter batch
  order) — WITHIN-room face order also flips (OT sort exists, reverse
  it). CORRECTNESS RISK: painter overlap artifacts where coverage is
  only tile-conservative — faces that partially overlap now draw in
  near-first order → overlapping translucent/coplanar cases must be
  audited (TR1 rooms: portal-adjacent coplanar floors are the known
  case — the "phantom higher surface" class).
  MITIGATION to evaluate: keep far-to-near painter BUT run a PRE-PASS
  computing coverage from the near rooms' OPAQUE face bboxes only
  (approximate, no draw-order change; step-0 sim decides if the win
  survives the approximation).
STEP 0 (mandatory, offline): extend trap_analyze.py to replay the span
trace in paint order, maintain the tile grid, and report per scene:
faces skippable / % of rows+px saved / test cost (faces x ~20cyc +
grid updates) / NET cycles. Run at spawn + PLAY_PS_r12 trace (the
working junction trace from the overdraw campaign). GO only if net >
10% of mid-scene frame.
ALSO QUEUED (cheaper, independent): span MERGING across same-texture
adjacent faces (subdivision seams — SUBDIV 6144 splits big walls into
tiles; adjacent spans on the same row often continue u/v exactly →
merge at emission = fewer spans with ZERO per-span test — check
feasibility in the same step-0 trace pass: count exact-continuation
adjacent spans).
LOOK THREAD (parked until framerate goals hit, per user 2026-07-21):
RAMP K/M dial (30x8 → 40x6) for the "Death Star" grey; water mechanics
regression (memory: project_water.md, swim entry suspects).

---
# STEP 0 RESULTS + VERDICT (2026-07-21, agent session — DOUBLE NO-GO, NO KERNEL BYTES WRITTEN)

## Method (scratchpad occ_analyze.py; traces = current PLAY_PS config)
- Traces: trace_spawn_raw.log (TRAP_base = PLAY_PS play config, spawn) and
  trace_psr12.log (PLAY_PS_r12, the WORKING junction trace). Last 15 in-game
  renders each.
- FACE BOUNDARIES ARE REAL, not heuristic: the v4b rect-shade blit
  (cmd=01C00E08 at pkt_done) delimits each face's span group — the exact
  kernel-visible per-face quantity the brief demanded (k=0 faces lack the
  delimiter and merge into the next face; ~1% in Caves). In-game render
  boundaries = 68k full-fb clear cmd=01800200 O=240 (title uses 01800E01),
  renders keyed per-fb-base (PIPELINE-safe).
- Cross-checks vs independent instruments PASSED: spawn 5610 spans/render
  (trapezoid RUNC era: ~5600); r12 92.9K px/render (overdraw campaign fb-watch:
  93.1K). Scene stats: spawn 190 faces/5610 spans/93.4K px per render;
  junction 314 faces/4463 spans/92.9K px.
- Candidate (a) simulated as reverse-painter coverage pass (= the win pool of
  BOTH the near-to-far online skip and the pre-pass mitigation): face skippable
  iff its bbox tiles are all covered by later-painted (nearer) content at its
  paint turn. Semantics variants: X = exact per-pixel tile coverage
  (UN-IMPLEMENTABLE ceiling, zero update cost charged), R = inscribed-rect
  per-face updates (the only cheap conservative kernel primitive:
  max(xl)/min(xr) accumulate + tile OR at face end). Plus a tile-FREE
  pixel-perfect pool = the absolute physics ceiling.
- Cost model (documented constants; conclusions robust to all of them, see
  gate math): frame = 26.59e6/fps GPU-cyc (junction 9.4 fps silicon, spawn
  19.5); slices walk 27% (silicon D4; sensitivity run at the old 45%),
  fill 13.6%, stage 20%; test 20 cyc/walked face; R-updates 4 cyc/span +
  25 cyc/face; stage credit only for faces skippable pre-stage.

## Candidate (a) — tile-coverage face-skip: **NO-GO (hard gate 10%)**
JUNCTION r12 (the gate scene):
- ABSOLUTE ceiling (tile-free, pixel-perfect, zero-cost test): faces 31.6%,
  **spans 13.6%, px 6.4%** of the drawn stream.
- 16x16 X (perfect coverage, tile-quantized bbox test): 21.6% faces /
  9.6% spans / 5.2% px → NET +3.1% (walk+fill) .. +3.9% (+stage).
- 8x8 X: 10.7% spans; 32x16 X: 7.4% spans — tile size is not the limiter.
- 16x16 REALISTIC (inscribed-rect updates): 3.8% faces / **0.8% spans /
  0.2% px → NET −0.9..−0.7%** (NEGATIVE). 32x16 R: −1.0%.
SPAWN: ceiling 6.8% spans / 3.8% px → 16x16 X net +0.6..+0.8%; R variant
−2.3%. (Spawn is the WORSE scene despite 1.56x overdraw — see retractions.)
GATE MATH at the most generous constants (old 45% walk share, stage credited):
0.45×13.6% + 0.136×6.4% + 0.20×(99/1597) ≈ **8.2% at an un-implementable
ceiling** — still under the 10% gate. Honest current-build constants: ~5.8%.
The implementable design (16x16 + inscribed-rect) is net NEGATIVE at both
scenes. NO-GO is pool-limited, not cost-limited: no tuning rescues it.

## Candidate (b) — span-merge freerider: **NO-GO (bar 4%)**
- EXACT x-adjacency (xB == xA+wA, same texture base, u/v continuation
  consistent): **spawn 1.0 merges/render, junction 2.2/render = 0.02-0.05%
  of spans → GROSS +0.01% of frame.** Three orders of magnitude under the bar.
- Root cause (premise retraction): subdivision seams rasterize their shared
  edge INDEPENDENTLY per face → adjacent spans overlap or gap by 1px. At
  tol=±1px "adjacency" jumps to 45-54% of spans and continuation-merges to
  12.5% (junction) — but a ±1px merge is not pixel-exact (double-draw or
  1px shear) AND the realistic mechanism (pending-span row buffer, ~15-30
  cyc/span DRAM-contended check) nets ≤0 even then. Consistent with the
  SUBDIV 12288 silicon null result (+0.02 ± noise): pre-merging these faces
  at extraction moved nothing.

## Retractions / findings (binding for successor campaigns)
1. **The overdraw pool is NOT a face-skip pool.** Overdraw (1.56x spawn /
   1.16x junction) is dominated by PARTIAL overlaps; faces contributing ZERO
   final pixels are only 6.8% (spawn) / 13.6% (junction) of spans. Only
   span-level clipping (s-buffer class) can touch the partial-overlap
   majority — and at the junction its fill-side pool is itself only ~12% of
   writes ≈ 1.6% of frame in fill terms; the overdraw campaign's "heavy views
   are visible-content-bound" re-ranking stands, reinforced.
2. **Scene ranking INVERTED vs the brief:** spawn has the higher overdraw
   factor but HALF the junction's skippable-face pool (big near walls
   partially cover many far faces; full coverage of whole faces is rare).
3. **Correctness kill: pure near-to-far reorder + face-skip is unshippable
   regardless of speed.** Every partially-occluded (unskipped) far face would
   repaint OVER nearer content (~39K px/frame at spawn) — far-wins-overlap is
   wrong rendering, not an artifact class to audit. Only the pre-pass
   mitigation shape (painter order kept, coverage from nearer content) is
   correct at all, and its pool is what was measured above.
4. Method keeper: 01C00E08 pkt_done shade blits give REAL per-face span
   groups in any SHADEPASS=1 trace — use them, never column heuristics.

## Ladder / tree state
- Step 1-3 vacated by the gates: NO kernel bytes written, NO flags added,
  tree untouched (kernel stays 3656/3680 with 24B free). Default build
  re-verified green post-campaign (make MULTIROOM=1 JERRYPOSE=1 →
  openlara.cof 1413496B). Ship bins confirmed in tree (mrt.bin md5
  40ba1535…, matches scratchpad bins_ship). No OCC_*.cof staged — there is
  no implementation to A/B, and staging twins of an unbuilt flag would be
  theater.
- Analyzer: scratchpad/occ_analyze.py (parse + simulate_a/simulate_b +
  true-pool ceiling), reusable for any future coverage-culling proposal.
