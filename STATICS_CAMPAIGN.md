# STATICS CAMPAIGN — full Level-1 geometry (opened 2026-07-22, user: "all
# of level 1 implemented when it comes to geometry")

GOAL: bake TR1 Caves' STATIC MESHES (stalactites, rock formations,
bridge pieces) — currently DROPPED by the extractor — into the room
geometry, plus verify no reachable rooms are missing, so level 1's
world geometry is complete.

KNOWN FILE FACTS (verified in tools/tr2jag_multiroom.py):
- read_all_rooms() SKIPS per-room static placements: the room tail reads
  `r.seek(2); r.seek(r.u16()*20); r.seek(r.u16()*20)` — first *20 list =
  lights, second = STATIC PLACEMENTS (PSX 20B each; expect
  {s32 x,y,z world; u16 rotation (16384=90deg); u16 intensity; u16
  staticID; u16 pad} — VERIFY layout against format.h loadTR1_PSX
  before trusting; OpenLara-master/src/format.h is in this repo).
- The GLOBAL staticMeshes table is skipped at line ~414:
  `r.seek(r.u32()*32)` — 32B records mapping staticID -> mesh index +
  visibility/collision boxes (format.h has the layout).
- The MESH PARSER ALREADY EXISTS (Lara's): mesh header 12B {cx,cy,cz,
  radius,flags,vCount}, short4 world-unit coords, normals-or-intensity,
  rCount textured quads {v0..v3,flags}, tCount tris. Meshes live in the
  meshData/meshOffsets arrays the Lara path already walks.
- Static mesh faces reference OBJECT TEXTURES (same objtex table Lara
  uses) — the atlas packing path for Lara textures is the pattern.

DESIGN (extractor-only if budgets hold — engine sees just more room
faces, and STAGEDIET FACE_PLANES/XCULL/etc apply automatically):
1. CENSUS FIRST: parse placements + global table + mesh sizes. Report:
   statics per room, verts/faces added per room vs VERT_BUDGET (500) and
   the engine's per-room caps (rqc<=448 rtc<=256 in main.c guards),
   atlas growth from new object textures, and file-total room count vs
   the 38 extracted (any unreachable/alternate rooms?). If any room
   busts a budget, propose the split/downsample before implementing.
