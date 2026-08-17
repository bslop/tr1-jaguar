# jag_openlara — autorun state

RUN: 48

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

# ✅ THE LEDGE PROBE COULD NOT SEE PAST 848 UNITS. FIXED, +1 SPOT.

    CAVES    24/25 CLIMBED   0 PARTIAL   1 NO-CLIMB   (the 1 is a 2048 WALL)
    MANSION  21/30 CLIMBED   0 PARTIAL   9 NO-CLIMB   (6 of the 9 are WALLs)

Run 46 left "4 genuine mansion failures, clustered in room 8." Hand-driving one
with a new tool (`tools/probe_spot.py`, prints every gate on the climb path per
step) showed she walks at the ledge, **falls 768 through it**, ends up in a
different room, and hits a wall she correctly refuses. So the arm never fired.

### ★★★★★ THE ROOT CAUSE: A WINDOW SIZED FOR THE WRONG QUESTION
`room_floor_mr` discards candidate floors more than `FLR_UPWIN_LEDGE` = **848**
above the caller. That is correct for the AIRBORNE GRAB (hands reach 720+slack).
But the **AUTO JUMP-REACH ARM sets the same flag** while looking for ledges up to
`LARA_JUMPGRAB` = 1920 — so every ledge above 848 was thrown away *before it was
scored*, and could never arm.

★★★★★ **It worked in the Caves by luck.** Where only ONE room covers the column,
every candidate fails the window, `nfound` stays 0, and the search falls back to
"lowest floor wins" — which IS the ledge, being the only candidate. Lara's Home
room 8 has room 12 UNDERNEATH it (verified in the sector data: rooms 7, 8 and 12
all cover x=49664 z=40448 at floors 0 / 1280 / 2560). Room 12's floor passes the
window, so the fallback never runs and the ledge stays invisible.
**A bug that hides wherever the geometry is simple is the kind that ships.**

Fixed in three parts, both levels re-swept:
  1. `g_flr_upwin` — a caller-settable up-window; 0 keeps the mode default.
  2. **Ledge mode now prefers the floor ABOVE her**, not the nearest. "Plain
     closest" picked room 12's floor 768 BELOW over the ledge 1024 above, so the
     probe reported a drop where there is a block — and she walked into the
     block's column and fell through it. A vault/grab search asks "what can I get
     onto"; a floor below is the absence of an answer, not an answer. Ground mode
     keeps its own preference or walking off a crate snaps to the room above.
  3. The arm probe sets the window to `LARA_JUMPGRAB + 128`.

Mansion 20 -> **21** (the room 1 CLIMB2 recovered), Caves unchanged at 24/25,
render baseline still exactly 1.1% / maxluma 217. Nothing regressed.

### ⬜ NEXT: THE 3 REMAINING ROOM-8 SPOTS — AND THERE IS A NAMED SUSPECT
Two JUMPGRAB 1024 and one CLIMB3 768, all at x=**49664** (the four room-8 spots
that DO climb are at x=46592). The probe showed something the fix does not
address: seated in room 8 standing on room 8's floor, **`g_curroom` reads 12**.
Room resolution, not the floor search, put her there.

That matters because `room_floor_mr` SKIPS rooms up front:

    if (g_flr_limit ? !room_reachable(g_curroom_fwd(), r)
                    : !room_within3(g_curroom_fwd(), r)) continue;

If she is registered in room 12 and room 8 is not reachable/within-3 of it, then
**room 8 is never a candidate at all** and no window or preference can help. Test
that first: probe the spot and print `g_floorroom` alongside `g_curroom`, and
check `room_reachable(12, 8)`. ☠️ Do not "fix" the window again — it is not the
window this time.

### ★ `tools/probe_spot.py` — hand-drive ONE spot, see every gate
    tools/probe_spot.py <rom> <elf> --at X,Y,Z,ROOM,YAW [--keys up,b] [--shots D]
Prints per step: g_gunst, feet Y, floor, room, g_fwdblk, g_autoj, g_autojv,
g_jumped, g_lavy, g_hang, g_vault, x/z. A failure names the FIRST gate that did
not open, which is what separates a collision bug from a harness bug in one run
instead of three. It is how both of this run's findings were made.

### ☠️ NINTH INSTRUMENT NOTE: DRIVE UP+ACTION, NOT UP ALONE
Run 46 changed the JUMPGRAB drive to hold UP by itself and it passed all of the
Caves, which made it look right. It is not: the arm gate is
`(pad & PAD_UP) && g_fwdblk && (g_gunst == GST_OFF || (pad & ACT_ACTION))`, so UP
alone only works while her hands are EMPTY — and TR1's own rule is that she never
climbs without ACTION. The Caves have no guns, so it happened to work there and
would have broken the moment a build armed her. Now drives `up,b`. (Measured: it
changed no verdict either way, since `g_gunst` was already 0. Correct by rule,
not by luck.)

### ⬜ ALSO STILL OPEN
  * Mansion black outliers are REAL (baseline re-measured at 36.8%, essentially
    the old 36.3%): room 8 CLIMB3 reads 51.1 / 38.7 / 37.4% *while climbing*,
    room 0 WALL reads 77.5% twice. Same room as the climb failures.
  * Caves is CLEAN: 0 over baseline, 0 PARTIAL, walls refuse.

### ✅ RELEASE READY (run 46, unchanged)
`tools/build_cof.sh` ran end to end: 7 files, `OPENLARA.COF` 1,538,068 B, output
in `/tmp/cofout4`. Booted in jagemu with `--sd` and FILMED: Core Design logo ->
full FMV intro -> TOMB RAIDER title with a working ring menu. ☠️ That ROM predates
this run's collision fix and run 46's jump fix — **rebuild before shipping**.

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
