# jag_openlara — autorun state

RUN: 1

**This file is how work survives a context ending.** A context can end without
warning; anything the next run needs must be here, not in the conversation.
`tools/session_run.sh end` **refuses to advance the counter** if this file was
not touched, because a run that learned something and did not write it down has
lost it.

Every run: `tools/session_run.sh start` → work → `tools/session_run.sh end "<summary>"`.
Every 25 runs the script prints a PROGRESS REPORT DUE banner — the user reviews
direction at that point and decides whether it is still valid. **Do not
summarise that checkpoint away.**

### What may interrupt the 25 — and what may not (user, 2026-08-17)

> **The full working agreement lives in `/home/jvilla/Documents/Git/jaguar-shared/DEVELOPMENT.md`** — read it
> before your first run. This is the project-local copy of the loop rule.

Run the 25 autonomously. Exactly **two** things break the loop early, and only
one of them ends it:

1. **A question for the user.** Direction, an irreversible change, a fact only
   they can supply. Stop, ask, wait.
2. **Needing the real Jaguar — this does NOT stop the loop.** Queue it and
   carry on:

   ```sh
   jagq run <rom> --frames 3   # queue → flash → capture → hand back
   jagq status                 # who holds it, who is waiting
   ```

   `jagq run` blocks until your turn arrives, so *waiting for the rig is part of
   the run*, not a reason to end it. Exit 0 = ran; 3 = job failed
   (`jagq log <id>`); 4 = broker down (`jagq up`).