2. Implement behind env STATICS=1: transform each placement's mesh into
   room-local coords (position - room info, Y-rotation quantized like
   Lara's), light from placement intensity (ramp path: vertex k), append
   to quads/tris BEFORE _subdivide/_face_sort (subdivision + planes +
   sorting then apply automatically). Legacy path byte-identity without
   the flag (STAGEDIET/FACE_PLANES pattern).
3. Verify: bins regen both ways; emu screenshots spawn + r12 + a
   statics-rich room (census tells you where); driven gameplay; fps
   A/B (the added faces cost frame time — measure it; XCULL/backface
   should eat most off-screen cost); collision NOTE: statics have
   collision boxes in TR1 — OUT OF SCOPE this campaign (geometry only,
   log it as a follow-up) unless trivially cheap.
4. Stage PLAY candidate (PLAY_XBR flags + STATICS bins) + telemetry twin
   in the SESSION SCRATCHPAD *AND copy keepers to probes/* (scratchpad
   is session-scoped). Bins: regen with the standard env (RAMP_PAL=1
   RAMP_K=30 RAMP_M=8 TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144
   LARA_MINAREA=0 LARA_WINDSIGN=-1 LARA_WINDSKIP=14 FACE_PLANES=1
   STATICS=1). Tree ends on ship bins; default builds green.
All predecessor doctrine binds (TRAPEZOID_CAMPAIGN.md gotchas, atlas/
palette coupling: new textures must not disturb the reserved band
240-255 or Lara swatches — see the ramps memory).

---
# RESULTS (2026-07-22, agent session — census + implementation + emu verify)

## Census (statics_census.py, copy in probes/)
- File has 38 rooms; BFS from room 0 reaches ALL 38 (no unreachable or
  alternate rooms). Room-count completeness: COMPLETE.
- 44 placements in 9 rooms (r0:1 r13:12 r14:1 r16:2 r20:9 r22:2 r24:1
  r26:15 r30:1), 8 distinct staticIDs {6,8,10,33,34,38,39,43} -> meshes
  209-216 (4-40v, 2-20q, 0-1t each). All rotations are exact quadrant
  multiples. Placement record layout verified vs format.h readRoom
  (TR1_PSX): s32 x,y,z; s16 rot; u16 intensity(0=bright); u16 staticID;
  u16 pad. Global table 32B: u32 id; u16 mesh; vbox+cbox s16[6] each;
  u16 flags.
- Totals across the set (post-subdiv, ship env): +455 verts, +283 quads,
  +8 tris. Atlas 258.0 -> 274.0 KB (+16KB; 31 new object textures, 0
  shared with rooms). geom blob 333224 -> 347296 B (+14.1KB). Palette:
  0 new slots (texels quantize into the existing 30x8 ramps; Lloyd bases
  refit with statics colours — whole-set regen, reserved band 240-255
  untouched).

## BUDGET BUST + the fix chosen
- rooms 13 (666v) and 26 (712v) exceed the 512-vert kernel vtxcache.
- **CENSUS SIDE-FINDING: the SHIPPED bins already bust it** — room 8=529v,
  room 26=620v. gpu.c vtxcache[1024] u32 = 512 verts x 8B; the kernel
  pre-pass writes up to 864B PAST it, over jerry.c's dsp_mailbox
  (nm-verified adjacent) and __bss_end, on every room-8/26 render. Latent
  bug in every resident build; not fixed silently (byte-identity), logged
  here for a user decision.
- Fix implemented: `make STATICS=1` -> -DSTATICS -> vtxcache[1536]
  (768 verts, +2KB BSS). Rejected alternatives: room splitting (dispatch/
  portal surgery), mesh decimation (lossy) — both unnecessary at +2KB.
- Subdivision starvation non-issue: rooms 13/26 already sat at/over
  VERT_BUDGET in the baseline (no splits ran there before either).

## Implementation (all opt-in, defaults byte-identical)
- Extractor env STATICS=1: placements parsed in read_all_rooms tail,
  global staticMeshes table captured, meshes parsed with the Lara-layout
  parser, verts transformed room-local (exact integer quadrant rotation,
  convention verified vs Box::rotate90; float rotY fallback), vertex
  light = format.h intensity conversion (255-(i>>5)), faces appended
  UNSWAPPED (initMesh winding, like Lara; TR two-sided statics are
  mirrored pairs so the cull keeps the visible side) BEFORE the texture
  union/subdiv/sort passes — atlas packing, RAMP k, FACE_PLANES, painter
  order all applied automatically. No windfix needed (no holes seen).
- gpu.c vtxcache under -DSTATICS; Makefile plumbs STATICS.

## Verification
- STATICS=0 regen: ALL 10 outputs byte-identical to pre-edit baseline
  AND to the bins_planes stash; play rebuild from them is BIT-EXACT with
  the resident probes/PLAY_XBR.cof. STATICS=1 regen matches census
  predictions exactly (r13 666/491/17, r26 712/620/4).
- Emu (jagemu, 650f screenshots + A/B pairs in probes/STATICS_ab_*):
  spawn = no visible change (room 0's single static id38 sits AT the
  spawn point, behind the camera; 3429px diff = palette refit + edges);
  r13 spawn-variant (local 18 @ 21504/4096/52224 yaw64) = plant/vine
  clusters clearly render both flanks (absent in base); r12 junction =
  vines now visible THROUGH the doorway portal. Driven gameplay (serve/
  ctl, 2100 frames run+turn): stable, no hangs, no garble, no holes.
- fps A/B (render-count via --watch front_fb, vblanks 300..900):
  spawn 76 vs 76 = 0% | r12 80 vs 76 = -5% | r13 (statics-richest room
  on screen) 69 vs 62 = -10%. Emu-model numbers; silicon pending.

## Staged (probes/ + session scratchpad)
- probes/PLAY_XBRS.cof (PLAY_XBR flags + STATICS=1 + statics bins),
  probes/DIAG_XBRS.cof (telemetry twin: +NOGD PROFILE NOPROFGPU QUIETFPS),
  probes/bins_statics/ (full regen set, standard env + STATICS=1),
  probes/statics_census.py, probes/STATICS_ab_r12/r13_*.png.
- NOT FLASHED — silicon untouched; user gates play-feel + silicon fps.

## Follow-ups (logged, out of scope)
1. Statics COLLISION (cboxes parsed by the census, unused): Lara walks
   through plants — TR1 behaviour for most Caves statics is near-passable
   anyway; revisit for levels with furniture/blocking statics.
2. The baseline vtxcache overflow (rooms 8/26 -> dsp_mailbox clobber):
   decide whether to take the 1536-word cache unconditionally.
3. Silicon A/B of PLAY_XBRS vs PLAY_XBR (user rig time; emu says
   0/-5/-10% by scene).
