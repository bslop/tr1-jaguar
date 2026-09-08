# jag_openlara — autorun state

RUN: 14

**This file is how work survives a context ending.** A context can end without
warning; anything the next run needs must be here, not in the conversation.
`tools/session_run.sh end` **refuses to advance the counter** if this file was
not touched, because a run that learned something and did not write it down has
lost it.

Every run: `tools/session_run.sh start` → work → `tools/session_run.sh end "<summary>"`.
Every 25 runs the script prints a PROGRESS REPORT DUE banner — the user reviews
direction at that point and decides whether it is still valid. **Do not
summarise that checkpoint away.**

#### ☠️☠️ THE COBWEB PIN WAS DEAD AND THE DOCKER CACHE HID IT — 2026-09-07
`ARG COBWEB_REV=9da2f99` in the Dockerfile no longer resolves: cobweb's history
was rewritten upstream when it went public/MIT, and `git fetch origin 9da2f99`
returns *couldn't find remote ref*. **Every build kept working anyway** — Docker
had `jas` and `jcc68k` cached as layers from 2026-08-26 (`docker history` says
"12 days ago" while the image manifest says today), so the `RUN` that clones and
checks out has not executed since. A clean machine or one `--no-cache` would
have been the first to find out, weeks after the cause.

Re-pinned to the **same commits** by post-rewrite hash, matched on subject line
and date: `ba9c680 → f86794a` (mem-to-mem MOVE dropped its DEST reloc) and
`9da2f99 → 98a9822` (object-mode `.align` is section-relative). Verified the new
pin is an ancestor of `origin/main` — the assertion the old pin would have
failed. `cbb22a1`. Written back to `jaguar-shared` as the sixth stale-build
mechanism (`04b6f62`): the first five are a stale *artefact*, this is a stale
*reference*, and **a reference consumed only from a cache stops being validated**.

⚠ **The tip is NOT a free upgrade and demo34 was not built with it.** cobweb tip
additionally carries `e86d294` (2026-09-01) which *scopes* that `.align` fix to
relocatable objects only — a correction to the very behaviour the A10 boot
lottery turns on. demo34 is built by the cached Aug-26 toolchain, i.e. the exact
one behind every silicon-verified ROM. Bumping to the tip is a deliberate change
that wants its own A/B, and it will cost a full cobweb rebuild because the pin
edit busts the layer cache.

#### ☠️ TWO CONTAINERS WERE BUILDING INTO THE SAME `/out` — 2026-09-07
`docker ps` found **two** `tr-jaguar` containers, started 3m 49s apart, both
mounting `demo34/` as `/out` and both in the video stage. Killed the newer; the
survivor then exited **1** with `--rm` already having taken its logs, and
`demo34/` was empty. Rebuilt as a single container with the log teed to a file —
which is the actual lesson: **`docker run --rm` in the background leaves you
with an exit code and no way to ask why.** The rebuild script lives in the
session scratchpad and pins `BUILD_FLAGS` explicitly rather than relying on
`build_cof.sh`'s default, so the flag set is in the log too.

★ Added `tools/gate_release.sh` — the four cheap pre-rig checks (HRESN on all
three define paths and *absent* from the HQ kernel · zero instrument flags ·
no `move.l abs.l,abs.l` below `$4000` · DRAM headroom). ☠️ It counts SKIPs and
exits 2 with **GATES INCOMPLETE**: the first draft printed GATES PASS having run
one check of four.

#### ⬜ WHAT A VALID A1 TEST ACTUALLY REQUIRES — established 2026-09-07, before spending a turn
Tried to redo the DIVSAFE A/B in a better scene. Rotating the level-start spawn
180° (`SPAWNAT_ROOM=0 X=75264 Y=3072 Z=3584 YAW=32768`) puts her crown against a
lit corridor instead of the black cave mouth — but measured, the backdrop band
around the crown is **mean luma 45, and 49% of it is still darker than 40**.
Not good enough; half of it would still hide the notches. Did NOT flash it.

☠☠ **AND THE METRIC WAS WRONG ANYWAY, WHICH MATTERS MORE THAN THE SCENE.** The
whole-frame pixel diff I used on #2726/#2727 has a noise floor of **585..1182
differing px** (same arm, consecutive frames). Two notches in a crown are a
handful of pixels — they sit UNDER that floor in any composition. A perfect
scene would still have returned "no difference".
⇒ `OPEN_ISSUES.md` A1 already says this: three metrics have failed on this bug
(cream-threshold, neck-confound, fill-holes), and *"any future A1 metric must be
a silhouette/convex-deficiency measure"*. My pixel diff is a fourth instance of
the same mistake.

⬜ A VALID A1 TEST NEEDS ALL THREE, and none is expensive on its own:
  1. a scene where the crown sits against a genuinely BRIGHT backdrop (target:
     <10% of the band below luma 40, vs 49% at the rotated spawn);
  2. a SILHOUETTE / convex-deficiency metric over the head region — a notch is a
     boundary concavity, which is why hole-counting and pixel-diffing both read
     zero whatever the truth is;
  3. both arms in ONE rig turn, which #2726/#2727 already did correctly.
★ Build 1 and 2 OFFLINE first — jagemu renders the head solid (A1 is
silicon-only), so the emulator cannot validate the metric's verdict, but it CAN
validate the composition and exercise the metric's plumbing for free.

#### ⬜ DIVSAFE ON SILICON — jobs #2726/#2727 — FIX VERIFIED FREE, A1 VERDICT NOT AVAILABLE
Both arms in one turn, single variable confirmed on the assembler command line
(`DIVSAFE=0` 31 shadow warnings / `DIVSAFE=1` 21), 4 stills each, Lara idle
under PADMUTE.

    NOISE FLOOR  same arm, consecutive frames:  585 .. 1182 differing px
    CROSS-ARM    DIVSAFE off vs on:             314 ..  519 differing px

**The two builds differ LESS than consecutive frames of the SAME build.** The
cross-arm delta is under the capture chain's own noise, and the differing pixels
are a wide scatter (x 167..505, sx=118 sy=96), not a cluster at her crown.

☠☠ **BUT THE SCENE WAS WRONG, SO THIS IS NOT AN A1 RESULT.** The capture shows
Lara from behind with her head against the DARK CAVE MOUTH. `OPEN_ISSUES.md` A1
says in terms: *"Judge it against SNOW; the cave wall hides it."* That is
precisely the background here. The null is about THIS SCENE and says nothing
about A1.
⇒ ESTABLISHED: the fix is free, byte-identical with the flag off, and changes
nothing visible at the spawn.
⇒ NOT ESTABLISHED: whether it fixes A1. Needs a capture with her crown against a
LIGHT background before any verdict.

★ Fourth time today that verifying the measurement WINDOW before trusting the
number prevented a wrong claim — and the first three were mine catching mine.
Here the repo's own ledger had already written down the precondition I skipped.

### ✅ A5 REPRODUCES OFFLINE — recipe + metric, no rig needed (2026-09-07)
A5 ("black polygon at the cave mouth") was rig-bound and un-iterable. It now
reproduces deterministically in jagemu:

    # arm: ship flags + FASTBOOT=1, NO PADMUTE (it zeroes the injected pad word)
    jagemu video a5_walk/OPENLARA.COF --press up --press-after 380 \
        --start 380 --count 24 --every 6 --cols 6 --dir OUT -o strip.png

☠ `--press` is NOT in `jagemu`'s usage text for `video`/`run`, but both call
`press_args`, so it works. Buttons: up/down/left/right/a/b/c/option/start.

**MEASURED CHARACTER OF THE DEFECT** (160x120 view, largest connected component
of luma<=6, excluding the letterbox bands at y<6 and y>117):

    frame  2   180 px   x67..82  y37..51   75% bbox fill
    frame 10   219 px   same region
    frame 20   304 px   x64..83  y34..52   80% bbox fill

⇒ dense, compact, **pure luma 0**, at a FIXED world position, GROWING with
perspective as she approaches. That is a solid polygon on a world surface — not
cave darkness (the surrounding cave texels are luma 31/40/48/55, plainly varied)
and not a coverage hole. Consistent with the ledger's standing suspect: **a face
sampling black atlas texels (extractor UV issue)**.

⬜ NEXT, all offline and cheap now that there is a metric:
  · the blob-size number above IS the regression metric — bisect against it.
  · candidate discriminators: does it survive with SHADEPASS off? does the atlas
    contain a black region the UVs could land in? does the face's UV rect fall
    outside the packed cell?
  · only once a candidate fix moves that number does the rig matter.
★ The reproduction is the deliverable here, not a fix: A5 was previously
"OPEN, hopcap and shade-pass eliminated" with no way to iterate.

### ☠☠☠☠ SEVEN DIVIDE-SHADOW HAZARDS IN THE SHIPPING KERNEL — jas has been reporting them all along
Found 2026-09-06 by reading the assembler's own output instead of the tagged gcc
warnings. **`jas` emits these on every build and nobody had read them:**

    gpu_geotex.gas:3951: warning: r1 is read-modify-written inside the DIVIDE
    shadow from line 3946 — its operand is the destination field, which the
    scoreboard does not interlock, so the divide's late write can land AFTER
    this one and discard the result
    [HW: seen on silicon, invisible in emulation]
      fix: read the quotient into a scratch first (`move r0,rT` stalls
           correctly), operate on that, then write it back

