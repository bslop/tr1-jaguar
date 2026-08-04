# Jaguar findings — things that cost a day each to learn

Architecture notes that are not in the TRM, not in the emulator, and not
anywhere else we could find. Written down so they are paid for once.

## 1. What jagemu CANNOT arbitrate
The emulator is an excellent oracle for *geometry and logic* and a poor one for
*the bus*. Every item below rendered perfectly in jagemu and failed on silicon:

| effect | why jagemu misses it |
|---|---|
| DRAM page locality | charges a flat cost per texel read |
| OP bus starvation | its OP never writes objects back, is never starved |
| open-bus reads (absent cart hardware) | returns a fixed value |
| uninitialised memory | RAM is zero-filled |
| Blitter pressure | documented to OVERCHARGE the Blitter |
| **anything on the display path** | **its screenshot comes from the FRAMEBUFFER, so a broken OP list is invisible** |

That last row is the sharp one. A build whose object list is wrong renders a
perfect emulator screenshot. **If a change touches the OP list, the scale
factor, or the display window, jagemu cannot test it at all.**

## 2. The render buffer is vertically compressed 2x
LOWRES renders **320x119** and the OP scaler stretches it to a 240-line window.
Any aspect ratio or ANGLE measured off a screenshot must be corrected before
comparing with a 4:3 reference: an aspect of 1.00 in the buffer is 2.00 on
screen, and `tan(theta_display) = 2 * tan(theta_buffer)`.

## 3. Fitting a 3D model's orientation — the metric ladder
Ring items need yaw/pitch/roll. Score candidates by projecting the model's
vertices offline (milliseconds each, versus ~70 s per emulator render), but
score the RIGHT thing — each rung below shipped a visibly wrong result first:
1. **bounding box** — cannot tell a solid rectangle from a thin diagonal bar.
2. **principal extents** (2nd-moment eigenvalues) — cannot tell FACE-ON from
   EDGE-ON; an edge-on book is also wide and level.
3. **+ convex hull AREA** — correct. A flat object's projected area is maximal
   when it faces the camera.
4. **Geometry can NEVER tell FRONT from BACK.** A 180-degree flip has an
   identical silhouette. Only the texture distinguishes them.
Also: these PSX inventory models are authored LYING DOWN. Yaw spins them on the
table and roll turns the flat shape in the image plane; **pitch is what stands
one up**. The ring bake had no pitch term for months.
And calibrate the offline projection against real renders first — it must
include the CAMERA's own pitch (`tcam` uses COS(6)/SIN(6)).

## 4. Where the frame time actually goes
Measured twice, independently, and they agree: the cost is **per-SPAN, not
per-pixel**. ~5,000 Blitter launches per render of ~9 px each; the Blitter is
busy on **92% of polls**; ~19.5 cycles per launch.
Consequences, all confirmed by measurement:
- **Any per-pixel optimisation measures zero** (flat-shaded floor: null).
- Reducing face count barely helps (overdraw is only **1.21x**, so perfect
  occlusion caps at ~17%).
- Coarser geometry does nothing — the level is already un-subdivided at
  `SUBDIV_MAX >= 6144`.
- Freeing the 68k does nothing — `GCCHOT` cut 68k instructions 22% for **0 fps**;
  the 68k already spins ~49% of its awake time in `blit_wait`.
- **The only lever that scales is fewer SCANLINES**, because spans are emitted
  per scanline. 240->120 bought 6.00 -> 7.50 fps; 120->60 targets 15.00.

