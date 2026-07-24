# Implementation spec: Gouraud + the three perf levers

Written 2026-07-20 at the end of a context window, for the next session to
execute without rediscovery. Baseline: ~~6.00 fps (jsim, within ~1% of silicon)~~
**RETRACTED 2026-07-20: silicon says 4.72 fps for the JERRYPOSE build (jsim
over-predicts 27% — see COBWEB_GAP_jerrypose_fps_overprediction.md). jsim cannot
price 68k<->Jerry work moves; HW is the only oracle for those. Lever A's 1.49x
ceiling is also wrong: the 68k phase is 6% of the frame on silicon, not 33%.**
`make MULTIROOM=1 JERRYPOSE=1`. Tree clean, zero build errors.

**Ground rules learned the hard way today — follow these or repeat my mistakes:**

- `make clean` on EVERY flag change (make does not track `-D`).
- A/B needs TWO clean builds and a pixel-diff proving the variable actually changed.
- Animation cannot be verified by one screenshot — diff two frames ~30 vblanks apart.
- Never trust a derived number resting on a constant you did not measure. Four
  wrong conclusions today came from exactly that.
- Verify SRAM/PARAMS changes on the **PROFILE+AUTOSTART multi-room dispatch**
  build, not a single-room build (single-room reads PARAMS once and hides clobbers).

Measure with:
```
jagemu run <cof> --frames 2600 --fidelity silicon --pc-histogram --map build/openlara.map
jagemu screenshot <cof> --frames 2600 --fidelity silicon -o f.png   # fps bar row 28, px*3/100
```

---

## Current honest profile (release build, corrected async-Blitter model)

```
wall: Tom busy 67.1% (Blitter 20.6%) | 68k awake 33.1% | Jerry 98.2%
Tom cycles: blit 30.8%  jump_refill 14.5%  mem_external 5.5%
            stall_alu 4.7%  stall_flags 3.0%  stall_div 2.8%  (IPC 0.56)
```

---

## GOURAUD — FINAL 2026-07-20: PER-FACE RAMP SHADING IS LIVE ON HARDWARE

`make MULTIROOM=1 JERRYPOSE=1 SHADEPASS=1` + RAMP_PAL bins = the shipped-
quality build (probes/RAMP_shaded2.cof): per-face lighting from real vertex
light, 8 ramp levels, atlas 470->258KB, HW 3.93 fps vs 4.72 unshaded, Lara
animating, look matches legacy baked (mean luma 77 vs 68 — tune FACM or the
face_k curve in tr2jag_multiroom.py if wanted; k=(223-avg)>>5 today).
Kernel 3596/3600 (SX_BUF moved $F03E00->$F03F24 to raise the ceiling).
Caves k-histogram: k3-k5 dominate, only 1% k=0 — the skip-if-bright lever is
useless in Caves, so the fps recovery is:
1. **Phrase-mode the shade pass** (2 DRAM accesses per 8px instead of per
   px): needs partial-phrase edges — CLIP_A1 in the working kernel context
   (test as a SHADEPASS variant), or pixel-mode edge stubs + phrase middle.
2. jopt the kernel (proven -68B and small speed; fixture recipe in memory).
Then TASK 6 (vertical Gouraud): interpolate k along the edge chains like
U/V (uv_edge already walks u/v per scanline — add a third DDA for k, update
SHADEK per scanline instead of per face; SRAM budget is the fight again).
NOTE the once-per-face k extract sits in the stage loop — per-scanline k
replaces the SHADEK write, not the extract.

## GOURAUD — END-OF-DAY 2026-07-20: SHADE PASS PROVEN, TASK IS NOW MECHANICAL

