# SECTOR RENDERER — SCOPE (opened 2026-08-01, user: "Sector Render Rewrite. Go big.")

Branch `sector-renderer`. Fallback line: `stable-2026-08-01-polygon-renderer`
(tag `checkpoint-2026-08-01-polygon-renderer`), byte-verified, assets backed up
outside the repo. **Nothing here is pushed.**

---
# 1. THE CASE (all measured on silicon, this project)

| fact | number |
|---|---|
| per-face SETUP share of Tom's GPU busy | **82%** (rasterisation only 18%) |
| cost of full 240-line rendering | **+4.4%** — pixels are nearly free |
| flat-shading the entire floor | **0.0%** — per-pixel work is not the cost |
| halving the scanlines | +2.6% |
| face count: Lara −45% | **+1 vsync rung** (7.50 → 8.57) |
| face count: Lara −13% | null |
| "launch count remains the whole story" | ~5000 Blitter launches/frame |

The engine is a general polygon pipeline whose cost is **O(faces)**. Every
Blitter-side lever lives inside the same ~35% of Tom and **all are now closed**
(flat floor, TRAPEZOID, phrase dest, BWOVER, span batching, halving scanlines,
DIVHIDE, occlusion, FARCLIP, frustum at both room and within-room scope,
Jerry co-transform, C→asm). The other ~65% is per-vertex/per-face work that
scales **only** with how much geometry Tom is handed.

**The thesis:** make the cost O(screen columns) instead of O(faces), and the
bottleneck lands on the resource already measured as nearly free.

## ☠️ WHAT THE THESIS IS *NOT*
The current renderer already draws horizontal affine-mapped spans, which is
what the Blitter wants. **A sector renderer does not make the FILL faster.**
Its entire win is upstream: far fewer, longer spans, and near-zero per-vertex
transform. Anyone who pitches this as "faster rasterisation" has misread the
profile — and that misreading is exactly how TRAPEZOID burned a campaign.

---
# 2. GATE MEASUREMENTS — TAKE THESE BEFORE WRITING ANY RENDERER CODE

TRAPEZOID's lesson, in this project's own words: *"a 'gate measurement' that
counts a quantity the IMPLEMENTATION cannot actually exploit is not a gate."*
Its step-0 counted RUNC runs the kernel could never batch across, predicted
4.52 rows, delivered 1.07, and the campaign died after full implementation.

All three gates below are **offline and free** (extractor + Python, no board).

## G1 — DOES TR1 GEOMETRY ACTUALLY FIT A SECTOR MODEL?
Classify every level-1 face as:
  (a) **flat** — floor/ceiling of a sector, horizontal or sloped-planar
  (b) **wall** — vertical, spanning a height range on a sector boundary
  (c) **neither** — genuinely 3D (bridges, ramps, statics, decoration)
**KILL: if (c) exceeds ~15% of drawn faces, you are maintaining TWO renderers
and the win evaporates.** TR1 has slopes (Build-style, fine), rooms above rooms
(portals, fine) and real 3D props (STATICS=1 bakes stalactites into room
geometry — those are category (c) and may be a large share).
★ Reuse `ROOMAUDIT` / `MERGEAUDIT` plane classification; it already computes
per-face normals and plane keys.

## ✅ RESULTS — ALL THREE GATES PASS (measured 2026-08-01)

| gate | threshold | measured | verdict |
|---|---|---|---|
| **G1** sector-model fit | fallback <15% | **6.3%** (487/7750 faces) | PASS |
| **G2** span budget, room 26 | sector <1500/frame | **674** vs 1651 now = **2.4x fewer** | PASS |
| **G3** transform budget | as low as possible | **8804 -> 2381 points, -73%** | PASS |

★ **The win is dominated by G3, not G2.** Setup is 82% of Tom and rasterisation
only 18%, so a 73% cut in transformed points (each also ~half the cost: 2D
rotate + one divide vs 3D rotate + two) matters far more than 2.4x on spans.
G2's job was only to prove the fill does not REGRESS. It does not.

☠️ **Honest caveats on G2** (`tools/g2_span_budget.py`, z-buffered offline
rasteriser at the real constants FOCAL=160/FOCAL_Y=80/NEAR=64, 8 yaws from the
room centre):
- Coverage is 65-71%, not 100%, because the dump is ONE room; the rest of the
  view is portals into neighbours. Both counts are room-26-only.
- The sector figure assumes ideal per-row surface runs after z-buffering. A real
  portal renderer emits MORE (portal splits, overdraw), so **674 is a floor**.
- ☠️☠️ **The first run of this was WRONG and flattered the sector renderer 3.1x**
  because it did not backface-cull. Adding the cull with the sign backwards then
  collapsed coverage to 12-30% (impossible inside a closed room) — the tell that
  saved it. **Check coverage is plausible before believing any span count.**
- New costs the gates do NOT measure: portal traversal, clipping windows,
  per-column height computation. Those eat into the saved setup by an unknown
  amount. **This is the residual risk and no gate can retire it.**

