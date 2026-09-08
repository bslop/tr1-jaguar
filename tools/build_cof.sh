#!/usr/bin/env bash
# build_cof.sh <disc> <outdir>
#
# The whole pipeline in one shot: extract a user's Tomb Raider PSX disc, convert
# every asset to Jaguar form, and build the GameDrive payload. Same script the
# Docker image runs as its entrypoint, so a local run and a container run are
# identical.  Reproducible: given the same disc it produces the same OPENLARA.COF.
#
#   tools/build_cof.sh  /path/to/TombRaider.(iso|bin|cue|chd|7z)  ./out
#
set -euo pipefail

# ☠️ RUN LOCK. tools/session_run.sh refuses to commit while this runs, because
# this script REWRITES TRACKED ASSETS (CORE.JV, EIDOS.JV, sound.h, pass2.h, the
# mrt_* headers) and a blind `git add -u` mid-run commits a half-built tree.
# It is an explicit PID file rather than `pgrep -f build_cof.sh`, because that
# string ALSO matches any shell waiting on this script - and matches the very
# command doing the check. The first version of the guard blocked a clean commit
# by detecting its own watcher, then a pkill of those watchers killed the shell
# issuing it. ★ Never gate anything on a process list you are yourself in.
BUILD_COF_LOCK=/tmp/.build_cof.lock
echo $$ > "$BUILD_COF_LOCK"
trap 'rm -f "$BUILD_COF_LOCK"' EXIT

# TOOLCHAIN (2026-08-26): the SHARED cobweb checkout, no pin. The 08-16 pin to
# 59e5896 (/tmp/cobweb-old) was for a "jcc68k ABI regression": ROMs built with
# newer cobweb ran at full speed and rendered 100% BLACK. That was never an ABI
# change - newer jcc68k began emitting direct memory-to-memory `move.l sym,sym2`
# and JAS DROPPED THE DESTINATION RELOCATION (cobweb ba9c680), so every
# global-to-global copy stored into the 68000 exception vectors and the OP-list
# shadow words stayed zero. jas is fixed at 9da2f99+ (also section-relative
# .align, the A10 lottery). Every ROM measured on silicon on 2026-08-26 was built
# with the shared checkout at 9da2f99. COBWEB_DIR still overrides if you must.
# The container pins COBWEB_REV in the Dockerfile; keep it >= 9da2f99.
if [ -n "${COBWEB_DIR:-}" ]; then
    echo "   toolchain: COBWEB_DIR=$COBWEB_DIR (override)"
fi

DISC="${1:?usage: build_cof.sh <disc> <outdir>}"
OUT="${2:?usage: build_cof.sh <disc> <outdir>}"

HERE="$(cd "$(dirname "$0")/.." && pwd)"     # the jaguar/ tree
cd "$HERE"
RMAC="${RMAC:-$HOME/jaguar-tools/bin/rmac}"
# LOWRES (2026-07-25): render 320x120 and let the OP's hardware vertical
# scaler display it at 240, instead of HALFRES's Blitter line-double.  That
# deletes 2 Blitter passes / 76800 B per frame.  MATCHED silicon A/B, same
# session and route: 6.00 -> 7.50 fps (+25%), frames whole.  The path was
# unusable until the flip grew a Blitter COMPLETION barrier (video.c) — see
# the LOWRES BARRIER notes; do not switch back without reading them.
# ── QUALITY: pretty or playable ──────────────────────────────────────────────
# ONE knob, two values, because they are mutually exclusive - you cannot ship
# both resolutions in one ROM, and two independent PRETTY=/PLAYABLE= flags
# would need an "both set" rule nobody would remember.
#
#   QUALITY=pretty     (default)  120 render lines. Full vertical resolution.
#   QUALITY=playable              80 render lines through the OP 3.0x scaler.
#                                 Same full screen and field of view, coarser
#                                 vertically, but it holds the rock detail that
#                                 60 lines smears into bands - chosen at the
#                                 pad after seeing 120/96/80/60 on a TV.
#
# They differ ONLY in VRESN - identical feature set, identical assets - so a
# bug in one is a bug in both.  Measured on silicon 2026-08-11 with the demo
# feature set (see PLAY_BUILD.md for the full ladder and both columns):
#
#      120 lines  6.40 fps        80 lines  7.15 fps  (+11.7%)
#
# RES=<N>: pick the render-line count explicitly (OVERRIDES QUALITY). The OP's
# vertical scaler fixes the rungs (240/N must be a 1/32 multiple), so the ladder
# is exactly: 120 (full LOWRES, sharpest+slowest) / 96 / 80 / 64 / 60 (coarsest+
# fastest). Higher = better quality, lower = higher fps.
#   local:  RES=120 tools/build_cof.sh <disc> <out>
#   docker: docker run ... -e RES=96 tr-jaguar
# With no RES, QUALITY picks it: pretty=120, playable=60.
QUALITY="${QUALITY:-pretty}"
if [ -n "${RES:-}" ]; then
    case "$RES" in
        120)         QUALITY_FLAGS="" ;;               # 120 = full LOWRES, no VRESN
        96|80|64|60) QUALITY_FLAGS="VRESN=$RES" ;;
        *) echo "error: RES must be one of 120 96 80 64 60 (the hardware OP-scale ladder)" >&2
           exit 2 ;;
    esac
    echo "   resolution: ${RES} render lines (RES override)"
