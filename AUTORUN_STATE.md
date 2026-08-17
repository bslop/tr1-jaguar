# jag_openlara — autorun state

RUN: 82

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

# ☠️ THE DOOR IS NOT THE BUG. ALL FOUR MOVE-GATE CLAUSES PASS AND SHE STILL STANDS.

`MVDIAG=1` (new this run) makes the move gate REPORT which clause vetoed a
blocked step, instead of me eliminating them by inference. At the Caves 11->12
door, with fresh addresses from THIS build:

    up0..up3  walks 50169 -> 49441
    b5..b11   swact 45 (switch pull anim), swpull[10]=1, door angle 6 -> 36
    b12       swact 0 (anim completed and RELEASED), door 42
    up14..23  **veto = 5**, door **66** (fully open, cap 64), x pinned 49441

    veto legend: 1 wall · 2 no floor · 3 door blocks · 4 step-up · **5 NOTHING refuses**

So: the switch fires, its animation completes and releases, the door swings fully
open, **and every clause in the gate passes** - yet `g_lax` never changes and
`g_fwdblk` is 1. The door and the switch are both EXONERATED. So is the sector
data (run 79) and the door-clearance check.

### ⬜ NEXT: THE DESTINATION EQUALS HER POSITION. INSTRUMENT nx AND spd.
`veto=5` with no movement leaves exactly one shape: the gate assigned
`g_lax = nx` and **nx was already g_lax**, i.e. the step computed to zero.
    nx = g_lax + (SIN(g_layaw) * (spd*mv)) >> 16
    spd = RUN_SPEED_TR1 * g_ticks >> 1     (TIMESTEP)
Both x AND z are pinned, and a unit vector cannot have both components zero, so
**spd is 0** or the shift underflows. Add `nx`, `spd`, `g_ticks` and `g_layaw` to
MVDIAG and read them at x=49441 - one run, one answer.
☠️ Do NOT go back to reading the source and reasoning about it. Three eliminations
by inference (sector, door, switch) each looked sound and each cost a run; the
peekable byte settled all three in one probe.
★ Suspect worth having in mind but NOT assuming: `g_swact > 0` forces `mv = 0`
(main.c:4717 and :8117). It reads 0 in the telemetry above, so it is not the
cause - but note there are `g_swact = 99; /* DEBUG: never expire */` lines at
:7324 and :8794. **:8794 is behind `#ifdef SWDEBUG` (inactive), :7324 is NOT** -
check what guards :7324 before trusting that it cannot fire.

### ★ `MVDIAG=1` and `probe_spot.py --phases` (both new, runs 80-81)
    --phases "up:4,-:1,b:8,-:1,up:10"     ("-" releases; one key set cannot work
                                           a switch - holding up,b leaves her
                                           completely immobile)
☠️ **A `static int` that is only WRITTEN does not exist.** The first MVDIAG build
had no `g_mvveto` symbol at all - gcc discarded it as dead. Anything whose only
reader is a debugger must be `volatile`.
☠️☠️ **SYMBOL ADDRESSES ARE PER-BUILD and I tripped on my own rule this run**: I
read `g_dooff` at the previous build's address and got `door=0` for the whole
run, which flatly contradicted run 80 and would have "proved" the door never
opens. Re-read every address from the ELF that came out of the build under test.
    this build: g_mvveto 0x154ba8 · g_mvnf 0x154ba4 · g_swact 0x154b78
                g_dooff 0x140502 (int16[60], entity 9 at +18)
                g_swpull 0x14057e (uint8[60])

### ★ FROM jag_viewpoint: A SCREENSHOT IS EVIDENCE ONLY WHEN SOMETHING READS ITS PIXELS
Their "missing" 1-pixel border was never missing - rows 0/223 and columns 0/319
measure 100% white and always did; a 1px line is invisible in a downscaled view.
They invented a phantom bug from a *working* image minutes after writing down
"measure, don't eyeball" from the OLP trap. Their response was to stop relying on
discipline: `make verify` runs a ~90-line `checkshot.py` that asserts geometry,
all four border edges, per-band channel isolation and that each ramp ramps (13/13).
⬜ **WORTH COPYING HERE.** Our equivalent of their border was the rightmost pixel
column, black in EVERY scene for months (`RCLIPFIX`) because a right-exclusive
span end was clamped to an INCLUSIVE clip value. An assertion over a rendered
edge would have caught it immediately. We currently eyeball black%/maxluma against
a remembered baseline - which has already produced one phantom defect (run 67's
"LOADING screen" read off a contact sheet).

### ⏳ STILL AWAITING THE USER: WHAT DOES THE TV SHOW?
`/tmp/cofout7/OPENLARA.COF` on the real Jaguar since run 75 (`OK!`). Capture is
dead upstream, so the ladder collapses to the TV.

### ☠️ CLOSED - DO NOT REOPEN
    mansion holes (50-59) · pickups (65) · mid-walk LOADING (67) ·
    caves black wedges (69, OPEN SKY) · caves room crossing (72, FIXED) ·
    gym room 18 (73/78, the POOL - swimming WORKS) ·
    7 "dead doors" (74, balconies) · caves 11->12 geometry (79) ·
    the DOOR and the SWITCH themselves (81 - both work; veto=5)

### ✅ WHAT IS DONE
    climbing   CAVES 24/24 ledges, 6/6 walls · MANSION 24/24, 6/6
    walking    CAVES 50/58 doors · MANSION 9/18
    swimming   the mansion POOL: enter, swim, room 18, renders correctly
    switches   fire, animate, release, and swing their door fully open
    ☠️ but     she cannot walk through afterwards - cause narrowed, see above
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