**Seven live sites in `gpu_geotex.gas`** (the shipping renderer), each a `div`
whose DESTINATION register is read-modify-written inside its latency shadow:

    site  divide                                        dest
    1149  1145  div r25,r0                               r0
    1179  1175  div r25,r0                               r0
    2512  ~2456 div r7,r28 / r0                          r0
    2524  ~2515 div r7,r27 / r0                          r0
    3951  3946  div r23,r1   (|duint|<<16)/dy            r1
    3988  3983  div r23,r1   (|dvint|<<16)/dy            r1
    4119  4113  div r0,r1    (ua<<16)/dy  [16.16]        r1

☠ THE MECHANISM, at 3946 — and it is not subtle:
    3946  div  r23,r1      ; quotient lands LATE in r1
    3949  jump PL,(r22)    ; skipped when the slope is POSITIVE
    3951  neg  r1          ; RMW of r1 inside the shadow
    3955  store r1,(r2)    ; USL = uslope
If the divide's late write lands after `neg r1`, the negated value is replaced
by the raw quotient: **a negative U slope silently becomes positive**, reversing
texture stepping across that span. `neg` runs only on negative-slope spans, so
it corrupts SOME faces and not others — partial, orientation-dependent,
silicon-only.

⭐⭐ **THIS IS A STRONG A1 CANDIDATE.** A1 ("Lara's head: two notches /
see-through") is silicon-only, drops/misplaces FACES, has survived 12 flashes,
and its own ledger says the remaining candidates are HAZARDS, not arithmetic —
naming bug 25 (DIV re-issued while busy) and bug 13 (WAW). This is precisely
that family, it is in the per-span U/V gradient path, and it is invisible to
every offline oracle. ⚠ CANDIDATE, not proven: A1 has killed three metrics and
two theories already. Do not record it as the cause without a silicon A/B.

⬜ NEXT: fix per jas's own advice (quotient -> scratch, operate, write back).
Kernel budget is ~200 B free of 3680, seven sites at ~3 instructions each fits.
☠ Verify: kernel must still assemble under the ceiling, ROM byte-compare with
the fix OFF must be identical, then a silicon A/B judged against SNOW (the ledger
says the cave wall hides A1).
★ Other kernels carry the same warnings (gpu_bltex, gpu_textured, gpu_geomdirect,
gpu_geomwalk, gpu_geomxform) but are NOT in the shipping path — fix the shipping
one first.

★★★★★ METHOD NOTE: 516 warning lines per build, of which only 59 carry a `[-W]`
tag. The other 457 are the assembler's, and that is where the silicon hazards
live. Grepping for `[-W` misses them entirely.

### ★★★★★ THE BRIDGES ARE THE ROOM — user's hunch confirmed, already measured in-repo (2026-09-06)
User: *"I have a feeling it's the bridges."* They were right, and the number was
already sitting in `main.c` unreferenced by any campaign doc.

`MRT_ENTAUDIT=1` against the disc:
    type 68/69/70  BRIDGE_1/2/3   x4 each = **12 bridges, ALL in one room**
    type 7 ENEMY_WOLF             rooms [14, 27, 33, 35]  -- incl. that same room
`main.c:11019-11021`: *"Room 22 draws all twelve bridges on a pure DISTANCE
gate, and the bridges are worth **33% of that room's frame rate** (measured:
**6.07 fps with, 8.07 without**, Lara walking in)."*
`main.c:11027`: r22 is the level's worst room (**3.69** in the 38-room sweep)
and **the only one with bridges**.

⇒ The user's real-play collapse (8.2 -> 3.2 entering the room, -> 2.3 with
wolves) is that room: heavy geometry + 12 bridge entities + 2 wolves.

☠☠ **DUAL ROOM NUMBERING — I got this wrong once and it inverted a conclusion.**
`MRT_ENTAUDIT` reports **ORIGINAL TR1** room numbers; the code and the sweep use
**LOCAL** indices. Local 22 == original 14. I read "bridges in room 14", checked
14 against the heaviest-rooms table, did not find it, and briefly concluded the
room was cheap — when it is in fact r22, the worst room in the level. Always say
which numbering.

#### ⬜ ENTVIEWCULL ON SILICON — content-aligned, LOW confidence, does NOT support shipping it
Live playtest A/B, single variable vs job #2584 (same build config, ENTVIEWCULL=1
flipped): jobs #2584 control / #2699 cull, 300 s each, user driving.

☠ **A naive time-index comparison said +0.54 fps mean and is WORTHLESS** — the
two playthroughs put different content at the same timestamp (control dies at
t=230 and blacks out; cull has a dark stretch t=80-130 and RECOVERS). Same
fixed-index error as the offline arms, this time from human variability.
`tools/tour_pair.py`'s own docstring records this exact failure: index pairing
"silently compares different rooms", and once showed a 94% fix in a room that
was a different room.

✅ CONTENT-ALIGNED (nearest-image match on the bridge-room view), with the
metric CALIBRATED first so "match" means something:
    same view, +/-2s in the control's own run   dist  442 .. 2229
    anywhere else in the level                  dist 1619 .. 5020
    ENTVIEWCULL best match                      dist 1445   <- below any
                                                   "elsewhere" frame, inside band
    control 3.83 fps   |   ENTVIEWCULL 3.00 fps
⚠ **LOW CONFIDENCE, and it points the WRONG way for the cull.** One ~6 s window
each, two different playthroughs, different camera angles, and the same-view
band OVERLAPS the elsewhere band. It also contradicts the one comparable offline
arm (-2.0% cycles, -3.1% blits, i.e. slightly favourable).
⇒ **Do not ship ENTVIEWCULL on this.** Do not reject it on this either.
⬜ What would settle it: **ROOMTOUR** captures — a deterministic camera per room
is what `tour_pair.py` was built for, and it removes the human variable that
makes free-play A/B unmeasurable.

★★★★★ FOUR ATTEMPTS, FOUR FAILURE MODES, ONE ROOT: comparing two things without
first establishing they are the same thing. Per-field offline (arms diverge),
per-published-frame offline (scenes differ), fixed-index silicon (playthroughs
differ), content-matched (needed a calibrated distance before the match meant
anything). The check is always cheaper than the retraction.

#### ☠☠☠ FRAME-INDEXED OFFLINE A/B IS IMPOSSIBLE IN THIS ENGINE — closed, with the reason
Second attempt at pricing the bridges offline (spawn moved to the far end of the
cluster, ~6 sectors from the wolves, to get a quiet window). It failed the same
way, and the third check explains why the first two failed:

    invariant   removing bridge draws changed blits/frame  944 -> 1,960 (+107.6%)
                — arithmetically impossible, so DISCARD
    lockstep    published frames at field 360:  ctl 86 | none 73 | cull 80
                at 380/400/410/420/440: never equal, at any field

☠ **There is no lockstep window at all — not even before the level starts.** The
divergence is NOT the wolves. Removing code changes the image layout, which
changes SD load pacing, which changes how many frames have been published by a
given FIELD. So two builds are at different game states from boot onward.
⇒ **Any frame-indexed offline A/B in this engine is invalid**, regardless of
scene. Sampling "at field N" compares different moments. This is not fixable by
picking a better window, a quieter room, or normalising per published frame —
all three were tried.
⇒ What remains valid offline: comparisons that need no scene equivalence at all
(byte-identical ROM checks, static counters, per-blit costs). What needs
silicon: anything whose answer is a RATE in a live scene.
★ That is presumably why the in-repo 6.07 -> 8.07 bridge number was taken on
hardware with Lara walking in: a human driving the same ROM sidesteps the whole
problem, because there is only one build.

★★★★★ THE REUSABLE PART IS THE CHECK, NOT THE RESULT. Three invalid readings
were caught by two cheap self-tests, and neither needed judgement:
  1. **an invariant that cannot legitimately move** — removing draws must not
     increase blits. Caught attempts 1 and 2.
  2. **a lockstep test** — published frames must match across arms at the sample
     point. Caught the cause, and would have caught 1 and 2 before they ran.
Run the lockstep test FIRST on any multi-arm offline measurement here.

#### ☠☠☠ AN OFFLINE A/B IN A SCENE WITH LIVE AI IS UNMEASURABLE — three attempts, all invalid
Tried to re-price the bridges at 160x120 by SPAWNAT into the bridge room
(LOCAL 22) with three arms: control / `ENTVIEWCULL=1` / `NOBRIDGEDRAW=1`.
**Every reading was confounded and the tell was arithmetic, not intuition:**

    per FIELD      br_none (bridges REMOVED): cycles -5.6% but blits +9.7%
    per PUBLISHED  br_none: cycles +57.4%, blits +82.9%, only 15 frames vs 25

Removing draws cannot ADD blits. Both readings are impossible, which is what
exposed them.

☠ **CAUSE: the world advances per PUBLISHED FRAME, so arms that render at
different rates are at different GAME STATES by the same field.** At field 450:
control 100 published frames, cull 102, **NOBRIDGEDRAW 82**. The wolves in that
room had done different things in each arm — Lara alive in one, dead in another.
⇒ Normalising per published frame does NOT rescue it: the scenes are not the
same scene, so there is nothing common to normalise. The fix is a scene with
**no live AI**, not better arithmetic.
☠ And `ENEMIES` cannot be compiled out (call sites escaped the `#ifdef`), so the
obvious remedy is unavailable.

⭐ The one arm that stayed comparable (26 published frames vs 25) is
`ENTVIEWCULL=1`: **-2.0% GPU cycles, -3.1% blits/frame.** Weak but the right
sign, and it is the only number here I would repeat.

⇒ **To price bridges properly: spawn where bridges are VISIBLE but no enemy is
live.** The code says the cluster spans stacked LOCAL rooms 18/22; original-room
13 (LOCAL 18) carries no wolves in the entity audit, so a viewpoint there is the
candidate. Or drive/played capture, where the user reaches it naturally.