**`make ... SHADEPASS=1` renders on real silicon**: gpu_geotex chains a 2nd
blit per span (`$01C00808` DSTEN|LFU-OR|DSTA2, B_SRCD=k, re-store A2_PIXEL+
B_COUNT only) and the whole scene shows the texel|k shift. Chained per-span
RMW works in the production kernel; 16 rounds of STANDALONE blit probes all
failed (first-blit-only anomaly — unsolved, handed to cobweb) so never trust
a standalone blitter probe again: test in-kernel. Cost of the always-on
pixel-mode pass: 4.72->3.89 fps; recover by (a) skipping k=0 faces (most of
Caves is full-bright lvl3) and (b) phrase-mode for long spans. Remaining
work for per-face flat shading: build with RAMP_PAL=1 bins, kernel reads
per-face k (avg of VERTS+6 light, k=(255-light)>>5, clamp 0..7) into the
B_SRCD pattern, skip launch when k=0. SRAM budget: 3576/3584 used by the
experiment — the k-fetch needs the ~50B freed by dropping the fixed-k movei
and reusing face-setup registers; budget is THE constraint, plan carefully.
Then task 6 = per-scanline k interpolated along edges like U/V.

## GOURAUD — STATUS 2026-07-20 (supersedes the section below; keep for context)

Done today (see memory `project_gouraud_ramps.md` + COBWEB_REQ_srcshade_gourd_compute.md):
- **Silicon probes (BLITPROBE builds, probes/BP_srcshade*.cof):** SRCSHADE /
  GOURD write ZEROS on an 8bpp dest (16-bit-only path, TRM literal). Worse:
  v2-v4 suggest any D-path blit (DSTEN / D-LFU / ADDDSEL / PATDSEL) may WEDGE
  the Blitter so subsequent blits write zeros — v5 (order-controlled) decides
  whether that is real or a harness artifact. If D-path RMW is unusable, FB8
  runtime shading via Blitter is dead; options: keep legacy baked tiles, or
  CRY16 rework (SRCSHADE on CRY Y was HW-proven 2026-07-07 a340f02).
- **RAMP_PAL=1 extractor DONE + verified** (tr2jag_multiroom.py; legacy path
  regression-checked byte-identical). 30 bases x 8 shades, atlas 470->258KB.
  Full-bright ramp build renders correctly in jagemu (probes/RAMP_fullbright.cof,
  5.97 jsim-fps = neutral). Repo mrt_*.bin/mrt.h currently HOLD THE RAMP
  OUTPUT — `git checkout -- mrt*.bin mrt.h mrt_lara.h mrt_spawn.h` restores legacy.
- Per-vertex light already flows: extractor emits real 0..255 light in VERTS+6
  (255=bright); kernel ignores it today. k = (255-light)>>5 -> ramp delta.

## GOURAUD SHADING — **MY "IT IS FREE" CLAIM WAS WRONG. READ THIS FIRST.**

`gpu_geotex.gas:107` already says it: **"No SRCSHADE/GOURD: they would corrupt an
8bpp palette INDEX."** We render FB8 — 8bpp INDEXED with an OP CLUT. `B_IINC`
adds intensity to the pixel VALUE, but our pixel is a palette INDEX, so adding to
it jumps to an unrelated CLUT entry = colour garbage, not shading. The hardware
mechanism exists; it is incompatible with our pixel format. Ignore the "free"
framing below-the-line in earlier notes.

### The real options, in order of sanity

1. **PALETTE RAMPS (the Doom/Quake trick — recommended).** Rebuild the CLUT so
   each base colour occupies a contiguous run of N brightness steps (e.g. 32 base
   colours x 8 shades). Then adding to the index DOES walk brightness, and
   `SRCSHADE`/`B_IINC` becomes correct and stays free at the pixel level.
   COST: asset pipeline, not kernel — palette reorganisation + atlas re-index
   (`tools/`, `mrt_pal.bin`, `mrt_atlas*.bin`), and you trade colour variety for
   shade levels. Clamp handling matters: `B_IINC` saturates, so put ramps in
   ascending order and keep them from bleeding into the next base colour.
2. **16bpp CRY framebuffer** — CRY carries intensity natively, so Gouraud is
   natural. COST: doubles fill, and the Blitter is ALREADY 30.8% of Tom. Also
   prior notes say the OP scaler struggles under heavy fill. Probably a net loss.
3. **Per-face flat shading into a ramp palette** — pick one ramp entry per face
   from its normal. Much cheaper than (1) to implement, gives faceted-but-lit
   (TR1-PC-like) rather than smooth. Good stepping stone: it proves the ramp
   palette works before adding interpolation.

**Suggested order: build the ramp palette, do (3) to validate it end-to-end, then
add edge interpolation to reach true (1).** The per-vertex plumbing below is still
correct and needed for either.