⭐ **The arbiter is now `jagq`, not `jaghw`** (user's call, 2026-08-17).
`jag_gd.sh` keeps working unchanged — `hw/jaghw` forwards to `jagq`, and `jagq`
exports `JAGHW_HELD` as well as `JAGQ_HELD` so its re-entrant nesting still
composes. Two things that were this project's problem are now the broker's:

- **The turn cap is enforced centrally** — 300 s for every lease, session token
  and hold, whatever the caller asks for.
- **The capture card is inside the exclusion.** `climb_matrix.sh:110`'s five
  acquisitions with the settle and the `grab` *unlocked* — the shape that
  photographs another project's frame — cannot happen through `jagq run`, which
  flashes and captures inside one turn. Worth re-reading that loop against
  `jagq run --capture`.

See the migration section at the top of `jaguar-shared/hw/PROTOCOL.md`.

---

## NEXT STEP

# ★★★★★ THE "GRAINY" FMV IS **SILICON-ONLY**, AND IT IS NOT THE ENCODER.
# ✅ ENEMIES TURN, WALK AND STAND ON THE GROUND - FOR FREE.
# ✅ ROOM 22 (THE BRIDGE ROOM) IS **+20.6%** FASTER.

The user played the release on the TV and reported three things. All three are
now diagnosed, and two are fixed and measured.

### 1. ★★★★★ "GRAINY VIDEO" ON EIDOS/CORE/INTRO - DO NOT RE-ENCODE
He sent a photo: dense speckle over the whole picture, image still legible
underneath. **The same ROM, the same `.JV` file, is CLEAN in jagemu** (frames in
`/tmp/fmv_emu`) and CLEAN when the file is decoded offline
(`tools/jv_decode.py`, new). The hardware capture measures **~14% of pixels far
from their local median**; the emulator measures none.
⇒ The file is right, the encoder is right, the kernel LOGIC is right, and the
corruption happens in the **write path on real silicon**. This is case 7 of the
emulator-passes/silicon-dies class.
☠️ I spent the first half of this thread sweeping VQ encoder settings
(`JV_VQCOLS=256` bought +0.27 dB PSNR, nothing visible) before taking the one
measurement that split the hypothesis in half. **Ask "does it reproduce
offline" BEFORE tuning the thing you assume is at fault.**
⬜ NEXT for this: `gpu_jvdec.gas` writes decoded blocks straight into the
framebuffer. Suspects, in order - phrase alignment of the block writes, and
writes landing while the OP reads the same buffer (the kernel header already
records a CRT flash from writing the DISPLAYED buffer, so this path has form).
★ The encoder now has knobs (`JV_VQDN`, `JV_VQTRAIN`, `JV_VQITER`) and
`tools/jv_decode.py` scores any clip in PSNR against its source - keep them,
they are how a real quality change gets judged, but they are not this bug.

### 2. ✅ "THE LAND ENEMIES FLOAT TOWARDS LARA" - fixed, and it costs NOTHING
Three separate defects, all confirmed in the source before touching it:
  * `build_ent_model` had **no rotation term at all** - every wolf/bear/bat was
    drawn in its baked orientation whatever direction it moved.
  * the chase stepped X and Z **independently** at a fixed speed - a homing box.
  * `g_baty[e]` tracked **Lara's** Y, so they hovered to her height.
  * the run cycle came off `g_batframe`, ONE counter shared by every enemy and
    stepped on the bat's wing-flap clock.
Now: a per-enemy heading (`ent_ang8` + a 33-byte atan table) that turns at a
RATE and then walks FORWARD; the floor sampled under its own feet; a per-enemy
gait driven by distance covered; and the model rotated by that heading (.14
fixed so gcc emits `muls.w` - 16.16 would call the 32x32 helper per vertex on a
264-vertex wolf).
**Measured in room 22, Lara walking in: 6.02 fps before, 6.07 after.** Free.

### 3. ☠️☠️ THE FIRST CUT OF THAT COST 31% OF THE FRAME RATE (6.02 -> 4.13)
Two causes, both worth remembering:
  * `room_floor_mr(rsect, g_nrooms, ...)` **scans every room it is handed** -
    all 38, per enemy, per frame. The spawn code already had the cheap form:
    hand it ONE room (`&rsect[room], 1`). Now it also only re-asks when the
    animal actually moved.
  * the blob was rebuilt every frame for a wolf standing still. It is keyed on a
    **pose generation** counter now (exact, not a hash - a collision would
    freeze an animal mid-stride), so a biting wolf rebuilds zero times.

### 4. ✅ THE BRIDGE ROOM: +20.6%, MEASURED, AND THE CEILING IS KNOWN
Room 22 is the level's worst room and it holds **all twelve** bridge entities
plus 2 wolves. They were gated on DISTANCE only (6144), so bridges behind the
camera were submitted in full every frame.
    bridges drawn (baseline)        6.02 fps
    ENTVIEWCULL=1 (behind camera)   **7.32 fps   +20.6%**
    NOBRIDGEDRAW=1 (the ceiling)    8.07 fps   +33%
So the cull banks 62% of everything the bridges cost. It is BEHIND-ONLY on
purpose: a wrong lateral test pops a bridge out at the screen edge, trading a
visible bug for invisible cycles.
⚠️ **The cull is reasoned, not pixel-verified** - see the trap below - so it is
a FLAG, not yet in the shipping recipe. The user is sitting in that room; if a
bridge pops, that is what to look for.

### ☠️ FOUR INSTRUMENT TRAPS, ALL HIT THIS RUN
  1. **`frame_count` is FIELDS.** Reading it as frames says 60.00 fps on every
     arm. `g_drawframes` is the rendered counter (main.c:1143 says so).
  2. **`g_drawframes` was OPTIMISED AWAY.** Nothing in a shipping build reads
     it, so gcc deleted it and `nm` had no symbol - the offline fps instrument
     the source documents could not be read on any shipping-flag arm. It is
     `static volatile` now.
  3. **A STATIC SCENE PRICES NOTHING.** Room 22 with Lara idle and the wolves
     asleep beyond their 8192 activation radius gave two arms with completely
     different enemy code the same 400 frames. `tools/fps_offline.py --drive up`
     exists for this.
  4. **A PIXEL A/B BETWEEN ARMS IS INVALID IN THIS ENGINE.** Movement advances
     per RENDERED frame, so the faster arm is somewhere else at the same field
     number: 10 of 10 frames differed, one by 74%, for a change that touches
     nothing visible. Even "stand still and turn" diverges, because the turn is
     per-frame too.

### ⬜ NEXT
  1. The FMV write path on silicon (item 1) - the one open defect, and the
     user can see it.
  2. Get `r22_try_p408.cof` in front of the user (queued, see RIG below).
  3. If no bridge pops, put `ENTVIEWCULL=1` in `tools/build_cof.sh`'s shipping
     flags and re-gate both levels with `release_check.py`.
  4. Wolves still have no PATH - they walk into walls on the way to Lara. The
     visible complaint is fixed; TR1's zones/moods are not implemented.

### ⚠️ BUILD STATE
`/tmp/cofout11/` = the shipping payload with the SLIDE fixes, `QUALITY=playable`
/VRESN=80, PADTEXT=136, md5 `eb73701d8800e6455c064f3b6d4135bc` - **this is the
one that boots and that the user played.**
Arms built on top of it (enemy AI + `ENTVIEWCULL=1`):
    /tmp/ship_ai_p408.cof   shipping path (title ring) - **BOOTS on silicon**
    /tmp/r22_try_p408.cof   AUTOSTART + SPAWNAT room 22 - queued, untested
    /tmp/r22_{base,ai,ai2,cull,nobr}_dv.cof   the measured A/B set (DREWVIS=1)
☠️☠️ **ANY CODE CHANGE RE-ROLLS THE A10 PAD.** PADTEXT=136 booted the slide-fix
ROM and is BLACK with the enemy AI in it. Rolled: 136 black · 0 black · 544
black · **408 BOOTS**. Build all six pads first (`make PADTEXT=$pt ...`), then
flash them one at a time; the emulator cannot tell you which.
☠️ jagq reports `signal content` on a frame that is 1501 bytes of nearly-black.
**Look at the frame**; its blank/content threshold is not a boot verdict.
★ Only `main.c` changed, so a ROM rebuild + `cp` into a copy of the payload dir
is enough - the disc extraction does not have to be re-run.
☠️ `tools/build_conf.sh` takes **caves|gym|both** - not `mrt`.
☠️ SPAWNAT does NOT land where you ask when the point is inside two rooms'
boxes: room 10's (56832, 57856, y 3328) put her in room **11** at y 6656.
Walk her to the spot instead, or pick a cell no other room's box contains.
☠️ `release_play.py` WIPES its output dir first: a drive that dies while booting
otherwise leaves the PREVIOUS run's complete frame set sitting there (54 stale
PNGs read as this run's output; only the mtimes gave it away).

### ⬜ ALSO OPEN
  1. `jagq run /tmp/r22_try_p408.cof` - queued and FAILED for rig reasons twice
     ("Jaguar GameDrive not found / Insufficient permission"); jag_viewpoint's
     reboot failed in the same minute, so it is the shared board re-enumerating,
     not our ROM. Re-queue it.
  2. The 8 untestable door seats; `HW_TESTCARD`.

### ⭐ jaguar-shared MOVED - re-read on the user's instruction (2026-08-18)
14 new commits. The four that touch this project:
  * **`hw/POWER.md`** - the Kasa cycle is now a shared, documented PROCESS, and
    the driver moved to `hw/jagpower`. Use
    `jagq exec --lease 120 -- ~/Documents/Git/jaguar-shared/hw/jagpower cycle`
    rather than this repo's `./jag_power.sh`, and walk its "is it actually
    hung?" table first - reaching for the plug too early has cost sessions their
    turn.
  * **"a wedged USB link looks like your own bug"** - SIX turns failed across
    FIVE projects on 2026-08-17 before anyone said so. That is exactly what my
    `exit -6` and "GameDrive not found" were; jag_viewpoint's reboot failed in
    the same minute. **Check `jagq history` across projects before blaming your
    ROM or re-queueing.**
  * **"one clone per project" + "a foreign commit in your clone can invalidate
    every number you have"** - jag_quake's cobweb had a stray commit in the
    OBJECT PROCESSOR model, the part of jsim that sets display bus load. ✅
    CHECKED OURS: `jag_openlara/cobweb` is 12 behind origin/main, **0 ahead,
    clean** - no stray, so this run's fps numbers stand. ⭐ The rule to carry:
    after ANY shared-instrument change (cobweb pull, jas fix, new jagemu),
    RE-MEASURE THE BASELINE before trusting a comparison against it.
  * **video: the OP-list rebuild must follow the field WRAP** (jag_rr, measured
    on hardware) and **jsim reports PAL while the rig is NTSC**. Not our symptom,
    but the same family as the FMV grain: a display-path defect the emulator
    cannot show.
★ A LEAD FOR THE GRAIN FROM THAT TABLE: "per-field gate compares raw VC - bit 11
is the FIELD flag, so it misses every second field and the capture AVERAGES the
two". `gpu_jvdec.gas` writes the BACK buffer only and re-applies the previous
frame's tokens to keep the pair coherent. If that re-apply is not exact, buffers
A and B differ by the blocks that changed two frames ago - and a display
alternating between them is speckle on a CRT and in a capture, while jagemu
grabs ONE buffer and looks clean. **Test offline first: grab consecutive
published frames during the FMV and see whether the picture alternates between
two versions.**

### ☠️ THE RIG, LEARNED THE HARD WAY THIS RUN
  * **`jaggd ... exit -6` means the USB link is WEDGED, not that the ROM is
    bad.** It began right after a `--no-reboot` turn left a ROM running and
    another upload went in on top of it. `jagq reboot` did NOT clear it.
    What did: `jagq exec --lease 120s -- ./jag_power.sh cycle` (the Kasa plug),
    then the very next upload succeeded. ★ Take the power cycle UNDER A LEASE -
    raw `jag_power.sh` yanks the board out from under whoever holds it.
  * The rig is genuinely busy now - five projects, a real queue. `--no-wait`
    plus `jagq status` beats blocking on `jagq run`.

### ☠️ CLOSED - DO NOT REOPEN
    mansion holes (50-59) · pickups (65) · mid-walk LOADING (67) ·
    caves black wedges (69) · caves room crossing (72) · swimming IN (78) ·
    7 "dead doors" (74) · caves 11->12 + switch/door (79-82) ·
    all mansion door failures (85) · all Caves door failures (86-88) ·
    **mansion pool climb-out (93)** ·
    **Lara's Home soft-lock + "the mansion is silent" (116) - one bug, the
    slide travelled uphill; do not re-open either as an AUDIO question**

### ✅ WHAT IS DONE
    climbing   CAVES 24/24 ledges, 6/6 walls · MANSION 24/24, 6/6
    walking    CAVES 51 doors · MANSION 9 · zero defects either level
    swimming   enter/swim/room 18/renders · climb-out HALF FIXED (see above)
    switches   fire, animate, open the door, and she walks through
    enemies    BEAR and WOLVES render on shipping flags
    pickups    MEDIKIT_SMALL collected on contact, verified against a control
    frames     asserted by checkshot at BUILD time and at every driven capture
    rig        leases capped at 5 min; `jag_gd.sh endturn` reboots at turn end
    release    /tmp/cofout11 - 9/9 in the EMULATOR on BOTH levels; hw pending
    slopes     she slides DOWNHILL and faces the slope; mansion lock closed

### Toolchain — STILL PINNED to 59e5896 (`COBWEB_DIR=/tmp/cobweb-old`)
`beb2c15` does NOT fix the jcc68k regression (16-bit param read moved +2,
breaking gcc/jcc mixed links). `tools/toolchain_smoke.sh` guards updates.
⬜ cobweb moved to `8d09c43` (6 commits: jcc68k unsigned literal suffixes, a
jcc68k graphical smoke test, a jagemu profiling-coverage fix, the green-is-six-
bits bench). **None of them touch the 16-bit parameter ABI**, checked by log -
so the pin stands and taking the update buys nothing this project needs today.
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

- ✅✅ **THE CAPTURE CARD IS BACK** (verified 2026-08-17) — this answers run-25
  checkpoint question #1. `/dev/video0` and `/dev/video1` both enumerate, and a
  grab through the broker returned the **GameDrive menu listing our own
  `OPENLARA.COF`, `CAVSLOAD.BIN`, `GYMLOAD.BIN`, `INPUT.BIN`** off the SD card —
  183 KB PNG, where a no-signal frame is ~1 KB. **The `WORLDCOUNT` framebuffer
  diagnostic is viable again**, and rig time no longer produces a picture only a
  human can read. The 2026-08-16 "physically unplugged" finding is superseded.
  ⭐ What survives from it: there is still **no channel to read a number
  *directly* off the board** — no jaggd memory-read, no GD BIOS FWRITE, bulk IN
  times out, CDC has no Jaguar-side write path. Telemetry still has to be
  painted into the framebuffer and read off a capture; the difference is that
  the capture now works.
- **The rig is shared, and arbitrated by `jagq`.** A session not on the roster
  is **queued, not refused**. `jaguar-shared/hw/RESOURCES.md` is the agreement,
  `hw/GD_ACCESS.md` is this project's gateway, and
  `/home/jvilla/Documents/Git/jaguar-shared/DEVELOPMENT.md` is the working
  agreement over both.
- **Batch what needs the console.** Items #2, #8, #10, #11 plus visual
  confirmation of the enemy skins and the mansion are ONE console session, not
  six — now bounded by the 5-minute turn, so batch only what fits in a turn.
- Claim the rig **only** for work the emulator genuinely cannot answer.