★ This generalises past bridges and past this project: **any offline A/B whose
scene contains an agent that advances per frame will diverge, and the divergence
is silent unless you check an invariant that cannot legitimately move.** Blits
under a draw-removal was that invariant here.

#### ⬜ THE MEASUREMENT THAT IS NOW OWED
Tooling already exists; nobody has run the A/B on the shipping config:
- `NOBRIDGEDRAW=1` — DIAGNOSTIC gate. Prices the **CEILING** on any bridge cull.
- `ENTVIEWCULL` — culls bridges BEHIND the camera. ☠ Behind-only ON PURPOSE: a
  lateral test pops a bridge at the screen edge, trading a visible bug for
  invisible cycles.
- Known target: 6.07 -> 8.07 fps with bridges gone entirely.
⬜ Three arms in ONE rig turn at 160x120: control / ENTVIEWCULL state / 
NOBRIDGEDRAW. Tells us how much of the 33% the existing cull already recovers
and how much is left.
★ The 33% was measured at the OLD resolution. Re-measure at 160x120 before
quoting it — SLIVERW is the standing proof that a number does not survive a
configuration change.

### ✅ THE WOLVES ARE THE RIGHT MODEL — the size is the CAMERA (2026-09-06)
User: *"the dogs look nothing like the wolves in the PS version. They seem a lot
bigger, are you sure they're the right ones?"* Answered from the disc via
`MRT_ENEMYAUDIT=1`:

    model 7 WOLF  meshes=26 verts=264 faces=251  bbox 190 x 255 x 521
    model 8 BEAR  meshes=18 verts=241 faces=261  bbox 538 x 538 x 868
    model 9 BAT   meshes= 8 verts= 45 faces= 41  bbox 329 x  62 x 200

✅ Distinct models, correctly assigned — a wolf is not a bear, and 255 tall by
521 long is the right shape for one (longer than tall).
✅ **Rooms and models share raw TR world units** — neither path scales in the
extractor (both just append (x,y,z)), and `build_ent_model` (main.c:2918-2934)
only rotates and translates. So the wolf's size RELATIVE to the world is right,
and "no scale is applied" is correct behaviour, not the bug I first suspected.

☠ **SO THE SIZE COMPLAINT IS THE FOV, AND IT IS A BY-EYE TUNING DECISION.**
`gpu_geotex.gas:285` — `FOCAL .equ 190 ; TR1's ~80-degree FOV (160 was 90: too
wide -> world read as too far)`. FOCAL was raised 160 -> 190, and a HIGHER focal
is a NARROWER field of view, which magnifies everything by **19%**. It was
chosen because 160 "read as too far", not by matching the reference.
⇒ Everything is 19% larger than a 160 focal would draw it. Lara is centred and
familiar so it does not read as wrong on her; a wolf running at you does.
⬜ **This is checkable against `res/` PS1 footage, which the corpus calls THE
AUTHORITY** — measure a wolf's on-screen height in a PS1 frame against ours at a
comparable distance before changing FOCAL. Do NOT re-tune it by eye a second
time; that is how it got here.

☠ Diagnostic defect found while doing this: the audit prints the header
"(Lara for scale: model 0)" and then NO DATA. The comparison its own label
promises is an unimplemented stub, so the one number that makes the enemy bboxes
interpretable is missing. Same shape as the three dead guards: a check whose
useful half never runs.

### ★★★★★ E ANSWERED IN REAL PLAY — THE ROOM IS THE COST, THE WOLVES ARE A THIRD OF IT
Live user playtest on silicon, 300 s capture with FPSBEACON, job #2584 (beacon
bimodal 1.00). The first frame-rate data this project has from ACTUAL PLAY.

    corridors (t<=172s)                   8.2 fps
    LARGE COMPLEX ROOM, no wolves yet     3.2-3.9      <- 2.3x collapse
    same room + wolves engaged            2.3-2.7      <- ~1.4x more
    whole 300 s run                       4.12 mean, min 0, max 10