### Per-vertex plumbing (still valid, still needed)

GOOD NEWS: the blob format ALREADY reserves the slot. `main.c:426` writes
`w[3]=255; /* full-bright */` per vertex, and `gpu_geotex.gas:37` documents the
VERTS record as `+0 s16 x, +2 s16 y, +4 s16 z, +6 u16 shade(IGNORED)`. So the
stride does NOT need widening (that would have cost Tom DRAM bandwidth on its
hottest read) — the field exists and is simply pinned to 255 and ignored.



---

## LEVER A — overlap the 68k with Tom (biggest: ~1.49x ceiling, ~9 fps)

Today the frame is serialised: 68k builds (33%) -> sleeps in `gpu_sync` while Tom
draws (67%). Running frame N+1's logic inside that sleep hides the 68k entirely.

- `OVERLAP` flag already exists (Makefile:55; `main.c` 1419/3191/3328) from the old
  GEOMXFORM path — read it before writing new machinery.
- Triple buffering exists (`fb0/fb1/fb2`, `video.c`). NOTE `HALFRES` aliases
  `fb2=fb0` — irrelevant since HALFRES is last-resort.
- **HAZARD (the whole difficulty):** Tom reads `camblk` and `lara_blob` for the
  WHOLE render. Game logic for N+1 updates camera and Lara state. Double-buffer
  both, or restrict the moved work to code that touches neither.
- Frame layout: game logic + sort (before `pfA`, ~main.c:2441-2660) -> clear
  (`pfA`->`pfB`) -> render kick + `gpu_sync` (`pfB`->`pfC`) -> lara finish -> flip.
  The move is: kick Tom, run N+1 logic, THEN sync.
- Incremental path: move only the pure-computation logic first (Lara movement,
  collision, animation advance — they write game state, not render buffers),
  measure, then consider the sort and blob builders.

## LEVER B — overdraw (blit is 30.8% of Tom, the single biggest term)

Painter's algorithm, no z-buffer, so overlapping faces rewrite pixels.