## 4b. ONLY ~35% OF TOM IS THE BLITTER (the thing that resolves everything)
First in-game 68k profile, window verified to be gameplay by screenshot BEFORE
trusting it (the first attempt profiled the TITLE SCREEN, whose 320x120 byte-copy
row decimation looks exactly like a smoking gun - 70% of the 68k in a
3-instruction loop):
```
68k asleep in STOP  63.9%   <- the 68k waits on Tom; Tom IS the critical path
68k awake           36.1%   diffuse, top single PC only 3.43%

Tom cycles       2,738,985,436
  Blitter           947,977,971  34.6%
  jump_refill       471,539,211  17.2%
  stalls (alu/load/div/flags)    14.0%
  useful compute                ~34%
```
**Every lever tried on this renderer lives inside that 35%** - flat floor,
trapezoid, phrase-mode dest, BWOVER, span batching, and halving the scanlines.
So their combined ceiling is ~35%, and VRES60's (which halves only part of the
Blitter's work) is ~17% - making its measured **+7% exactly what the split
predicts**. The apparent contradiction ("Tom is the critical path, yet halving
Tom's work did nothing") was never real: the model assigned Tom's ENTIRE budget
to the Blitter.
⇒ The other ~65% is Tom's **PER-VERTEX / PER-FACE** work: transform, projection
divides, cull, staging, plus 17% branch refill. It scales with **how many
vertices and faces Tom is handed** and with nothing else - not resolution, not
span count, not pixels.
⇒ So the lever is **fewer faces reaching the kernel**: room-count draw distance
(`ROOMCAP=N`) rather than distance (`FARCLIP` is inert at spawn - no room is
beyond 3000 units in the caves), portal culling, or LOD.
⚠️ The "occlusion caps at ~17%" bound came from OVERDRAW (1.21x), which bounds
**pixel** work. It does NOT bound vertex work, so it does not apply to this lever.

## 4c. THE FACES BEING TRANSFORMED ARE THE FACES YOU CAN SEE
`ROOMCAP=4` (draw only the 4 nearest rooms) is **NULL** on silicon - matched
pair, both `NOGD+QUIETFPS+EARLYCON` with a console attached:
```
control: all rooms   108 frames  median 9.0 fields = 6.67 fps
ROOMCAP=4            116 frames  median 9.0 fields = 6.67 fps
```
(An unmatched first attempt read 7.50 -> 6.67 and looked like a real slowdown.
It was the CONSOLE CONFIG costing ~1 field, not the cap. Always match the arms.)
It also only changes **0.4% of pixels**, and those two facts together say the
same thing: **the renderer is already handing Tom close to the minimum set of
faces.** There is no invisible geometry to cull - the ~34% of Tom spent on
per-vertex/per-face compute is spent on faces that are actually on screen.
⇒ So the vertex-work lever is NOT culling. It can only be:
  - **fewer faces in the SOURCE DATA** (real decimation - merging coplanar
    neighbours, simplifying, accepting a visual cost). Note `SUBDIV_MAX` being
    inert does NOT rule this out: that knob only controls SPLITTING faces, it
    never merges them.
  - or cheaper per-face work in the kernel: the 17% branch refill, the
    projection divides, `imul32`.
★ Calibration fact: a console build (`NOGD+QUIETFPS+EARLYCON`, jcp attached)
runs ~1 vsync field slower - 6.67 vs 7.50 fps. Never compare a console build
against a plain one.

## 4d. ★ 48% OF TOM'S TRANSFORM WORK IS THROWN AWAY (the found waste)
`CULLCOUNT=1` writes two DRAM counters: `n_staged` ($1C0000, faces that passed
the world backface cull and were FULLY transformed + screen-tested) and
`n_rastered` ($1C0004, faces that actually raster >=1 span). Differenced over
400 fields (~50 rendered frames) in-game:
```
staged     981 faces/frame
rastered   506 faces/frame
DISCARDED  475 faces/frame  = 48.4%
```
**Nearly half of every frame's per-face transform work is spent on faces that
never draw a single pixel.** They survive the cheap world-space backface test,
pay the full transform + projection + screen test, and are then rejected by
screen-area / XCULL / empty-y-range.
This is the first identified WASTE inside the ~65% of Tom that is not the
Blitter, and it is consistent with everything else measured:
  - `ROOMCAP` (distance) is null and only changes 0.4% of pixels, so the rooms
    being walked are the near ones...
  - ...but "near" is not "in view". A room beside or behind the camera is near,
    fully walked, and every one of its faces transformed before rejection.
⇒ **The untested lever is FRUSTUM culling, not distance culling** - reject whole
rooms (or face groups) against the view frustum BEFORE the per-face transform.
`FACE_PLANES` already puts a plane per face in the data, so the machinery for a
cheap pre-test exists.
Ceiling estimate: 48% of the per-face compute, which is ~34% of Tom -> ~16% of
frame time, plus the staging traffic those faces cost. Not 2x on its own, but it
is the largest single addressable waste found in this renderer.

## 5. Measuring fps here
fps is vsync-quantised, so a mean lies. Capture the ACTIVE picture area only at
60 fps (`crop=540:480:90:0`), reduce to 160x120 gray, and report the **median
gap in FIELDS** between rendered frames. **Calibrate the change threshold from
the data** — the per-field changed-pixel histogram is bimodal (analogue noise
below 0.3%, real frames above 0.7%); a naive threshold reports 60 fps. Sanity
check: the shipping build must reproduce **median 8 fields = 7.50 fps**.

## 6. Rig
- **`jcp -r` before every flash.** jcp will NOT flash over a running ROM: it
  reaches ~99% and dies with `can't connect with skunkboard`. A reset first
  replaces the "one flash then a physical bounce" ritual.
- A flash is ~200 s. **Never put a timeout on jcp**, and never block-buffer its
  output to a file — use `stdbuf -o0` and leave it detached, or you lose the log
  and can wedge the board.
- **One action per command.** Chaining build + flash + wait invites a harness
  timeout that kills the shell mid-flash.
- Wedged board = `jcp -s` itself hangs and the device re-enumerates. That needs
  a physical power cycle; nothing else clears it.
- Full-screen GREEN is the loader idle (no ROM). Uploads go to RAM at $4000, so
  any power cycle wipes the build.

## 7. Generated assets are an INDEX plus BLOBS
`mrt.bin` holds per-room byte OFFSETS into `mrt_geom.bin`; `mrt.h` /
`mrt_lplanes.h` are likewise derived. **Never mix a restored blob with a
regenerated index** — the sizes can all match and the game still reads
misaligned geometry and dies in a later room. Regenerate the whole set or
restore the whole set. See ASSET_BUILD.md for the recipe, including the three
knobs that are NOT in build_cof.sh (`FACE_PLANES`, `RAMP_PAL`, `STATICS`) and
`LARA_WINDFIX=0` (the default, 1, reverses ~190 of Lara's faces and renders her
see-through with no backpack).
Recovery trick: the blobs are incbin'd CONTIGUOUSLY, so any pre-clobber `.cof`
is a backup — anchor on a blob you still have, then walk mrt_data.S's incbin
order with the known sizes, honouring `.balign 8`. Validate by byte-comparing a
blob you already trust before believing any of it.

## 8. The skunk console needs `QUIETFPS=1` or it hangs before the game
`jcp -c <rom>.cof` works fine (flash and attach in ONE command - a bare
`jcp -c` against a running ROM wedges the board). But a console build
**freezes right before the game loop**, and it is not the game: main.c carries a
JERRYX room-transform VERIFICATION HARNESS guarded by
`#if defined(JERRYPOSE) && defined(SKUNK_CONSOLE) && !defined(QUIETFPS)`.
JERRYX was retired; the harness was not. It kicks `jerry_roomx_kick`, then
blocks in `jerry_pose_sync` waiting for a reply that never comes. On silicon it
reports `rx_flag=1` (never cleared), `rx_j0=rx_j1=0` (Jerry wrote nothing) and
`rx_badlongs=536` of 702 - i.e. the retired path is simply broken.
**Any console build must pass `QUIETFPS=1`.** Correct recipe:
`make ... NOGD=1 QUIETFPS=1`.
Also: `SKCONONLY=1` used to build with SKUNK_CONSOLE defined but WITHOUT
linking skunk.o/skunkglue.o/skunkdbg.o (only `NOGD` pulled them in), so it
silently produced a build with no working `dbg_kv` at all. Fixed.
☠️ And never `pkill -f "jcp -c"` - the pattern matches the shell running it, so
it kills your own command. Kill by PID (`pgrep -x jcp`).

## 9. Instruments must be validated on a positive control
Two instruments lied this project into wrong conclusions:
- a beacon that painted the **BG register** — invisible on this display path,
  read black even on a build that demonstrably executed it. Three conclusions
  had to be withdrawn. A beacon must **take the display over** (OP list = a
  lone STOP object + BGEN) — see `hangbeacon.c`, whose `BEACON_AT=1` is its own
  self-test.
- `blit_wait` (the modelled stall counter) reads 5.2% while the PC histogram
  shows 24.5% in the poll loop. They measure different things; the kernel
  comment at `bwait` says so. **Do not judge Blitter pressure by `blit_wait`.**
Rule: before trusting a NEGATIVE from any instrument, prove it fires on a build
you know is good.

## 4e. Frustum culling is already there, and it is inert (2026-07-29)

The request was "build the frustum culling", on the strength of §4d (48.4% of
staged faces never raster). Measuring first showed there is nothing to build at
the room level and nothing frustum-shaped to win at the face level.

**Room level is already done and already inert.** A build that skips every room
except the one Lara stands in (`CURONLY=1`) produces *bit-identical* face
counters to the normal build - different .cof md5, same 429844/221483. Only one
room is drawn at this pose anyway: the portal-visibility walk, the behind-camera
test, `FARCLIP` and the sliver test have already reduced it to one, and each
room is additionally handed the kernel as a portal-rect clip window. This is
the single explanation for the ROOMCAP, FARCLIP and HOPBOOT nulls - they were
all dialling something that had already converged.

**Face level is not frustum-shaped.** Splitting the 48.5% of staged faces that
never raster, by differencing the `rastered` counter across builds that disable
one cull at a time (ratios, not absolute counts - a slower build completes
fewer frames, so absolute counters shrink):

| class                                  | share of discards | flag used |
|----------------------------------------|-------------------|-----------|
| screen-space backface / signed area     | 57.5%             | `NOBFCULL=1` |
| empty y-range (sub-row at 320x119)      | 23.1%             | remainder |
| degenerate, area <= 1-2 px^2            | 15.3%             | `KEEPDEGEN=1` |
| off-screen in X                         | 3.1%              | `XCULL=0` |
| vertex behind the near plane            | 1.0%              | `BEXCNT=1` |

Only the bottom two rows - 4.1% of the waste - are what a frustum test rejects.
Lara is **0.1%** of staged faces (`LARACOUNT=1`), so none of it is her, despite
her runtime blobs carrying a dummy plane (`d = $F8000000`) that the pre-transform
cull structurally cannot reject.

**The pre-transform cull works, and tightening its data does nothing.** Positive
control: forcing the keep branch never to be taken (`SDPROBE=1`) drops staged to
**0**, so the cull is reached and functional. Yet two independent tightenings
both changed `mrt_geom.bin` and produced bit-identical counters:
  - `FACE_PLANES_SLACK=0` (was 96 world units) - the margin is inert.
  - `FACE_PLANES_TIGHT=1`, anchoring `d` at vertex 0 instead of `min(N.v_i)`.
    The screen test builds its cross product about vertex 0, so it rejects iff
    `N.C < N.v0`; `min` over verts makes the world test strictly weaker by the
    bent-quad gap. Closing that gap exactly changed **nothing**, so the faces
    slipping through are nowhere near the threshold under either convention.

So these faces are genuinely front-facing in world space and come out
negative-area or sub-row in the **320x119 integer** projection. That is the
effect `TINYCULL`'s comment already documents from the other end (9.4 faces per
frame get the wrong area sign per frame on Lara's head at LOWRES, decided by
rounding rather than geometry). It is a raster-resolution/LOD problem: the faces
are too small for the grid they are being projected onto.

