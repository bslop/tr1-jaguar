# jag_openlara — autorun state

RUN: 49

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

# ✅ SHE COULD CLIMB INTO SPACES SHE DOES NOT FIT IN. TR1 CANNOT.

                LEDGES climbed        WALLS correctly refused
    CAVES       24/24  (0 PARTIAL)    4/6   (was 1/6)
    MANSION     24/24  (0 PARTIAL)    2/6   (was 0/6 - the class did not exist)

Every genuine ledge in both levels now climbs. What is left is one named defect,
below.

### ★★★★★ TR1 HAS A SECOND CLIMB CONDITION AND THIS PORT NEVER HAD IT
`Lara::checkClimb`, OpenLara `src/lara.h:2546`:

    canClimb = (floor - ceiling >= LARA_HEIGHT) && (h >= 256);

The step must be the right HEIGHT **and Lara must fit on top of it**. All four
ceiling tests in main.c asked a weaker question — "is the ledge below the ceiling
*above her*", `rsect[g_curroom]` at `g_lax/g_laz` — which says nothing about the
space over the LEDGE.

Measured, Caves room 22: one ceiling plane at 4352 sits over ledge tops at 4608
and 4864, giving **256 and 512 of clearance against her 762** — and she climbed
onto all of them, into the ceiling. Lara's Home is worse: **105 of 304** step-up
pairs are too short to stand in.

Fixed with `climb_fits()` (main.c:343), wired into all three climb entries
(vault, jump-reach arm, airborne grab). It reads the ceiling from `g_floorroom`
— the room the floor actually came from — so it answers about the same space,
and no ceiling data means ALLOW, so it can only ever refuse, never invent a
climb. Caves WALL refusals 1/6 -> 4/6 with LEDGES unchanged at 24/24; render
baseline still exactly 1.1% / maxluma 217.

### ⬜ NEXT: THE 256 AUTO-STEP IGNORES HEADROOM (all remaining failures)
Both levels' leftovers are `rise 256` WALKUPs — Caves rooms 3 and 19, mansion
room 1 x2 (+2 more). A 256 step is taken by the WALK floor-follow, which never
goes through vault/arm/grab, so `climb_fits` is not consulted and she walks up
into a 256-high gap. Gating the walk's floor-follow on headroom is the fix, and
it is RISKIER than this run's change because it touches ordinary walking on every
frame, not three climb entries. Do it behind an A/B and re-sweep both levels.
☠️ Note TR1 allows `h >= 256` steps *with* headroom - do not simply ban 256s.

### ☠️☠️ THE SWEEP WAS SCORING ITS OWN NEGATIVE CONTROL AS A PASS
`conformance.py` counted any "CLIMBED" as good. For a WALL spot climbing is the
FAILURE — they are the control that proves the engine refuses. It went unnoticed
while WALL only meant a 2048/2816 step nothing could climb. The moment
`ledge_census` began classing no-headroom pairs as WALL, the engine climbed five
of six and the summary printed **29/30 CLIMBED — its best score ever, describing
a level that had just got more wrong.** Now scored by class:
`LEDGES n/n climbed` and `WALLS n/n correctly refused`, listing each wall she
climbed. ★ A metric that cannot go DOWN when the thing gets worse is not a metric.

### ★ `ledge_census.py` now applies TR1's headroom rule
A pair whose target has < 762 of clearance is emitted as **WALL** rather than
dropped — an unclimbable step is exactly what a WALL spot is, and dropping them
cost the Caves its only control (25 spots -> 24, "0 NO-CLIMB" with nothing left
that could fail). Both levels now carry a balanced 6 per class.
☠️ I nearly reverted this: 105/304 mansion pairs being "too short" looked like a
misread field. It is not — the ceiling distribution across both levels is 1280 to
5632, i.e. real room heights, and Caves room 22's ledges genuinely sit under one
low plane. **Check the data's distribution before dismissing a result as a bug in
your reading of it.**

### ★ `tools/probe_spot.py` — hand-drive ONE spot, see every gate
    tools/probe_spot.py <rom> <elf> --at X,Y,Z,ROOM,YAW [--keys up,b] [--shots D]
Per step: g_gunst, feet Y, floor, room, g_floorroom, g_fwdblk, g_autoj, g_autojv,
g_jumped, g_lavy, g_hang, g_vault, x/z. ☠️ Frames come out 320x**80** (VRESN=80) —
upscale 3x vertically before judging one, or the room is unreadable.

### ✅ CLOSED THIS RUN: run 47's suspect was WRONG
Run 47 predicted room 8's failures were `room_floor_mr` skipping unreachable
rooms. Probing showed `g_floorroom` reaching 8 while `g_curroom` read 12, so room
8 WAS a candidate — the reachability filter was never the problem. Those spots
were the crawlspace above, and the census should never have offered them.

### ⬜ ALSO STILL OPEN
  * Mansion black outliers: rooms 15/16/17 CLIMB3 at ~45% and room 0 WALL at
    77.8%, against a 36.8% baseline. Not yet investigated.
  * ☠️ `/tmp/cofout4` (the filmed release) predates runs 46-48. **Rebuild before
    shipping** — it has neither the jump-reach solve nor either collision fix.

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