- **ESTIMATE ONLY: ~7x screen coverage/frame** (252.4M blit cycles / 260 frames
  / ~1.77 cyc-per-px from cobweb's 256px=450tick probe). DO NOT ACT ON THIS
  NUMBER — it rests on an imported constant. **Measure first:** add a span-pixel
  counter in `sp_fill` and read it, exactly as the `bwait` spin counter was done
  (guard-derived, ~22 B, negligible cost).
- `gpu.timing.blit` counts only GPU-launched blits — the 68k's `blit_band` clear
  is NOT in it, so it cannot be used as a 1x-screen calibration reference.
- If overdraw is real, options in order of cheapness: tighter portal/frustum
  culling per face, front-to-back with a span-coverage skip, or reducing the
  full-screen clear where geometry is guaranteed to cover.

## LEVER C — `jump_refill` = 14.5% of Tom

- `jopt` is FIXED and safe (two soundness bugs found and fixed via this project),
  but its accepted fills landed in COLD init code: **+0.002%**. The hot raster loop
  had no legal donors.
- So this is hand work on the raster inner loop (`face_loop` / `sp_fill` /
  `uv_edge`). Fill delay slots manually, verify each with `jtest diff` on the
  captured region, and pixel-diff the whole render.
- Working jopt fixture (if retrying): capture the VERTEX CACHE `0x001E10C0`, NOT
  the framebuffer (the Blitter writes that and does not run in a standalone-core
  harness), and zero-fill the capture region first or a post-render snapshot
  already contains the expected output and nothing appears to change.

---

## SRAM repack (needed if Gouraud does not fit in 58 B)

My repack raised the ceiling 3584 -> 3736 and was REVERTED as broken (rendered
512x8). Known faults: (1) `AU` was placed at `$F03F0C`, INSIDE the PARAMS block
`$F03F00-$F03F1F`, clobbering params[3] fb / params[4] camblk; moving AU..BVSL to
the genuinely free `$F03F20-$F03F3F` did NOT fix it, so there is at least one MORE
fault. Free space: `$F03F20-$F03F3F` (32 B) and `$F03F74+` (140 B). The lowest
variable sets the code ceiling (`CAM_BUF $F03E98` would give 3736).

---

## Also open

- **Passport** (see memory `project_perf_campaign_2026-07-20-passport.md`):
  renders as a small flat card upper-right; should be a large OPEN book
  lower-centre with prompts. Pointing `rsrc[0]` at pose 4 changed NOTHING, so
  identify which blob actually reaches the kernel FIRST.
- **Options ring:** the missing option models (74 DETAIL, 75 SOUND, 76 CONTROLS,
  77 GAMMA) are NOT in `TITLE.PSX` (it has only 71 PASSPORT, 73 HOME, 81 UZIS).
  They live in the level files — needs a second ring + selection UI.

# STAGE DIET — early backface cull before vert/UV staging (TOP LEVER, 2026-07-20)

WHY NOW: silicon probe session (cobweb calib, bench_20260720_jagB.log)
measured staging-under-blit contention 0.64 (a GPU external load under an
active blit pays +2.7 cyc marginal vs 4.2 free; jsim charged 0). Staging =
~23% of frame and roughly half of dispatched faces are backfaces that pay
full staging (4x stage_vert + 2n UV loadw) before render_pkt's screen-space
cull. Culling them BEFORE the fetch saves loads at the contended price.
ALLCULL ladder (9.55 vs 3.89 fps) bounds the whole stage+project+cull
pipeline at ~59ms; the early cull reclaims a large fraction of the backface
half, cost ~20-24 cyc plane test on every face. Est +0.4-0.6 fps.

DESIGN
- Extractor (tr2jag_multiroom.py): per face compute N = cross(v1-v0, v2-v0)
  in ROOM-LOCAL coords, d = N.v0. Quantize: shift N right until all three
  components fit s16 (shift d identically; only the SIGN of N.C - d
  matters). Emit 8 bytes appended per face record: nx,ny,nz,d_hi as 4x s16?
  NO — d needs range: emit nx,ny,nz s16 + d s32 = 10B, pad to 12B for
  alignment (record = 6n+12). SIGN CONVENTION: must match the kernel's
  screen cull (ar<0 = backface); verify by replaying both tests in the
  emulator and counting disagreements (target: early cull only ever KEEPS
  what screen cull keeps — conservative epsilon, see below).
- Kernel (face_loop, BEFORE stage_loop): C_local = camera - room offset
  (68k already has camera world pos; add C_local to the per-room dispatch
  entry or params — zero GPU cost). Test: load N,d (3 loads), t =
  nx*cx + ny*cy + nz*cz - d via 3x imult (32-bit ok after quantization),
  if t < -EPS jump to face-skip (advance FACE_CUR by 6n+12, FACE_LEFT--,
  next face). EPS conservative: bent TR1 quads make v0v1v2 normals
  approximate — the screen cull STAYS as the exact gate; early cull must
  only take clear backfaces.
- BUDGET BLOCKER: kernel at ~3670/3680 — the test needs ~60-90B. Get bytes
  from: PROFGPU=0 scraps (16B), another jopt pass, or moving the skip path
  to share stage_done's FACE_CUR-advance code (it already computes 6n; add
  the +12 constant there for both paths).
