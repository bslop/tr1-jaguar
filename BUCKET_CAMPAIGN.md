# BUCKET CAMPAIGN — per-room spatial face clusters (opened 2026-07-21, user: "That is what we need.")

GOAL: stop touching every face of every admitted room. Extractor groups
each room's faces into spatial CLUSTERS with 3D bboxes; the kernel tests
ONE bbox per cluster against the view (frustum/portal rect/facing) and
skips whole clusters — attacking the staging+face-touch quarter of the
frame at cluster granularity.

BINDING LESSONS (predecessor campaigns, non-negotiable):
- Per-span tests killed TRAPEZOID (-15-25% silicon); per-face tests broke
  even at best (STAGEDIET +2%). Cluster tests amortize over 50-100 faces:
  the ONLY granularity with proven headroom. Charge every test honestly.
- Analyzer humility: TRAPEZOID's 48% ceiling was an artifact; OCCLUSION's
  step-0 gates saved two implementations. Step 0 here must replay REAL
  frames (jagemu traces / kernel-visible quantities in paint order).
- ifdef traps: enable flags by OMISSION; make clean between configs; jas
  dialect rules in TRAPEZOID_CAMPAIGN.md; the SHADEPASS=0+RAMP-bins
  polluted-U trap; blitter/GPU-SRAM ownership under PIPELINE (main.c).
- STAGEDIET plane prefix precedent for record-format changes (extractor
  FACE_PLANES pattern: env-gated, byte-identical legacy path, Makefile
  guard tying bins to kernel flags, runtime-blob dummy planes).

CURRENT STATE: session mean 18.6 (Jaguar C, re-baseline on B pending);
frame at heavy views = Tom walk+fill of visible + staging/face-touch of
admitted; 68k tick ceiling ~21.5 idle. hopcap=1 (room±1 dispatch) eye-
cleared by user; FARDIAL per-vert far cull just landed (dial sweep in
progress — its settled values may reshape the heavy-view baseline; take
step-0 traces WITH the settled dials).

DESIGN SHAPE (step-0 decides parameters):
- Extractor: after _subdivide/_face_sort, k-means-or-grid faces into
  N=4-8 clusters/room (spatial, by face centroid); emit per-cluster:
  {bbox (6xs16), face count, offset} table + faces re-ordered cluster-
  contiguous. Record format version ties to a -d flag (STAGEDIET
  pattern). Face order changes = painter order within room changes —
  AUDIT: the OT/depth sort currently orders faces far->near per room;
  clusters must preserve painter correctness (sort clusters by depth,
  faces within cluster stay depth-sorted — verify emu pixel parity).
- Kernel: at cluster boundary in the face walk: test cluster bbox vs
  (a) portal/clip rect in screen space (project 8 corners? EXPENSIVE —
  prefer view-space conservative test: bbox behind camera / fully
  outside frustum planes / fully beyond FARD) then skip cluster's faces
  (FACE_CUR += cluster size). Budget: kernel at 3670/3680 with FARDIAL —
  needs bytes: candidates = another jopt pass, PROFGPU scraps, or the
  now-dead NCULF/RUNHIST remnants. Test cost budget: <=60 cyc/cluster.
- STEP 0 (mandatory, offline): from real traces + geometry, for spawn +
  r12 + the user's mid-level heavy spot (get its coords from the dial
  session log context if possible, else r12): per frame, how many faces
  sit in clusters that a conservative view-space bbox test would skip?
  Report % faces skipped, % staging cycles saved net of test cost, and
  the cluster-count sweep (N=4/6/8). HARD GATE: net >= 8% of heavy-view
  frame time or STOP.
VERIFY LADDER: legacy-path byte-identity; cluster-build emu pixel parity
(spawn + r12 + driven gameplay); jsim fps; silicon telemetry A/B; user
play-feel. NO silicon flashing by the agent — stage telemetry twins.

---
# STEP 0 RESULTS + VERDICT (2026-07-21/22 agent session)

