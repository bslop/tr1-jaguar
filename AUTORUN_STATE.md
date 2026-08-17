# jag_openlara — autorun state

RUN: 56

**This file is how work survives a context ending.** A context can end without
warning; anything the next run needs must be here, not in the conversation.
`tools/session_run.sh end` **refuses to advance the counter** if this file was
not touched, because a run that learned something and did not write it down has
lost it.

Every run: `tools/session_run.sh start` → work → `tools/session_run.sh end "<summary>"`.
Every 25 runs the script prints a PROGRESS REPORT DUE banner — the user reviews
direction at that point and decides whether it is still valid. **Do not
summarise that checkpoint away.**

---

## NEXT STEP

# ✅ THE HOLE IS MISSING GEOMETRY, NOT A RENDERER BUG. FIVE RUNS TO GET HERE.

Along Lara's line of sight at the probe spot there are **19 faces in the whole
level**, and every one of them spans y -1024..2560 - entirely ABOVE her feet at
y=3072. Nothing exists between room 13's lowest faces and the floor she stands
on. **The vertical riser between the two floor levels is absent from the mesh**,
so no cull, window or clip was ever going to fill those pixels.

    # the query, reusable - walk every room's quads, keep centroids in the
    # +X corridor at her z, print each room's y span
    35400 <= centroid_x <= 41000 and 38900 <= centroid_z <= 39950
      -> room 13: 19 faces, y span -1024 .. 2560     (she is at y=3072)

### ✅ THE WORLD PLANE CULL IS EXONERATED - it was the last suspect
Built an offline model of it and **validated it against the hardware**: 21% of
room 13's faces culled offline vs the ~19% `worldcull` the ROM counts. With that
model:
  * planes are internally consistent - all 471 quads in room 13 and all 117 in
    room 15 share one winding convention (a uniform "disagreement" with my cross
    product is MY sign convention, not a defect - a 100% result is a convention,
    a 5% result is a bug);
  * the SLACK is a constant **6144** subtracted from d, and since the test is
    `cull iff N.C < d`, lowering d culls FEWER faces. The margin errs toward
    keeping geometry, so it cannot be over-culling.
  * ☠️ `nz` is stored as 65472 in a 32-bit field and that is NOT a sign bug -
    `imult` takes the low 16 bits as signed, so it reads as -64. Checked because
    it looked exactly like one.

### ⬜ NEXT: WHY DOES THE EXTRACTOR DROP THE RISER?
The question is now about level conversion, not rendering:
  1. Does the ORIGINAL TR1 room data carry that face? Read Lara's Home room 13/15
     out of the source level with the extractor's own reader and look for quads
     spanning y 2560..3072 in that corridor.
  2. If TR1 has it and we do not, find where the converter drops it -
     the likely candidates are portal-boundary handling (a face coincident with a
     portal may be discarded) or a room-bounds clip.
  3. If TR1 does NOT have it either, then the original relies on the player never
     being able to stand where the census put her, and the honest fix is a
     collision one: room 15's floor at 3072 next to room 13's at 2560 with no
     wall between them is a place TR1 never lets you see from.
     ★ That is worth checking FIRST - it is cheap, and if true this hole is not
     a bug to fix but a place to make unreachable.

### ☠️ THE SHAPE OF THIS HUNT - worth reading before starting another
Runs 50-55 eliminated, with measurements: ALLVIS, room-level and kernel XCULL,
NEARLOW, SLIVER, HOPBOOT/hop, missing portal, undrawn rooms, VRESN/resolution,
screen-space backface, empty-y+area as a whole, and the world plane cull.
  * Run 52 found a 45%-vs-5% controlled correlation and called it the cause. It
    was not. **Removing the suspect (run 54) closed 7 of 33 points** while the
    reject rate fell to the clean-room level. A correlation that survives a
    control still is not a cause.
  * Six static A/Bs returned EXACTLY 33.1%. Identical numbers across unrelated
    changes is one strong result, not six weak ones: none of them was in the path.
  * The thing that finally worked was asking what geometry EXISTS, which is a
    question about the data and needed no build at all.

### ☠️☠️ TWO KERNEL DIAGNOSTIC FLAGS BUILD AND DO NOT RENDER
    NOEMPTYY=1   GPU halted, flat luma-76 field
    NOSDCULL=1   GPU halted, HOLEVIS 100% white = nothing drawn
