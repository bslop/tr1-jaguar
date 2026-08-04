# Cull-walk early-out — SCOPE (2026-07-23)

Cobweb's sim-side note: "40% of GPU wall survives with every face culled —
each face still staged+transformed+cull-tested. Hierarchical/room-level
early-out and LOD attack this directly." Scoped against the shipping play
config with the now-available pc-histogram + GPU cycle counters (jsim
silicon fidelity).

## The opportunity is real and large

- **82% of Tom's GPU busy is per-face SETUP**, only 18% is rasterization
  (ALLCULL 291M vs baseline 355M cyc, 1200f spawn). Tom is 75% of the frame
  → **setup ≈ 61% of the whole frame.** Even a fraction of this dwarfs
  RUNBATCH's +4%.
- Setup = (a) the vertex PRE-PASS (transforms every room vert to SX/SY_BUF
  once per room, O(vcount), imul32 chains) + (b) the per-face loop (plane
  load, N·C backface test, cached-vert load, area/XCULL screen test).

## Room/cluster-LEVEL frustum early-out: MEASURED DEAD (~3% ceiling)

`ROOMCAP=1` (current room only) vs baseline (all dispatched rooms):
- spawn:    100% of baseline (only 1 room dispatched)
- corridor: 97% of baseline (driven, multi-room hallway confirmed)

**Neighbor rooms are ≤3% of GPU busy.** The 68k already does the coarse
cull — portal_rect visibility + HOPDIAL hop-cap (room±1) gate which rooms
dispatch, and STAGEDIET's N·C test backface-culls per face BEFORE staging.
A Tom-side room-level frustum test recovers ≤3% for significant extractor+
kernel work. **Do not build the room-level version.**

## The OPEN question: WITHIN-room cluster culling

ROOMCAP measures neighbor rooms only. It does NOT measure the within-room
waste: front-facing faces OUTSIDE the view frustum (the walls beside/behind
you that still face you) get fully staged+transformed, then rejected by the
post-staging screen-cull/XCULL. STAGEDIET culls backfaces cheaply but does
NOT frustum-cull front-faces before staging. This fraction is UNMEASURED and
is the only path that could beat 3%.

**KILL-CRITERION PROBE (build first):** add two DRAM counters to the kernel —
`n_staged` (faces passing N·C, incremented at scan_done) and `n_rastered`
(faces passing screen-cull, at the launch). Run spawn + corridor. The
cullable fraction = 1 − n_rastered/n_staged. Decision:
- **< ~20% cullable** → within-room culling is also a dead end; the 82% is
  mostly drawn geometry. Redirect to LOD (below). STOP.
- **> ~30% cullable** → cluster culling is worth designing (next section).

## IF the probe says GO: cluster-cull design

- **Extractor:** partition each room's faces into spatial clusters (natural
  fit: TR rooms are boxy — cluster by dominant face normal / the room's 6
  wall-planes, ~4–8 clusters/room). Emit per-cluster: an AABB (6×s16, room-
  local) + face index range. Reorder faces so a cluster is a contiguous run.
- **Kernel (per room, once):** transform the 4 frustum side-planes into
  ROOM-LOCAL space (camera is already room-local under STAGEDIET — cheap).
- **Kernel (per cluster):** AABB-vs-4-planes test (the fast n-vertex AABB
  reject, ~12 instr) in room-local space, NO projection. Outside → skip the
  whole face range (both pre-pass verts if the cluster owns them, and the
  face loop). Amortizes when cluster ≫ ~8 faces; rooms have ~20–40/cluster.
- Cost: ~70–100 kernel bytes (kernel is at 3664/3680 — needs a byte diet
  first, e.g. more alt-bank residents) + extractor clustering pass.
- Risk: MODERATE. Touches the hot loop and the pre-pass/vert ownership;
  cluster boundaries must not split a vert used by two clusters (or transform
  it twice). The vtxcache pre-pass currently transforms ALL verts up front —
  cluster-skipping the pre-pass is the hard part (chicken-and-egg with vert
  ownership); a first cut can cluster only the FACE loop (skip staging) and
  leave the pre-pass whole, capturing the per-face setup but not the vert
  transform.

## The parallel lever, independent of culling: LOD (data-side)

Cobweb's other word. Fewer verts+faces per room cuts BOTH the pre-pass and
the face loop with zero kernel fragility — a pure extractor change. Tradeoff
is visual detail, tunable per room / by distance. This is the biggest SAFE
lever on the 82% and does not depend on the cull-fraction probe. Scope
separately: per-room vert/face census + a decimation target + CRT visual A/B.

## Recommendation

1. Build the n_staged/n_rastered counter probe (~1 hr, emulator-only). It's
   the kill-criterion for the entire cluster-cull effort and is cheap.
2. In parallel, census per-room vert/face counts and prototype extractor LOD
   (data-side, safe) — likely the higher-ROI lever regardless of the probe.
3. Room-level frustum early-out: SHELVED (measured 3%).

---

## RESOLUTION (2026-07-23, "do both": probe + LOD) — it's NOT the cull walk

Built the kill-criterion probe (CULLCOUNT=1: n_staged @ $1C0000, n_rastered
@ $1C0004) and swept LOD. Results decompose Tom exactly:

| component | % of Tom | % of frame | reachable by |
|---|---|---|---|
| **vertex pre-pass (transform ALL verts)** | **65%** | **~49%** | MMULT hardware / real decimation |
| per-face loop setup | 17% | ~13% | cluster-cull (42% cullable → 7% of Tom ceiling) |
| rasterization | 18% | ~13% | RUNBATCH etc. |

Measured via PREPASSONLY=1 (skip face loop → pre-pass only = 229.5M cyc) vs
ALLCULL (291M) vs baseline (355M), 1200f spawn, jsim silicon.

**Every cull/LOD lever is a dead end:**
- Room-level frustum early-out: ≤3% (68k portal+hopcap already cull rooms).
- Cluster-cull (within-room): 42% of faces cullable BUT only recovers the
  17% face-loop → **5.5% of frame ceiling**. Barely beats RUNBATCH, hard+
  fragile kernel change. NOT worth it.
- LOD via SUBDIV_MAX: DEAD. Shipping 3072 is already at the no-subdivision
  floor (geom 255KB @ 3072 vs 254KB @ ∞; 450KB @ 512). Face count is
  intrinsic TR geometry, not subdivision. Real decimation = big effort.

**THE REAL TARGET: the vertex pre-pass — 65% of Tom, ~49% of frame — runs
in SOFTWARE imul32 (20 calls/vert-loop), and Tom's hardware MMULT matrix
unit is used ZERO times.** jas supports mmult (opcode 54). Converting the
per-vertex 3×3 rotate (+ project) from imul32 chains to MMULT is the single
biggest lever in the campaign — potentially multiples of every span/cull
optimization combined. THAT is the next campaign. Probes CULLCOUNT/
PREPASSONLY kept (flag-gated, ship build byte-identical c4f42fe3).
