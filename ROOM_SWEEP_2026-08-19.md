# Room sweep — defects and frame-rate opportunities (2026-08-19)

Offline sweep of all 38 Caves rooms, run while the rig was with other projects.
**Nothing here is silicon-verified.** Every item says which instrument produced
it and how much that instrument can be trusted, because two of them lied to me
tonight and one of them (`room_cycles.py`) is structurally blind to 30% of the
frame.

---

## A. DEFECTS

### A1. 64 doorcell mismatches — cells baked OPEN that recompute as WALL
`tools/mrt_boundary_audit.py`, run over the whole level:

    0 phantom seam floors (of 481 seam cells), 64 doorcell mismatches
    e.g. DOORCELL cell (18, 2): baked 0x7ffe want 0x7fff   [and 63 more]

`0x7FFE` = open, `0x7FFF` = wall. So 64 `floor == -127` cells are baked
passable where a recompute of the wall-portal footprint says they are solid.
**If the bake is wrong, Lara can walk into 64 cells that should be walls.**

⚠ CONFIDENCE: MEDIUM. The audit's recompute is a heuristic and has been wrong
before — run 74 found 7 "dead doors" that were the harness ignoring that a
portal has a HEIGHT. Do not patch on this alone.
⬜ NEXT: pick 3 of the 64, spawn Lara at each (`SPAWNAT_*`) and walk into the
cell. One turn, and it settles whether the bake or the audit is wrong.

### A2. Floor coverage is CLEAN — recorded so nobody re-runs it
`tools/floor_coverage.py`: **0 of 1,985 walkable cells across 38 rooms** lack a
mesh above them. The seam-floor campaign's 403 patches still hold (A1's audit
reports 0 phantom seam floors). This is a negative result worth keeping.

### A3. Still open from earlier runs (not re-tested here)
  * r17's dead doors and the 8 untestable door seats (`HW_TESTCARD`)
  * the Caves level does not load after CAVES.JV (user, 2026-08-19) —
    first suspect is the missing CAVSLOAD.DAT, now emitted by build_cof.sh

---

## B. FRAME-RATE OPPORTUNITIES

### B1. ★ A ~98,000-cycle FIXED cost per KERNEL INVOCATION — ⚠ SEE THE CORRECTION
Fitting all 38 rooms (`tools/room_cycles.py`, jtest at silicon fidelity):

    cycles ≈ 91,000 + 712 × faces

The evidence is the light rooms: room 7 draws **19 faces** and still burns
**104,513 cycles**; room 32 draws 43 faces for 122,091. That is ~91k of setup
that every room pays whether it draws 19 faces or 624.

**Why this is the most interesting number in the sweep:** the campaign so far
has attacked per-face cost (flat-merge, face culling, LOD) and found face count
is NOT the lever (r34 −36% faces = same fps). A fixed per-frame cost explains
that: cutting faces cannot touch the 91k floor.
Least squares over all 38 rooms: **cycles = 98,284 + 701 x faces, R^2 = 0.993**.
In the lightest rooms the fixed part is 82-94% of the whole kernel cost.

☠ **CORRECTION (same day, before anyone acts on it).** I first wrote this up as
a per-FRAME cost and called it the most interesting number in the sweep. That
overstates it. `room_cycles.py` times ONE standalone kernel invocation for ONE
room, so the intercept is per INVOCATION — and the shipping build does not kick
per room. `NODISPATCH` (per-room kicks) is absent from build_cof.sh and
gbuild.sh; the shipping path is SINGLE-DISPATCH, which queues up to 39 rooms
and dispatches them together (main.c:10635). So the shipping frame pays this
once, ~98k cycles ~= 3.7 ms at 26.6 MHz, not once per visible room.

That is still worth having - it is a floor no face-count lever can touch, which
is a candidate explanation for why flat-merge and LOD both measured flat - but
it is a few percent of a 150 ms frame, not the lever I implied.

⬜ WHAT WOULD SETTLE IT: does the batched path re-pay any of that per queued
room (room-list init, per-room clip setup) or only once? Instrument the real
build - count cycles between the dispatch and the first face of room 2 - rather
than inferring it from a single-room harness.
⚠ CONFIDENCE: HIGH for the fit itself. The per-frame IMPACT is unproven and my
first reading of it was wrong.