else
    case "$QUALITY" in
        pretty)   QUALITY_FLAGS="" ;;
        # 2026-08-27: playable is now 60 lines (OP 4.0x scale) - the fastest rung.
        playable) QUALITY_FLAGS="VRESN=60" ;;
        *) echo "error: QUALITY must be 'pretty' or 'playable' (got '$QUALITY')" >&2
           exit 2 ;;
    esac
fi

# HRES=<N>: the HORIZONTAL dial (2026-09-05).  Render only the leftmost N
# columns and widen the VMODE pixel clock (PWIDTH) so they fill the active
# line.  Composes with RES/QUALITY -- they pick the LINE COUNT, this picks the
# COLUMN COUNT.
#   docker: ... -e RES=120 -e HRES=160     -> 160x120, the measured ship config
# ☠ NOT the OP horizontal scaler, which starves the bus ~15-20x and blacks the
# display.  PWIDTH is the opposite and is a bus WIN.  [HW] jag_quake ships
# VMODE=$0EC7.  Measured on silicon 2026-09-05 (jobs #2538/#2539):
#   320x120  6.450 fps (9.30 fields)   ->   160x120  7.500 fps (8.00 fields), +16.3%
# ★ 160x120 lands on EXACTLY 8.00 fields/frame, so it is repeatable across rig
# turns rather than the bimodal coin flip a field-straddling build gives.
# ☠ The ladder is closed: the Blitter width field encodes only 2^e x (4+m)/4,
# so 160 and 80 are expressible and e.g. 72 is NOT -- and it fails SILENTLY.
if [ -n "${HRES:-}" ]; then
    case "$HRES" in
        320)     : ;;                                  # full width, no flag
        160|80)  QUALITY_FLAGS="$QUALITY_FLAGS HRESN=$HRES" ;;
        *) echo "error: HRES must be one of 320 160 80 (the Blitter width field encodes" >&2
           echo "       only 2^e x (4+m)/4; anything else fails SILENTLY on hardware)" >&2
           exit 2 ;;
    esac
    echo "   width: ${HRES} render columns (PWIDTH-stretched to the full line)"
fi

# ☠️ THE SHIPPING FLAG SET.  This used to read
#   MULTIROOM=1 LOWRES=1 CFLAGS_EXTRA=-DJERRYPOSE
# which had drifted years behind the game: no entities, no doors, no enemies,
# no kernel stack, no sound.  A container build produced something that was not
# the game anyone was playing.  Keep this in step with PLAY_BUILD.md's CURRENT
# recipe - that file is the authority and records why each flag is here.
# ☠️ PADTEXT is deliberately ABSENT: it is an A10 boot-lottery roll tied to one
# exact code layout, and the layout a container produces is not this tree's.
# If a container ROM boots black, roll PADTEXT (see project_a10_reproducible).
# ☠️ AUTOSTART is deliberately ABSENT: with it the ROM drops straight into the
# caves, and this build is meant to come up on the TITLE RING like the real
# game. There is no MENU flag - "menu" IS the absence of AUTOSTART.
# ☠️ `make FOO=0` still DEFINES FOO in this Makefile, so a feature is turned off
# by OMITTING it, never by setting it to 0.
BUILD_FLAGS="${BUILD_FLAGS:-MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 \
STAGEDIET=1 PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 DIVZGUARD=1 MOVESET=1 \
SPANSHADE=1 SHADEEXCL=1 TIMESTEP=1 ANIMRATE=1 OPDBL=1 LPLANES=1 \
VCBIG=1 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 BLOBCACHE=1 \
JCENT=1 JOVL=1 SECTLONG=1 INLINEMUL=1 OFFHOIST=1 VPACK=1 NOPCLIP=1 \
TRAPFLOOR=1 GUNS=1 BOOTVID=1 JVFASTKICK=1 PROBE_AHEAD=256 RCLIPFIX=1 \
FITSTEP=1 M68A2=1 VCDRAIN=1 ENTLISTS=1 DARTS=1}"

# ENTLISTS=1 (2026-08-27): the per-frame entity draw loops (doors, switches,
# bridges, pickups, bats) each rescanned all 60 entities - 13 scans/frame, each
# a DRAM read of mrt_ent[].type that contends with Tom's render under PIPELINE.
# Entity type is static, so type-indexed lists are built once at level select.
# Pixel-identical (room 20 w/ wolf: 0 px diff), byte-identical with the flag
# off, +2.5% on silicon (7.90 -> 8.10). A 68k-bus-contention lever in the
# M68A2 / HUDTEXT family - invisible offline, sub-rung alone, stacks."