☠☠ **THE ROOM DOMINATES, NOT THE ENEMIES.** My first read attributed the whole
3.5x drop to wolves; the user corrected it ("the room itself is large and
somewhat complex") and the capture proves it — the collapse happens on ENTERING
the room, ~25 s before any wolf is on screen. Geometry ~2.3x, wolves ~1.4x on
top. Do not quote a wolf cost without saying which room.

☠☠☠ **EVERY fps NUMBER THIS PROJECT HAS EVER QUOTED IS FROM AN EMPTY ROOM WITH
LARA STANDING STILL.** 7.98 / 7.50 / 8.00 fields are all PADMUTE at the spawn.
Real play is **4.12 mean**. The engine is roughly HALF as fast as the ledger
says when someone is actually playing it.
⇒ This reconciles the old room sweep (median 5.83, worst r22 3.69) with the
user's experience: the heavy rooms were always the frame rate. The sweep knew;
every campaign since measured the spawn instead.
⇒ **E's lever is heavy-room geometry cost, not enemies and not fill.**

#### ⬜ ALSO SEEN, NOT YET CHASED
- ⬜ **"YOU DIED" is STILL cut off at the right edge** in a build made AFTER the
  `VIEW_W` centring fix (`f979f65`) — so that fix does NOT do what I intended.
  Re-open; do not assume the text work is done.
- ⬜ After Lara died the capture went **black and completely static** (frame diff
  0.00, luma 1.0) for 60 s. Most likely the FASTBOOT measurement build: death
  soft-reboots to the EIDOS attract and FASTBOOT has no boot-video path to
  return to. ☠ NOT confirmed — this repo has 9 phantom "black boots" that were a
  dead capture card. Ask what the TV showed before treating it as a game bug.

★ Methodology note that cost two rig turns: a SCRIPTED drive is not gameplay.
My drive.sh run passed its own go/no-go (start-vs-end diff 31.7 vs the 28.8 bar)
while Lara was jammed against a wall for most of it, which inflated the reading
to 9.29 and had me report the engine as FASTER than the spawn. The go/no-go
proves she MOVED, not that she moved anywhere useful. A human on the pad found
the real number in 90 seconds.

### ⚠ SUPERSEDED BY THE ABOVE — THE SPAWN IS THE WORST ROOM (drive was invalid) — every headline fps here is a FLOOR (2026-09-05)
First DRIVEN fps measurement (`tools/drive.sh`, jobs #2579/#2580, 51 s, host-driven
pad, beacon bimodal 1.00, go/no-go start-vs-end diff **31.7** against the 28.8 bar):

    t=0-10s   6.00-7.50 fps   <- the level START, where EVERY arm has been measured
    t=12-30s  9.00-10.50
    t=32-48s  9.00-11.00
    WHOLE RUN mean **9.29 fps**

☠☠ **The 7.500 fps / 8.00 fields figure quoted everywhere is the SPAWN, and the
spawn is the slowest place in the level.** Every static arm this project has ever
run — mine included — parks Lara at the level start with PADMUTE. In actual play
the build sits at **9-11 fps**, not 7.5. The shipping number is a floor, not a
typical case, and the demo reads better than the ledger says.
⇒ Any future fps claim must say WHERE. "7.98 at the spawn" and "9.29 driven" are
both true and describe different things.

#### ⬜ THE WOLVES ARE STILL UNPRICED — the drive never reached one
51 s of forward walking from the spawn met no wolf; the only dip (6.00 at t=10) is
heavier geometry, confirmed by eye. The user's "slows tremendously with the wolves"
is real and remains the open perf question. ⬜ NEXT: locate a wolf room and drive
there, or SPAWNAT into one — do not assume forward-from-start reaches them.
☠ **`ENEMIES` CANNOT BE TURNED OFF ANY MORE**, so the obvious build A/B is
unavailable: `ent_is_wolf` / `ent_is_bat` / `ent_is_bear` / `g_batinit` are called
from sites that escaped the `#ifdef ENEMIES`, and the build fails to compile
(`main.c:3476-3478`, `:9121`). Either fix the guards or price the wolves
within one run (same ROM, two scenes), which is the better experiment anyway.
☠ **A real controller contaminates a driven run.** The first attempt (#2579) was
invalidated because the user touched the pad mid-capture — same family as the
earlier "pad resting on FORWARD" confound. Re-run clean before quoting anything.

### ⬜⬜ USER PLAYTEST ON SILICON (2026-09-05) — 160x120 IS THE BEST-PLAYING BUILD YET
User, on the TV, verbatim: *"Gameplay wise, that plays the best out of all that
I've played thus far."* The 160x120 direction is validated by the only oracle
that matters. Two problems reported, one of them the next perf lead.

#### ★★★★★ ⬜ THE WOLVES — "still slows tremendously with the wolves" (E, STILL OPEN)
☠☠ **EVERY PERFORMANCE NUMBER IN THIS REPO WAS MEASURED WITHOUT WOLVES.** All my
arms are `PADMUTE=1` at a spawn point: Lara idle, nothing hunting. The room
sweep (median 5.83, worst r22 3.69) is static geometry. The 7.500 fps at 8.00
fields is a **static** figure. The user is reporting a large, reproducible drop
in the one case nobody has instrumented.
⇒ This is the SLIVERW lesson a third time: **measured the wrong scene**, not the
wrong quantity. E is NOT closed and this is where it reopens.
⬜ NEXT: price a wolf. `SPAWNAT_ROOM` into a wolf room (r32 is named in the
corpus), FPSBEACON, and A/B `ENEMIES` on/off in ONE rig turn. Enemies are
skinned + posed on Jerry + AI on the 68k, so the cost could be in any of three
places and the split is the whole question.

#### ⬜ THE MENU / TEXT — three symptoms, TWO causes, one fixed
User: *"The menu isn't right"*, *"The you died is not right either. Letters
aren't right"*, *"the in game menu is messed up as well."*
✅ **CAUSE 1, FIXED (`f979f65`)** — HRESN was never made phase-aware beyond the
display path. Three consumers were still laid out for 320:
  · `gpu_geotex_hq.bin` (the TITLE's 3D) is built with
    `$(subst -dLOWRES=1,-dLOWRES=0,...)`, which leaves **HRESN=160 in place** —
    verified on the emitted command line. Ring items projected at CENTER_X=80 /
    FOCAL=95 then displayed at 320: half scale, shifted. HRESN now substituted
    out exactly as LOWRES is.
  · `menu_text`'s `W` is BOTH row stride and clip bound in one expression.
    Split: stride stays RENDER_W, bound follows VIEW_W in the game phase.
  · "YOU DIED" centred on `(RENDER_W - tw)/2` and the pause label sat at a hard
    `x=130`. Both now anchored to VIEW_W.
  Silicon #2577: ring geometry visibly CHANGED vs #2576, so the fix took.
⬜ **CAUSE 2, STILL OPEN — this is A6.** The ring's front item still does not
render as a passport. A6 ("Passport renders wrong: pose/scale/position") predates
HRESN and is now the *remaining* cause of "the menu isn't right". Do not treat
the title as fixed.
⬜ YOU DIED and the in-game menu fixes are IN but UNVERIFIED on silicon — both
need input to trigger, so a static capture cannot show them.

★★★★★ **THE PATTERN, THIRD TIME TODAY: making ONE consumer phase-aware is not
making the FEATURE phase-aware.** The FMV came back cut off, I fixed the display
path and stopped. The projection and the text layout were two more consumers of
the same fact. **Nothing in the build, the test suite or the emulator could see
any of it** — all three needed a human looking at a television.

### ✅✅✅ demo33 BUILT BY THE CONTAINER AT 160x120 (2026-09-05)
`out_demo33/` = full SD payload. ROM md5 `f941b8184722…`, 1,543,564 B.
    docker run ... -e RES=120 -e HRES=160 tr-jaguar
Gates: HRESN=160 on all THREE define paths · 0 debug flags · 0 `23f9` ·
op_list 0x17cd00 (0 mod 32) · jagemu 0/0/0/0/0 · DRAM 102,272 B headroom.
Silicon: FMV full width (#2548) · game 160x120 at 7.550 fps / 7.95 fields
(#2550, matches #2539's 7.500/8.00 — the phase code costs nothing).
⬜ **TITLE RING still not captured** — front end runs long and loops to the
attract cinematic. Offline it is back to 320 wide and it shares the
`g_disp240=1` path with the FMV, which IS confirmed. Catch it next turn.

#### ☠☠☠ THREE CONTAINER GUARDS WERE DEAD — ALL FIXED THIS RUN
1. `build_cof.sh` unclosed quote (`54e55a6`→`fb2b316`): make got `>` from a
   comment, printed usage, **exited 0**. NO ROM for 9 days. demo32 is therefore
   NOT a container build; PLAY_BUILD.md corrected.
2. The SHIPPING kernel had **no size guard**: a blank line does not end a make
   recipe, so both guards were commands of the `_hq_` rule and both stat'd the
   HQ file. `124daba`.
3. `strtonum()` is **gawk-only**; the container ships mawk, so `$end` was empty
   and `[ -gt ]` errored to stderr while the build carried on. The 2MB DRAM
   guard had NEVER run in any container ROM. Now POSIX, fails loud on a missing
   symbol, and **prints headroom on success**.
★★★★★ All three are the same shape: **the check ran, on something else, and
said nothing.** A missing check is visible in review; a check pointed at the
wrong subject is not. Make guards report positively.

#### ☠☠ RC1 SHIPPED A CUT-OFF FMV — jagemu CANNOT SEE THE DISPLAY PATH
Narrowing all four `op_list` sites broke the front end: the intro FMV was cut
off at the right edge on silicon (#2545) while jagemu rendered it plausibly.
`IWIDTH`/`VMODE` are now phase-dependent on `g_disp240`; VMODE changes ONLY in
`video_set_disp240()` (gpu.c:423-431 — a VMODE write disturbs the timing
generator, so never per-frame). The offline tell existed but was WEAK: the
title screenshot's bounding box had gone 320→288 and I nearly argued past it.
☠ Second bug caught by the NO-OP CHECK: the ternaries were ungated, so at 320
`g_disp240 ? 0x06C7 : JAG_VMODE` picked the same value — identical behaviour,
different codegen — and the 320 ROM stopped being byte-identical. **"Same
value" is not "same build".**

#### ☠ MY OWN HELPER BROKE THE SAME WAY
`BF=$(... sed -n "109,117p" tools/build_cof.sh)` extracted BUILD_FLAGS by LINE
NUMBER. Adding the HRES block shifted those lines onto the new `case`, my
host-side eval fell into its `*)` branch and exit 2'd, and the failure LOOKED
like the container rejecting HRES=160. Match the assignment, not a line number:
`sed -n "/^BUILD_FLAGS=/,/}\"$/p"`.

### ✅✅✅ 160x120 SHIPS ON SILICON — jobs #2538/#2539, one turn, 2026-09-05
    320x120 control      6.450 fps   9.30 fields/frame   6.40 6.40 6.60 6.40
    160x120 HRESN=160    7.500 fps   8.00 fields/frame   7.40 7.40 7.40 7.60
**+16.3%**, both bimodal=1.00. ★★★★★ **HRESN=160 lands on EXACTLY 8.00
fields/frame** — dead on a whole-field step, so it is repeatable across turns
rather than the bimodal coin-flip a straddling build gives.

#### ★ PWIDTH CONFIRMED ON SILICON — fills the line, hard edges, no artifact
Capture #2539: full active width, no letterboxing, no stretch. Chunky
double-width pixels, exactly as jag_quake's 3,273 grabs describe. The OP
H-SCALER (bus-starving, blacks the display) was never touched — HSCALE stays
1.0x. `VMODE=$0EC7`, PWIDTH field 7.

#### ★★★★★ THE PREDICTION LADDER — offline under-predicts, and by a LOT
    offline HALFW gate (jagemu)        +7.3%
    silicon HALFW (B_COUNT proxy)     +14.1%
    silicon HRESN=160 (the real thing) +16.3%
The proxy beat its own offline number by 2x, and the REAL lever beat the proxy
by another 2 points — the extra is PWIDTH's bus saving, which HALFW cannot
capture (it only halves B_COUNT) and which jagemu cannot model at all.
⇒ **An offline fill A/B in this engine is a FLOOR.** Under the old reading my
+7.3% was "inconclusive, do not build"; the truth was +16.3%.

#### ★ THE TRADE, MEASURED (all same instrument, FASTBOOT+PADMUTE+FPSBEACON)
    320x60  (ships today)   8.650 fps   6.94 fields   half vertical detail
    160x120 (new)           7.500 fps   8.00 fields   FULL vertical detail
-13.3% fps to buy the vertical axis back, and it lands on a stable step.
160x120 also beats QUALITY=pretty (320x120, 6.450) by +16.3% at the same
vertical resolution.

#### ☠☠ A FLAG CAN LAND IN 2 OF 3 DEFINE PATHS AND THE ROM STILL DIFFERS
The HRESN ladder was first written inside `ifdef VRESN`. With QUALITY=pretty
(no VRESN) the CFLAGS line never ran: `-DHRESN` reached **jas and jcc68k but
not gcc**. main.c kept VIEW_W==320 while the kernel used CENTER_X=80 — and the
ROM DIFFERED FROM ITS CONTROL, so "the flag landed" read TRUE. It rendered 309
columns instead of 160; only measuring content extent caught it.
★★★★★ This Makefile has THREE define paths — CFLAGS (gcc), JCCDEFS (jcc68k,
which builds video.c), GEOTEX_DEFS (jas). Grep the actual compile lines for all
three. "The ROM differs" proves ONE of them took.

### ✅✅ RUN 14 SILICON BATCH — jobs #2530-2533, ONE turn, 2026-09-05
Four arms, all `FASTBOOT=1 PADMUTE=1 FPSBEACON=1`, each verified distinct from
its control BEFORE flashing. Read with `tools/beacon_find.py` (new).

    arm                          fps     fields/frame   per-5s
    320x60  control            8.650        6.94        8.60 8.40 8.40 8.80
    320x60  + SLIVERW=56/24    8.600        6.98        8.60 8.60 8.40 8.60
    320x120 control            6.400        9.38        6.40 6.40 6.40 6.40
    320x120 + HALFW=1          7.300        8.22        7.20 7.20 7.40 7.20
All four bimodal=1.00, span ~246-249. Stable, not straddling.

#### ☠️☠️ SLIVERW IS A NULL — era C's +17% DOES NOT REPRODUCE
8.650 -> 8.600 = **-0.6%**, inside scatter. Pre-declared criterion was "<+3%
⇒ era C's +17% was a 4-fps-era artefact". It was. **The open list has lost its
last unspent item**, and the offload campaign plan's Stack A -- which crossed
the rung ONLY if SLIVERW survived -- does not cross. Do not re-propose it.

#### ✅✅✅ HALFW PASSES ON SILICON — AND jagemu UNDER-PREDICTED IT BY 2x
6.400 -> 7.300 = **+14.1% frames** (frame time -12.3%). Pre-declared bar was
>= +11% at 120 lines. **PASSES.**
★★★★★ Offline said **+7.3%**; silicon says **+14.1%**. The emulator under-valued
a fill saving by nearly 2x. That is the opposite direction to "jagemu
OVERCHARGES the Blitter" and it means an offline fill A/B here is a FLOOR, not
a ceiling. HALFW uses no PWIDTH at all, so this is not the PWIDTH hole -- it is
jagemu missing the bus/DRAM-page cost that a real Blitter imposes.
⇒ The horizontal lever is real and the 160-wide build is authorised by its own
pre-declared gate. And the PWIDTH bus saving (fewer fetches per line) is
ADDITIONAL and still unmeasured, because jagemu cannot see it at all.

#### ★ THE 160x120 TRADE, PRICED
Against today's 320x60 at 8.650: 160x120 lands at **>= 7.30 fps** (the HALFW
number is the fill half of it; PWIDTH should add). So the vertical axis costs
roughly **-15% fps**, not the -2 fps guessed earlier. User has chosen 160x120.

#### ☠️ MEASUREMENT: THE BEACON BOX MOVES WITH RENDER_H — CONFIRMED, NOT THEORY
`main.c:10344` paints fb x32..63, y24..31. Measured capture boxes:
    VRESN=60   x 168..216  y 192..256      (24/60  = 40% down)
    VRESN=120  x 168..216  y  96..128      (24/120 = 20% down)
Same x, different y. A box hard-coded for one resolution reads a plausible
WRONG number at the other. ☠️ The capture is PILLARBOXED: active picture is
x 114..637, y 0..473 of 720x480 -- a naive full-frame mapping lands off the
beacon and `beacon_find.py read` correctly REFUSED (flat box, p5..p95 28.4..28.5).
☠️ The beacon flips per PUBLISHED frame, not per FIELD: diffing jagemu fields
N and N+1 finds Lara's idle animation, not the beacon. Use >= 12 fields.

### ✅ RUN 14 (2026-09-05) — OFFLINE MEASUREMENT BASELINE STANDS UP FROM COLD
Branch **`offload-campaign`**, cut from `main` at tag
`checkpoint-2026-09-05-pre-offload`. Campaign: what moves off Tom onto the
Blitter / OP / 68k / Jerry.

#### ☠️☠️☠️ `build_cof.sh` HAS BEEN UNABLE TO BUILD A ROM SINCE 54e55a6 (9 DAYS)
Fixed in `fb2b316`. `BUILD_FLAGS="${BUILD_FLAGS:-...}` opens a quote on line 109;
the closing `"` used to sit on the last flag line and **54e55a6 (ENTLISTS) dropped
it**. The shell scanned on to the next `"` — end of the ENTLISTS comment block —
so BUILD_FLAGS expanded to the flags PLUS comment text containing `(7.90 -> 8.10)`
and `store->load`. Those `>` reached make:
    make: invalid option -- '>'   → make prints usage, **exits 0**, run ends with empty out/
★★★★★ c3ee230's "a run with no ROM now fails" guard did NOT catch it: the guard is
downstream of a step that never reported failing. **A guard placed after a silent
step guards nothing.**
⚠ **demo32 IS NOT A CONTAINER ROM.** demo31 (08-26) predates the break and is real.
PLAY_BUILD.md calls demo32 `make $BUILD_FLAGS VRESN=60` with "build_cof.sh = the
container entrypoint, so identical" — the parity half was never true after 54e55a6.
Re-verify demo32 before releasing it as a container build.
☠️ Also latent in the image build: `awk: function strtonum never defined` (mawk vs
gawk) and `/bin/sh: 2: [: -gt: unexpected operator` — a ROM size check that silently
does nothing. Not fixed yet.

#### ✅ THE BASELINE, REPRODUCIBLE FROM COLD — recipe of record
Tree had NO assets (`disc/` did not exist) and no ROM. Full path, ~15 min:
    7z x -o disc "$_ARCHIVE/Tomb_Raider_(USA)_(v1.6).7z"     # user's own disc, gitignored
    docker build -t tr-jaguar .                              # image was STALE (VRESN=80, no RES)
    docker run --rm -v "$PWD/disc:/disc:ro" -v "$PWD/out_auto:/out" \
      -e DISC_NAME="Tomb Raider (USA) (v1.6).cue" -e QUALITY=playable -e VIDEO=0 \
      -e BUILD_FLAGS="<the 109-117 flags> AUTOSTART=1 PADMUTE=1" tr-jaguar
    cobweb/sim/target/release/jagemu run out_auto/OPENLARA.COF --frames {600,900} --fidelity silicon
★ `COBWEB_REV` stays pinned at **9da2f99** deliberately — that is the toolchain every
silicon-measured ROM used; bumping it re-rolls A10 and changes the ROM. Local cobweb
(03b4ef5) is used only to MEASURE.
☠️ **`AUTOSTART=1` is mandatory or you profile the TITLE RING** — a plain release ROM
sat at 491 GPU cycles/field and blit_count 29 over 200 frames. Same trap as
JAGUAR_FINDINGS §4b. Window verified by SCREENSHOT (Lara in the caves, 320x60) before
any number was trusted.
☠️ **`cycles_per_field` as emitted is boot-contaminated** — it is `cycles/frame` over
the WHOLE run. Naive read 188,754; the true steady-state figure is **367,266**, a 1.9x
error. Take a DELTA between two runs (600 → 900) instead.
☠️ redirect with `>file 2>/dev/null`, not `2>&1` — jagemu writes hazard warnings to
stderr and they corrupt the JSON.

#### ★★★★★ THE BASELINE NUMBERS (VRESN=60, frames 600–900, --fidelity silicon)
    GPU cycles/field        367,266      <- the campaign's cost metric
    blit                     20.3%   (launch 1.8% · transfer 18.5%)
    jump_refill              14.8%
    mem_external             16.2%
    stalls alu/load/div/flg  13.5%
    contention                0.6%   <- jsim mis-prices this; do not trust
    408 blits/field · 182.9 cyc/blit · launch 16.0 · transfer 166.9
    DSP 443,304 cycles/field · 68k op_tax 4,501/field
★★★★★ **CORRECTIONS TO THE PROFILE THIS CAMPAIGN WAS SCOPED AGAINST:**
1. **Launch is 8.7% of blit cost, transfer is 91.3%** (16.0 vs 166.9 per blit).
   Even weaker than the shared brain's 14/86 model. **Amortising launches is dead** —
   RUNBATCH/TRAPEZOID measured null and this is why. Only the transfer side matters.
2. **Spans are ~15 px here, not 9** (166.9/5.6 ≈ 30 accesses ≈ 15 px at 2/px).
3. **Blitter is 20.3% of Tom, not 34.6%** — VRESN=60 cut the fill, not the
   per-vertex/per-face work. Exactly what §4b predicts. The 34.6% figure is a
   120-line number and must not be quoted at 60.
4. ☠️ **Jerry is BUSIER than Tom** — 443k vs 367k cycles/field. "Offload to the DSP"
   starts from a processor with less headroom than the one we are unloading, not more.
   (Caveat: the resident mixer spins; some of that may be idle. Needs a PC histogram.)

#### ⬜ NEXT
- `PHRASEDST=1` is the one lever that ever touched the 91% (brain: +9.1–11.3%). ☠️ It
  is plumbed (`GEOTEX_DEFS`) but it ONLY swaps `A2_FLAGS_VAL` to XADDPHR at
  gpu_geotex.gas:161 — **it does NOT phrase-align the span start**. The `XL &= ~7`
  alignment exists only in the PHRASESHADE path (gpu_geotex.gas:3678); the textured
  path stores unaligned `xl` (:2712, :2897, :3356). Establish whether the probe as
  built is even correct before quoting its number.
- Offload audit (Tom/Blitter/OP/68k/Jerry/prior-art) was still running at run end.
- ⚠ `hw/LOOPMODE` lapsed 08:42; `hw/burnmode` reads SLOW (DEVELOPMENT.md §12).
- Roster: `jag_openlara` flipped to **active** on the Foreman board this run.

### ☠️☠️☠️ 68k/KERNEL MICRO-PERF IS AT ITS CEILING — the vein is tapped (2026-08-27)
User picked "stack more 68k contention" to try to cross 8.30→8.57. Measured, it
does NOT: after M68A2 (+14%) and ENTLISTS (+4.1%, reproducible: OFF 7.975 twice,
ON 8.30), the remaining savers are each <1% of the frame and read FLAT on silicon.
- **BFSCACHE [16]** (portal hop-BFS cache, the audit's biggest remaining DRAM
  saver): OFF #2065 8.300 vs ON ×4 8.30/8.30/8.30/8.275 = FLAT. Correctness-verified
  (vanish test: game render pixel-identical across 12 ROOMTOUR crossings). `0092c28`.
- **Full stack** ENTLISTS+BFSCACHE+MICROOPT: 8.300→8.37 (+0.8%, #2078/79/81) —
  sub-field, does NOT cross the rung (+3.3% needed). BFSCACHE + MICROOPT stay GATED.
★★★★★ A field is 12.5%; the remainder is <1% each. The rung needs a STRUCTURAL
lever (Blitter fill = 30% of frame, overdraw 2.07×, or lower VRESN [rejected]),
not more micro-opts. ⬜ **RECOMMENDATION: pivot** — either the fill/overdraw
(Tom-side, harder) or to the DEMO ENDPOINT (the actual goal). ENTLISTS +4.1% is
the shippable win to bank into the next container ROM of record.

### ⬜ MICROOPT — per-face kernel stack BUILT + correctness-verified, silicon perf PENDING (rig flaky) (2026-08-27)
The audit's level-wide per-face stack, behind one `MICROOPT` flag in `gpu_geotex.gas`:
`[11]` inline imul32 into the backface cull (extends INLINEMUL — removes 4 taken
branches/face from jump_refill), `[8]` drop the redundant per-span A2_PIXEL store,
`[12]` delete the dead fl_go geometry-constants block, `[7]` delete the dead
render_pkt colour build, `[13]` fix the stale stage_vert header doc.
**VERIFIED OFFLINE:** gate OFF (flag omitted) == original kernel BYTE-FOR-BYTE;
MICROOPT=1 renders PIXEL-IDENTICAL in jagemu (0 non-beacon diff px @ frame 900);
kernel 3480→**3464 B** (net −16, under the 3680 budget); assembles clean.
☠️ **`MICROOPT=0` does NOT turn it off** — `$(if $(MICROOPT),1,0)` returns 1 for
the string "0" ([[feedback_make_flag_zero_trap]]); OFF = OMIT the flag.
**MEASURED AT SPAWN: no clear fps movement** — OFF #2055 **8.25** vs ON #2061 **8.35**
(beacon +1.2%) BUT frame-change (beacon-blind) **9.30→9.03 = −3.0%**: the two
instruments DISAGREE IN SIGN, and both deltas sit inside the ~0.3 fps run-to-run
scatter (identical ENTLISTS config read 7.975 and 8.25 across two captures). Both
sit at the same 8.2 fps field rung ⇒ the per-face cycle saving is SUB-FIELD at
spawn, exactly as the audit predicted ("nothing crosses a rung alone"). Stays
GATED, NOT in shipped BUILD_FLAGS — enable only inside a stacked, together-measured
batch. Value banked: −16 B kernel budget + real cycles/face; may show in FACE-HEAVY
rooms (like M68A2, spawn was its small end) — a tour/dense-room A/B is the open test.
☠️☠️☠️ **THE UPLOAD WEDGE WAS MY OWN DESYNC**: I bounced with DIRECT `jagpower`
(unlocked) while the jagq daemon held its lease — "two divergent locks wedge the
USB link." Bouncing THROUGH the lock (`jag_gd.sh power cycle` → jaghw → jagq) cleared
it and the ON upload went first try (#2061). ALWAYS bounce through jag_gd.sh, never
raw jagpower, while jagq is the arbiter. Box (185,148,209,180); ROMs in `/tmp/…/entlists_proof/`.

### ☠️→✅ ENTLISTS WAS NEVER COMPILED — NOW WIRED, HONESTLY +4.1% (2026-08-27)
The micro-opt audit's first hit was a **plumbing bug**: `ENTLISTS=1` sat in
`build_cof.sh` BUILD_FLAGS but the **Makefile had no `ifdef ENTLISTS`** and no
generic var→`-D` passthrough, so `-DENTLISTS` never reached the compiler and
every `#ifdef ENTLISTS` block compiled to the `#else` full-scan. PROVEN: the
shipped `main.c` compile line carries no `-DENTLISTS`; adding it moves `main.o`
146,964→149,980 B. **⇒ the recorded "+2.5% (#2045→#2046)" was two BYTE-IDENTICAL
ROMs — pure beacon noise.** (M68A2 IS plumbed, via `-DM68D_A2`; VCDRAIN rides
GEOTEX_DEFS; only ENTLISTS was orphaned.)
FIXED: added the `ifdef ENTLISTS` block (Makefile) + completed the lever —
`54e55a6` left the wolf/bear draw loops and the pickup-collect loop unconverted;
`ent_lists_build` now builds `g_el_wolf/g_el_bear` and all three loops iterate
the lists. OFF = byte-identical to shipped; ON differs (+1232 B ROM).
**HONEST SILICON A/B (FASTBOOT, VRESN=80, PADMUTE, spawn, 40s):**
jagq #2048 base **7.975** → #2049 ENTLISTS **8.300** = **+4.1%**, bimodal 1.00
both, every 5 s window separated; frame-change instrument (beacon-blind) agrees
**8.65→9.03 = +4.3%**. The real lever beats its own ghost. ★★★★★ **`make FOO=1`
does NOTHING unless the Makefile maps FOO→-DFOO — grep the compile line for
`-DFOO` before trusting ANY A/B.** ⬜ ships on the next container build
(BUILD_FLAGS already lists ENTLISTS=1; now it will actually take effect).
Committed on `perf-sliver-4bpp`. Beacon box for this capture rig = (185,148,209,180).

### ✅ THE CONTAINER BUILT THE ROM OF RECORD (2026-08-26, after three fixes)
`out/OPENLARA.COF` 1,540,812 B, QUALITY=playable, PADTEXT=136, cobweb 9da2f99:
**0** dropped-destination moves (`23f9` scan) · `op_list` 0 mod 32 ·
jagemu OP misaligned hits **0**, stray writes 0 · **no** stale wolf UV table
(fresh enemy skins) · renders with its own SD payload (`--sd out`, frame 1500).
It took three container fixes to get here — **the container had produced NO
ROM since the 08-19 `disc/` move and exited 0 anyway**: patch tools wrote to
the root, `build_cof.sh` stat'd bare names, title tools read `title_pal.bin`
from the root. All routed through `disc/`; a run with no COF now exits 1. The
59e5896 cobweb pin is RETIRED (its "ABI regression" was the jas reloc bug).
✅ The clip defect was **numpy** (the `JV_VQ=1` encoder imports it; the image
lacked it) — `9c9d3e9`. All four clips convert fresh now.
**`demo31` STAGED: `WORK_ROMS/demo31/` (full SD payload) + `demo31_p136.cof`,
md5 `00872e51f9ab…`, container run 4 (20:28).** ✅ **CONFIRMED ON SILICON (jagq #2025): CORE → cafe → intro → laptop → TV →
TITLE ring**, clips fresh. Holds at the title (no AUTOSTART in a release).
**`demo31` IS THE ROM OF RECORD** — PLAY_BUILD.md updated.

### (was) ⬜ NEXT — a container build is the ROM of record for all of this (2026-08-26)
Everything measured today was built LOCALLY (stale `disc/` enemy skins and
all). The shipping artifact is the container: `Dockerfile` now pins cobweb
`9da2f99` (both jas fixes), `build_cof.sh` BUILD_FLAGS carry `M68A2=1
VCDRAIN=1`, `jaguar.ld` places `op_list`, `main.c` has the sleep fixes.
**Run the container (QUALITY=playable, PADTEXT=136 — PADTEXT no longer
matters for booting) and put its ROM on the rig once**: expect boot on any
pad, ~8 fps at the spawn, no whole-face flicker, fully-skinned enemies. That
ROM becomes `demo31`. Nothing else is owed before that.

### 2026-08-26 — THE RESOLUTION LADDER WITH TODAY'S LEVERS (silicon, spawn, PADMUTE+FASTBOOT)
120 lines **6.63** fps (was 6.33 without levers, +4.7%) · 96 lines **7.35** ·
80 lines **7.98** (jagq #2018/#2019/#2021 + #1990). ☠️ The +14% the levers give
at 80 lines does NOT transfer to 120 — more pixels, longer Tom render, less
68k overlap to remove. **80 lines (QUALITY=playable) STAYS the shipped
setting** — a "that's unplayable, hard to see" comment received today was
meant for ANOTHER Claude instance (user, same day: "What we had was fine").
The numbers stand; the verdict does not apply here.

### 2026-08-27 — ENTLISTS shipped (+2.5%), OT concluded (2.4× slower), micro-opt audit running
* ✅ **ENTLISTS=1 in the ship recipe (`54e55a6` on perf-sliver-4bpp): +2.5% silicon
  (7.90→8.10)**, pixel-identical, byte-identical off. Type-indexed entity lists built
  once instead of 13 rescans/frame of all 60 entities. A 68k-bus-contention lever
  ([[project_68k_contention_lever]]); sub-rung alone, STACKS. Next: 68k __udivsi3/
  __mulsi3 (~10%), blit_band fb clear (~3%).
* ☒ **Ordered-table renderer (OTLIST) CONCLUDED: 2.4× slower (3.25 vs 7.90), dead.**
  Renders correctly but two GPU kicks + un-overlapped sort = ~18 fields vs 8; GPUHALT/
  per-kick/SRAM-list all no-change (field quantization). Its benefits were already won by
  M68A2 (sort) + VCDRAIN (flicker). Parked on `ot-campaign`. See [[project_ot_campaign]].
* ⬜ Exhaustive micro-opt audit workflow in flight (kernel per-vert/face + 68k contention).
* ☠️ RIG FLAKY 2026-08-27: repeated EXECUTE LIBUSB_ERROR_TIMEOUT every few jobs; USB reset
  + `jagpower cycle` recovers it (device re-enumerates), but it re-wedges. May be hardware.

### ✅✅✅ 2026-08-26 — A10 SOLVED: TWO BUGS, BOTH FIXED, PAD 0 RENDERS (interactive session)

1. **A10 = OP scaled-object ALIGNMENT.** The OP fetches a TYPE-1 object as one
   32-byte burst and dies on a straddle (jag_quake silicon, cobweb `905188f`).
   `op_list` was `aligned(16)`. jsim now reproduces our whole silicon history:
   shipped pads 0/408/544 are 16 mod 32 → 37366 misaligned hits, black;
   136/272/816 clean. **Fix: `op_list` is defined in `jaguar.ld` under
   `ALIGN(32)`** (`aligned(32)` in C does not survive `jas`). Commit `1cc0b9f`.
   The 07-29 "VI never fires" was the consequence, not the cause.
2. **`jas` dropped the DESTINATION reloc of `move.l sym,sym2`** — every
   global-to-global copy in a jcc68k TU stored into the exception vector table
   (24 sites; `fs_ph1/2/3` stayed zero → height-0 object → **black on every
   pad** since video.c moved onto jcc68k; `ship_p136` dodged it via `GCCHOT=1`).
   **Fixed in cobweb `ba9c680`** with a regression test. Pull cobweb and rebuild
   jas before building ANYTHING.

**Proof:** `final_p0.cof` and `final_p136.cof` render Lara in the caves in
jagemu (mean 0.0854, 0 OP hits, 0 stray writes). **Pad 0 has never lit before.**

✅ **CONFIRMED ON SILICON — jagq #1988, `WORK_ROMS/final_p0.cof`:** CORE
logo → intro clip → title ring → Lara in the caves, on the pad that had never
lit. `~/.jagq/jobs/1988/frame_0[0-7].png`. Capture card works.

**✅✅ +13.9% ON SILICON — `M68A2=1` (jagq #1989 base vs #1990): 7.000 → 7.975
fps** (beacon box, bimodal, every 5 s window; frame-change 7.12 → 8.09 agrees).
The O(n²) room selection sort (main.c:9877-9888, `#else` branch) was 31.8% of
68k main-line cycles and jagemu priced it at ZERO frames — the win is BUS
CONTENTION while Tom renders under PIPELINE. Order-identical (pixel diff 0 /
12 px). Added to `build_cof.sh` BUILD_FLAGS. ☠️ `fps_measure.py` locked onto an
edge pixel at VRESN=80 and read 6.48 for a true 7.00; use `tools/beacon_box.py`.

**✅✅✅ A1 HALF-FIXED — `VCDRAIN=1` (jagq #1998, 38-room tour, rooms paired
by CONTENT, `tools/tour_pair.py`):** racing px **9797 → 6219 (−37%)**; the
whole-wall face in PSX room 20: **2698 → 12**; rooms 3/10: 832 → 93, 484 → 35;
fps 6.83 → 6.75. **Mechanism:** Tom's face loop read back vertex-cache words
the self-transform pre-pass had JUST stored (jsim: 118-cycle min gap) and
silicon returned the stale word — one wrong vertex = a face toggling. Only
rooms dispatching >8 rooms (Jerry's cache cap) self-transform, which is why it
was room-specific. Fix = ~8k-cycle drain after the pre-pass, self-transform
path only. **Now in `build_cof.sh`.**
✅ **The "second mechanism" in PSX room 32 was a WOLF.** jagemu live session
at the hold: camera block bit-identical for 96 fields, floor-patch luma steps
48.6 → 44 → 41 → 54 at a fixed game time; unchanged by NEARLOW, BEXIT off,
TRAPFLOOR off, sort hysteresis; **constant 48.6 with ENEMIES off.** The tour
parks a PADMUTEd Lara beside a wolf that wakes and walks through the patch.
⇒ **A1 accounting after VCDRAIN: 6219 − ~4300 (wolf) ≈ 1900 racing px
level-wide, from 9797 — the whole-face class is gone and no room stands out.**
✅ **The wolf's flat BLACK polygon is a LOCAL-ASSET artifact, not a shipping
bug.** `disc/mrt_entex.h` here is dated Aug 11 — before the all-three-enemies
skin work — and carries 121/173 wolf quads (and 100% of bear/bat) as `0xFFFF`
flat faces; those fall back to the tone swatch, which samples black against
the current atlas. With ENEMYTEX off the wolf renders whole. **The shipping
`ship_p136.cof` does NOT contain this wolf UV table (byte-signature search);
`final_p0`/`tour_*` (built here) do.** The container regenerates it
(`build_cof.sh` MRT_ENEMYTEX pass, default models `wolf,bat,bear`). ⇒ Before
judging ANY enemy on a locally-built ROM, regenerate `disc/` with the
build_cof.sh recipe. None of today's A10/M68A2/VCDRAIN results depend on it.
☠️ Still-camera flicker metrics must run with ENEMIES off (or mask entities);
`tools/tour_pair.py`/`race_px.py` do not know a wolf from a wall.
☠️ Index-paired segments lied (the drain "fixed 94%" of a room that was a
different room) — pair by content, always.

**A1 — the earlier hypotheses, for the record**
* ☠️ **Refuted: "Tom's 3 ms per-room guard times out and self-transforms with
  different arithmetic than Jerry."** `JXWAIT=1` makes the 68k wait (polite
  150 µs backoff) for every Jerry room flag before Tom's first dispatch. The
  wait executed (jagemu: 336 backoffs / 600 frames) and changed NOTHING on
  silicon: racing world px **469 vs 463** (M68A2 control), fps 8.00 vs 7.98
  (jagq #1993). Jerry is already done by dispatch; the race is elsewhere.
* ☠️☠️ **THE FIRST METRIC WAS BROKEN** — its hand-placed beacon box missed the
  block's top rows, so 536/463/469/482 were ~90% beacon edge + Lara breathing
  (the mask IMAGE showed it). `tools/race_px.py` now derives the beacon mask
  from the data. **Corrected, world only: base 60 → M68A2 30 → +JXWAIT 31 →
  +COLLECTEARLY 31.** ⇒ M68A2 HALVES the still-camera flicker at the spawn
  (the sort's DRAM traffic was a real A1 contributor, as PIPESTAGE=0's 89%
  predicted); neither experiment adds anything beyond it; the residual ~30 px
  do not respond to the 68k going quiet at all — Tom-internal. The spawn is a
  weak A1 site; the user's "worst flickering" spot is elsewhere.
* ☠️ **Refuted: `COLLECTEARLY=1`** (jagq #1997): racing px **482** vs 463,
  fps **6.55 (−18%)**. The 68k sleeping through most of Tom's render does NOT
  quiet the wedge, so "68k bus pressure" is not the A1 mechanism either.
  ☠️ And the flag had a BUG: its `#if … && !defined(COLLECTEARLY)` gate at the
  old collect ran to the `#endif` after the deferred `video_flip`, so it
  compiled out the flip and the HUD paints — black on silicon (jagq #1995)
  and in jagemu. Fixed (gate no longer excludes COLLECTEARLY; the old collect
  is a no-op under it). Flag stays off.
* ★★★★★ **THE A1 MAP (from the 38-room tour, frame-rate-fair metric,
  `tools/race_px.py` masks): level-wide the two arms are EQUAL** (10,014 vs
  9,797 racing px, 14 rooms ≥20 each) — M68A2 is not a regression, it shuffles
  which near-tied faces flip. **Two shapes, two rooms:**
  - **tour seg 19 = PSX room 32: SPECKLE** across the floor (5227 base / 4278
    a2) — per-pixel nondeterminism; M68A2 visibly thins it ⇒ the bus-pressure
    class PIPESTAGE=0 attacked.
  - **tour seg 26 = PSX room 20: a WHOLE WALL FACE** toggling (2960/2698) —
    painter-order flip between overlapping rooms; the idle camera breathes,
    near-tied rooms swap. Candidate fix: hysteresis in the room sort.
  ☠️ A 60-field window at 5 fps holds 5 frames: a ≥4-toggle threshold is
  biased toward the faster arm. Use toggles ≥ 50% of frames rendered.
* ⇒ Both 68k-side hypotheses are dead. What is left is nondeterminism INSIDE
  Tom's frame: the kernel's own "stale-BUSY window" (a Blitter status poll
  that sails through before BUSY asserts) is the named candidate — jsim's
  `blitter.bcmd_poll_in_settle` counter is the instrument to read first.

**✅✅ 38-ROOM TOUR CONFIRMS IT, LARGER: jagq #1991 base vs #1992 M68A2** —
32 valid rooms each, median **5.30 → 6.83 fps (+29%)**, mean 5.04 → 6.46;
rooms 0–17 pair cleanly and are ALL faster (+17.6% … +52.2%, median ≈ +33%).
The spawn's +14% was the small end. Per-segment tables in `sweeps/`, tool =
`tools/tour_box.py` (tour_fps.py's locator does not work at VRESN=80).

**Gameplay profile (jagemu, silicon fidelity, frames 300-1200, `final_p136`)**
— kernel attribution verified by `cmp` of Tom SRAM against `gpu_geotex.bin`:
* Tom busy 73.6% of wall; **~30% of Tom's cycles are WAITING**: `ss_bw` +
  `bwait` Blitter spins ~17%, the `halt` idle spin 12.4%. Blitter-bound in the
  span phase, as the closed perf memory says — now quantified per PC.
* 68k: 43.5% asleep in STOP, 56.5% awake, 99% of that main-line. ~20% of
  awake time is ONE comparison loop in `main` (`0xFE64`–`0x10298`: stack-table
  lookups at sp+0x614/+0x814, cmp/blt/beq) — the per-frame depth/paint sort.
  ~150k cycles/frame ≈ 11 ms ≈ 8% of a 133 ms frame IF serial with Tom.
* ☠️ `m68k_dram_poll_max` = **5710** at `0xF08A` (budget 128): a 96-field
  `frame_count` busy-wait in `main` after a `(8,8)` call — one-shot, not
  per-frame, but a DRAM spin the detector flags. Find and STOP-sleep it.
* ☠️ GPU store→load round trips: **36** in `stage_vert+0x16..0x2a` (the
  vertex-cache reads of words the vc_loop pre-pass STORED, min gap 258 cyc).
  Silicon can return stale/0 there — the same shape as the A1 "PIPELINE race"
  flicker. Lead, not fixed.
* `imul32` is NOT hot in geotex (INLINEMUL did its job); my first read said
  otherwise because I attributed with the geomdirect map. Verify the resident
  kernel by dumping SRAM before trusting any GPU PC name.

⬜ `PLAY_BUILD.md` recipe is stale (needs `GUNS BLOBCACHE JCENT JOVL SECTLONG`);
build from `tools/build_cof.sh`'s `BUILD_FLAGS` (+`VRESN=80 AUTOSTART=1`).
⬜ jas still leaves `.bss` `sh_addralign` at 16 under `.align 32` (filed in
jaguar-shared) — keep the linker-script placement.
★ Method: a commit bisect 18→19 Aug found NOTHING — the assembler was the
variable. Diff linked bytes against generated asm.

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

# ✅ PIPELINE VERIFIED TO THE END OF CAVES.JV — ⬜ THE LEVEL DOES NOT COME UP
# (user, 2026-08-19, on the fixed build from the CARD)

Startup → EIDOS → CORE → INTRO → title ring → Start Game → **CAVES.JV plays to
the end** → **no Caves level**. Everything up to and including the pre-level
cinematic is good; the hand-off into the level is where it stops.

### ★ FIRST SUSPECT, AND IT IS MINE: `CAVSLOAD.DAT` IS NOT ON THE CARD
The Caves loading screen STREAMS from SD, and the order is
CAVES.JV → loading screen → level. I restored the card after it was wiped and
wrote OPENLARA.COF + EIDOS/CORE/INTRO/CAVES.JV + MUSIC.PCM + GYMLOAD.DAT, but
**not CAVSLOAD.DAT** — `build_cof.sh` only ever emits GYMLOAD.DAT, so the gap
was inherited. It is now generated from the disc's AZTECLOA.RAW (76,800 B, in
`SD_RESTORE/CAVSLOAD.DAT`) and the push was cancelled mid-queue at the rig
handover. **One push, do it first.**
☠️ Caveat, so nobody over-reads this: main.c:7444 says a failed loading-art
read "falls through to the text panel", so a missing file SHOULD degrade, not
hang. If the level still does not come up with the file present, the lead is
dead and the level-load path itself is next.

### ✅ WHAT THE SAME RUN PROVED
  * the FMV fix holds on the shipping build, from the CARD, all four clips
  * title ring renders and takes input
  * ⬜ the title MUSIC CUTS OUT when stepping between ring options (user) —
    MUSDIAG is built for exactly this: `musdiag_p{136,0,272,408}.cof`,
    panel rows[5] = musgap<<24 | musloop<<16 | musfill



# ☠️☠️☠️ **THE FMV CORRUPTION IS A jcc68k MISCOMPILE — NOT THE CONSOLE, NOT THE
# CODE, NOT THE ASSETS.** Found 2026-08-18 by the USER'S hypothesis ("it worked
# before the automation"), which three sessions of bus-contention theory had
# talked past. The user nearly discarded a working $600 Jaguar over run 12's
# "marginal console" write-up. That write-up was WRONG.

### THE BRACKET (all measured the same night, same console, same card, same clip)
    demo30_p0.cof (2026-08-12)                                   0.00%   job 436
    same C code @ 3aded3d, today's toolchain                    19-21%   job 485
    same C code + demo30's OWN assets, today's toolchain        19-21%   job 498
    same C code + today's assets + jcc68k @ 84d9fc2 (07-27)      0.00%   job 506
  => code, assets, flags, kernels and the assembler are ALL innocent.
  => the ONLY variable that moves it is the jcc68k revision.

### THE FIX
    JCC68K=/home/jvilla/Documents/Git/jag_openlara/jcc68k-0727   (cobweb 84d9fc2)
  good = 84d9fc2 (2026-07-27)   bad = 59e5896 (2026-08-16)
  jcc68k compiles EVERY C TU except main.c - including video.c (OP list, flip
  protocol, ISR repair values), gpu.c and blit.c. Prime suspects in the window:
  b21f1e1 "thirteen wrong-code fixes" and beb2c15 "an odd-sized global sent
  every runtime helper to an odd address" (odd alignment = hardware-only fault).
  ⬜ Bisect 84d9fc2..59e5896 to name it, then file in COBWEB_ISSUES_OPENLARA.md.
  ⬜ Pin JCC68K in the Makefile AND COBWEB_REV in the Dockerfile.

### ☠️ WHY EVERY OFFLINE GATE PASSED FOR 13 RUNS
  jsim EXECUTES THE MISCOMPILED BINARY. An emulator cannot disagree with the
  compiler that fed it. Every check agreed with itself while the TV showed
  static. ⭐ The transferable rule: when hardware and emulator disagree and the
  SOURCE is identical, suspect the TOOLCHAIN before the code.

### ☠️ FALSE INSTRUMENT THAT COST THREE SESSIONS
  A flat field scores 0.00% on a local-median metric whether or not the fault
  is present (run 7 proved it). Run 6's "STOP fixed it 54.65% -> 0.00%" was
  measured on a flat test card and is therefore UNPROVEN. Tonight the same trap
  scored a flat YELLOW HANG SCREEN as "perfectly clean" - caught only because
  the user looked at the TV and said "I just see a yellow screen".

### ☠️ THREE HYPOTHESES KILLED ON SILICON (do not re-run them)
  OP scaler TYPE-1 vs TYPE-0     11.59% vs 11.15%   no effect
  OP fetch 240 vs 120 lines      ~20%   vs ~20%     no effect (120 = what the
                                                    GAME asks, and the game is
                                                    clean - so fetch is not it)
  cartridge streaming off        19.7%  vs 20.2%    no effect



# ★★★★★ **THE CONSOLE IS EXONERATED** - the user swapped in a DIFFERENT JAGUAR
# and reported "same issue", and the sweep agrees. ⏸ A different GAMEDRIVE is
# now in, but it does NOT enumerate on USB yet.

### ✅ THE NEW-CONSOLE SWEEP (job 315, full shipping build with video, pad 136)
    t=12s 54.8% · 18s 57.4% · 24s 55.2% · 30s 43.0% · 36s 43.3% · 42s 38.4%
    t=48s 37.4% · 54s 36.2% · 60s 43.4% · 66s+ NO VIDEO SIGNAL
Corrupt across the whole first minute, same appearance as the old console.
⇒ **not the console.** (User, verbatim: *"Same issue, different Jaguar."*)
☠️ It also drops the video signal entirely at ~66 s - worth chasing separately;
1 KB grabs are NO SIGNAL, not a black picture.

### ☠️☠️ AND I MUST WITHDRAW THE "IT CLEARS AT 70 s" RESULT
Job 299's clean frames came from `sg1024_p272.cof`, which carried the run-8
**object-shape regression** (plain TYPE-0 instead of scaled TYPE-1). So "clean
at t=70 s" cannot be attributed to time - it may have been the regressed object
shape rendering *correctly*, which would be a finding of the opposite kind.
⬜ **THAT IS NOW A LEAD, NOT A RETRACTION:** if a TYPE-0 plain object renders
CLEAN where the shipping TYPE-1 scaled object renders corrupt, the fix is to
stop displaying the clips through the game's scaled object - which is exactly
what the user suspected on their first question ("a high video mode so you
don't need to switch"). Re-test it DELIBERATELY, at one pad, with the mask.

### ⬜ RUN 14 STARTS HERE
1. ⏸ **The new GameDrive is not enumerating** - `jagq` says
   `GameDrive OFFLINE (03eb:800e)`, `lsusb` shows zero matches, and a
   `jagpower cycle` did not bring it back. Reported to the user; do not hammer
   the link. `jagq status` will show it when it returns.
2. ⬜ **The TYPE-0 vs TYPE-1 A/B, done properly.** `g_op_plain240` is now
   correctly declared and gated on `JVDECMASK` bit 16. Build masks 0 and 16 at
   ONE pad (they differ by one byte) and capture both at the SAME delay.
   This is the single most promising open lead and it is one turn.
3. ⬜ Then the NOBOOTCLIPS "title early" test (`WORK_ROMS/nbc_p*.cof`) to
   separate the display from the FMV path.

### ✅ SOLID, CONSOLE-INDEPENDENT, ALREADY SHIPPED IN THE TREE
  * Two 68k DRAM spins fixed (pacing loop; `gpu_jvdec_wait`'s 240,000-read
    mailbox poll). Offline gate: our ROM scores **5** where the pre-fix ROMs
    score 4216/6100, against a hardware budget of 128.
  * The FMV audio ring overran `g_arena` by 7,224 B every clip - fixed with a
    compile-time `sizeof(g_arena)` assert.
  * The jcc68k use-before-definition regression - fixed and filed upstream.
  * Pushed: cobweb `f377c95`; jaguar-shared `ff74328`, `674b403`, `be79e43`,
    `ee0e4d6`, `6c278b4`.
  * ☠️ Method: never A/B across two PADTEXT values · never across two capture
    times · never judge a capture by file size · LOOK at every capture ·
    rebuild the last known-good commit FIRST · and **check the job log before
    reading a blank as an A10 miss** (a USB failure looks identical).

### ⬜ ALSO OPEN
  1. ✅ `r22_try_p408.cof` DID run - jagq job 125, 07:18 today. It is not a rig
     fault any more. But the grab landed 35 s in, which is still **inside the
     boot FMV**, so the bridge room was never photographed. ☠️ THE ROM IS GONE
     WITH /tmp - rebuild it, and this time either build it `FASTBOOT=1` or ask
     jagq for a longer settle, because 35 s does not clear the clips.
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