Both report **illegal=0**. `NOBFCULL=1` and `ALLVIS=1` are safe.
★ `illegal=0` is not "it rendered" - screenshot every diagnostic build.

### ★ INSTRUMENTS (all off in shipping builds)
    DREWVIS=1                     g_visrooms / g_drewrooms room bitmasks
    CULLCOUNT=1 BEXCNT=1 WCCNT=1  per-face counters, RAW DRAM:
                                  $1C0000 staged  $1C0004 rastered
                                  $1C0010 bexit   $1C0014 worldcull
                                  total = worldcull + staged
                                  screen-space = staged - bexit - rastered
    probe_spot.py --raw=N=0xADDR / --set=SYM=VAL
    build_conf.sh EXTRA= / SKIP=
☠️ Counters ACCUMULATE - take DELTAS. Geometry layout for offline work: room blob
header 16B `>HHHHH`+`>hhh`, verts `>hhhH` at +16, quads 36B = 12B plane
`{(ny<<16)|nx, nz, d}` + 4 u16 idx + 8 u16 UV; tris 30B.

### ✅ STILL TRUE — climbing is closed (runs 46-49)
    CAVES    LEDGES 24/24   WALLS 6/6 refused   0 black outliers
    MANSION  LEDGES 24/24   WALLS 6/6 refused   7 black outliers (this hole)

### ⬜ ALSO STILL OPEN
  * Room 0's hole is a different shape (everything above the floor missing) -
    run the line-of-sight query above at that spot FIRST, it is one command.
  * ☠️ `/tmp/cofout4` (the filmed release) predates runs 46-49. Rebuild before
    shipping.
  * Run-25/50 direction questions unanswered; the run-50 report was delivered.

### Toolchain — STILL PINNED to 59e5896 (`COBWEB_DIR=/tmp/cobweb-old`)
`beb2c15` does NOT fix the jcc68k regression (16-bit param read moved +2,
breaking gcc/jcc mixed links). `tools/toolchain_smoke.sh` guards updates.
ROMs: `/tmp/conf.cof` (Caves), `/tmp/gym.cof` (mansion, AUTOGYM, **no PADMUTE**).

### Instruments (all offline, no rig)
    conformance.py    driven per-spot sweep; best-height verdict, per-level
                      black baseline, jump drive for JUMPGRAB
    room_black.py     per-room black% baseline (caves 51.2 max, gym 36.3)
    floor_coverage.py collision-vs-mesh scan + patch (--prefix, --faces)
    mrt_boundary_audit.py  seam-floor audit + patch (--prefix)
    toolchain_smoke.sh     build a ROM and LOOK at it after a toolchain move

### ⬜ AWAITING THE USER (run-25 checkpoint)
  1. Capture card replugged? 2. Ship Lara's Home? 3. Release or keep polishing?
Nothing pushed to `origin` (public `tr1-jaguar`).

---

## WHAT CHANGED IN RUNS 9-11 (2026-08-16)

- ✅✅ **Release recipe proven end to end** with all four videos, both guards, and
  a real SD-card boot in jagemu.
- ✅ **Title-music refill fixed** to read the DRAM mailbox and stamp stale —
  correct per the codebase's own law, but **offline-unverifiable**; needs silicon.
- ✅ **Filed a real emulator divergence** to jaguar-shared: jagemu lets the 68k
  read a running core's local SRAM honestly, so this whole bug class is invisible
  offline. Asked for a counter, not a behaviour change, because other projects
  read those ranges today.
- ✅ **`session_run.sh end` guards against mid-build commits** — hit for real with
  three of four videos converted.
- ★ Lesson worth keeping: *a fix that produces no measurable change is not
  automatically wrong* — but it is also not confirmed. Say which one it is.

---

## WHAT CHANGED IN RUN 8 (2026-08-16)

- ✅✅ **Mansion verified through the real menu path** and the release now ships
  it selectable (`GYMSD` dropped from `build_cof.sh`).
- ✅ **`AUTOGYM=1` added** — the first offline test that covers the shipping
  menu-exit path rather than the GYMTEST shortcut.