- Makefile/verify: LARA path UNCHANGED (Lara never backface-culled by this;
  her records keep old format? NO — kernel is shared. Either emit planes
  for Lara too (fine; skinning moves verts though! Lara is POSED at runtime
  — baked normals are WRONG for her). => gate the early cull on the ROOM
  phase only (RET_PHASE distinguishes room vs Lara phases already) or a
  per-packet flag bit (NCULF plumbing precedent). Lara keeps 6n records:
  format divergence per phase — simpler: put the plane block only in ROOM
  records and key record advance on phase.
- Regen env: RAMP_PAL=1 RAMP_K=30 RAMP_M=8 TEXSCALE=2 MRT_ROOMS=64
  SUBDIV_MAX=6144 LARA_MINAREA=0 LARA_WINDSIGN=-1 LARA_WINDSKIP=14.
  NOTE: _subdivide() splits faces — compute planes AFTER subdivision.

VERIFY LADDER: (1) extractor unit: replay plane test vs screen cull over a
spun camera in jagemu, mismatch count / conservativeness; (2) 0-px or
anim-phase-only diff vs current build with EPS=inf (test compiled in,
never fires); (3) jsim fps; (4) silicon fps + play-feel gate.

# OVERDRAW CAMPAIGN — design brief (2026-07-21, post-pipeline)

MEASURED CASE: content ladder (silicon): first two rooms = 114ms of the
140ms world; play-state stalls = 150-400ms heavy-view frames (pipeline
telemetry maxvbl/spind fingerprints). Painter order draws FAR rooms
first, then the near room repaints most of those pixels inside the
portal rect. A large fraction of far-room walk+fill is pixels that
never survive.

DIRECTION (ranked by risk):
1. S-BUFFER (span coverage, Quake-class): render NEAR-TO-FAR; per
   scanline keep covered-interval list; clip each incoming span to the
   gaps. Kills both fill AND walk for occluded far content (walk still
   pays edge DDA per line, but span emission clips to gaps; fully
   covered lines skip the blit entirely).
   - SRAM budget: interval lists for 240 lines won't fit GPU SRAM
     alongside the kernel (~10B free). Options: (a) DRAM interval lists
     (contended loads — but saved fill likely dominates), (b) per-line
     1-interval approximation (single covered [x0,x1] per line — rooms
     through portals are mostly convex-ish; measure miss rate offline
     first!), (c) coarse 8px-granule bitmask per line (40 bits = 8B/line
     = 1.9KB — DRAM or a stolen buffer).
   - ORDER FLIP: near-to-far requires flipping room paint order (batches
     currently far-first painter). Lara last stays (painter for her).
     WITHIN a room, faces are OT-sorted far-first — with an s-buffer the
     room's own order flips too (near-first) to self-occlude.
   - VERIFY OFFLINE FIRST: instrument jagemu (or a python replay of the
     span stream) to measure the actual overdraw factor per scene — if
     overdraw is only ~1.5x the s-buffer ceiling is small; ladder's
     114ms suggests 2x+ in the near rooms.
2. CHEAP PARTIAL: keep painter order but SHRINK far-room portal rects by
   the near room's OPAQUE silhouette... (portal rects already clip; the
   win would come from clipping far rooms to portal MINUS near-wall
   coverage — needs coverage anyway → collapses into option 1).
3. PER-FACE occlusion (plane tests between rooms) — cheap but only
   catches whole-face occlusion; misses partial. Fallback if s-buffer
   SRAM/complexity stalls.

FIRST STEP (offline, no rig): overdraw-factor measurement — replay the
kernel's span emission in jagemu (watch fb writes per pixel per frame,
count writes/pixel) at spawn + a stall view. Decides the ceiling before
any kernel bytes move.
PIPELINE INTERACTION: s-buffer state lives per-frame on the GPU; no 68k
involvement → composes cleanly with PIPELINE. STAGEDIET composes too
(fewer faces reach the walk).

MEASUREMENT RESULT (2026-07-21, jagemu watch, PLAY_PS combined build,
spawn scene): fb0 writes 5,434,260 / 900 frames, 137 renders total →
~118.9K writes per fb0-render; minus ~9.6K phrase clears ≈ 109K scene
pixel-writes vs ~70K visible → **OVERDRAW ≈ 1.56x at spawn ≈ 39K wasted
px/frame ≈ 8ms fill + similar walk share ≈ ~15% of the frame.** Junction
stall-views expected 2-3x worse (repeat measurement with a room-12
teleport before committing kernel bytes). s-buffer go/no-go = user call.

JUNCTION MEASUREMENT (room-12 spawn variant, PLAY_PS_r12.cof): 3.475M
writes / 37.3 fb0-renders = 93.1K/render − 9.6K clear ≈ 83.5K scene
writes vs ~72K visible = **OVERDRAW 1.16x — LOWER than spawn (1.56x).**
Portal rects already choke far rooms at junction doorways; heavy views
are heavy because their VISIBLE content is large. RE-RANKED ROADMAP:
(1) TRAPEZOID walk restructure (visible-span cost = the stall driver),
(2) s-buffer/overdraw = mid-tier (~10-25% of fill+walk, spawn-class
views only), (3) visibility-loop early-out (~5ms 68k). Spawn header
restored; tree = ship bins.