⇒ The lever is face COUNT, not visibility: stop generating sub-pixel faces
(`SUBDIV_MAX=6144` is what creates them) rather than trying to reject them
faster. Any pre-transform test would also have to be cheaper than the transform
it saves, and would still leave 80% of the waste behind.

Instruments added, all off by default: `CULLCOUNT` (+`BEXCNT` stub, 20 B - does
not fit alongside `LARACOUNT`), `NOBFCULL`, `SDPROBE`, `CURONLY`,
`FACE_PLANES_TIGHT`.

## 4f. VRES60 boots on silicon - the blocker is gone (2026-07-30)

`VRES60=1` renders 60 lines instead of 120 and lets the OP scaler stretch 4.0x
(`FS_VSCALE 0x80`) instead of 2.0x. It was recorded as **blocked by A10** (black
on silicon while rendering fine in the emulator). That is no longer true: flashed
as a play build it comes up **in-game on hardware**, full scene, Lara correctly
proportioned - which also confirms `FOCAL_Y=47` (FOCAL/4) is the right constant
for the halved render height. The old "Lara looks really funny" was `FOCAL_Y=95`,
i.e. half the vertical FOV.

jagemu, matched viewpoint, 3800 emulated frames:

| build    | staged  | rastered | rastered % |
|----------|---------|----------|------------|
| baseline | 429 844 | 221 483  | 51.5%      |
| VRES60   | 551 831 | 270 495  | 49.0%      |

Same work per frame, **~28% more frames completed**.

**A theory killed before it cost anything.** I expected VRES60 to black out from
sub-row culling: at 120 rows the "empty y-range" class is already 23% of all
discards (SS4e), and halving to 60 rows should roughly double it. The plan was to
hand-edit the sub-row reject in kernel asm - with ~24 spare bytes - to clamp tiny
faces to a single row. Measured instead: the discard mix barely moves (51.5% ->
49.0% rastered) and the picture is intact. The kernel change is unnecessary.

**Do not quote a silicon fps number for VRES60 yet.** A 20 s capture of live play
read median 9 fields (6.67 fps) against a 7.50 baseline, but the arms were NOT
matched - the baseline is a fixed scene, that capture was wherever the player
happened to stand, and its gap histogram was spread (8x59, 9x41, 10x29) rather
than the tight bimodal shape a matched arm produces. Unmatched arms have already
produced one false alarm here (the ROOMCAP 7.50->6.67 "slowdown" was the console
config, not the lever). The valid rig is **AUTOSTART** (boots into the level and
idles at spawn, zero human variance), or `SPAWNAT` for a chosen spot.

## 5b. Measuring fps needs MOTION, and it must be the same motion (2026-07-30)

The flip-detection rig counts frames whose pixels changed. An **idle** scene
defeats it: `AUTOSTART` boots into the level and Lara stands still, and a 20 s
capture of that read **1.50 fps from 26 flips** - not a slow build, a rig that
had nothing to detect. Live play works only because the player supplies motion,
but then the arms are not comparable.

`AUTOSPIN=1` (rig only) rotates Lara 2/256 of a turn per tick. The whole screen
redraws every frame, so every flip is detectable, and the motion is identical in
both arms with zero human variance. It turns a 20 s capture into a trustworthy
number: baseline read **median 7 fields = 8.57 fps** with a tight histogram
(7x43, 8x33), against the earlier idle attempt's scattered garbage.

Judge a capture by the SHAPE of the gap histogram, not just the median. Tight and
bimodal = a real vsync-quantised reading. Spread across 7/8/9/10 or peppered with
16/17/25 = unmatched scenes or a static screen, and the number means nothing.

