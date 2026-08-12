# The shipping build recipe (verified 2026-07-29)

## ★★★★★ 2026-08-12 — GUNS YOU CAN ACTUALLY DRAW: `demo28_p272`
```
tools/gbuild.sh demo28 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 \
                       VRESN=80 TRAPFLOOR=1 GYMSD=1 GUNS=1
```
**Lit pad: 272** (demo27 was 136 — same flags, so this is the layout re-roll,
not a setting).

    DRAW/HOLSTER  ★ OPTION      JUMP   A / keypad 2
    ACTION/FIRE   B / keypad 1  WALK   C / keypad 3
    ROLL  Y / 5                 LOOK   Z / 6

★ TR1 has **no separate fire button** — ACTION fires when the pistols are out.
**Verified on silicon: OPTION drew, ACTION fired, r20 wolf died (K00 → K01).**

**Why OPTION and not X / keypad 4.** User on a real pad: *"I can't draw the
guns."* X and keypad 4 both come from the standard bit layout, which this
project has never measured — GDPAD injects the final mask and never touches the
matrix, so both tested fine from the host and did nothing in the hand. OPTION
has been decoded since day one and in-game is only ever a MODIFIER (OPTION +
direction tunes HOPDIAL), so **OPTION with no direction held** is free on every
controller, 3-button included. X / keypad 4 still work if they work for you.

☠️☠️ **AND MOVING IT TO OPTION WAS NOT ENOUGH THE FIRST TIME.** `HOPDIAL` does
`pad &= ~PAD_OPTION` whenever OPTION is held, ~100 lines ABOVE the draw check —
so the bit was already gone and the counter stayed K00 while she was mauled.
The edge must be read BEFORE HOPDIAL. That is the project's own *"pad consumers
must run LAST"* rule from the other side: **a consumer that CLEARS a bit must
run after everyone who wants to READ it.**

★ `PADPROBE=1` prints the raw controller matrix word on screen — press each key
on a real pad and it says which bit that key asserts. That is the only way to
settle the bit-to-key table, and it is worth one minute with fingers on a pad.

## (previous) 2026-08-11 — EVERYTHING AT ONCE: `demo25_p0`
```
tools/gbuild.sh demo25 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 \
                       VRESN=80 TRAPFLOOR=1 GYMSD=1
```
**Lit pad: 0.** Boot video + wolf fur + medikit sprites + collapsing floors +
scripted cameras + 80-line + numbers off + native-240 loading screen, all in one
ROM. Verified on silicon: EIDOS → CORE → the Los Alamos intro → the caves.

★★★★★ **`GYMSD=1` IS WHAT MADE IT POSSIBLE.** 316,336 bytes of Lara's Home
(`gym_atlas/geom/sect/index/pal`) were linked into every CAVES ROM — a level
that can never be resident at the same time as the caves. Leaving it out:

| | before | after |
|---|---|---|
| margin to the guard | 208 B | **311,920 B** |
| ROM size | 1,653,100 | **1,341,468** |
| pads that build | **2 of 6** | **6 of 6** |

More pads that build is also a wider A10 lottery, which is free reliability.

☠️ Selecting Lara's Home in a GYMSD build is REFUSED (the pointers are NULL
stubs). The proper end state is streaming BOTH levels from the card into one
shared buffer — this is the first step, not the whole thing.


## ★★★★★ 2026-08-11 — THE SHIPPABLE DEMO: `demo24_p136`
```
tools/gbuild.sh demo24 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80
```
**Lit pad: 136.** ☠️ Only pads 0 and 136 build at all — the rest overflow the
stack-headroom guard. Verified on silicon: the whole front end runs (EIDOS →
CORE → the Los Alamos intro) and it drops into the caves.

Carries everything shippable:
- **`VRESN=80`** — 80 render lines through the OP's 3.0x scaler, full screen and
  full FOV. Crosses the 8-field rung in the enemy-free ship recipe.
- **numbers OFF** (`HUDTEXT` unset) — the 68k text readout was **+56%** of the
  frame all by itself.
- **medikit SPRITES** — the 7 medikits billboard the real TR1 sprite art;
  cheaper than the cube placeholder they replace.
- **scripted CAMERAS** — the level-start side view and the CAMERA_TARGET shots
  the level has always carried.