# VCDRAIN=1 (2026-08-26): A1 - the still-camera flicker - was TWO mechanisms,
# and this kills the first: Tom's face loop read back vertex-cache words the
# pre-pass had just STORED (jsim store->load detector: 118-cycle minimum gap)
# and silicon returned the STALE word under bus traffic, so one vertex was
# wrong and a whole face toggled frame to frame - only in rooms that dispatch
# more rooms than Jerry's 8-entry cache, where Tom transforms the rest itself.
# An ~8k-cycle drain after the self-transform pre-pass: 38-room tour, rooms
# paired by content, racing px 9797 -> 6219 (-37%), the worst whole-face room
# 2698 -> 12, fps median 6.83 -> 6.75. The floor SPECKLE in PSX room 32 is the
# second mechanism and is untouched (4278 -> 4311) - still open.

# M68A2=1 (2026-08-26): the 68k's room painter sort was an O(n^2) SELECTION
# SORT over all 38 rooms recomputing TWO Manhattan distances (4 abs) per inner
# iteration from DRAM stack tables - 31.8% of the 68k's main-line cycles,
# ~20 ms/frame.  jagemu said it costs NOTHING (frames 136 -> 136: PIPELINE
# overlaps it with Tom).  SILICON says +13.9%: 7.000 -> 7.975 fps, beacon and
# frame-change agreeing, because that DRAM traffic runs WHILE Tom renders and
# the bus is what Tom is starved of (the HUDTEXT lesson, fourth time).  M68A2
# sorts only the admit loop's own candidates with the distance hoisted; same
# comparator, same scan order => order-identical (0 px diff @420, 12 px @700
# = animation phase).  A July PERFHUNT arm that read flat under the old stack.

# FITSTEP=1: TR1's headroom rule on the automatic <=256 step-up, so Lara stops
# walking up into gaps she does not fit in (Lara's Home had 105 of 304 step-up
# pairs shorter than she is). Only evaluated on a step UP - level ground and
# drops short-circuit past it - so it costs nothing on a flat frame. Measured
# on both levels: ledges 24/24 unchanged, walls 6/6 refused vs 4/6 and 2/6.

# RCLIPFIX=1: paint the LAST PIXEL COLUMN. Spans are right-EXCLUSIVE
# (SHADEEXCL) but xr was clamped to CLIPX1, which gpu_geotex_setclip sets
# INCLUSIVE (0,319) - so the rightmost span ended at 318 and column 319 was
# black in EVERY scene (measured: caves, mansion, and a spawn arm each showed
# exactly one dead column). Both the texture and shade clamps take the +1;
# fixing only one would leave 319 textured-but-unshaded, which is the "bright
# 1px line down the right edge" this kernel already paid for once.
# Measured free: rendered frames 232/398 identical to baseline, illegal=0.

# ☠️☠️ GYMSD IS DELIBERATELY *NOT* IN THAT LIST ANY MORE (2026-08-16).
# GYMSD left Lara's Home out of the image, and the ring menu then REFUSES the
# item on purpose (its gym_* pointers are NULL stubs - selecting it would drive
# the renderer through a null geom pointer). So with GYMSD the mansion is not
# merely absent, it is UNREACHABLE - which is what "Lara's house is broken"
# looked like from the title screen.
# It now fits, measured: aliasing gym_lskin onto mrt_lskin recovered 107,040 B,
# TEXSCALE=4 + RAMP_PAL + no STATICS puts the mansion at 254,296 B of payload,
# and the result builds 6 OF 6 PADS at 1,554,268 B - no A10 pad options lost.
# Verified through the REAL menu-exit path (AUTOGYM=1): title ring -> Lara's
# Home renders, illegal=0, vector 64 intact.
# ☠️ If you put GYMSD back, also expect the menu to refuse the item again.

# ☠️ PROBE_AHEAD IS NOT OPTIONAL. The default is WALK_SPEED*2 = 94 units against
# a 1024-unit sector, and collision holds Lara ~100 units off a wall - so the
# forward probe never leaves the cell she stands in, finds her own floor, and
# EVERY climb refuses (standing climb, running vault and the airborne grab all
# feed off that one probe). Verified on silicon 2026-08-16: at 94 the on-screen
# verdict reads "flat ground ahead" while she faces an obvious ledge; at 256 a
# 512 and a 768 ledge both read VAULT and she climbs. Leaving this out of the
# container would have shipped a demo where nothing can be climbed.

say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

# ── 1. disc -> a private extraction dir (never touches the repo's assets) ─────
ASSETS="$(mktemp -d)"; trap 'rm -rf "$ASSETS"' EXIT
say "Reading disc: $DISC"
WANT_FMV="${WANT_FMV:-CORELOGO.FMV,CAFE.FMV,SNOW.FMV,INTRO.STR}" \
    python3 tools/extract_disc.py "$DISC" "$ASSETS"
PSX="$ASSETS/PSXDATA"