**VRES60 could not be measured this way: the arm black-screened (A10).** The same
VRES60 config had booted and played minutes earlier as a plain play build; adding
the one-line AUTOSPIN edit to main.c flipped it to black, while the baseline arm
with that identical edit ran fine. VRES60 renders correctly and boots
*sometimes* - which makes A10 (task #10) the blocker for the PERF campaign too,
not only for stability. Do not treat VRES60 as a banked win.

## 6b. A10: the ISR-deadline theory, and the OPMOVEM null (2026-07-30)

A reproducible black build finally exists, which A10 work has lacked:
`VRES60 + AUTOSTART + AUTOSPIN` is **0.0% black on silicon**, while
`VRES60` alone **boots and plays** and `baseline + AUTOSPIN` **boots**. So this
is an INTERACTION, not VRES60 alone, and it can be re-flashed at will.

`video.c:145` already records the mechanism that fits every symptom: the vblank
ISR must rebuild the scaled object before the OP re-fetches it at **VC 32**, and
"the full rebuild chronically finishes at VC 33-35 once render-time bus
starvation cuts the 68k to ~10-20% bus service". That reframes A10 from *"the
vertical interrupt never fires"* to *"the ISR misses its deadline"* - and it
explains the property that has blocked every previous attempt: **instrumenting a
failing build flips it to booting, because timing IS the bug.**

VI itself is exonerated as the difference: the console dumps show identical
`vdb=$19 / vde=$1FB` for VRES60 and baseline, so `VI = a_vdb - 4` is the same
line in both, far from the HW-probed dead zones (VI=15, VI>=515), and video.o is
byte-identical between booting and failing builds.

**OPMOVEM (tested, NULL).** Under bus starvation the expensive part is
instruction FETCH, and the repair was six `move.l abs,abs` = 5 words of fetch
each (~30 words) before any data moves. `OPMOVEM=1` collapses that to two
instructions over a contiguous `op_shadow[6]`:
    movem.l op_shadow,%d0-%d5
    movem.l %d0-%d5,op_list
Verified in the disassembly (not just the flag), including matching the ISR exit
restore to the wider entry push - the first build pushed d0-d5/a0-a1 and popped
only d0-d2/a0-a1, 8 bytes of stack corruption per interrupt that would have made
`rte` return to garbage. Result on silicon: **still 0.0% black.**

That is a real constraint on the theory. If the repair were finishing only a few
cycles late, removing ~26 words of instruction fetch from the critical path
should have bought ample margin. It bought nothing. Either the ISR is late by far
more than that (i.e. it is not being ENTERED on time, rather than running slowly),
or the black screen has a different cause in this configuration.

NEXT: distinguish "ISR entered late" from "ISR never entered" WITHOUT perturbing
timing - a counter the ISR bumps in DRAM, read back after the fact, rather than
a probe that changes the code path. OPMOVEM is kept, flag-gated and OFF by
default: it is a strictly shorter critical path, just not this bug's fix.

## 6c. A10 CONFIRMED: the ISR is never entered (validated instrument, 2026-07-30)

The `A10BG` probe settles it, and unlike the 2026-07-29 attempt it is validated
in BOTH directions.

`hangbeacon.c` records that a BG-register beacon read black on a build that
demonstrably booted, and that three conclusions had to be withdrawn off that dead
instrument. The reason is stated there: the OP's object covers the whole active
area, so BG is hidden. But that is exactly why BG WORKS here - in the failing
case **nothing paints**, which is precisely why the screen reads black (BGEN is
on and `BG=0`). The instrument is alive exactly when it is needed.

Probe: `BG=BLUE` at the end of video_init, `BG=RED` as the FIRST instruction of
the ISR (one `move.w`, so it cannot shift the race the way a console attach
does; confirmed in the disassembly at 411a, not merely by the flag).

| build | screen | means |
|---|---|---|
| failing (`VRES60+AUTOSTART+AUTOSPIN`) | uniform **BLUE** | video_init ran; **ISR never entered** |
| same + one extra store (flipped to booting) | picture, **RED edges** | ISR runs; BG shows where the object does not cover |
| failing, no probe | pure **BLACK** | same state, BG just left at 0 |

The red edges on the booting build are the positive control the earlier attempt
lacked. So: **A10 is a genuinely dead vertical interrupt, not a late ISR.**

⇒ **The deadline theory of SS6b is REFUTED**, and the OPMOVEM null is explained -
I was shortening a code path that never executes. My reasoning there was sound
but aimed at the wrong stage; the instrument beat the theory.

Two further facts:
- **Reproducible**: the failing ROM read 0.0% non-black on two independent
  flashes. A10 finally has a deterministic handle.
- **IRQREARM is ON by default and does not save it.** The re-arm lives inside
  `gpu_sync`'s poll loop, so it only runs if the 68k wakes; with no interrupt
  ever arriving the `stop` sleeps forever. Mitigation that presupposes a wakeup
  cannot recover a total interrupt failure.
- Adding ONE store to video_init flipped the failing build to booting - the
  codegen cliff, reconfirmed.

### Avenues still open
1. Does arming complete? Test by **MOVING** the single BG store after the
   VI/INT1/cpu_irq_on block rather than ADDING one (net instruction count
   unchanged, so it does not flip the build).
2. Is the 68k alive at all in the failing state, or stopped? (BG cycled from the
   main loop.)
3. Bisect the three stores between the black and booting builds to find which one
   flips it - that measures whether the trigger is code SIZE/alignment.
4. `VRES60` half-bisect: kernel projection vs display geometry, flags only.
5. **Make the display independent of the ISR** - double-buffer the OP list, or do
   the per-field repair from the GPU (Tom is not bus-starved). This is a FIX
   rather than a diagnosis, and it is the strategically valuable one: if the OP
   list did not need per-field repair, a missing VI would cost animation instead
   of the entire display, and A10 would stop being fatal.

## 6d. A10 bracketed: init completes, the first flip never happens (2026-07-30)

Extending the `A10BG` probe (BLUE before arming, GREEN at the end of video_init,
RED as the ISR's first instruction, and a red RAMP cycled on every
`video_flip`):

| observation | reading |
|---|---|
| screen settles **GREEN** | video_init ran to COMPLETION - VI armed, `INT1=0x0003`, `cpu_irq_on()` returned, VMODE written |
| never **RED** in 45 s+ | **no interrupt of any kind is delivered** - not one field |
| ramp never cycles (3 samples identical) | **`video_flip` is NEVER reached** |

So the arming block is exonerated (avenue 1 closed), and the hang sits between
the end of video_init and the first flip: level load, first render, or the first
`gpu_sync`.

**A10 IS NOT VRES60-SPECIFIC.** This run was the *baseline* 120-line build - the
probe stores alone pushed it over the cliff. VRES60 was never the cause; it only
shifts codegen. Treat A10 as a property of the whole build, not of one feature.

`cpu_stop_unless` is correct on both paths (masks, tests, then either
`stop #0x2000` - which unmasks atomically - or restores `#0x2000` and returns),
so it does not leave interrupts masked. But it sleeps waiting for EITHER a
GPU-done or a video interrupt, and with both silent that sleep is permanent.
That is also exactly why `IRQREARM` cannot rescue this case: its re-arm lives
INSIDE the poll loop, and the loop cannot iterate until the `stop` returns. A
mitigation that presupposes a wakeup cannot recover a total interrupt failure.

### Next
Bracket the remaining window with the same BG technique - markers at level-load
start/end and immediately before the first `gpu_sync` - to find which of the
three it stops in. Then either fix that site or, better, remove the dependency
(SS6c avenue 5): drive the OP-list repair from the GPU, or double-buffer it, so a
dead VI costs animation instead of the whole display.

## 6e. A10: it is a TIMING race, and how to probe it without moving it (2026-07-30)

Two results this round change the method more than the theory.

**1. Padding does NOT flip it.** `PADBYTES=1024` links dead, never-executed
.rodata into the failing build - pure size/layout, nothing the 68k runs. It
stayed **0.0% black**. So the trigger is NOT image size and NOT the addresses
things land at.

**2. But added INSTRUCTIONS always flip it - 4 for 4 today.** The green
end-of-init marker, `BOOTCRUMBS`, the `video_flip` liveness ramp, and `AUTOSPIN`
each turned a black build into a booting one.

⇒ **A10 is a timing race, not a layout artefact.** What matters is cycles
executed, not where code sits. A handful of extra instructions in the boot path
is enough to move it.

### The probe rule that follows (this is the useful part)
**REPLACE an existing instruction; never ADD one.** That is exactly why the
readings differ:

| probe | form | result |
|---|---|---|
| `A10BG` blue | REPLACED the existing `BG = 0` store | stayed black -> gave a clean reading |
| green marker / crumbs / ramp / AUTOSPIN | ADDED stores | flipped to booting, no reading |
| `PADBYTES=1024` | added no instructions | stayed black (but tells us nothing on its own) |

Every future A10 probe must be cycle-neutral: overwrite the VALUE an existing
store already writes, or swap an instruction for one of equal cost. Adding a
`move.w` to a boot path is enough to destroy the experiment.

### Also eliminated this round
- The ISR's INT1 ack is **correct**: `lsl #8 / andi #0x1F00 / ori #0x0003` -
  it re-asserts the video+GPU enables, so a first ISR cannot leave interrupts
  disabled. (Tested as a hypothesis for "green forever": a first VI firing
  inside video_init would paint RED, then green would overwrite it, and if that
  ISR had killed the enables we would see exactly what we see. The code says
  otherwise.)
- All 256 vectors point at `exc_catch`, which paints a RED border. We never see
  it, so **no exception handler runs**. Either there is no exception, or a
  double fault halted the CPU before any handler could execute - and a halted
  68k takes no interrupts, which would look identical to a dead VI.

**Still open:** whether the 68k is asleep in `stop #0x2000` (alive, unmasked, no
interrupt ever arriving) or HALTED by a double fault. These need opposite fixes
and no probe so far distinguishes them, because every probe capable of answering
it has flipped the build. A cycle-neutral probe per the rule above is the way in.

## 6f. A10 IS POSITIONAL - retracting 6e's timing-race claim (2026-07-30)

`PADTEXT=272` grows `.text` by 272 dead bytes so everything after it lands where
the *booting* build puts it, executing **not one new instruction**. On the
reproducibly-black config it **BOOTS (96.9% non-black)**.

That retracts SS6e. A10 depends on **where code is linked**, not on cycles
executed, and it reconciles every result:

| change | moves code? | result |
|---|---|---|
| `PADBYTES=1024` (.rodata, sits AFTER .text) | no | still **BLACK** |
| `PADTEXT=272` (.text, shifts everything after) | yes | **BOOTS** |
| GPU-kernel probe (+272 B blob linked BEFORE the 68k code) | yes | **BOOTS** |
| green marker / BOOTCRUMBS / liveness ramp / AUTOSPIN | yes - added instructions shift later addresses too | **BOOTS** |

So all four "instrumentation flips it" cases were ADDRESS SHIFTS. The probes were
never perturbing timing; they were relocating things. My 6e inference was wrong -
the observations were right, but every probe moved code as a side effect, so
"added instructions" and "shifted addresses" were perfectly confounded.
`PADBYTES` vs `PADTEXT` is what separates them, and the difference is documented
in main.c:488 - .rodata sits after .text and moves nothing.

**The probe rule from 6e still holds**, but for the right reason: a probe must be
**position-neutral**, not merely cycle-neutral. Compensate any added bytes with
`PADTEXT` so later addresses are unchanged, or replace instructions in place.

### What to hunt now
Something in this build is **address-dependent**. Candidates, cheapest first:
1. **Alignment of a hardware-visible structure.** The OP list and every
   framebuffer feed the Object Processor, which requires PHRASE (8-byte)
   alignment; `op_list` carries `aligned(16)`, but the framebuffers, the Jerry
   pose blob and the dispatch list should be audited the same way. A structure
   that lands phrase-aligned in one link and not the next would make the OP
   render nothing - exactly the observed "BG fills the screen, no object".
2. **A cart-ROM bank/page boundary** straddled by a critical routine.
3. Compare the link maps of the black vs `PADTEXT=272` builds and diff the
   addresses of everything the hardware reads directly - that names the culprit
   without another flash.

Note (3) is the strongest next move: it is offline, needs no board, and the two
builds are already on disk.

### 6f.1 Link-map diff: alignment and boundary-crossing ELIMINATED (offline)

Symbol maps of the black build vs `PADTEXT=272` (which boots), compared with no
board time at all. Everything shifts by exactly 0x110 (272) and:

- **No structure changes 8-byte (phrase) alignment.** `op_list`, `op_shadow`,
  `fb0/1/2`, `front_fb`, `pending_fb`, `mailbox`, `lara_blob`, `dsp_mailbox` all
  keep identical `addr & 7` in both builds. So the OP-alignment theory is dead.
- **No structure changes its boundary-crossing behaviour** at 8 KB or 64 KB.
  `fb1` straddles a 64 KB boundary in BOTH builds; nothing else differs.

So whatever is address-dependent is NOT the alignment or page-straddling of the
buffers the Object Processor and DSP read. Both theories eliminated for free -
this comparison is repeatable any time with `nm -n` on the two ELFs.

Remaining positional candidates: a cart-ROM bank/page boundary straddled by a
specific routine (the image is ~1.6 MB and the skunkboard banks it), or a
same-alignment address collision with something not in the symbol table (GPU/DSP
scratch, the `$1C0000` debug window, or a region the Blitter writes).

## 4g. THE RENDERER IS NOT FILL-BOUND - it is FACE-bound (2026-07-30, silicon)

Matched arms (`AUTOSTART + AUTOSPIN`, identical scene, deterministic motion),
20 s captures, mean gap over gaps <= 12 fields (outliers = load stalls dropped):

| build | mean frame time | fps |
|---|---|---|
| baseline, 120 lines | 6.98 fields | **8.59** |
| `VRES60`, 60 lines | 6.80 fields | **8.82** |

**Halving the rendered scanlines is worth 2.6%.** Both medians read 7 fields
because fps is vsync-quantised - the MEAN is the honest measure here, and the
flip counts (172 vs 176) hint at it before the mean confirms it.

⇒ The render is **not fill-bound and not span-bound**. At 60 lines most faces are
already only 1-2 rows tall, so the span count is floor-limited by the NUMBER OF
FACES rather than by the height; work paid once per face cannot be halved by
halving resolution. That also explains the diminishing returns: LOWRES 240->120
bought **+25%**, VRES60 120->60 buys **+2.6%** - the per-face floor was already
reached at 120.

This converges with SS4e from the other direction: 48.5% of staged faces never
raster, and the survivors are degenerate/sub-row rather than off-screen. Both
roads end at the same place - **the only lever left is FACE COUNT (or per-face
setup cost)**.

**And that lever is big enough.** 8.82 -> 15 fps needs ~1.7x. Eliminating the
48.5% of per-face work that never produces a pixel is worth close to that, IF it
is removed at the source (extractor-side: merge coplanar faces, decimate geometry
that can never exceed a pixel, drop faces below a screen-area threshold at the
distances they are actually viewed from) rather than rejected at runtime, which
still pays the per-face cost to decide.

**Do not spend further effort on resolution, fill rate, span batching or Blitter
micro-optimisation.** They are measured null-to-marginal, and this is the
experiment that says why.

## 4h. Extractor-side face reduction: there is nothing small to remove (2026-07-30)

Census of all 7750 room faces, measuring each face's SMALLER in-plane extent:

    median 1024 units (a full sector)   mean 880   p10 = 256   p25 = 768

A quad of side s at distance d covers s^2*190*95/d^2 px^2 (FOCAL 190 x, 95 y at
LOWRES), so it is sub-pixel when s < d/134. Applying that:

| view distance | sub-pixel threshold | faces below it |
|---|---|---|
| 2048 | 15 units | **0** (0.0%) |
| 4096 | 31 units | **0** (0.0%) |
| 8192 | 61 units | **0** (0.0%) |
| 16384 | 122 units | **3** (0.0%) |

**There is no small geometry to drop.** Every face is sector-sized. So the 48.5%
of staged faces that never raster (SS4e) are NOT tiny meshes - they are large
faces killed by PROJECTION (edge-on, off-screen, behind). Decimation by size has
nothing to remove, and merging coplanar quads is blocked by texture tiling (each
quad maps exactly one atlas tile; a merged quad would stretch it, since the
affine mapper cannot repeat within a face).

⇒ Face COUNT cannot be reduced in the extractor without changing how textures are
mapped. The per-face win must instead come from **rejecting faces before the
transform**, and SS4e says where the mass is: **57.5% of the waste is faces that
pass the world-space backface test and then fail the screen-space one.** Making
those two agree is worth 27.9% of ALL per-face work. That disagreement is still
unexplained (SLACK=0 and anchoring d at v0 both changed nothing) and is now the
highest-value open question in the perf campaign.

## 4i. The screen backface cull is CORRECT - and the 27.9% is still unexplained

Two offline results, both negative, both worth keeping:

**The screen cull is not dropping visible geometry.** Disabling it (`NOBFCULL=1`)
changes **0.55% of pixels** in the spawn scene. So the faces it rejects really are
invisible; it is not the integer-rounding sign flip that `TINYCULL`'s comment
documents for tiny triangles. That kills the "it is silently deleting polygons"
theory - the rejections are legitimate.

**Geometric simulation does not reproduce the runtime disagreement.** Simulating
both tests over all 7750 faces with the camera at each room's centroid:

| outcome | faces |
|---|---|
| culled by BOTH | 872 (11.3%) |
| SCREEN only (the wasted class) | **42 (0.5%)** |
| WORLD only (would be a hole) | 0 (0.0%) |
| kept by both | 6836 (88.2%) |

The world cull catches **95.4%** of back-facing faces there and never over-culls.
But the runtime measures **27.9% of staged faces** failing the screen test. 0.5%
vs 27.9% - the simulation does not reproduce it.

Also note only 11.3% of faces are back-facing from inside a room, which is
expected: a closed room's faces all point inward.

The obvious candidate is that a room centroid is not a representative camera
position - in play the camera sits CLOSE to the floor and walls it is next to, so
`N.C` approaches `N.v0` and the world test's weakness (`d = min(N.v_i) -
SLACK*|N|` is strictly less than `N.v0`) starts to matter. **But
`FACE_PLANES_TIGHT=1` closes exactly that gap and changed the runtime counters
not at all** (SS4e), so that explanation is not sufficient either.

**Status: unexplained.** The next step is to stop simulating and capture the real
thing - log the actual camera position and the per-face cull outcomes from a live
frame (the `SPAWNAT` + `POSDUMP` rig makes a fixed, reproducible pose available)
rather than guessing at a representative pose.

## 6g. A10 unified: a TIMING race modulated by CODE POSITION (2026-07-30)

`PADTEXT=272` flips the failing build to booting. **272 = 34x8, so it preserves
every alignment** - checked against the symbol maps, `gpu_kernel` sits at 4 mod 8
in BOTH builds, and no hardware-visible structure changes `addr & 7` or its
8K/64K straddling. So the flip is not alignment and not a fixed-address
collision.

What a uniform shift DOES change is which **DRAM pages** the code occupies, and
page hits vs misses change instruction-fetch timing. That reconciles 6e and 6f,
which looked contradictory:

| change | moves code? | changes cycles? | result |
|---|---|---|---|
| `PADBYTES` (.rodata after .text) | no | no | **black** |
| `PADTEXT` (.text) | yes | yes (via DRAM paging) | **boots** |
| added instructions | yes | yes | **boots** |

⇒ **A10 is a RACE. Code position matters because it perturbs timing, not because
any address is special.** Both earlier readings were half-right.

Consequences that matter for how we work:
- **`PADTEXT` is a workaround, not a fix, and it is per-build.** 272 rescues one
  configuration and breaks another; 0 and 136 both rescue a third. A ROM that
  boots only at lucky offsets is a fragile artifact - any future change
  reshuffles the timing.
- Also eliminated this round: the stack is at `0x200000` (top of 2 MB) with
  ~345 KB of headroom above `.bss`, so no stack/bss collision; and the image
  runs from **DRAM** (`jcp`: "base addr is $4000", "accepted start at
  $00004000"), so cart-ROM bank boundaries are irrelevant - a candidate I had
  wrongly listed in 6f.

**The race itself is still the thing to find.** What is known: `video_init`
completes (VI armed, INT1 enabled, VMODE written), NO interrupt of any kind is
ever delivered afterwards, `video_flip` is never reached, and `cpu_stop_unless`
is correct on both paths. The next probe must be cycle-neutral AND
position-neutral - replace an instruction in place and compensate with
`PADTEXT` so later code does not move.

## 4j. The vanishing-face artifact: reproduced OFFLINE and attributed (2026-07-30)

User report: large polygons near Lara blink between black and their texture as
the camera moves, worst to her left.

**It reproduces in jagemu** with `AUTOSTART + AUTOSPIN` - `jagemu video --count N
--every K --start S` renders a contact sheet and the black wedges appear and
disappear across frames exactly as on hardware. That ends the
one-hypothesis-per-flash cycle this bug had been costing (four wrong theories,
~10 min each). It also proves the bug is **logic, not Blitter timing** - unlike
the `DSTEN` clobber, which was silicon-only.

Attribution, by counting black pixels over a 4-frame strip:

| arm | black px | vs baseline |
|---|---|---|
| baseline | 17078 | - |
| screen backface cull OFF (`NOBFCULL`) | 17078 | **0.0%** |
| near plane 64 -> 16 (`NEARLOW`) | 11580 | **-32.2%** |
| `XCULL` omitted | 19944 | **+16.8% (WORSE)** |
| `XCULL` omitted + near 16 | 9039 | -47.1% |

- **The screen-space backface cull is innocent** - not one pixel changes.
- **The near plane is a real cause, worth ~1/3.** `BEXIT` discards an ENTIRE
  face as soon as one vertex crosses `NEAR`; there is no near-plane CLIPPING. A
  large floor/wall face beside Lara therefore vanishes wholesale and pops back.
  Lowering `NEAR` only shrinks the band - the fix is to clip the polygon at
  z = NEAR.
- **`XCULL` is doing useful work**: removing it makes the artifact WORSE.
- ~50% is still unattributed, and some of it is legitimate void where the room
  ends - not all black is a bug.

Also found: **`main.c` defines `NEAR 32` while the kernel used `NEAR .equ 64`**,
despite the kernel comment saying they must match. Worth reconciling regardless.

Four theories were killed getting here - `TINYKEEP` (the flickering region is
~50x60 px, far too large for integer rounding), the portal-sliver cull, the
portal-window clip rect (`NOPCLIP` changed nothing, and the video showed the
holes have SLANTED edges - stddev 3.7-8.6 px - so they are geometry-shaped, not
rect-shaped), and initially the near plane itself when a single flash did not
visibly fix it. The offline rig is what turned guesses into measurements.

## 4k. The black triangles are SILICON-ONLY - stop looking at cull logic

`POSDUMP` gave the exact camera where the user sees two black triangles
(room 0, x=74240 y=3762 z=12800 yaw=51 - note `pd_x`/`pd_z` are world/256).
Feeding those to `SPAWNAT` and rendering in jagemu gives a **clean frame: 0.4%
black, no triangles**.

⇒ **The artifact does not reproduce offline. It is a HARDWARE behaviour, not a
logic error.** Every culling hypothesis was doomed on principle, because the
logic produces a correct frame. That is five dead theories (`TINYKEEP`, sliver
cull, portal clip rect, near plane, palette base 0) explained at once.

This is the same class as the `DSTEN` clobber earlier today - jagemu rendered
that perfectly while silicon showed a flat grey world - and as the LOWRES flip's
missing `blit_wait` (worth +25% when found). **jagemu does not model Blitter
timing.**

The shape corroborates it: a right triangle is what a run of consecutive
UNWRITTEN spans looks like, each one pixel narrower than the last, leaving the
cleared framebuffer behind. The region grows and shrinks with view angle because
the number of affected spans tracks the face's slope.

**Prime suspect: a missing or insufficient `blit_wait`.** On hardware the Blitter
takes several cycles after the B_CMD store to assert BUSY, so polling too early
reads a stale idle bit and the next register write corrupts the in-flight blit.
The SPANSHADE path already documents exactly this hazard and guards it with two
dummy settle reads; the ordinary per-span texture path may not.

### How to work it (do NOT theorise about culling again)
1. Bisect on hardware with the shade pass OUT of the picture (`SPANSHADE=2`
   launches nothing) to see whether the triangles are pre-existing or something
   the second per-span blit provokes.
2. Compare the span path's wait against the settle-read guard SPANSHADE uses.
3. `--fidelity silicon` in jagemu is worth trying, but the functional model has
   already been shown not to reproduce this.

## 4l. The toggling face is a RACE, not a cull - seven theories down

Measured on hardware with the camera HELD STILL (user standing at the spot):

    black px alternates ~9630 <-> ~11100, bimodal
    swing 1562 px (2.0% of frame), 2.5 toggles/s, ~2-3 rendered frames per state

**At a fixed camera the same face is sometimes drawn and sometimes not.** Culling
is deterministic - identical geometry and camera give identical decisions - so
this cannot be a cull. That single observation retro-explains every failed
hypothesis: I kept hunting a wrong DECISION when the decision is right and the
EXECUTION varies.

The user also separates two artifacts, which had been conflated:
- the **higher/back** one is **STATIC black** (genuinely missing or black
  geometry - a different bug),
- the **near** one **TOGGLES** (this race).

### Ruled out, all measured, none of them it
| suspect | evidence |
|---|---|
| `TINYKEEP` / sub-pixel sign | region is ~50x60 px, far too large for rounding |
| portal-sliver cull | no change |
| portal-window clip rect | `NOPCLIP` no change; holes have SLANTED edges (stddev 3.7-8.6 px) |
| near plane / `BEXIT` | owns ~1/3 of general black, not this |
| palette base 0 forced black | `BASE0PROBE` recolours it magenta - nothing turned magenta |
| screen backface cull | `NOBFCULL` changes 0 pixels |
| room visibility / degenerate | `NOVISCULL`, `KEEPDEGEN` change 0 pixels |
| `ROWDIET` span batching | toggling persists |
| Jerry co-transform race | toggle swing unchanged (1618 vs 1562 px) |
| bounded `bwait` guard | `BLIT_GUARD` = 200000 spins, cannot expire on a ~20-cycle blit |
| framebuffer rotation | only TWO buffers ship (`fb2` aliases `fb0`); a buffer-tied defect would alternate every frame, the observed runs are 2-3 frames and irregular |
| the per-span shade blit | pre-existing: still toggles with `SPANSHADE` removed entirely |

**And it does not reproduce offline.** `SPAWNAT` at the user's exact camera
(room 0, x=74240 y=3762 z=12800 yaw=51) renders clean in jagemu - 0.4% black -
under BOTH `--fidelity functional` and `--fidelity silicon` (pixel-identical).

### Do this next, and stop toggling flags
Flag bisection has hit its limit: eleven arms, one real find (the far clip, a
DIFFERENT bug). The next step is to instrument on hardware - per-frame counters
in DRAM (faces staged / rastered / rejected-by-reason) read back over the
console, comparing a frame where the face draws against one where it does not.
That names the stage instead of eliminating suspects one flash at a time.

## 4m. Flashing face: all four cull exits eliminated ON HARDWARE

Enumerated every jump to `pkt_done` (the face-skip target) rather than trusting
flag names, and tested each on silicon at the user's exact spot:

| exit | guard | hardware result |
|---|---|---|
| BEHINDF (any vert behind) | compiled out under `BEXIT=1` | n/a |
| face X-cull vs clip window | `XCULL` | disabled -> **still flashing** |
| screen-space signed area | `NOBFCULL` | disabled -> **still flashing** |
| empty y-range after clip clamp | `NOEMPTYY` | **CRASHES** (see below) |

Plus, from the counters: world-space plane cull and near plane both eliminated -
`rastered` fell 6442 while the screen-space bucket rose 6227, with `wcull` and
`bexit` nearly flat.

**The empty-y exit cannot simply be removed.** Doing so lets a face with y1 < y0
reach the edge-walk with an inverted row count: silicon painted the `exc_catch`
RED border, then the beacon's bit rows ("wavy lines"), then black. The
`TINYKEEP` comment documents this exact hazard two tests away - `ar == 0` must
stay culled because the degenerate walk black-screened `KEEPDEGEN`. A clamp
variant (y1 = y0, one row) also crashed.

⇒ Either the empty-y clamp is the cause and the rasteriser cannot tolerate the
test that would prove it, or no cull is responsible and the face is lost AFTER
culling, in span emission or the blit.

### Two invalid experiments to not repeat
- **`NEARLOW` reached only the kernel.** `main.c` (`NEAR 32`) and `dsp_pose`
  (`RX_NEAR 64`) were untouched, so Tom and Jerry disagreed about which vertices
  were behind - and which one transforms a room is decided by a per-frame race.
  Fixed by wiring the flag to the DSP too; `main.c` is STILL 32 and nothing
  enforces agreement at build time.
- **`NOBFCULL` sat inside `TINYKEEP`'s `.else`.** With `TINYKEEP=4` it was never
  compiled, so "screen-area ruled out" was a no-op experiment reported as an
  elimination.

**Rule earned:** verify the change LANDED (disassembly, or image size) before
believing any A/B. Both failures were flags that silently did nothing.

### Also worth knowing
The crash beacon is a real instrument: `exc_catch` paints the exception frame
(SR/PC) as bit-rows with an A5A5A5A5 calibration row. RED-then-black means
CRASHED; plain black means hung or not drawing. `tools/decode_beacon.py` is
referenced in the comments but **does not exist** - writing it would turn any
crash into a PC.

## 4n. Lara's head notches: root cause and the fix (2026-07-31)

Her crown shows notches - the inside of the skull shows through where front
faces are missing. Established this session:

- **Her head renders SOLID in jagemu** (full-precision) and notched on silicon.
- **At 320x240 (LOWRES off) the head fills in noticeably** - user-confirmed. So
  it is a PRECISION artifact, not geometry and not a mis-tuned threshold.
- `TINYKEEP` 2 -> 4 -> 8 -> 16 px^2 changed nothing. Widening the keep-band
  cannot help: below the noise floor the test cannot tell front from back, so a
  wider band keeps back faces as readily as front ones.
- Disabling the screen cull shows her SCALP (no depth buffer: back faces
  overwrite front ones wherever per-mesh face order is not back-to-front).

**Mechanism.** The signed area comes from INTEGER projected coords. LOWRES halves
the vertical scale, so her small head faces have near-zero area and the SIGN is
decided by rounding - the kernel already measured 9.4 faces/frame flipping "at
LOWRES". Doubling the resolution doubles the areas and most clear the noise.

**Why she is uniquely affected.** Every ROOM face carries a baked plane and gets
an exact world-space `N.C < d` test BEFORE projection. Lara's faces carry a
DUMMY plane (`d = $F8000000`, which the kernel already detects for LARACOUNT)
because she is skinned at runtime, so the rounding-prone screen test is her ONLY
backface check.

**The fix: give her real per-face planes at pose time** - one cross product per
face after skinning - so she is culled exactly, like the level is. Jerry already
poses her and has spare cycles; the 68k is ~64% idle. Cost is negligible: she is
**0.1% of staged faces**.

☠️ **A winding "fix" made it WORSE and was reverted.** An audit found 11 of her
375 faces wound against their own mesh's majority (mesh 1: 2, mesh 4: 2, mesh 14
/head: 7) - matching the old "11 mis-wound TR1 faces" note. Flipping them to the
majority produced see-through geometry. Since jagemu renders her head SOLID from
the same data, the data is good enough and the flip was aimed at the wrong layer.
Kept behind `LARA_WINDAUTO` (default OFF).
★ Lesson: her head renders solid offline, so ANY head hypothesis should be
checked offline FIRST - a flip that corrupts geometry shows up there immediately.

## 4o. LPLANES does NOT fix the head - culling is exonerated entirely

`LPLANES` (built 2026-07-26, never enabled) gives Lara REAL per-face planes and
culls her on the 68k with an exact world-space test, replacing the screen-space
signed area whose sign is unreliable on her sub-pixel head triangles. Its own
comment names the exact symptom ("her face painted over the back of her skull").

Enabled and flashed (`PADTEXT=40` boots; 0/64/120 are black): **the head is still
see-through.**

⇒ **Backface culling is not the cause.** An exact cull changes nothing, so no
threshold, no plane data and no winding fix can help. That retires SS4n's
precision theory as the ROOT cause (resolution still visibly helps, so precision
is a contributing factor, not the mechanism).

### The unifying hypothesis
The head notches and the flashing world triangles now share every measured
property:
- render CORRECTLY in jagemu, wrong on silicon (both `--fidelity` models agree)
- survive every cull-side fix (six for the triangles, four for the head)
- look like faces / face-fragments that were simply never drawn

That is the `DSTEN` signature: a Blitter-level failure jagemu cannot model. One
root cause behind both is now more likely than two independent bugs, and it lives
in span emission or the blit - NOT in any culling decision.

**Stop testing culls for either artifact.** The next instrument has to observe the
span/blit path on hardware.

### Rig note: GREEN IS NOT "RUNNING"
A full-screen GREEN display is the skunkboard LOADER - no ROM running. Counting
non-black pixels reads that as 99.4% and calls it healthy; I reported a build as
in-game when nothing was running. `screencheck.sh` in the session scratchpad now
classifies GREEN-loader / BLACK-hang / RED-crash / title / in-game by mean RGB.

## 4p. Lara's head + the flashing faces: everything eliminated so far

The two artifacts share every measured property and are probably ONE bug.
Eliminated ON HARDWARE, each with a real test:

| suspect | test | result |
|---|---|---|
| screen-area / backface cull | `NOBFCULL` (with `TINYKEEP` omitted, so it really compiled) | no change |
| keep-band too narrow | `TINYKEEP` 2 / 4 / 8 / 16 px^2 | no change |
| `XCULL` clip-window test | omitted | no change |
| empty-y skip | cannot be removed - **crashes** (inverted row range) | n/a |
| world-space plane cull | counters: `wcull` flat while `rastered` swung | no change |
| near plane | `NEARLOW` on BOTH Tom and Jerry | no change |
| exact per-face planes for Lara | `LPLANES` (existing, never enabled) | no change |
| Jerry pose handoff | `JERRYPOSE` omitted - 68k skins her | no change |
| vertex-cache overflow | `VCBIG` 768 -> 2048 verts | no change |
| `ROWDIET` span batching | omitted | no change |
| per-span shade blit | `SPANSHADE` removed entirely | no change |
| portal-window clip rect | `NOPCLIP` | no change |
| room visibility / sliver | `NOVISCULL`, `SLIVERW/H=0` | no change |
| mis-wound source faces | 11 faces flipped to mesh majority | **WORSE**, reverted |

Also known: her hair is **81% unstable frame to frame** (measured from the user's
screencast: 3622 px ever hair, only 689 always hair, 30% swing in hair pixels) -
so this is not "a few faces wrongly culled", it is geometry being rendered
differently every frame while she stands still.

**Resolution helps** (320x240 fills her head in noticeably) but is not the
mechanism - `LPLANES` proves an exact cull changes nothing.

**Everything upstream of rasterisation is now eliminated.** The remaining
suspects are span emission and the blit itself, which is where the `DSTEN` bug
lived and which jagemu cannot model (both `--fidelity` levels render these
scenes correctly).

### What a real next step looks like
Flag bisection is exhausted - 14 arms, no fix. Options, in order of value:
1. **Write `tools/decode_beacon.py`** (referenced in `exc_catch`'s comment but
   MISSING). It turns any crash into an SR/PC. Cheap, and permanently useful.
2. **Instrument span emission on hardware**: count spans issued vs blits
   completed per frame, dumped to DRAM and read over the console. That is the
   one stage never measured.
3. **Give cobweb the failing cases** so jagemu can model Blitter timing - two
   concrete ones now (`DSTEN`, and this).

### Genuinely fixed along the way (keep)
`VCBIG` - the vertex cache really did overflow (rooms 712 + Lara 300 vs a 768
cap), writing up to 864B over `dsp_mailbox`. Not this symptom, but a live bug.

## 6h. The crash beacon: decoder written, two real bugs fixed, still not visible

`tools/decode_beacon.py` now EXISTS (it was referenced in `exc_catch`'s comments
for weeks but had never been written) and is **self-tested**: `--selftest`
synthesises a beacon, pushes it through a simulated pillarboxed 1920x1080
capture with 2x vertical stretch, and decodes all seven rows exactly. It
calibrates its black/white threshold from the `A5A5A5A5` row, and REFUSES to
report values if that row does not decode - so it cannot print plausible
garbage, which is how it correctly rejected the first real capture.

Two genuine bugs found in the beacon path while trying to read a crash:

1. **The display dies before the beacon can be seen.** `exc_catch` painted into
   the framebuffers, but after a crash the vblank ISR is dead, so nothing
   repairs the OP list, the object stops being fetched, and the screen shows
   only BG - a flat red field. FIXED: the spin loop now repairs the scaled
   object itself, exactly as the ISR would. Confirmed on hardware (the user saw
   "red bars on both sides with black in between" - object alive again).
2. **The painter assumes an RGB16 framebuffer.** It uses a 640-byte scanline
   stride and 16-bit writes, but the game builds `-DFB8`: 8-bit paletted, so a
   320px line is 320 BYTES. Every row landed twice as far down as intended
   (7 rows x 10 lines x 2 = 140 lines) - past the end of a 120-line
   framebuffer. FIXED behind `FB8BEACON=1`: byte writes, 320-byte stride.

**Still not visible after both fixes.** `exc_catch` demonstrably runs (red BG,
OP repair holding), the GPU is halted first (so it cannot overwrite the rows),
yet the displayed framebuffer stays black. At least one more thing is wrong -
most likely WHICH buffer is displayed vs which `crash_fbs` entries get painted
(note `fb2` is aliased to `fb0` under the FLIPSTATIC path, so "all three" is
really two).

Next: have `exc_catch` point the OP list at a KNOWN framebuffer (the one it
paints first) instead of trusting `op_fix0`, removing the front/back ambiguity
entirely.


## PSX dynamic RE available (2026-08-01, note from jag_sotn)

PCSX-Redux is built and headless-driveable; a read-watchpoint on the TR1 PSX disc
shows the real engine decoding its own data, which can retire the remaining
*inference-plus-validation* asset facts here (the 40-word PSX anim frame + swapped
joint angles, and the scanned `soundMap`). See
[`PSX_DYNAMIC_RE.md`](PSX_DYNAMIC_RE.md).