## Method (all offline/emu; hardware untouched per coordination note)
- Real per-frame kernel inputs captured from jagemu serve: DRAM displist
  (count + {room ptr, portal clip rect} per entry) + camblk (cY4..camz) +
  curroom peeked EVERY frame; FARD=9000, hopcap poked to 1 (the eye-cleared
  dial; g_hop_cached_a invalidated with it). Scenes: spawn (50 fr), r12
  junction static (50 fr), r12 junction DRIVEN run+turn (185 fr,
  cap_r12_drive.jsonl) — the heavy-view proxy.
- Simulator (scratchpad bucket_step0.py): EXACT integer replica of the
  kernel per admitted room: vc pre-pass transform (>>12 sar chain, NEAR=64,
  FARD sentinel), STAGEDIET plane test (s16 imult semantics + keep-all
  flag), stage/BEHINDF, screen backface ar, y-clip-empty — then k-means
  clusters (per room, quads+tris jointly, per-phase segments) + a
  conservative 6-half-space cluster bbox test (near/far/left/right/top/
  bottom derived from the entry's portal rect, p-vertex-equivalent, 2px
  margins for DDA/trunc jitter). Costs instruction-counted from
  gpu_geotex.gas: plane-cull 75cyc, staged+behind quad 380/tri 315,
  +backface 435/370, y-empty 535/450, walked-empty row 80cyc/row
  (conservative low), 60cyc/cluster-segment test charged on EVERY walked
  segment. Conservativeness PROVEN in-sim: 0 violations (no skipped
  cluster ever contained a face that painted).
- **r12 black-room mystery SOLVED**: PLAY_PS_r12 vs TRAP_base_r12 differ by
  ONE byte — the patched MRT_SPAWN_ROOM moveq: working=12, broken=15.
  The junction recipe is LOCAL index 12 (orig room 10) @ 44032/7168/56320;
  15 (orig room 12) puts the spawn outside the room's own floor -> curroom
  wrong -> empty visibility set -> Lara-on-black. Fresh r12 builds with
  ROOM=12 render the junction (verified this session).

## Numbers (per frame, BASE costs, hopcap=1 FARD=9000 — the settled dials)
Scene            faces  drawn  classes (A=plane-cull, B=behind-staged,
                               D=y-empty, W=walks-but-all-spans-x-clip)
spawn (1 room)    291     77   A53 B124 D30 W7      pool 129K cyc
r12 static (2rm)  552    119   A47 B318 C2 D22 W44  pool 370K cyc
r12 DRIVEN (3rm)  633avg  --   pool 435K cyc (B 150K, W 267K!)
Cluster sweep (net cyc/frame after 60cyc/seg tests; k-means):
  spawn:      N4 +27.5K | N6 +40.4K | N8 +33.7K
  r12 static: N4 +53.3K | N6 +69.8K | N8 +139.7K
  r12 driven: N4 +105K  | N6 +129K  | N8 +233K (p90 354K; heavy-decile 341K)
  (N12/16 probes: capture saturates ~50-62% of pool; k-means seeding is
  noisy — a grid/BSP splitter would be steadier.)
Skip-plane attribution (driven, N=8): near 1501, far 915, right 548,
left 200 per 185 frames — near+far do ~76% of the work.
GATE MATH: standalone N=8 driven-junction net 233K cyc/frame = 12.3% of a
14fps heavy frame (1.90M cyc) / 8.3% of the old 9.4fps junction frame —
**PASSES the 8% gate standalone**. BUT:

## THE DECOMPOSITION THAT REFRAMES THE CAMPAIGN (retraction of premise)
The cluster win pool is NOT "cluster-only" money:
- **W class** (faces fully left/right of the clip window that still WALK
  their whole y-range emitting clamped-empty spans — up to 240 rows each,
  ~2950 empty rows/frame at the driven junction = 267K cyc pool): the
  kernel simply has NO face-level x cull. A ~68-byte in-kernel test kills
  the walk for ALL of them (not just the clustered ~40%).
- **B class** (staged-then-BEHINDF faces, 150K pool): aborting the stage at
  the FIRST behind-sentinel vertex costs ZERO added work on live faces and
  REMOVES bytes (the BEHINDF flag machinery goes away).
