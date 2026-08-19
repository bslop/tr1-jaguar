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

### B1. ★ A ~91,000-cycle FIXED cost per frame, before any geometry
Fitting all 38 rooms (`tools/room_cycles.py`, jtest at silicon fidelity):

    cycles ≈ 91,000 + 712 × faces

The evidence is the light rooms: room 7 draws **19 faces** and still burns
**104,513 cycles**; room 32 draws 43 faces for 122,091. That is ~91k of setup
that every room pays whether it draws 19 faces or 624.

**Why this is the most interesting number in the sweep:** the campaign so far
has attacked per-face cost (flat-merge, face culling, LOD) and found face count
is NOT the lever (r34 −36% faces = same fps). A fixed per-frame cost explains
that: cutting faces cannot touch the 91k floor.
⬜ NEXT: profile what the kernel does before its first face — param block
setup, atlas/CLUT staging, room-list init. `jtest` can time a 0-face room.
⚠ CONFIDENCE: HIGH for the fit, UNKNOWN for what the 91k is spent on.

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