- ☠️ **Caught a hook-precedence trap**: AUTOSTART preempted AUTOGYM even though
  `-DAUTOGYM` was on the compile line. Made the precedence explicit in source so
  no caller can hit it by flag ordering.
- ★ The measurements that made shipping it possible, in one place: gym_lskin
  alias −107,040 B; TEXSCALE=4 + RAMP_PAL + no STATICS = 254,296 B payload;
  6 of 6 pads at 1,554,268 B.

---

## WHAT CHANGED IN RUN 7 (2026-08-16)

- ✅ **Mansion now fits: 6 of 6 pads, ROM 1,554,268 B**, by dropping STATICS
  from its extraction (−107,368 B). Boots clean, vector intact.
- ☠️ **Corrected a wrong claim I made twice** — runs 5 and 6 both reported the
  mansion linking, from a bare `make` that skips the stack-headroom guard.
  Under `gbuild.sh` it built **nothing**.
- ✅ **`tools/build_cof.sh`** now carries the exact working combination and the
  measurements behind it, plus `$MRTENV_NOSTATICS`.
- ★ Found that under **GYMSD the menu refuses Lara's Home on purpose** — so
  shipping it playable means building without GYMSD.

---

## WHAT CHANGED IN RUN 6 (2026-08-16)

- ✅✅ **The mansion looks right** — re-extracted with the full recipe including
  `RAMP_PAL` (whose "extractor dies" note was stale), atlas 90,112 → 134,144 B.
  Went from nearly black to a properly lit interior.
- ✅ **Proved no caves regression** — pixel-identical render, all pads boot.
- ✅ **`tools/build_cof.sh` updated** so the release build reproduces it.
- ☠️ **Recorded a clean negative**: the A10-boot-lottery link is unproven, and
  the offline test cannot decide it. Better than leaving the hypothesis dangling.

---

## WHAT CHANGED IN RUN 5 (2026-08-16)

- ✅✅ **Lara's Home is fixed** — no crash, vector intact, Lara renders. The bug
  that made it "broken" is closed.
- ✅ **`gpu_kernel_ensure()`** added and called from all six kicks; jvdec load
  invalidates residency, jvdec done restores it.
- ✅ **All kicks now set `params[1]`** (mailbox pointer), closing a second,
  independent instance of the same overlap hazard.
- ✅ **Published `techniques/gpu-kernel-residency.md`** to jaguar-shared — the
  general lesson (a kick launches whatever is resident; overlapping param slots
  turn a stale kernel into vector-table corruption) plus the diagnostic that cut
  through it: dump GPU SRAM and byte-compare against every built kernel.
- ★ Method note: the bug wore three disguises — "renderer bug" (the stripes were
  the crash beacon), "GPU wedge" (exc_catch halts Tom itself), and "corrupt
  pointer" (it was a constant, +2). Runs 2-4 each chased one.

---

## WHAT CHANGED IN RUN 4 (2026-08-16)

- ✅ **Root cause found and proven**: 68k vector 64 overwritten with
  `MAGIC_DONE` by the GPU's completion store through a bad `r30`. Caves vector
  verified intact as the control.
- ✅ **Fixed a real latent bug in 5 kick functions** — `params[1]` (the kernel's
  mailbox pointer) was never restored after `gpu_jvdec_kick` reused it as
  `prevLen`. Verified corrected at the world kick.
- ✅ **Localised the remaining cause** to `r30` being clobbered inside the
  kernel (holds the atlas pointer at halt), between f166 and f172.