## G2 — SPAN/LAUNCH BUDGET, THE THING THAT ACTUALLY COSTS
For representative cameras (room 26, room 13, room 0 spawn), compute what a
sector renderer would ISSUE: wall spans + flat spans per frame.
Current baseline: **~5000 launches/frame** (38400 px ÷ ~9 px/span × 1.21
overdraw). **KILL: if the sector renderer does not get this under ~1500,
the O(faces)→O(columns) claim is false for this content.**
⚠️ Count spans the way the KERNEL would emit them, not idealised columns.

## G3 — TRANSFORM BUDGET
Count per-frame transformed vertices: currently every room vertex (306–712 per
room) plus Lara's 300. A sector renderer transforms **wall endpoints in 2D**.
**Target: <100.** This is where the 82% setup cost dies, and it is the single
strongest reason to believe the rewrite.

---
# 3. THE BIG ARCHITECTURAL RISK — COLUMNS ARE WRONG FOR THIS BLITTER

Doom draws **vertical 1-pixel-wide columns**. The Jaguar Blitter is a
**phrase (64-bit) oriented DMA engine**: a 1-px-wide, N-tall blit moves 1 byte
per row and wastes 7/8 of every bus cycle. Naïvely porting Doom's inner loop
would be *slower* than what we have, on the machine's own terms.

Three ways out — **G2 must be evaluated for whichever is chosen**:
1. **Horizontal wall spans.** Rasterise walls as trapezoids in horizontal
   scanline spans (what the current kernel already does well), driven by
   sector edges rather than by faces. Keeps the Blitter happy; keeps the win
   (which is upstream anyway). **RECOMMENDED — it is the conservative option
   and loses nothing, because the fill was never the problem.**
2. **Rotated framebuffer** (render 90°, walls become horizontal runs). Then
   FLATS become columns and inherit the same pathology. Also fights the OP.
3. **True column rendering.** Only if a probe shows 1-px blits are not
   catastrophic. Assume they are until measured.

---
# 4. THE OTHER HALF: USE THE OBJECT PROCESSOR

The OP composites scaled bitmap objects per scanline **at zero CPU and zero
Blitter cost**, and we currently use it for exactly one thing — stretching one
framebuffer. Lara is **425 faces for ~100 px of character**, and one phase
profile put her at **38% of frame time**.
- **Lara / enemies / pickups as OP sprites** (pre-rendered angles, streamed from
  the GameDrive SD) would cost essentially nothing.
- Hybrid is available: polygonal up close, OP sprite at distance.
☠️ **Constraint, already known:** `video.h` records that the OP hardware scaler
**blanks the display under heavy multiroom fill** — bus starvation. Any OP-heavy
design must solve bus contention first. Corroborating good news from the cobweb
silicon probe: the Blitter is *not* the bottleneck and blit/compute overlap is
silicon-exact in jsim, so jsim can be trusted to model this before flashing.

---
# 5. PHASING (each phase ends in a measurement, not a milestone)

- **P0 — gates G1/G2/G3.** Offline. Go/no-go. *Nothing else starts until these
  pass.*
- **P1 — extractor emits sector data**: per-sector floor/ceiling planes, wall
  segments with texture refs, portal links. Alongside the existing geometry, so
  both renderers can run from one asset build.
- **P2 — a 2D sector renderer, flats only**, no walls, into the existing
  framebuffer. Verify against the polygon renderer at a fixed camera.
- **P3 — walls**, as horizontal spans. Now the span budget is real: re-measure
  G2 against reality.
- **P4 — portals + clipping windows** (Build-style x-range recursion).
- **P5 — fps A/B in ROOM 26, never at the spawn** (see below).
- **P6 — Lara/objects**: keep the polygon path for her initially; OP sprites are
  a separate campaign.

---
# 6. RULES CARRIED FORWARD (bought with real time this session)

- ☠️☠️ **BENCHMARK IN ROOM 26 OR 13, NEVER AT THE SPAWN.** Only one room draws;
  the spawn (306 faces vs Lara's 425) is the *worst* room in the level for any
  room-geometry lever. The atlas-tiling win reads −6.2% there and **−23.1%** in
  room 26. Use `SPAWNAT_ROOM/X/Y/Z/YAW`.
- ☠️☠️ **Read a campaign to its VERDICT before re-opening it.**
- ☠️☠️ **Never leave a broken control arm on the board**; restore immediately.
- ★ **A static camera separates a race from geometry** in one capture.
- ★ **Decode the palette before theorising about a colour.**
- ☠️ `rm -rf build` before any A/B; verify the flag landed on the command line.
- ☠️ Any layout change re-rolls A10 — roll `PADTEXT` (0/272/544/816) before
  suspecting the change itself.
- fps rig: `scratchpad/fps.py` — active-bbox detection (the capture card has
  served 720x480, 1920x1080 AND 3840x2160 in one session), Otsu threshold,
  ≥3-field gaps, and **report the MEAN**.

---
# 7. WHAT IS PRESERVED IF THIS FAILS

`stable-2026-08-01-polygon-renderer`: Lara's head solid, floors clean,
7.55 fps, md5 `66eefbbc0a265919e9ee2a92dab8cc83`, assets at
`../../../../RESTORE_2026-08-01_headfix_spanshade/`. Plus two characterised
open defects handed forward: the **A1 PIPELINE race** (confirmed, −89% under
`PIPESTAGE=0` at a cost of 25% fps, narrowed to a one-shade-step draw-order
flip) and the **missing near-plane clip**.