# ── 2. convert every asset (order matters: gen_titlebg writes title_pal.bin,
#       which tr2jag_title.py then maps the passport/photo textures onto) ──────
# ☠️☠️ THE FULL RECIPE, NOT THE FIRST FOUR FLAGS. ASSET_BUILD.md spells this
# out and this script had only ever passed TEXSCALE/MRT_ROOMS/SUBDIV_MAX/
# LARA_MINAREA, which is why container ROMs STREAKED: without RAMP_PAL the
# atlas is baked with four shade levels per tile (481280 B) instead of one
# full-bright copy (280576 B) that the runtime shade pass darkens - the kernel
# samples a layout that is not there. The others matter just as much:
#   FACE_PLANES=1  the 12-byte plane prefix STAGEDIET backface-culls from
#   STATICS=1      stalactites/plants baked into the rooms (kernel: STATICS=1)
#   LARA_WINDFIX=0 the extractor DEFAULTS to 1, which reverses ~190 of Lara's
#                  faces and renders her SEE-THROUGH
#   SUBDIV_MAX=6144 at 3072 it still splits ~104 faces; 6144 is byte-identical
#                  to the shipped mrt_geom.bin
MRTENV="TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 \
FACE_PLANES=1 RAMP_PAL=1 STATICS=1 LARA_WINDFIX=0"
# Same, minus STATICS - Lara's Home only fits without it (see the mansion
# extraction below). Built by OMITTING the flag, never STATICS=0: the extractor
# reads these as presence flags and a "=0" would read as set.
MRTENV_NOSTATICS="TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 \
FACE_PLANES=1 RAMP_PAL=1 LARA_WINDFIX=0"

say "Extracting levels + Lara (Caves)"
env $MRTENV TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py

# ☠️☠️☠️ COLLISION PATCH. mrt_sect.bin is baked by the extractor and is WRONG
# until this runs: TR1's GetHeight DESCENDS into the room below at a seam and
# uses its floor, while the bake stores the seam height as solid ground - so
# Lara runs off a ledge and keeps running on air - and some floor==-127
# doorcells come out classified as WALL, which SEALS OFF parts of the level.
# 403 such sectors in the Caves. The file is gitignored, so it must be
# re-patched after EVERY regeneration; the container regenerates it every run
# and never did this (user 2026-08-15: "an issue with both wall boundaries as
# well as being able to access the area up above for the rest of the level").
say "Patching collision boundaries (phantom seam floors + doorcells)"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/mrt_boundary_audit.py --patch 2>&1 \
    | tail -4 || echo "   note: boundary patch failed"

# Atlas PATCH passes. Each re-reads the shipping palette+atlas, APPENDS its rows
# and exits, touching nothing else - so they must run after the base extraction
# and before the build. Without them the doors, medikits, pistols and wolf fur
# sample rows that do not exist.
# ☠️☠️ SECOND COLLISION PASS - MUST RUN AFTER THE BOUNDARY PATCH, NEVER BEFORE.
# The boundary patch fixes SEAM floors. This one fixes BORDER-RING floors: every
# room's mesh spans local 1024..(n-1)*1024, because in TR1 the outer ring of the
# sector grid is the wall border - but 350 of those cells carried a real floor
# height, so collision let Lara stand and CLIMB onto squares the renderer has no
# geometry for. That is the "I fall into some blackness then the area" report:
# driven repro showed her standing on room 11 cell (11,4) with the screen 81%
# black and only Lara lit. 350 of 2424 walkable cells, 14.4%.
# Order matters: this pass skips 0x7FFE OPENING cells, which the boundary patch
# creates - a doorway on the border ring IS legitimately walkable because the
# neighbouring room supplies its floor. Running this first would wall real
# doorways and seal the level.
# Verified: size unchanged (24720), scan drops 350 -> 0, Lara still walks freely
# from the level start, rendered frames 232/398 identical to baseline.
say "Patching collision coverage (standable cells with no mesh)"
# --faces included: it is now VERIFIED on both levels. It was wrongly blamed in
# run 33 for a black ROM that was actually the cobweb jcc68k regression; with a
# good toolchain it renders fine on Caves (1.1% black) and mansion (3.1%) and it
# FIXES confirmed bugs - Caves room 22's 60.4%-black standing spot, and four
# mansion spots where Lara fell instead of climbing (room 8 held 76 of its 84).
python3 tools/floor_coverage.py --faces --patch 2>&1 | tail -2 \
    || echo "   note: floor_coverage patch failed"
# ☠️☠️ THE MANSION'S THREE COLLISION PASSES USED TO LIVE HERE AND COULD
# NEVER HAVE WORKED. They ran at this point in the script, and Lara's Home is
# not extracted until the `case` further down -- so every one of them opened
# `disc/gym*.bin` before anything had written it, printed a FileNotFoundError
# and a "note: ... failed", and the build carried on and shipped the mansion
# with UNPATCHED collision. Even had the files existed from a previous run, the
# extraction below would have overwritten the patches minutes later.
# They now run immediately AFTER that extraction; see "Lara's Home collision".

