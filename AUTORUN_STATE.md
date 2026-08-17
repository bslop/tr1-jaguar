# jag_openlara — autorun state

RUN: 72

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

# ☠️☠️☠️ THE CAVES ARE NOT WALKABLE PAST ROOM 0. 25 OF 38 ROOMS ARE SEALED.

This is the most important open item in the project and it is in the SHIPPING
level. Two independent measurements agree:

**Empirical.** Walking +Z from (74240,3072,18944) she advances z 19178 -> 21430
and then STOPS for 11 straight samples. `g_floorroom` flips 0 -> 1 (the floor
ahead really is room 1's); `g_curroom` never changes.

**In the data.** The 0->1 portal is a plane at z=21504, x 72704..76800
(`mrt_portalv`). The two rooms tile perfectly across it and NEITHER side has a
walkable cell at the seam:

    room 0 cell 18 (z 20480..21504)  WALL 2560 3072 3072 2816 WALL   <- she is here
    room 0 cell 19 (z 21504..22528)  WALL WALL WALL WALL WALL WALL
    room 1 cell 0  (z 20480..21504)  WALL WALL WALL WALL WALL WALL WALL
    room 1 cell 1  (z 21504..22528)  WALL WALL 2560 3072 3072 3072 WALL

The move gate blocks on `room_wall_at(rsect[g_curroom])` - the room she is IN -
so a WALL cell in room 0 stops her even though room 1 has floor beyond it. That
rule is correct TR behaviour; what is missing is the OPENING.

    mrt: 38 rooms, **25 with ZERO 0x7FFE openings**, including 0, 1, 2, 3
         [0,1,2,3,5,6,7,8,9,11,12,14,15,17,23,24,25,28,29,30,31,32,35,36,37]

### ☠️ WHY - and it is NOT the runtime
`project_room_crossing_fixed` (2026-07-30) recorded this as FIXED, asset-only:
mark wall cells under a portal's footprint as 0x7FFE, 71 cells opened, silicon
reached 9 rooms. The extractor's actual rule is
`tools/tr2jag_multiroom.py:3191`:

    hport = (sector_portal(fidx) is not None) or (_ci in doorcells)
    fy = (0x7FFE if (below != 255 or hport) else 0x7FFF) if floor == -127 else floor*256

so a cell only opens if the SECTOR carries an FD portal command (`func==1`) or is
in `doorcells`. At the 0/1 seam neither side qualifies, so both stay 0x7FFF.
**The room's PORTAL LIST is not consulted at all** - and that list is exactly
what the memory note says the fix was supposed to use.

### ⬜ NEXT: OPEN THE CELLS FROM THE PORTAL GEOMETRY
  1. For every room, for every portal quad (dst + 4 corner verts, already parsed
     for render clipping), mark the WALL cells on BOTH sides of the portal plane
     within its XZ span as 0x7FFE. ☠️ The plane is ZERO-THICKNESS and lies
     exactly on a cell boundary, so "cells whose centre is inside the quad"
     matches NOTHING - that is very likely why the original fix missed these.
     Expand by half a cell on each side, or mark the cells the plane separates.
  2. Re-measure: `25 rooms with zero openings` must drop, and room 0 must gain
     cells at cell 19 x-lanes 1..4.
  3. Re-verify empirically - the same walk must cross:
     `probe_spot.py ... --at 74240,3072,18944,0,0 --keys up --frames 20`
     and `g_curroom` must go 0 -> 1.
  4. ☠️ `mrt_sect.bin` is GITIGNORED and regenerated - whatever fixes this must
     live in the extractor or in a `--patch` that `build_cof.sh` runs, or it will
     evaporate on the next asset regen. That is the most likely reason this
     regressed after being fixed once.
★ `room_floor_mr` already skips >=0x7FFE and takes the floor from the neighbour,
and `room_wall_at` treats only 0x7FFF as solid, so **no runtime change should be
needed** - exactly as the 2026-07-30 note says.

### ✅ VERIFIED WORKING (so the fault is narrow)
  * Room tracking: teleported to (74240,3072,28160) deep in room 1 - both
    `g_curroom` and `g_floorroom` read 1, floor agrees, stable.
  * The portal DATA is right: `mrt_portalv` has the 0->1 quad with sane
    coordinates; adjacency lists 0<->1.

### ☠️ CLOSED - DO NOT REOPEN
    mansion holes (50-59) · pickups (65) · mid-walk LOADING (67) ·
    caves black wedges (69, OPEN SKY - sightline prints the sky count first)

### ★ INSTRUMENTS - see the list in git history if trimmed; the load-bearing ones:
    probe_spot.py --raw= / --set=   per-spot telemetry; teleport anywhere
    sightline.py                    geometry NEARBY + is the room open to sky
    release_play.py --tour/--play/--gym   drive the RELEASE with telemetry
    entity_check.py                 photograph any entity
    room_cycles.py --prefix=gym     per-room kernel cycles
    HOLEVIS=1 / DREWVIS=1 / HOPDEPTH=N / CULLCOUNT=1 BEXCNT=1 WCCNT=1
☠️ Parse probe output BY COLUMN NAME - positional awk has misread it twice.
☠️ SYMBOLS ARE PER-BUILD.  ☠️ FPS cannot be measured offline.

### ✅ WHAT IS DONE
    climbing   CAVES 24/24 ledges, 6/6 walls · MANSION 24/24, 6/6
    enemies    BEAR and WOLVES render on shipping flags
    pickups    MEDIKIT_SMALL collected on contact, verified against a control
    release    /tmp/cofout6 - both levels into gameplay, symbols, tour capture
               ☠️ but the player cannot walk out of room 0 (above)

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