### B2. Heaviest rooms, by kernel cycles (yaw 0, room-centre camera)

| room | cycles | faces | cyc/face |
|---|---|---|---|
| 34 | 535,170 | 624 | 858 |
| 18 | 464,858 | 508 | 915 |
| 10 | 462,905 | 517 | 895 |
| 22 | 401,383 | 436 | 921 |
| 12 | 352,556 | 343 | **1,028** |
| 36 | 343,767 | 365 | 942 |

Total across 38 rooms: 9,167,699 cycles; median 252,949.
r22 is the known worst-fps room (3.69) and sits 4th here — consistent.

### B3. Room 12 is the efficiency outlier among heavy rooms
1,028 cycles/face against ~860–920 for its neighbours: ~15% more work per face.
That is geometry SHAPE (long thin spans, big screen-space faces), not count.
⬜ NEXT: `tools/occ_analyze.py` and `overdraw.py` on r12 specifically — if its
overdraw is above the 2.07x level average, span-merging or ordering pays there
first.
⚠ CONFIDENCE: MEDIUM — cyc/face is a ratio of two numbers from the same blind
instrument.

### B4. ☠ WHAT THIS SWEEP CANNOT SEE — read before acting on B1–B3
`room_cycles.py` prices the **geotex kernel only**. It cannot see:
  * the **Blitter fill**, measured at **30% of the frame** (`NOFILL=1`:
    6.67 → 9.32 fps) and **2.07x overdraw**
  * the 68000's own work, which the HUDTEXT finding showed can dominate
    (8 glyphs of text = +56% when removed)
  * bus contention, which is what tonight's whole FMV bug turned out to be
A room that looks cheap here can still be slow, and vice versa. Any suggestion
above is a HYPOTHESIS until a driven fps capture on silicon agrees.

---

## C. HOW TO SPEND THE FIRST RIG TURNS ON THIS

1. **B1** — time a minimal-geometry room to isolate the 91k fixed cost. Offline
   first (jtest), rig only to confirm.
2. **A1** — three spawn-and-walk tests against three of the 64 doorcells.
3. **B3** — overdraw on r12 vs the level average.
Each is one question per turn, which is the rule the rig enforces anyway.

---

## D. ☠ tools/overdraw.py IS NOT TRUSTWORTHY — do not quote its numbers

Tried to answer B3 (is r12's overdraw above the 2.07x level average?) and could
not. Three real defects found and fixed, one still open:

  ✅ it pointed JTEST at `~/Documents/Git/cobweb` - the USER's checkout, not
     this project's own clone - so it had been dead with FileNotFoundError.
     (jaguar-shared DEVELOPMENT.md: every project uses its OWN clone.)
  ✅ it passed `mrt_atlas.bin` into the assembler template by a path my disc/
     migration had not covered, so every room came back DNF.
  ✅ it captured **0x001C0020**, a DRAM address NOTHING WRITES, and therefore
     reported `0 blitted px = 0.00x screen` for every room - a clean false
     null. The kernel accumulates into GPU SRAM: ODP_PX $F03EF4 / ODP_N
     $F03EF8 (gpu_geotex.gas:361). ⭐ The kernel's own ODRAWS comment records
     someone reading ~0 from this same shape of mistake and concluding "both
     probes sit on a dead rasteriser path" - and being WRONG.
  ⬜ STILL BROKEN: with the capture pointed at ODP_PX the values are not
     credible - r5 returns 0xFFFFFDF8 (-1032 read unsigned) and the rest read
     0.13-0.15x screen, far too low for a full room render. I added code to
     zero the accumulators before the kick and **the output did not change by a
     single digit**, which says my zeroing never executed or the capture is not
     reading the counter the kernel writes. Next: dump $F03EF4/$F03EF8 straight
     out of jtest after a known-good run and compare against the printed value
     before touching the harness again.

⚠ So B3 is UNANSWERED. The 2.07x overdraw figure in the project's memory came
from a different measurement and still stands; nothing here contradicts it.