# ☠️☠️☠️ PORTAL OPENINGS - RUN LAST, AND NEVER SKIP IT.
# Without this the CAVES ARE NOT WALKABLE PAST ROOM 0: 25 of 38 rooms had ZERO
# 0x7FFE cells and Lara physically stopped at the room 0/1 seam (measured run 71:
# she sat at z=21430 for 11 samples while g_floorroom already read 1; with the
# patch she crosses at z=21712 and g_curroom flips 0 -> 1).
# The extractor only opens a cell whose SECTOR carries an FD portal command and
# never consults the room's PORTAL LIST, so an axis-aligned doorway - a plane
# lying exactly on a cell boundary - stays solid on BOTH sides.
# Runs LAST because it only ever turns 0x7FFF into 0x7FFE: it must see the walls
# the boundary and coverage passes leave behind, and it can never overwrite a
# floor either of them wrote. Idempotent (a second run opens 0 cells).
# ★ project_room_crossing_fixed recorded this fixed on 2026-07-30 and it came
# back, because *_sect.bin is GITIGNORED and regenerated - an asset fix that is
# not in this script does not exist.
say "Opening portal cells (room-to-room walking)"
# ☠️ gym is NOT in this loop: its data does not exist yet at this point in the
# script. Lara's Home gets the identical pass after its extraction, below.
python3 tools/portal_open.py --prefix mrt --patch 2>&1 | tail -2 \
    || echo "   note: portal_open mrt failed"

say "Atlas patches (doors, pickups, pistols, enemy skins)"
for patch in MRT_DOORPATCH MRT_PICKPATCH MRT_GUNPATCH; do
    env $MRTENV $patch=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
        python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: $patch pass failed"
    echo "   $patch -> atlas $(stat -c%s disc/mrt_atlas.bin) B"
done
env $MRTENV MRT_ENEMYTEX=1 MRT_ENEMYTEX_STEP=2 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
    python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: MRT_ENEMYTEX pass failed"
echo "   MRT_ENEMYTEX -> atlas $(stat -c%s disc/mrt_atlas.bin) B"
env $MRTENV MRT_GUNONLY=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt \
    python3 tools/tr2jag_multiroom.py >/dev/null || echo "   note: MRT_GUNONLY pass failed"
# The pistol ARM animations (LARA_PISTOLS' arm joints). ☠️ Disc-derived, so it
# is generated here rather than committed - this repo ships no game data.
say "Pistol arm animations"
env $MRTENV MRT_GUNANIM=1 TRLEVEL="$PSX/LEVEL1.PSX" TRPREFIX=mrt python3 tools/tr2jag_multiroom.py

# Lara's Home is only extracted when the ROM actually carries it. GYMSD leaves
# it out (mrt_data.S stubs the gym_* incbins), so extracting it is pure cost -
# and under the full RAMP_PAL recipe the mansion's palette overflows 256 entries
# and the extractor dies, taking the whole build with it. That is a real
# extractor bug, but it is in a level this ROM does not ship.
# ☠️ main.c includes gym_spawn.h unconditionally, so the mansion must be
# extracted even under GYMSD - the ROM does not LINK its data, but the build
# still needs its headers. Under GYMSD we drop RAMP_PAL for this level only:
# with it the mansion's palette overflows 256 entries and the extractor dies
# (a real bug, in a level this ROM does not ship), and since none of its atlas
# is linked, how it was shaded cannot matter.
case "$BUILD_FLAGS" in
  *GYMSD*)
     say "Lara's Home (headers only - GYMSD leaves its data out of the ROM)"
     # minimal flags on purpose: RAMP_PAL and STATICS each push the mansion's
     # palette past 256 entries and the extractor raises IndexError. This pass
     # exists only to emit gym_spawn.h/gym_lara.h, and none of its data is
     # linked under GYMSD, so the shading flags are irrelevant here.
     env TEXSCALE=2 MRT_ROOMS=64 SUBDIV_MAX=6144 LARA_MINAREA=0 LARA_WINDFIX=0 \
         TRLEVEL="$PSX/GYM.PSX" TRPREFIX=gym python3 tools/tr2jag_multiroom.py ;;
  *) say "Extracting Lara's Home (Mansion) - RAMP_PAL, TEXSCALE=4, no STATICS"
     # ☠️☠️ THE MANSION ONLY FITS WITH THIS EXACT COMBINATION. Measured:
     #   TEXSCALE=2                    atlas 188,416 - does not link
     #   +STATICS                      geom 205,968 + atlas 134,144
     #                                 => __bss_end 2,091,952 > 0x1FC000 and
     #                                    gbuild.sh SKIPS ALL SIX PADS
     #   RAMP_PAL, TEXSCALE=4, no STATICS   geom 153,896 + atlas 78,848
     #                                 => 6 of 6 pads, ROM 1,554,268 B
     # ★ RAMP_PAL is REQUIRED, not optional: without it the mansion renders
     #   nearly black. Its old "the extractor dies on the palette" note is
     #   STALE - fixed by the 8-slot flat band.
     # ★ STATICS is what has to go: it bakes the furniture into the rooms and
     #   costs 107,368 B across geom+atlas. The interior still reads correctly
     #   without it at 320x80.
     # ☠️ A plain `make` does NOT enforce the stack-headroom guard - only
     #   gbuild.sh does. "It linked" from a bare make is not evidence it fits.
     env $MRTENV_NOSTATICS TEXSCALE=4 TRLEVEL="$PSX/GYM.PSX" TRPREFIX=gym \
         python3 tools/tr2jag_multiroom.py ;;