After those two per-face guards, the cluster-only increment left is
~55-60K cyc/frame (~3% of heavy frame) — **BELOW the 8% gate**.
RETRACTION (of this brief's own premise): "cluster tests are the ONLY
granularity with headroom" was wrong for THIS pool — the TRAPEZOID/
STAGEDIET per-face-test lesson does not apply to tests that (a) piggyback
on already-loaded registers with no DRAM traffic and (b) guard
catastrophic per-face waste (240-row empty walks), not marginal waste.
**VERDICT: clusters NOT built (extractor untouched); two kernel-only
guards built instead. FACE_CLUSTERS stays on the shelf with this sim as
its step-0 harness (bucket_step0.py replays any future proposal).**

## What was built (kernel + Makefile only; NO extractor/record changes)
- `-d XCULL=1` (gpu_geotex.gas, render_pkt, after the SX/SY loads): reject
  a face when ALL verts are left of clipx0-1 or right of clipx1+2, via
  sign-folding (AND/OR of sx_i-threshold — no branches per vert, no
  divides). Quads test all 4; tris test 3 real + the STALE SX_BUF[3] slot,
  which can only make the test MORE conservative (documented in-source).
  2px margins absorb edge-DDA truncation. +68B.
- `-d BEXIT=1` (stage_vert): behind/far-sentinel vertex -> abort the face
  straight through face_adv (record cursor advances exactly as the
  early-cull path does); BEHINDF init/set/check compiled out. NET -28B.
- Makefile: both flags plumbed into GEOTEX_DEFS (enable by OMISSION-safe
  default 0); guard: XCULL=1 requires SHADEPASS=0 (byte budget,
  first-proof rule — kernel would be 3710/3680 with SHADEPASS=1).
  **Reconciliation gap is only ~30B** — candidates: SHADEPASS v4b diet,
  hand slot-fills (jopt reports 0 in active code — already saturated),
  CLIPX0/X1 packed into one long (saves a movei+load at 2 sites).
- Kernel bytes: flags-off 3670 (BYTE-IDENTICAL to pre-campaign, verified);
  BEXIT play config 3642/3680; XCULL+BEXIT @ SHADEPASS=0: 3526/3680.

## Verify ladder results
1. Legacy/default: flags-off kernel bin byte-identical; extractor NEVER
   touched (legacy path identity trivially holds); default build
   `make MULTIROOM=1 JERRYPOSE=1` + ship bins green, boots to title.
2. Emu pixel parity (silicon fidelity, frame-700 snapshots, both spawn +
   r12 headers): ref-vs-guard diffs OUTSIDE Lara idle-anim mask and the
   row-1 HB heartbeat strip = **0 px in all four pairs** (XCULL pair also
   0 px at the title, where ring faces legitimately cross the screen
   edges). Driven-input gameplay through the junction on both guard
   builds: stable, coherent, no hangs (bk_xb0_walk*.png, bk_bexit_walk*).
   NOTE: SHADEPASS=0 screenshots show the KNOWN RAMP-bins polluted-U
   texture garble on BOTH sides of the pair — expected, not a regression.
3. jsim A/B (600-vblank window, renders counted via displist[0] watch;
   render cadence is field-quantized so this instrument is coarse):
   - XCULL+BEXIT vs ref (SHADEPASS=0): r12 junction **85 -> 100 renders
     (+17.6%), 2.763M -> 2.312M gpucyc/render (-16.3%)**; spawn 112 -> 117
     (+4.5%), -5.1% cyc/render. Exceeds the step-0 model (jsim prices the
     per-row walk with heavy jump_refill charges; silicon is the judge —
     historically jsim UNDER-predicts wins on silicon).
   - BEXIT alone vs ref (SHADEPASS=1): 67 -> 67 renders, cyc/render
     within 3 cycles — **no measurable jsim effect** (model says ~1-3% of
     heavy frames; claim nothing until silicon). Kept because it is
     parity-clean and FREES 28B.
4. Silicon: NOT run (rig owned by the user's dial session). Twins staged.

## Staged artifacts (session scratchpad)
- Telemetry twins (play flags + HOPDIAL HOPBOOT=1 FARDIAL + NOGD PROFILE
  NOPROFGPU QUIETFPS, ship spawn header, bins_planes inside):
  **BUCKET_ref.cof / BUCKET_on.cof** = SHADEPASS=0 pair, on = XCULL+BEXIT
  (the fps datum pair — expect ramp-atlas texture garble, fps is valid);
  **BUCKET_bexit_ref.cof / BUCKET_bexit_on.cof** = SHADEPASS=1 pair
  (real look, BEXIT-only increment).
- A/B + parity builds BK_*.cof (+.elf with symbols), captures
  cap_spawn/cap_r12/cap_r12_drive.jsonl, sim bucket_step0.py + step0_*.json,
  BUCKET_trace_*.cof/.elf (peek-instrumented dial builds), screenshots.
- Tree at session end: ship bins restored (mrt.bin 40ba1535), default
  build green, mrt_spawn.h = ship, all new code behind XCULL/BEXIT=0.

## Open risks / next steps
- WALK_ROW=80cyc and the class costs are instruction-counted, not
  silicon-measured; the XCULL win rests on the W pool being real. The
  jsim -16.3% junction frame agrees directionally. Silicon A/B
  (BUCKET_ref vs BUCKET_on, fps100 console, ab_run.sh pattern) decides.
- XCULL x SHADEPASS reconciliation needs ~30B (see candidates above)
  before it can enter the play build; BEXIT can enter it today (pending
  silicon + play-feel gates).
- FARD synergy: lower FARD dial -> bigger far-sentinel pool -> both BEXIT
  and any future cluster walk gain; re-run bucket_step0.py with --fard
  after the dial settles.
- Cluster shelf: if revived, use grid/BSP clustering (k-means seeding
  noise), near+far planes first (76% of skips), and re-gate against the
  post-guard pool with this sim.

---
# XCULL x SHADEPASS RECONCILIATION — DONE (2026-07-21 follow-up session)

## Bytes found (needed 44 over ceiling at the full set; found 56)
Ground truth first: SHADEPASS=1 STAGEDIET=1 FARDIAL=1 XCULL=1 BEXIT=1
assembled to 3724/3680 (44 over, not the estimated ~30); without FARDIAL
3710 (30 over). Sources, in the sanctioned priority order:
1. **FARDIAL dropped (-14B when it was on)**: kernel `.if FARDIAL=1`
   far-cull block (vc pre-pass) + FARD equate ($F03F3C, slot now free
   scrap) removed; `-d FARDIAL` removed from GEOTEX_DEFS; `make FARDIAL=1`
   now $(error)s with the retirement note (prevents a silent no-op dial —
   main.c's #ifdef FARDIAL blocks remain but are unreachable via make).
   Sanctioned per this brief: measured NULL on silicon 2026-07-21.
2. **u/v DDA-init indexed-store diet (-42B, the real payer)**: AU..BVSL
   are 8 CONTIGUOUS longs ($F03FA8-C4); the render_pkt init was 8x
   (movei+store) + moveq = 66B -> moveq + movei #AU,r14 + 8 indexed
   stores = 24B. Moved BEFORE the chain-init lines (borrows r14 while
   ayend is not yet live). **Gated `.if XCULL=1` with the old block kept
   under `.if XCULL=0`** so every flags-off build stays BYTE-IDENTICAL
   (the first-proof discipline; the fat variant is the price of identity).
   Indexed stores are safe: jagemu's store-offset-field bug is RESOLVED
   (COBWEB_BUG_indexed_store.md) and the span section already relies on
   them against silicon.
3. jopt full-define pass (--gpu --allow-input-hazards, all 19 -d args):
   **0 transforms in active code** (3668 -> 3668) — still saturated; the
   only wasted slots it sees are inside inactive .if blocks. Not needed.

## Final kernel sizes (jas, ceiling 3680)
- SHADEPASS=1 STAGEDIET=1 XCULL=1 BEXIT=1 (the target): **3668 (12 spare)**
- XCULL+BEXIT @ SHADEPASS=0: 3470 (was 3512); BEXIT-only play: 3628
- flags-off play (SP1+SD1): 3656 — byte-identical, see below.
- Makefile: XCULL x SHADEPASS mutual-exclusion guard REMOVED.

## Verify ladder
1. **Flags-off byte-identity: PASS.** 5 configs assembled pre- vs
   post-edit and cmp'd byte-equal: {SP,SD}={1,1},{0,1},{1,0},{0,0} all
   XCULL=0 BEXIT=0, plus SP1+SD1+BEXIT=1. Default build
   (MULTIROOM=1 JERRYPOSE=1, ship bins) green, boots to title in emu
   (passport mis-render = known pre-existing). Only gpu_geotex.gas +
   Makefile touched; main.c untouched, so flags-off COFs are unchanged
   by construction (kernel bin equality + no C-flag deltas).
2. **Emu pixel parity (silicon fidelity, frame-700), SHADEPASS=1 pairs**:
   spawn ref-vs-XCULL+BEXIT = **0 px RAW** (no masking needed — even
   Lara aligned); title (frame 90, faces legitimately crossing screen
   edges — XCULL's risk case) = **0 px RAW**; r12 junction (ROOM=12
   one-byte fix, renders the junction) = 310 px ALL inside Lara's
   idle-sway bbox (y94-212, x133-186; verified visually = pose skew from
   render-cadence offset), environment 0 px. Driven run+turn through the
   junction on the guarded build: stable, coherent, frame 990 healthy,
   0 illegal ops (xb_walk1/2.png).
3. **jsim fps A/B** (silicon fidelity, 600-frame steady window = frames
   650->1250, renders = displist[0]-write watch / 2 — two writes per
   render: rooms count then +Lara):
   - r12 junction: **37 -> 41 renders (+10.8%)**, window/render
     6.79M -> 6.09M gpucyc (-10.3%). Smaller than the SHADEPASS=0 pair's
     +17.6% (heavier SHADEPASS=1 frames dilute the guard win).
   - spawn: 50 -> 50 (0.0%) — at SHADEPASS=1 cadence the field
     quantization is too coarse to resolve the previously seen +4.5%;
     honest read: spawn ~neutral, junction is where the W-pool lives.
   - Instrument note: GPU "cycles" ~= the whole window on both sides
     (resident kernel idle-loops between kicks), so cyc/render here is
     just inverse render rate, not compute cost.
4. Silicon: NOT run (no rig access this session). Twins staged below.

## Staged artifacts (session scratchpad)
- **Telemetry twins** (MULTIROOM GEOMDIRECT JERRYPOSE AUTOSTART NOGD
  PROFILE NOPROFGPU QUIETFPS STAGEDIET PIPELINE PIPESTAGE=2 HOPDIAL
  HOPBOOT=1 SHADEPASS=1, ship spawn header, bins_planes):
  **XB_ref.cof** (c3698ad5, no XCULL/BEXIT) / **XB_on.cof** (03027ee2,
  XCULL=1 BEXIT=1). fps100 console A/B per ab_run.sh pattern. NOTE:
  NOGD twins sit black at frame 700 in emu — so did the accepted
  BUCKET_bexit twins (console handshake stalls load without a host);
  expected, not a regression.
- **PLAY_XB.cof** (aa528dda) = the play-build candidate: NOGDONLY=1,
  no PROFILE/QUIETFPS, XCULL=1 BEXIT=1 SHADEPASS=1 — pending silicon
  fps + user play-feel gates.
- Parity/A-B play builds + .elfs: XB2_ref_play / XB2_on_play (ship
  spawn), XB2_ref_r12 / XB2_on_r12 (ROOM=12 header); screenshots
  ss_XB2_*, ss_title_XB_*, xb_walk*, diffs via xb_diff.py; jsim JSONs
  jsim_*_650/1250.json; pre-edit source snapshots
  gpu_geotex.gas.pre_xb_diet / Makefile.pre_xb_diet.
- Tree at session end: ship bins verified byte-equal (all 10 files),
  mrt_spawn.h = ship, default build green. XCULL/BEXIT remain opt-in.