- **loading screen at native 240** — it had been decimated to `RENDER_H`.

☠️☠️ **THE ONE THING IT CANNOT ALSO CARRY: `ENEMYTEX` (the wolf fur).**
The demo recipe adds `BOOTVID` + the read-out panel, and with the skins on it is
**3,728 bytes OVER** the guard; without them it fits with **208 bytes**. So it
is a straight either/or today:

| build | boot video | wolf fur | where |
|---|---|---|---|
| `demo24_p136` | ✅ | ✗ | THE SHIPPABLE ONE |
| `build_play/play_p272.cof` | ✗ | ✅ | plain recipe, 352 B spare |

The wolves still render with their per-type tone in demo23 — they are not
untextured, just not furred. **Freeing the difference means the WOLF UV tables**
(3,704 B for 251 faces of which only 114 are textured; a sparse form roughly
halves it) — that is the next byte to hunt, and it is worth ~1.8KB of the 3,728.


## ☠️☠️ FIRST: CHECK THE ASSETS ARE THE SHIPPING SET, NOT THE EXTRACTOR DEFAULTS
`head -8 mrt.h` before you build anything. It must say **`MRT_ROOMCOUNT 38`**
and **`MRT_FACE_PLANES 1`**. If it says `5` and `0`, someone ran
`tools/tr2jag_multiroom.py` with no environment and the whole level has been
replaced by a 5-room, unscaled, no-ramp-palette stand-in — the .bin files are
gitignored, so **git will not tell you and `git status` will look clean.**
Found exactly that on 2026-08-01: a ROM had already been built on top of it.
Regenerate with the full recipe in `ASSET_BUILD.md` (all eight variables, and
`SUBDIV_MAX=6144`, not build_cof.sh's 3072). Correct output is
`mrt_atlas.bin` = **280576 bytes**, `mrt.h` = 38 rooms.



Reconstructed by rebuilding until the output was **byte-identical** to the last
known-good ROM, not by reading it out of campaign prose — that mistake has cost
this project three flashes and the whole LOWRES win before (see
`feedback_build_from_artifact_not_docs`).

```
make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
     PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
     ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 NOSOUND=1 \
     DIVZGUARD=1 MOVESET=1
```

Without `MOVESET=1` this reproduces `probes/PLAY_MOVESET2.cof` exactly
(md5 `b56b103aef657e29e1cbe02aa880b9f0`, 1482088 bytes) when `MOVESET=1` is
added — that identity is what pins the list down.


## ★★★★★ 2026-08-11 — THE RESOLUTION DIAL: `demo15_p272` (96) / `demo16_p272` (80)
```
tools/gbuild.sh demo15 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1 VRESN=96      # or VRESN=80
```
`demo14` plus **`VRESN=N`** — render N lines and let the OP scaler stretch them
over the same 240-line window.  Full screen, full field of view; you spend
vertical RESOLUTION, not screen area.  **Both lit on pad 272.**

☠️ **ONLY 120/96/80/64/60 EXIST.** The OP's `VSCALE` is 3.5 fixed point, so the
scale that fills the window (240/N) must be a multiple of 1/32 ⇔ `VSCALE =
7680/N` whole.  72 is NOT expressible.  The Makefile refuses anything else.

Measured on silicon, same pad per column, `PADMUTE=1 FASTBOOT=1`, beacon +
frame-change.  ★ the 120 control reproduced the recorded **6.67** exactly:

| lines | OP scale | ship recipe (no `ENEMIES`) | demo recipe (`ENEMIES=1`) |
|---|---|---|---|
| 120 | 2.0x | 6.67 | 4.10 |
| 96 | 2.5x | **7.15  (+7.2%)** | **4.28  (+4.4%)** |
| 80 | 3.0x | **7.47  (+12.0%)** | **4.35  (+6.1%)** |
| 60 | 4.0x | 7.92  (+18.7%) | 4.53  (+10.5%) |

★★★★★ **80 lines crosses the 8-FIELD RUNG** in the ship recipe (7.47 against a
7.50 theoretical) — the threshold the whole perf campaign has been chasing.
★★ **96 is near-indistinguishable from 120** on a TV and still pays.

☠️☠️ **ENEMIES HALVE THE LEVER** (+12.0% → +6.1% at 80).  Their per-face cost is
resolution-independent, so they dilute every pixel-side win — the wolf is 251
faces with no LOD.  **Quote the column that matches the build you are shipping.**

## ★★★★★ 2026-08-10 — PREVIOUS DEMO ROM: `demo14_p272`
```
tools/gbuild.sh demo14 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1
tools/vsweep_game.sh build_demo14 demo14 <outdir> 45
```
`demo12` plus the ANIMATION work on Jerry: the spinning pickups and the swinging
doors have their vertex transforms done by the `dsp_ovl_ent` overlay, not the
68000.  **Lit pad: 272.**  Verified on silicon by A/B against the 68k path from
an identical spawn - door-band mean abs diff 1.1-1.3 vs a 1.2 control, i.e.
equivalent within rounding (Jerry's cos/sin carry 2 fewer fraction bits).

## ★★★★★ 2026-08-09 — PREVIOUS: `demo12_p0`  (6.28 -> 6.60 fps)

```
tools/gbuild.sh demo12 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 \
                       BLOBCACHE=1 JCENT=1 JOVL=1
tools/vsweep_game.sh build_demo12 demo12 <outdir> 45
```
`demo11` plus three flags off the 68k-eviction campaign.  **Lit pad: 0.**

**+5.1% on silicon, measured properly**: 6.28 -> 6.60 fps on the SAME pad, and
confirmed by two independent instruments (`tools/beacon_fps_fast.py` 6.60 and
`tools/framechange_fps.py` 6.62, which never looks at the beacon).
- `BLOBCACHE=1` - the 68k was rebuilding every bridge/door/lever blob EVERY
  FRAME although none of them had moved.  Cache per draw slot, keyed on the
  entity plus whatever can actually change.
- `JCENT=1` - Jerry emits each Lara mesh's centroid SUMS during the pose pass
  it was already doing, so `lara_finish` stops re-walking ~500 posed verts.
  Sums are the same quantity the 68k computed => painter order unchanged.
- `JOVL=1` - Jerry code-overlay loader (CMD=3).  Costs 96 bytes; no overlay is
  loaded yet, so it is inert in this ROM.

☠️ `ENEMIES=1` IS NOT PLUMBED IN THE MAKEFILE - it expands to nothing.  It is
carried here only because `demo11` carried it; the bats are NOT in this build.

## ★★★★★ 2026-08-08 — THE PREVIOUS DEMO ROM WITH THE VIDEO FIX: `demo11_p272`

```
tools/gbuild.sh demo11 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1
tools/vsweep_game.sh build_demo11 demo11 <outdir> 40      # rolls pads until one lights
```
`tools/gbuild.sh` carries the recipe (ship list + BOOTVID + JVFASTKICK +
INLINEMUL/OFFHOIST/VPACK, AUTOSTART on).  **Lit pad: 272.**  ☠️ pad 816 (and
544/408 with some flag sets) OVERFLOWS the 16KB stack-headroom guard, because
PADTEXT pads TEXT and BSS sits above it - gbuild.sh skips those and says so, so
the A10 lottery is only 4-5 pads wide for the demo set.

### what changed in the video block (all silicon-verified)
1. The stream-buffer refill compacted ~26KB **byte by byte every frame** on the
   68000 - 478ms/frame of work outside every instrumented phase, and the whole
   reason the boot clips chugged at 1.94 fps.  Now vidrom's shape: refill under
   8192, and the move goes through the **Blitter** (`blit_bytes()`).
2. The audio chunk copy into the DSP ring goes through the Blitter too.
3. `video_pend_at` alone does not pace anything - not every compiled ISR path
   honours it, so frames went out as fast as they decoded (19.45 fps for a
   15fps clip).  The player now waits on `frame_count` itself.
4. The read-out panel is painted ONCE per clip on a hold screen: painted every
   frame it cost **329ms/frame** and dominated its own measurement.

Result: EIDOS 152/152 frames at 14.23 fps, CORE 193/193 at 14.88, tomfail 0.

## ★★★★★ THE CURRENT RECIPE (2026-08-02) — SOUND ON, USE THIS ONE
## ★★★★★ CURRENT — verified LIT on silicon 2026-08-10 (`PADTEXT=136`)

```
make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
     PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
     ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 \
     DIVZGUARD=1 MOVESET=1 SPANSHADE=1 SHADEEXCL=1 \
     TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1 \
     VCBIG=1 ENTITIES=1 SWANIM=1 DOORTEX=1 \
     BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 \
     INLINEMUL=1 OFFHOIST=1 VPACK=1 NOPCLIP=1 PADTEXT=136
```

★ **`NOPCLIP=1` added 2026-08-10 — a coverage-hole fix that is FREE.** The
per-room portal clip rect was carving hard axis-aligned holes: room 10 rendered
with **22% of the screen never drawn** (the whole near floor). Drawing
neighbour rooms unclipped cuts that to 1.5%, and it measures **6.67 fps, dead
level with the clipped build** — the clip was almost never saving work, because
only one room really draws anyway. No geometry leaks through walls (the
far-first painter order repaints over it).

Adds to the 2026-08-07 list: the 68k-eviction trio (`BLOBCACHE JCENT JOVL`),
the long-aligned sector mirror (`SECTLONG`), and the kernel stack
(`INLINEMUL OFFHOIST VPACK`). Measures **6.67 fps**.

☠️ **`PADTEXT` MOVED 544 -> 136.** Those flags changed the layout, which
re-rolls the A10 lottery. 0 and 272 both boot BLACK for this exact layout;
136 lights. Change any flag and walk the rolls again — build 0/136/272/408/544
and roll until one lights (`scratchpad/walk.sh` does this unattended).

### Measurement arms only — NEVER ship these
`FPSBEACON=1` (fps block), `PADMUTE=1` (zeroes the in-game pad so both arms
render the same scene), `FASTBOOT=1` (skip logos, in-game in ~8s),
`SYNCDRAIN=1` (measured: no effect, leave off). Each has its OWN A10 roll —
the pad-muted pair lit at 0 and 408, not 136.

---

The list below is the July build and is now **superseded**. This is what the
user has actually been playing:

```
make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
     PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
     ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 \
     DIVZGUARD=1 MOVESET=1 SPANSHADE=1 SHADEEXCL=1 \
     TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 AUTOSTART=1 \
     VCBIG=1 ENTITIES=1 SWANIM=1 DOORTEX=1 PADTEXT=544

# ★★★ 2026-08-07: VIDEO v9 SHIPPED — delta streams + SHADOW DECODE.
# Player: tokens apply to one private DRAM frame, display gets whole-frame
# copies (no fb-history dependence — the ghost-proof architecture). The Tom
# jvdec kernel NEVER ran on silicon (probes under VIDDIAG); the 68k IS the
# video decoder and always was. SD carries DELTA CORE.JV/INTRO.JV/CAVES.JV
# (2.9x smaller streams); EIDOS.JV stays keyframe-only (no disc source).
# Demo flag set now: ship recipe + ENEMIES=1 BOOTVID=1 VIDPROF=1
#   INLINEMUL=1 OFFHOIST=1 VPACK=1 DBGROOM=1 (drop DBGROOM for release).
# ship_v9 lit on PADTEXT=544.
# ★★ 2026-08-06: add INLINEMUL=1 to all future builds — inlines the 10
# imul32 calls/vertex as exact s16 imults. +5.7% fps ON SILICON (9.30→9.83
# median at the caves spawn, FPSBEACON A/B, ranges non-overlapping), output
# bit-identical (vtxcache diff, 38 rooms × 5 yaws), kernel −114 B. The A/B
# pair (ship recipe + FPSBEACON ± INLINEMUL) lit on PADTEXT=272; adding
# INLINEMUL to THIS demo recipe re-rolls A10 as usual — build all rolls.
# ★ 2026-08-03: added SWANIM=1 (Lara's switch-pull animation) and DOORTEX=1
# (real carved-stone door texture). DOORTEX needs the atlas patched ONCE first:
#   MRT_DOORPATCH=1 TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=1600 \
#     FACE_PLANES=1 RAMP_PAL=1 STATICS=1 TRLEVEL=<PSXDATA>/LEVEL1.PSX \
#     TRPREFIX=mrt python3 tools/tr2jag_multiroom.py
# -> appends door/lever textures to mrt_atlas.bin (280576->299008) + writes
#    mrt_door.h, touching NOTHING else. NEVER run a full (non-DOORPATCH) regen:
#    it builds a broken 4506-byte Lara (see feedback_lara_asset_corruption).
# ☠️ PADTEXT re-rolled to 544 for this feature set (272 boots BLACK now).
```

★ **2026-08-02 (evening), `ENTITIES=1` build** — verified booting on silicon
from the GameDrive (`ent_p272.bin`, 1655296 B): capture 54.88% non-black, Lara
in the caves, flat-colour fix intact.  **`PADTEXT=544` BOOTS BLACK for this
layout; 272 boots.**  `VCBIG=1` is the room-26 vertex-cache overflow fix and
`ENTITIES=1` is real doors + switches.

☠️ **`PADTEXT` was 544 until 2026-08-02**, when the full-moveset work added
~2.2 KB and re-rolled A10 (544 went black). **272 is verified booting on
silicon** — captured, 49-65% non-black with real frame-to-frame change, not
just "the upload said OK".
★ When a layout change re-rolls it, build **all four rolls at once**
(272 / 816 / 0 / omitted) before touching the board: each miss costs a PHYSICAL
bounce from the user, and having the next candidate already built means a
bounce is all it costs. Verify each landed with
`m68k-neogeo-elf-nm -S build/main.o | grep g_padtext` — and `rm -rf build`
between them, because make tracks timestamps, not flags.

Differences from the July list, and why each is there:
| flag | why |
|---|---|
| **no `NOSOUND`** | sound is ON — the first of the four done-criteria, closed 2026-08-02 |
| `SPANSHADE=1 SHADEEXCL=1` | the A2 fix: grey/transparent floor slabs. **Omitting these brings the triangles straight back** |
| `TIMESTEP=1 ANIMRATE=1` | movement decoupled from the render rate; cadence from the anim record |
| `OPDBL=1` | double-buffers the OP list — the GameDrive ghosting fix |
| `LPLANES=1` | Lara per-mesh plane culling (needs `FACE_PLANES=1` assets) |
| `AUTOSTART=1` | drops straight into the caves, so visual checks need nobody at the pad |
| `PADTEXT=544` | the A10 roll that boots for THIS layout |

Turn rates are the **source defaults** (`TURN_RUN_TR1 3` / `TURN_WALK_TR1 2`,
main.c ~440) — no `TURNRUN=`/`TURNWALK=` on the command line.

☠️☠️ This list was RECONSTRUCTED on 2026-08-02 by diffing a clean rebuild's
objects against `build/` from the previous ROM, because it had only ever
existed in a shell history. That cost ~20 minutes. The tells, if it ever has to
be done again: `nm -S build/main.o | grep padtext` gives PADTEXT directly;
`dsp_blob` .rodata `0x9d8` = sound ON (`0x600` = NOSOUND); `olp_sw`/`op_cur` in
video.o = OPDBL; `mrt_lplane_*` in main.o = LPLANES; `_as_ctr` = AUTOSTART.
**A correct recipe rebuilds every object byte-identical except the ones the
change actually touches.**

## How the last two flags were found
The first guess was 992 bytes too large. Diffing the linker maps against the
known-good ROM's map isolated it to two objects: `dsp_blob` (−984 B ⇒ `NOSOUND`)
and `gpu_blob` (+8 B ⇒ `DIVZGUARD`). A single remaining byte then differed, at
`g_hopcap` in `.data` — that is `HOPBOOT`, whose value is baked in, and it was
`1`, not the Makefile default of `4`.

**`NOSOUND=1` is in this list.** The recent probe ROMs are SILENT. That is right
for measurement and wrong for a demo — drop it when building for the user, and
re-verify, because it moves ~1 KB of DSP code and re-rolls the boot lottery
below.

## ☠️ THE OFFLINE LOOP IS DOWN: jagemu FREEZES ON THE TITLE SCREEN (2026-08-01)
`jagemu` no longer reaches the level, so "reproduce it offline before you flash"
is currently unavailable and every visual change costs a real ~190 s flash.

What was measured, so nobody re-derives it:
- The title screen paints at ~frame 1440 and then **never changes again** —
  screenshots at frames 2000 / 2100 / 2200 are **byte-identical**.
- `AUTOSTART` is therefore innocent. Its `break` sits directly inside the title
  `for(;;)` in `main.c`, but the loop never completes another iteration. The
  flag does land: `-DAUTOSTART` is on the `main.c` compile line.
- The 68k is alive but idle — ~306 instructions per frame — parked at
  **`0x420C`, one instruction past the `stop #$2000` inside `cpu_stop_unless`**
  (`cpu68k.S`). It is waiting on `pending_fb`, which only the vblank ISR clears.
- **It is not a regression in our recent work.** `probes/PLAY_MOVESET2.cof` and
  `probes/PLAY_FLIPASM.cof` (27–28 July) freeze to the *same* image. The
  emulator does load each ROM correctly — `jagemu info` reports their true,
  differing sizes — so this is not a stale-instrument artifact.

★ This is the same shape as **A10** (`[[a10-codegen-cliff]]`: the vertical
interrupt never fires). Worth noting that the `IRQREARM` protection in
`video_flip_asm` **counts WAKES** — re-arm at 8, retire the flip inline at 24 —
so it only engages if *something* still wakes the CPU. Against a totally dead
interrupt the `stop` never returns and the bound never runs. That is a real gap
in the A10 mitigation, whether or not it is what jagemu is hitting.

## Flashing
`jcp` will NOT flash over a running ROM: the upload runs to ~99% and then fails
with `can't connect with skunkboard`. **Reset first** — `jcp -r`, wait ~3 s,
then flash. That replaces the "one flash, then a physical bounce" rule in the
older notes; no power cycle is needed for the normal case. A flash is ~190 s;
never put a short timeout on it. If `jcp -s` itself starts failing, the board
has wedged and DOES need a physical power cycle.

## ☠️☠️ CLEAN THE BUILD DIR BEFORE ANY A/B — THE MAKEFILE IGNORES FLAG CHANGES
`make` tracks file timestamps, **not the flag set**, so objects compiled under a
previous arm's flags are silently relinked. That is not theoretical: it produced
a ROM 1104 bytes short of the correct one and a **black screen on silicon**
(2026-08-01), which I nearly misread as an A10 re-roll. `rm -rf build` first, or
the comparison is meaningless. It is also why the byte-identity check below is
the only trustworthy proof.

## FULL RESOLUTION (2026-08-01) — REVERTED, see commit 52666ae
☠️☠️ **REVERTED IN CODE 2026-08-01 (52666ae).** Retracting it in prose was not
enough: the campaign's edits to `video.c` / `video.h` / `startup.S` broke the
**120-line shipping path too** — a plain `LOWRES=1` build put the picture in
half the vertical window (aspect **2.10**, want 1.33). Those three files are
back at `df00e96`, and a clean HEAD build is byte-identical (md5
`b9cee06777ff0921214b2026697dc9c6`) to the ROM verified on silicon at aspect
1.29. `VRES240=1` is now a hard `$(error)`.

☠️ RETRACTED: 240 lines builds and boots, but IN-GAME only Lara renders and
the room is black. I validated it on the TITLE screen, which uses a different
(always-240) path. The 120-line recipe above is still the shipping one.
The kernel (gpu_geotex.gas, GEOMDIRECT) still carries 120-line projection/clip
constants — FOCAL_Y is switched in main.c only. That is the fix.

```
make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
     PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
     ROWDIET=1 STATICS=1 BANKDIET=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 \
     LPLANES=1 VCBIG=1 SPANSHADE=1 SHADEEXCL=1 TINYKEEP=4 FARCLIP=30000
```
**No `LOWRES`** (that is the change) and **no `PADTEXT`** — this exact set boots.
`LPLANES=1` is REQUIRED for Lara: without it she has no exact backface cull.

**Cost of full resolution: 4.4%** — matched arms, same scene, AUTOSTART+AUTOSPIN,
identical detector: 120 lines 9.64 fps vs 240 lines 9.22 fps. The renderer is
face-bound, so pixels are nearly free.

☠️ A10 still re-rolls on every layout change: SHIP240 (PADTEXT=272) and
SHIP240B (816) were BLACK, SHIP240C (no PADTEXT) boots. Same source. If a build
is black, roll `PADTEXT` before changing anything real.