esac

# ☠️☠️☠️ LARA'S HOME COLLISION - AND IT HAS TO BE HERE, AFTER THE EXTRACTION.
# These three passes used to sit ~250 lines earlier, next to the Caves ones, and
# they could never have done anything: the gym data does not exist until the
# `case` above has run. Each printed a FileNotFoundError and a "note: ... failed"
# and the build shipped the mansion with raw extractor collision -- which is
# exactly the symptom the Caves comment above describes, "NOT WALKABLE PAST
# ROOM 0". User, 2026-09-07, playing demo34: "I can't seem to get to the
# obstacle course or the pool."
# ★ Same lesson as the portal note above, one level up: an asset fix that is not
# in this script does not exist -- and one that runs BEFORE the asset does not
# exist either, while looking in the log almost exactly like one that ran.
#
# ORDER IS LOAD-BEARING, and it is the same order the Caves use:
#   1. boundary  creates 0x7FFE OPENING doorcells
#   2. coverage  walls standable cells with no mesh, and deliberately SKIPS the
#                0x7FFE cells pass 1 wrote -- run first, it would seal doorways
#   3. portals   runs LAST, only ever turns 0x7FFF into 0x7FFE, so it must see
#                the walls the first two leave behind. Idempotent.
say "Lara's Home collision (boundary -> coverage -> portals)"
gym_fail=0
TRLEVEL="$PSX/GYM.PSX" python3 tools/mrt_boundary_audit.py --prefix gym --patch 2>&1 \
    | tail -1 || gym_fail=1
python3 tools/floor_coverage.py --prefix gym --faces --patch 2>&1 | tail -1 \
    || gym_fail=1
python3 tools/portal_open.py --prefix gym --patch 2>&1 | tail -2 \
    || gym_fail=1
# ☠️ FAIL LOUDLY. The old code wrote "   note: ... failed" and carried on, which
# is how an unwalkable mansion shipped for as long as the mansion has shipped.
# A collision pass that cannot run is not a note, it is a broken level.
if [ "$gym_fail" -ne 0 ]; then
    echo "!!! Lara's Home collision patching FAILED - the mansion would ship" >&2
    echo "!!! unwalkable (rooms with no portal openings). Refusing to continue." >&2
    exit 1
fi
# ☠️ GUARD FOR THE gym_lskin ALIAS. mrt_data.S no longer .incbin's a second
# copy of Lara's skeleton for the mansion - gym_lskin is a .set alias onto
# mrt_lskin, which saves 110,016 B in EVERY ROM (the old copy sat outside the
# GYMSD #endif, so even builds that ship no mansion carried it). That alias is
# only correct while the two extractions agree. They do today, by construction:
# _lskin is skeleton + all-animation joint angles, with no atlas dependence.
# If a future extractor change makes them diverge, Lara must get her own blob
# back - so FAIL LOUDLY here rather than render her wrong in Lara's Home.
if [ -f disc/gym_lskin.bin ] && [ -f disc/mrt_lskin.bin ]; then
    if cmp -s disc/mrt_lskin.bin disc/gym_lskin.bin; then
        echo "   gym_lskin == mrt_lskin (alias valid, saves $(stat -c%s disc/gym_lskin.bin) B)"
    else
        echo "☠️ BUILD STOPPED: gym_lskin.bin and mrt_lskin.bin DIVERGED."
        echo "   mrt_data.S aliases gym_lskin onto mrt_lskin; that is now WRONG."
        echo "   Restore the .incbin for gym_lskin in mrt_data.S before shipping."
        exit 1
    fi
fi

say "Title + loading backgrounds"
TR_DELDATA="$ASSETS" python3 tools/gen_titlebg.py
say "Title passport + Lara's-Home photo"
TRTITLE="$PSX/TITLE.PSX" python3 tools/tr2jag_title.py
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=photo PASS_TYPE=73 PASS_FORCE_TEX=265 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
# The OPEN passport spread (ring item 2). ☠️ These passes were missing, so the
# container died at `No rule to make target pass2_geom.bin` the moment the
# title ring was switched on - the ROM links the ring items whether or not the
# build knows how to make them. Recipe is ASSET_BUILD.md's.
TRTITLE="$PSX/TITLE.PSX" PASS_TYPE=71 PASS_PREFIX=pass2 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
# Sound + Detail ring items (ASSET_BUILD.md). ☠️ mrt_data.S .incbin's these
# whether or not anything generated them, and a missing .incbin is an ASSEMBLER
# error, not a link error - which is why it surfaces late and cryptically.
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=sound PASS_TYPE=96 PASS_POSES=1 PASS_DOUBLE=1 \
    python3 tools/tr2jag_title.py