- ⬜ Raised the possibility that this **is** the A10 boot lottery ("VI never
  fires") — a concrete, cheap test is written above.

---

## WHAT CHANGED IN RUN 3 (2026-08-16)

- ✅ **Disproved run 2's root cause** with a frame-by-frame timeline. The 68k
  faults first; the halted GPU is `exc_catch`'s own doing. Recorded above so the
  next run does not re-derive it.
- ✅ **Localised the real fault** to an `rts` returning to `MAGIC_DONE`, i.e. a
  stack imbalance of exactly 8 bytes, and cleared `cpu_stop_unless` of blame by
  reading it.
- ✅ **New technique: probe labels.** Label every instruction in a block,
  re-assemble, `cmp` to prove byte-identical, and read the exact PC→instruction
  mapping off the map. Left as `gpu_probe.gas`.
- ★ Named the wedge candidate precisely (`movei #DISPCUR,r1`, one after
  `load (r0),r2`) before the timeline showed it was a red herring — worth
  keeping as the shape of "precise but wrong".

---

## WHAT CHANGED IN RUN 2 (2026-08-16)

- ✅ **Root-caused the mansion failure to a GPU wedge** in the kernel's room-list
  init, with the crash frame, the GPU PC, and the watchdog path all identified.
  Reframes the bug completely: it is not textures, UVs or the atlas.
- ✅ **Published `techniques/reading-a-crash-without-a-capture-card.md`** to
  jaguar-shared (c70e277) — how to read a crash with `jagemu peek` when the
  capture card is gone, the "your garbage may be your own crash beacon" warning,
  the `PADTEXT` symbolisation trap, and the sentinel-vs-corruption lesson.
- ✅ **cobweb ad713ec → b8dd333** (4 commits incl. GOURD intensity and the
  blitter BUSY settle window). Renderer re-assembled **byte-identical**;
  `COBWEB_REV` bumped in the Dockerfile.
- ★ Learned: the archived gym assets predate the current extractor and their
  `gym_lskin` differs — the `build_cof.sh` alias guard added in run 1 is what
  makes that safe.

---

## WHAT CHANGED IN RUN 1 (2026-08-16)

- ✅ **Enemies fully skinned — 553/553 faces** (bat and bear were 0/41 and
  0/261). ★ Check the SPLIT not the total: 34 of the bat's 41 faces are
  coloured, so texturing alone helped it least. **Not yet seen on silicon.**
- ✅ **−107,040 B from every ROM** — `gym_lskin` was a duplicate of Lara's
  skeleton *and* sat outside the `GYMSD` `#endif`. Now a `.set` alias.
- ✅ **Lara's Home LINKS again** (1,524,188 B) via that alias + `TEXSCALE=4`.
- ✅ **`GYMTEST`/`CAVETEST` plumbed** — they never had been, so a GYMTEST arm
  silently rendered the Caves.
- ✅ **Two rig-protocol violations fixed** (`climb_matrix.sh`, `vroll_game.sh`).

---

## STANDING RULES FOR EVERY RUN

- ☠️ **Check cobweb first** — `tools/cobweb_check.sh`. Its updates change what
  is POSSIBLE here, not just what is buggy.
- ☠️ **Read `jaguar-shared`** — five sessions write hardware facts into it.
  `session_run.sh start` does both of the above.
- ☠️ **Push to `wip` (private), never `origin`.** `origin` is
  `bslop/tr1-jaguar` and it is **PUBLIC** — pushing there IS the release, which
  the user gated on his own sign-off and wants coordinated with a video.
- ☠️ **`rm -rf build` before any A/B**, and **verify the flag reached the
  compile line** — not just that it exists. A build arm producing byte-identical
  output to another arm is not "working", it is **unwired**.
- ☠️ **`make FOO=0` turns FOO ON.** Omit the flag instead.
- ☠️ Never `git add -A` in `jaguar-shared` — it is one working tree shared by
  every session, and another project's half-finished edit is usually sitting in
  it.

## HARDWARE — READ BEFORE PLANNING ANY RIG STEP

- **The capture card is PHYSICALLY UNPLUGGED** (jag_quake, 2026-08-16): no
  `/dev/video*`, no `0fd9:` on any bus. And there is **no other channel to read
  a number off the board** — no jaggd memory-read, no GD BIOS FWRITE, bulk IN
  times out, CDC has no Jaguar-side write path. **Rig time currently produces a
  picture on a TV that a human must read.** This devalues the `WORLDCOUNT`
  framebuffer diagnostic for task #11 — plan around it.
- **Roster is five** (openlara, quake, bubsy3d, rr, resident). A session not on
  it is **queued, not refused**. `jaguar-shared/hw/RESOURCES.md` is the
  agreement; `hw/GD_ACCESS.md` is openlara's gateway.
- **openlara has the slot after `jag_bubsy3d`**, batched — items #2, #8, #10,
  #11 plus visual confirmation of the enemy skins and the mansion are ONE
  console session, not six.
- Claim the rig **only** for work the emulator genuinely cannot answer.
