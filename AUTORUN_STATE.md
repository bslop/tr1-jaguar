# jag_openlara — autorun state

RUN: 47

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

# ✅✅✅ THE 1792 LEDGE WAS A REAL PSX-PARITY BUG, AND IT IS FIXED

    CAVES    24/25 CLIMBED   0 PARTIAL   1 NO-CLIMB   (the 1 is a 2048 WALL)
    MANSION  20/30 CLIMBED   0 PARTIAL  10 NO-CLIMB   (6 of the 10 are WALLs)

Run 45 closed the Caves 1792 JUMPGRAB as "physics, not a bug: measured peak jump
774, ledge 1018 further up." **That was wrong on both halves.** The 774 was the
harness, and TR1 does reach that ledge.

### ★★★★★ TR1 DOES NOT JUMP AT A FIXED SPEED — IT SOLVES FOR THE LEDGE
`fixed/lara.h:1586`, in the engine we are porting:

    else if (cinfo.f.floor >= -1920 && cinfo.f.floor <= -896) {
        goalState = STATE_JUMP_UP;
        extraL->vSpeedHack = sqrt(-2 * GRAVITY * (cinfo.f.floor + 800)) + 3;

For a ledge 896..1920 above her, TR1 computes exactly the launch speed that
arrives. We launched every auto-jump at a flat `JUMP_VEL_UP` 110 — and then,
because a flat 110 cannot reach higher, **capped the armed band at
`LARA_JUMPGRAB` 1664**, its own comment deriving the cap from the flat velocity.
So the cap was a workaround for the missing solve, and a 1792 ledge was never
even armed. ★ The formula self-checks: at the top of the band it gives 118, and
the branch just above it (ledge past 1920) uses a flat 116.

Fixed in three parts, each measured:
  1. `jump_reach_vel()` — TR1's solve, bit-by-bit isqrt (no libm, no 64-bit).
     Clamped to never return less than 110, so the change is MONOTONE and no
     jump that already cleared its ledge could regress. None did.
  2. `LARA_JUMPGRAB` 1664 -> **1920**, TR1's real band.
  3. `LARA_GRABTOP` 800, split from `LARA_GRABREACH` 720. **This was the last 18
     units.** With the solve in, she rose exactly 990 — the predicted discrete
     apex for a launch of 112 — but the grab TEST still reached only 720+64, so
     her hands topped out at 1774 against a 1792 ledge. The launch was solving
     for 800 (the `+800` in TR1's own formula) while the catch tested 720: two
     halves of one move disagreeing by 80. `LARA_GRABREACH` still PLACES her on
     the lip at 720, where it must equal TR1's `LARA_HANG_OFFSET` 724.

`ROSE 990 -> 1792` on that spot, and Caves went 23 -> 24 with the PARTIAL gone.

### ☠️ EIGHTH INSTRUMENT FALSE-DEFECT — the "774 peak" was the harness
`conformance.py` drove `up,a` for JUMPGRAB spots on the theory that a high ledge
needs a running jump. It does not: `main.c` arms the AUTO JUMP-REACH on
`(PAD_UP && g_fwdblk)` alone — walk into the wall and the game jumps for you.
The A launched a MANUAL jump first, and with UP held that selects the
DIRECTIONAL jump at `JUMP_VEL_FWD` 100, **apex 784**. That is the "774". It was
never the up-jump (110, apex 954), let alone the solved one. Now it holds UP and
stays out of the way. Same shape as the other seven: a uniform failure with a
uniform cause in the harness.

### ★★★★★ THE ANSWER CAME FROM THE SOURCE, NOT THE FOOTAGE
Run 45 left "does TR1 reach the 1792 ledge?" for a PSX-footage comparison. The
engine we port from answers it exactly, in a constant, with a cross-check —
which no amount of squinting at video would have given. **When the question is
"what does TR1 do", `OpenLara-master/src/fixed/` is a better authority than the
capture.** `res/` still owns questions about how it LOOKS.

### ☠️☠️ `tools/build_conf.sh` — THE ROM FLAGS WERE NEVER WRITTEN DOWN
Runs 40-45 recorded only the PATHS `/tmp/conf.cof` and `/tmp/gym.cof`, never the
flags. This run needed to re-measure and could not reproduce the ROM the old
numbers came from — the "a value only in shell history is not a setting" trap,
and it cost two dead builds:
  * AUTOSTART with no BOOTVID -> flat blue screen past 2600 fields, all 25 spots
    UNTESTABLE with garbage floor reads.
  * FASTBOOT instead -> renders to ~frame 400, then **100% black**. (Ruled out
    as my own patch by rebuilding the identical flags from the pre-patch source:
    identical failure. Always do that before blaming the change under test.)
The set that works is **`tools/toolchain_smoke.sh`'s**, deliberately, because it
is the only combination with a RECORDED RENDERING BASELINE — `.toolchain_baseline`
1.1% black / maxluma 217 at frame 1500, which this ROM reproduced exactly. Both
conformance ROMs now come from one script that also verifies each flag reached
the compile line.

### ⬜ WHAT IS ACTUALLY LEFT
1. **4 genuine mansion failures**, and they CLUSTER — three in room 8 at floor
   1280 (one CLIMB3 768, two JUMPGRAB 1024) and one CLIMB2 512 in room 1 at
   floor -1280. Every other spot at those same classes climbs, so this is a
   place, not a class. ☠️ Hand-drive ONE before believing it.
2. ☠️ **ROOM 8 IS THE SUSPECT, and the two instruments agree on it.** I assumed
   the mansion black outliers were a baseline artifact (the old 36.3% came from
   a ROM at a different VRESN) and re-measured to prove it — **the new baseline
   is 36.8%, essentially identical**, so that explanation is dead and the
   outliers are REAL. What is left points one way:
     * room 8 CLIMB3 spots read **51.1 / 38.7 / 37.4%** black *while climbing
       successfully* — over baseline with no failure to blame it on;
     * room 8 holds **3 of the 4** genuine climb failures, all at floor 1280;
     * room 0 WALL reads **77.8%** twice, the highest in either level.
   A climb failure and a black outlier in the same room is the signature both
   earlier collision bugs had (53% and 81%). Start at room 8, floor 1280.
3. Caves is CLEAN: 0 spots over baseline, 0 PARTIAL, walls refuse.

### ✅ RELEASE READY — the full recipe ran end to end this run
`tools/build_cof.sh` produced all 7 files, `OPENLARA.COF` 1,538,068 B, with the
gym boundary patch and `--faces` on both prefixes for the first time. Booted in
jagemu with `--sd` and FILMED: Core Design logo -> the full FMV intro -> TOMB
RAIDER title with a working ring menu (passport rotating, "A Select / Game").
The whole front-end chain is verified, not assumed. Output in `/tmp/cofout4`.
★ `build_cof.sh` now DEFAULTS `COBWEB_DIR` to the pinned toolchain instead of
trusting the caller to export it — forgetting it ships a black release.

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