TRTITLE="$PSX/TITLE.PSX" PASS_PREFIX=detail PASS_TYPE=95 PASS_POSES=1 PASS_DOUBLE=1 \
    PASS_GAIN=1.55 python3 tools/tr2jag_title.py
# PASS_GAIN rescues the lit lens but drags the FRAME swatch onto the neutral
# grey ramp (246); the PS1's frame is warm. Patch it back.
python3 - <<'PYEOF'
b = bytearray(open("detail_atlas.bin","rb").read())
for i, v in enumerate(b):
    if v == 246: b[i] = 119
open("detail_atlas.bin","wb").write(bytes(b))
PYEOF
python3 tools/gen_polaroid.py    # photo_geom -> a real polaroid
python3 tools/gen_openbook.py    # model 71 is a near-closed fan; author the spread
say "UI font"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/tr2jag_font.py
say "Sound effects"
TRLEVEL="$PSX/LEVEL1.PSX" python3 tools/tr2jag_sound.py
AUDIO="$(ls "$ASSETS"/TRACK02.* 2>/dev/null | head -1 || true)"
if [ -n "$AUDIO" ]; then
    say "Title music (streams from MUSIC.PCM on the SD card)"
    MUSIC_SRC="$AUDIO" python3 tools/tr2jag_music.py   # -> music.bin + MUSIC.PCM
else
    say "No CD-audio track on this disc -> silent title (game unaffected)"
    rm -f MUSIC.PCM
fi
# ── 2b. the FRONT END: four clips, all off the user's own disc ───────────────
# EIDOS.JV  MOVIES/INTRO.STR  - that file OPENS with the Eidos logo (152 frames)
# CORE.JV   FMV/CORELOGO.FMV  - the Core Design logo
# INTRO.JV  FMV/CAFE.FMV      - the attract cinematic
# CAVES.JV  FMV/SNOW.FMV      - the trek that plays before the Caves
# Matched by frame count against the shipped clips (152/193 exact; the two long
# ones land within the encoder's audio-pad). Nothing here ships with the repo
# and no third-party footage is involved - it is all on the disc you mounted.
# Set VIDEO=0 to skip (much faster build; the game then boots to the title).
if [ "${VIDEO:-1}" = "1" ]; then
    say "Front-end videos (four clips off the disc - this is the slow part)"
    FMVDIR="$ASSETS/FMV"        # dumped by extract_disc.py via WANT_FMV
    conv() {   # conv <source> <OUT.JV>
        [ -f "$FMVDIR/$1" ] || { echo "   (no $1 on this disc - skipping $2)"; return 0; }
        JV_VQ=1 JV_CROP="0,0,320,208" python3 tools/tr2jag_video.py \
              "$FMVDIR/$1" "$2" 15 >/dev/null 2>&1 \
            && echo "   $2 <- $1  ($(stat -c%s "$2") B)" \
            || echo "   !! $2 failed to convert - the build continues without it"
    }
    mkdir -p disc
    conv INTRO.STR    disc/EIDOS.JV
    conv CORELOGO.FMV disc/CORE.JV
    conv CAFE.FMV     disc/INTRO.JV
    conv SNOW.FMV     disc/CAVES.JV
else
    say "VIDEO=0 - skipping the front-end clips"
fi

# The title theme STREAMS from MUSIC.PCM on the SD card; the ROM embeds only a
# 4-byte stub so the .incbin links without baking ~370KB of audio into DRAM
# (that overflows the 2MB budget). This matches the shipped build.
printf '\0\0\0\0' > music.bin

# ── 2b. card images for the level sets ───────────────────────────────────────
# Both level sets are linked into every ROM and only one is ever live. Packing
# them for the card is the first half of moving them there; nothing in the ROM
# changes yet, so this step is free and can be verified on its own.
# ☠️ MUST RUN AFTER EVERY PATCH PASS. It snapshots the .bin files, and the
# boundary/coverage/portal passes and the four atlas patches all REWRITE them
# in place. Packed before those, the card would carry the level the extractor
# emitted rather than the level the ROM ships - and the two would differ in
# exactly the collision cells that decide whether a room is walkable.
say "Level card images (both sets - only one is ever live)"
mkdir -p "$OUT/OL"
python3 tools/make_levpack.py mrt disc "$OUT/OL/CAVES.LEV" || {
    echo "!!! could not pack the Caves for the card" >&2; exit 1; }
