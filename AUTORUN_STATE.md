# jag_openlara — autorun state

RUN: 77

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

# ☠️☠️ THE CAPTURE CARD IS BACK AND STILL CAPTURES BLACK. THE TV IS THE ONLY EYE.

★★★★★ **READ `jaguar-shared/hw/RESOURCES.md` - it changed under us.** jag_bubsy3d
recorded 2026-08-17: an **Elgato Cam Link 4K** now enumerates, `jaghw own claim`
says `capture=present`, ffmpeg lists its formats - **and two ROMs uploaded `OK!`
still captured pure black.** They then built `-DHW_TESTCARD`, a ROM where the
68000 writes colour bars straight into the backbuffer with no GPU and no Blitter:
**that ROM cannot be black if the video path works, and it captured black too.**
So the fault is upstream of the card - Jaguar -> upscaler -> Cam Link cabling, or
the wrong Cam Link input.
⇒ **A capture CANNOT verify a hardware boot right now.** Do not spend a run
reading black frames as renderer bugs; that trap already cost this project nine
phantom "black boots". Ask what the TV shows.
⬜ **openlara should keep its own test-card ROM** for this fork - bubsy3d's advice
and it is right. `HW_TESTCARD` does not exist in this tree.

### ⏳ STILL AWAITING: WHAT DOES THE TV SHOW?
`/tmp/cofout7/OPENLARA.COF` was uploaded to the real Jaguar in run 75 (`OK!`,
lease clean). It is the first hardware boot carrying the portal fix, the TR1
jump-reach solve, the ledge-probe window fix, `climb_fits` and `FITSTEP`.
    title ring  -> the front-end works on silicon; press A twice and walk
    black       -> A10 boot lottery; `tools/roll_walk.sh <arm> 0 136 272 408 544 816`
                   (☠️ a power cycle fixes a LAYOUT miss, never a broken build)
    error screen-> a real fault; roll_walk scores a solid error screen as "LIT"

### ★ ROSTER: jag_bubsy3d IS SCRAPPED (user, run 76)
Recorded in `jaguar-shared/hw/RESOURCES.md` and pushed. Roster is FOUR:
openlara, quake, rr, resident. **The first slot is vacant and openlara is no
longer batched behind bubsy3d** - the rig is free to claim. Its capture findings
were deliberately kept in that file.

### ✅ MANSION DOORS: 10 OF 22 PROVEN TO CROSS - and that is a LOWER BOUND
    22 of 32 wall-portals are walkable at floor level; 10 crossed, 5 unproven,
    7 UNTESTABLE (seat resolves to an overlapping room)
☠️ **"FAIL" here means UNPROVEN, not broken.** `door_walk.py` walks in a straight
line from a stand-off, which only tests a door that is directly ahead and
unobstructed. The 5:
    2->5, 2->6   moved 164/188 - wedged ON the stand-off cell, never left room 2
    2->7         moved 7157, still room 2 (room 2 is large) - never lined up
    10->8, 11->8 moved ~6700 and saw room 12 - went somewhere else entirely
### ☠️ THREE HARNESS ITERATIONS THIS RUN, ONE OF WHICH WAS MY OWN REGRESSION
  1. Sweeping the span and taking the FIRST exclusively-owned cell sent her to
     stand-offs 1800 out at the span edges; she walked 10,000+ units into a third
     room and the score went **10/12 -> 7/14**. Fixed by RANKING candidates
     (doorway centre first, exclusivity and distance as tie-breakers) - back to
     10 crossed while testing 22 doors instead of 16.
  2. Walking 12,700 units at a door 1,300 away carried her through and out the
     far side, so an "ended in dst" test scored working doors dead. Now walks 5
     steps and asserts on the set of rooms REACHED.
  3. (run 74) A portal has a HEIGHT; ignoring it failed 7 balconies.
★ The lesson each time: **the tool was wrong, not the game.** A new instrument
that disagrees with something already seen working is measuring itself.

### ⬜ NEXT
  1. **`door_walk.py --prefix mrt`** - the Caves' 62 wall-portals, never tested
     door by door. The harness is now three fixes better; this is ready.
  2. The 7 UNTESTABLE seats need a stand point from a cell the source room owns
     exclusively *and* that the runtime agrees with - poke, settle, and CHECK
     `g_curroom` before walking (the tool already does; it just has no fallback).
  3. The pool: swim down through the room 14 water surface, expect room 18.
  4. A test-card ROM (`HW_TESTCARD`) so a black TV can be split into
     "renderer" vs "video chain" without borrowing another project's ROM.

### ☠️ CLOSED - DO NOT REOPEN
    mansion holes (50-59) · pickups (65) · mid-walk LOADING (67) ·
    caves black wedges (69, OPEN SKY) · caves room crossing (72, FIXED) ·
    gym room 18 (73, it is the POOL) · 7 "dead doors" (74, they are balconies)

### ★ INSTRUMENTS
    jag_gd.sh upload|status|power           the rig, via the shared jaghw lease
    door_walk.py <rom> <elf> --prefix P     walk every doorway, assert the flip
    portal_open.py --prefix P [--patch|--audit]
    release_play.py --tour [--gym] · probe_spot.py --raw= / --set=
    sightline.py · entity_check.py · room_cycles.py
    HOLEVIS=1 / DREWVIS=1 / HOPDEPTH=N / CULLCOUNT=1 BEXCNT=1 WCCNT=1
☠️ SYMBOLS ARE PER-BUILD.  ☠️ REBUILD THE ROM AFTER AN ASSET PATCH.
☠️ FPS cannot be measured offline, and on hardware it needs a working capture.

### ✅ WHAT IS DONE
    climbing   CAVES 24/24 ledges, 6/6 walls · MANSION 24/24, 6/6
    walking    CAVES crosses 0->1->2 · MANSION 10/22 doors PROVEN
    enemies    BEAR and WOLVES render on shipping flags
    pickups    MEDIKIT_SMALL collected on contact, verified against a control
    release    /tmp/cofout7 - on the real Jaguar since run 75, verdict pending

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