python3 tools/make_levpack.py gym disc "$OUT/OL/GYM.LEV" || {
    echo "!!! could not pack Lara's Home for the card" >&2; exit 1; }
{ set -- "$OUT/OL/CAVES.LEV" "$OUT/OL/GYM.LEV"
  c=$(stat -c%s "$1"); g=$(stat -c%s "$2")
  big=$c; [ "$g" -gt "$c" ] && big=$g
  echo "   shared arena needs $big B; both resident today costs $((c+g)) B"
  echo "   => frees $((c+g-big)) B of DRAM, and takes $((c+g)) B out of the ROM"
  # ☠️ THE ARENA SIZE IS GENERATED, NEVER TYPED. It is the max of the two card
  # images, and a level that grows past a hard-coded constant would overflow
  # the arena into whatever .bss follows it - a corruption with no message,
  # found only by whatever renders wrong afterwards. Emitting it here means the
  # size cannot disagree with the files it describes.
  { echo "/* generated by build_cof.sh - do not edit */"
    echo "#define LEV_ARENA_BYTES ${big}u"
    echo "#define LEV_CAVES_BYTES ${c}u"
    echo "#define LEV_GYM_BYTES   ${g}u"; } > disc/lev_arena.h
  echo "   disc/lev_arena.h: LEV_ARENA_BYTES=$big"; }

# ── 3. build the ROM ─────────────────────────────────────────────────────────
say "Compiling for Atari Jaguar ($QUALITY)"
make clean >/dev/null
# PADTEXT: the A10 boot lottery. It pads .text, and whether a given layout
# boots at all is positional - ANY change to code or assets re-rolls it. It is
# NOT a setting you can carry between builds. If your ROM comes up black, run
# the same build again with -e PADTEXT=0/136/272/408/544/816 until one boots.
# ★ The .JV clips are not linked into the ROM, so VIDEO=0 produces a
# BYTE-IDENTICAL ROM in a fraction of the time - roll with VIDEO=0, then do one
# full run with the pad that booted.
make RMAC="$RMAC" ${JAS:+JAS="$JAS"} ${JCC68K:+JCC68K="$JCC68K"} \
     ${PADTEXT:+PADTEXT="$PADTEXT"} $BUILD_FLAGS $QUALITY_FLAGS

# ── 4. stage the GameDrive payload (everything the SD card needs, together) ───
mkdir -p "$OUT"
# ☠️ A run with NO ROM used to exit 0 (2026-08-26: every patch pass had failed on a
# path and the make never produced a COF; the entrypoint reported success). Judge
# the run by the artifact.
[ -s build/openlara.cof ] || { echo "☠️ BUILD FAILED: build/openlara.cof was not produced - read the log above"; exit 1; }
cp build/openlara.cof "$OUT/OPENLARA.COF"
# ☠️ KEEP THE ELF BESIDE THE ROM. Symbol addresses are PER-BUILD, so without the
# matching .elf the release ROM cannot be instrumented AT ALL - no peek, no
# probe_spot, no entity_check. Run 66 hit exactly that: a "LOADING..." screen
# appeared mid-walk in the release and could not be chased, because the only
# build that reproduces it had no symbols. A conformance ROM is NOT a substitute
# (different flags, different addresses).
# It is a debug artifact, not an SD file - it is deliberately left OUT of
# COPY-THESE-TO-SD-ROOT.txt below.
if [ -f build/openlara.elf ]; then
    cp build/openlara.elf "$OUT/OPENLARA.elf"
    echo "   kept OPENLARA.elf beside the ROM (symbols for probe_spot/entity_check)"
fi
[ -f disc/MUSIC.PCM ] && cp disc/MUSIC.PCM "$OUT/MUSIC.PCM" || \
    { [ -f MUSIC.PCM ] && cp MUSIC.PCM "$OUT/MUSIC.PCM"; } || true
# the front-end clips and the Lara's-Home loading art stream from the card too
for v in EIDOS.JV CORE.JV INTRO.JV CAVES.JV; do
    [ -f "disc/$v" ] && cp "disc/$v" "$OUT/$v"
done
[ -f disc/gymload.bin ] && cp disc/gymload.bin "$OUT/GYMLOAD.DAT"
# ☠️ CAVSLOAD.DAT was never emitted here - the Caves loading art (AZTECLOA)
# streams from SD exactly like GYMLOAD, and a card without it drops to the
# text panel. Emit both.
[ -f disc/cavesload.bin ] && cp disc/cavesload.bin "$OUT/CAVSLOAD.DAT"
cat > "$OUT/COPY-THESE-TO-SD-ROOT.txt" <<'NOTE'
Copy EVERY file in this folder to the ROOT of your RetroHQ GameDrive SD card
(the top level, NOT a subfolder), then boot OPENLARA.COF from the GameDrive menu.

  OPENLARA.COF   the game
  MUSIC.PCM      the title theme -- the game STREAMS it from the SD card, so if
                 this file is missing you will hear a loud hiss on the title
                 screen. Copy it too.

Level loads take 10-15 seconds -- that's the 68000 doing its thing.
NOTE
say "Done!  ->  $OUT"
echo "   Copy EVERY file below to the ROOT of your GameDrive SD card, then boot OPENLARA.COF:"
ls -1 "$OUT" | sed 's/^/     /'
