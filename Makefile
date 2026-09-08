# OpenLara - Atari Jaguar target
#
# Bare-metal m68k GNU toolchain (neogeo-elf: 68000-native, C++-capable,
# libgcc has no 68020 bsr.l leak) -> ELF -> raw binary -> .cof.
# Deploy the .bin via open_jaggd to GameDrive load addr $4000.

CROSS   := m68k-neogeo-elf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
PYTHON  := python3
RMAC    := $(HOME)/jaguar-tools/bin/rmac
# Cobweb toolchain (2026-07-20): jas is the DEFAULT assembler for every
# actively-maintained kernel — it hazard-checks each build (TRM bug 13
# scoreboard races, indexed-store staleness, delay-slot waste, branch range).
# rmac is retained ONLY for `make verify-asm`, which byte-compares jas output
# against a second assembler. It no longer builds anything that ships: jas took
# over the last three kernels once its false bug-13 errors were fixed upstream,
# and emits byte-identical output for all of them.
# ☠️ Do not reintroduce it as a build dependency - github.com/ggnkua/rmac 404s.
# COBWEB (github.com/bslop/cobweb, MIT) builds every GPU/DSP kernel here.
# Prefer a copy checked out INSIDE THE PROJECT (jag_openlara/cobweb) so the tree
# is self-contained and does not depend on where else the machine keeps one;
# fall back to the old location, and stay overridable for containers:
#   make JAS=/usr/local/bin/jas
COBWEB_DIR ?= $(abspath $(CURDIR)/../../../../cobweb)
COBWEB_BIN  = $(COBWEB_DIR)/sim/target/release

JAS     ?= $(if $(wildcard $(COBWEB_BIN)/jas),$(COBWEB_BIN)/jas,$(HOME)/Documents/Git/cobweb/sim/target/release/jas)
# rmac writes defines as -dNAME=V; jas wants -d NAME=V
jasd     = $(subst -d,-d ,$(1))

# !!! FLAG SEMANTICS — `make FOO=0` TURNS FOO **ON** !!!  (verified 2026-07-25)
# Every boolean flag here is tested with `ifdef FOO` or `$(if $(FOO),1,0)`, and
# BOTH are true for ANY non-empty value — including the string "0".  Checked:
# `make SHADEPASS=0` expands to `-d SHADEPASS=1`.  Not one flag tests its value.
# ==> Express "off" by OMITTING the variable.  Never write FOO=0 in an A/B arm,
#     and never describe an omitted flag as "FOO=0" in a commit message — that
#     reads back later as an arm that was actually built with the flag ON.
# (Value-carrying flags are different and DO work: HOPBOOT, FARCLIP, SLIVERW,
#  SLIVERH, PIPESTAGE, ROOMCAP pass their value through with -DFOO=$(FOO).)

BUILD     := build
LOAD_ADDR := 0x4000

INCPATH := -I.

CFLAGS  := -m68000 -ffreestanding -fomit-frame-pointer -fno-strict-aliasing \
           -fwrapv -O2 -Wall -Wextra $(INCPATH)

# ☠️ DISC-DERIVED ASSETS LIVE IN disc/ (gitignored, 2026-08-19, after 43 MB of
# them were purged from the public repo). The extractor and tools/build_cof.sh
# write there and nothing under it is ever committed. Three things must agree:
#   * headers        -> -Idisc, both here and on the jcc68k command lines below
#                       (those pass only $(filter -D%,$(CFLAGS)), so an -I in
#                       CFLAGS alone never reaches them)
#   * .incbin data   -> mrt_data.S / room0data.S say "disc/<name>"
#   * the generators -> point their OUTDIR at disc/
CFLAGS   += -Idisc
CXXFLAGS += -Idisc
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -fno-use-cxa-atexit -fshort-enums
ASFLAGS := -m68000 $(INCPATH)
LDFLAGS := -nostdlib -T jaguar.ld -Wl,-Map=$(BUILD)/openlara.map \
           -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,-z,noexecstack

# make NOGD=1: Skunkboard build - compiles out the GameDrive input path
# (GD BIOS calls hang on a non-GD cart) and enables the Skunkboard USB
# console (dbg_kv prints readable live with `jcp -c`). Touch main.c when
# toggling (make doesn't track -D changes).
ifdef NOGD
CFLAGS   += -DNO_GAMEDRIVE -DSKUNK_CONSOLE
CXXFLAGS += -DNO_GAMEDRIVE -DSKUNK_CONSOLE
endif
ifdef NOGDONLY
CFLAGS   += -DNO_GAMEDRIVE
CXXFLAGS += -DNO_GAMEDRIVE
endif
ifdef SKCONONLY
CFLAGS   += -DSKUNK_CONSOLE
CXXFLAGS += -DSKUNK_CONSOLE
endif

# make GEOMWALK=1: Tom does the edge-walk (gpu_geomwalk.gas) instead of
# the 68k (gpu_spanfill drains a 68k-built span list). Switchable so the
# proven span-fill path stays the fallback. Touch main.c when toggling.
ifdef GEOMWALK
CFLAGS   += -DGEOMWALK
ASFLAGS  += -DGEOMWALK
endif

# make GEOMXFORM=1: Tom also does per-vertex transform+project+cull
# (gpu_geomxform.gas). The 68k passes world-space verts + a camera block
# and only computes a cheap per-poly depth key. Touch main.c when toggling.
ifdef GEOMXFORM
CFLAGS   += -DGEOMXFORM
ASFLAGS  += -DGEOMXFORM
endif

# make MMULTX=1: vertex pre-pass rotate via Tom's hardware MMULT (3 dot products
# from a precomposed 3x3) instead of 8 software imul32. 68k precomputes the
# matrix (gpu.c build_xform_mtx); kernel reads it under .if MMULTX. Phase 0
# silicon-validated (calib p_mmult). Default off = byte-identical imul32 path.
ifdef MMULTX
CFLAGS   += -DMMULTX
endif

# make NOSOUND=1: compile Jerry's AUDIO_PUMP out of the DSP kernel AND skip the
# 68k's SCLK/SMODE DAC start. Frees Jerry (he is ~88%% busy on audio+pose) so
# 68k work can be moved onto a RISC. Silent build - measurement/dev only.
ifdef NOSOUND
CFLAGS   += -DNOSOUND
CXXFLAGS += -DNOSOUND
endif

# make GEOMXFORM=1 OVERLAP=1: async - 68k builds frame N+1's packets
# while Tom draws frame N (double-buffered packets, fire-and-return kick).
ifdef OVERLAP
CFLAGS   += -DOVERLAP
endif

# make GEOMDIRECT=1: Tom reads the room geometry tables straight from
# DRAM (68k passes only per-room base pointers). The 68k is out of the
# room-render path entirely. Implies the geomxform transform/raster core.
ifdef GEOMDIRECT
CFLAGS   += -DGEOMDIRECT -DGEOMXFORM
ASFLAGS  += -DGEOMDIRECT
endif

# make TEXTURED=1: affine texture-mapped span kernel test (gpu_textured.gas)
# + embedded 256x256 atlas subset (texdata.o). Proof-of-concept quads.
ifdef TEXTURED
CFLAGS   += -DTEXTURED
ASFLAGS  += -DTEXTURED
endif

# make TEXROOM=1: render room 0 (Caves) TEXTURED (room0_tex.bin + compact
# room-0 atlas) via the gpu_textured kernel. Implies TEXTURED (same kernel).
ifdef TEXROOM
CFLAGS   += -DTEXROOM -DTEXTURED
ASFLAGS  += -DTEXTURED
endif

# make TEXROOM=1 BLTEX=1: use the Blitter A2 HARDWARE texture-map kernel
# (gpu_bltex.gas) + an 8bpp indexed framebuffer + OP CLUT. The Blitter
# texture-maps in hardware instead of the GPU looping per-pixel -> fast.
ifdef BLTEX
CFLAGS   += -DBLTEX -DFB8
ASFLAGS  += -DBLTEX
endif

# make GEOTEX=1: Tom reads textured room geometry from DRAM, transforms/
# projects/culls per face, AND Blitter-textures (gpu_geotex.gas) -> 68k does
# ZERO per-vertex work. 8bpp fb + OP CLUT. Uses room0_tex + orbit camera.
ifdef GEOTEX
CFLAGS   += -DGEOTEX -DFB8
ASFLAGS  += -DGEOTEX
endif

# make MULTIROOM=1: render a CONNECTED SET of textured rooms (Caves 0,6,4,2,5)
# via the gpu_geotex kernel + a SHARED atlas (mrt_*.bin from tr2jag_multiroom.py);
# Lara walks between rooms (multi-room collision). Reuses the geotex kernel + FB8.
ifdef MULTIROOM
CFLAGS   += -DMULTIROOM -DFB8
ASFLAGS  += -DMULTIROOM
endif

CFLAGS   += $(CFLAGS_EXTRA)

# make JERRYPOSE=1: Jerry (DSP) poses Lara (the "three-processor frame"); the
# 68k pose is auto-skipped (posed=2 at the kick). HW-parity-passed 2026-07-09
# but the flag was never wired into the Makefile — every MULTIROOM build has
# been 68k-posing. fps: jsim claimed +21%% but SILICON measured ~neutral
# (4.72 vs 4.83-4.94, 2026-07-20) — keep it for the freed 68k headroom, not fps.
ifdef JERRYPOSE
CFLAGS   += -DJERRYPOSE
endif

# make AUTOSTART=1: skip the title menu (auto-selects New Game after ~20 frames)
# so headless HW/emulator profiling needs no physical A press on the controller.
ifdef AUTOSTART
CFLAGS   += -DAUTOSTART
endif

# make BLITPROBE=1: boot into the SRCSHADE/GOURD-on-8bpp Blitter micro-probe
# instead of the game (main.c, after video_init). Read the bit-cell rows off
# the video capture.
ifdef BLITPROBE
CFLAGS   += -DBLITPROBE
endif

ifdef PROFILE
CFLAGS   += -DPROFILE
ifdef NOPROFGPU
PROFGPU_DEF := -dPROFGPU=0
else
PROFGPU_DEF := -dPROFGPU=1
endif
else
PROFGPU_DEF := -dPROFGPU=0
endif

# perf experiment: make ... ROOMCAP=N draws only the N nearest rooms (fps-vs-
# polycount scaling; combine with PROFILE=1 and read the fps bar).
ifdef ROOMCAP
CFLAGS   += -DROOMCAP=$(ROOMCAP)
endif

# PERFHUNT M68DIET=1 (2026-07-22, PERFHUNT_CAMPAIGN.md): 68k serial-segment
# diet — portal_rect 32x16 hardware-mul split, painter sort over the admitted
# candidate subset only, lara_finish word-read centroids + reciprocal
# divides.  C-only (no kernel bytes).  Opt-in; flags-off code identical.
ifdef M68DIET
CFLAGS   += -DM68DIET
CXXFLAGS += -DM68DIET
endif
# AUTOPSY SUB-FLAGS (2026-07-22): the three M68DIET pieces individually —
# M68A1=1 (portal_rect split-mul), M68A2=1 (subset painter sort),
# M68A3=1 (lara_finish word centroids + reciprocal).  M68DIET=1 = all three.
# M68PAD=N adds a calibrated 68k busy-pad after the sort (timeline
# discriminator — see PERFHUNT_CAMPAIGN.md autopsy).
ifdef M68A1
CFLAGS   += -DM68D_A1
CXXFLAGS += -DM68D_A1
endif
ifdef M68A2
CFLAGS   += -DM68D_A2
CXXFLAGS += -DM68D_A2
endif
ifdef M68A3
CFLAGS   += -DM68D_A3
CXXFLAGS += -DM68D_A3
endif
ifneq ($(strip $(M68DIET)$(M68A2)),)
ifdef ROOMCAP
$(error M68DIET/M68A2 and ROOMCAP are mutually exclusive (ROOMCAP indexes the full order[] list))
endif
ifdef NOVISCULL
$(error M68DIET/M68A2 and NOVISCULL are mutually exclusive (A2 prefilters by rdepth/prv))
endif
endif
ifdef M68PAD
CFLAGS   += -DM68PAD=$(M68PAD)
CXXFLAGS += -DM68PAD=$(M68PAD)
endif

# ENTLISTS=1 (2026-08-27): precompute type-indexed entity lists at level start
# so the per-frame draw / collect loops stop rescanning all 60 entities (each a
# DRAM read of mrt_ent[].type that contends with Tom's render under PIPELINE —
# the M68A2/HUDTEXT bus-contention class).  ☠️ THIS WAS UNPLUMBED until now:
# `make ENTLISTS=1` set the make var but NEVER reached the compiler, so the
# #ifdef ENTLISTS code compiled OUT and the 2026-08-27 "+2.5%" A/B (#2045 vs
# #2046) compared two byte-identical ROMs.  Now wired for the first honest test.
ifdef ENTLISTS
CFLAGS   += -DENTLISTS
CXXFLAGS += -DENTLISTS
endif

# BFSCACHE=1 (2026-08-27): the portal hop-depth BFS (rdepth[]) is a pure fn of
# g_curroom (S_adj/HOPDEPTH static), so recompute it only on a room crossing
# instead of every frame — kills a 38-entry reset + 3-pass relaxation from the
# Tom-overlap window on ~95% of frames (the M68A2 bus-contention class). prv[]
# still recomputes every frame; g_bfs_room invalidates on level load.
ifdef BFSCACHE
CFLAGS   += -DBFSCACHE
CXXFLAGS += -DBFSCACHE
endif

# GOVERNOR=1 (2026-07-22, PERFHUNT_CAMPAIGN.md smoothness campaign): frame
# governor — one frame longer than GOV_HI fields clamps g_hopcap to 1;
# GOV_K consecutive frames at/below GOV_LO restore the dial cap.  VARIANCE
# tool (judge on worst/median, not mean).  Requires HOPDIAL.  Tunables:
# make GOVERNOR=1 GOV_HI=6 GOV_LO=4 GOV_K=20 (silicon defaults; emu frame
# cadence is ~3x slower — use GOV_HI=11 GOV_LO=10 for in-emu exercises).
ifdef GOVERNOR
ifndef HOPDIAL
$(error GOVERNOR=1 requires HOPDIAL=1 (g_hopcap machinery))
endif
GOVDEFS := -DGOVERNOR $(if $(GOV_HI),-DGOV_HI=$(GOV_HI)) $(if $(GOV_LO),-DGOV_LO=$(GOV_LO)) $(if $(GOV_K),-DGOV_K=$(GOV_K))
CFLAGS   += $(GOVDEFS)
CXXFLAGS += $(GOVDEFS)
endif

# LEMITDIET=1 (2026-07-22, PERFHUNT_CAMPAIGN.md LARA EMIT DIET): lara_finish
# face re-emit via pre-grouped per-mesh payload banks + movem.l bursts
# (cpu68k.S) + one-time plane prefixes + changed-window emit.  C/asm only,
# zero kernel bytes.  Blob content byte-identical to the legacy emit.
# LPLANES=1 (2026-07-26, LARA_HEAD_CAMPAIGN.md): cull Lara's BACK FACES on the
# 68k with real per-face planes baked mesh-local by the extractor, instead of
# relying on the kernel's screen-space signed area (whose sign is quantised on
# her sub-pixel head triangles -> her face painted over the back of her skull).
# Needs mrt_lplanes.h from the extractor.  Also shrinks the blob Tom chews.
# LFREEZE=1: pin Lara's pose to one animation frame (head-twitch diagnostic).
# LARACOUNT=1: per-frame counters of HER faces staged/rastered/culled, painted
# as 3 bars (rows 12/16/20).  Diagnostic build.
ifdef LARACOUNT
CFLAGS   += -DLARACOUNT
CXXFLAGS += -DLARACOUNT
endif

ifdef LFREEZE
CFLAGS   += -DLFREEZE
CXXFLAGS += -DLFREEZE
endif

ifdef FB8BEACON
ASFLAGS  += -DFB8BEACON
endif

ifdef NOCLEAR
CFLAGS   += -DNOCLEAR
CXXFLAGS += -DNOCLEAR
endif

# make KSWAP=N: reload the GPU kernel N times before each dispatch. Prices a
# KERNEL OVERLAY for the sector renderer without writing one — the copy is a
# no-op (same bytes) so any fps delta is purely the swap cost. KSWAP=2 is what
# a flats+polygon overlay would pay per frame. Measure in ROOM 26, on the
# FPSBEACON clock, never at the spawn.
ifdef KSWAP
CFLAGS   += -DKSWAP=$(KSWAP)
CXXFLAGS += -DKSWAP=$(KSWAP)
endif

ifdef VCBIG
CFLAGS   += -DVCBIG
CXXFLAGS += -DVCBIG
endif
# SWANIM: Lara's switch-pull animation (TR1 anim 63). Off by default while the
# pose regression is investigated - with it on she got STUCK in the pull pose
# (arms extended, clamped at the last frame of a one-shot anim).
ifdef SWANIM
CFLAGS   += -DSWANIM
CXXFLAGS += -DSWANIM
endif
# BOOTVID: play EIDOS.JV + CORE.JV boot logos from the GameDrive SD before
# the title (tools/tr2jag_video.py makes them). Missing files skip cleanly.
ifdef BOOTVID
CFLAGS   += -DBOOTVID
CXXFLAGS += -DBOOTVID
endif
ifdef VIDPROF
CFLAGS   += -DVIDPROF
CXXFLAGS += -DVIDPROF
endif
ifdef DBGROOM
CFLAGS   += -DDBGROOM
CXXFLAGS += -DDBGROOM
endif
ifdef ROOMTOUR
CFLAGS   += -DROOMTOUR
CXXFLAGS += -DROOMTOUR
# ROOMTOUR_HOLD=N: fields to sit in each room (60 = 1s, default 120). Raise it
# for an fps sweep - 2s is ~14 published frames, too few to measure with.
ifdef ROOMTOUR_HOLD
CFLAGS   += -DROOMTOUR_HOLD=$(ROOMTOUR_HOLD)
CXXFLAGS += -DROOMTOUR_HOLD=$(ROOMTOUR_HOLD)
endif
endif
# DOORTEX: doors/levers use the real TR1 texture (objtex 897/896) appended to
# the atlas by `MRT_DOORPATCH=1 ... tr2jag_multiroom.py` -> needs mrt_door.h.
ifdef DOORTEX
CFLAGS   += -DDOORTEX
CXXFLAGS += -DDOORTEX
endif
ifdef SWDEBUG
CFLAGS   += -DSWDEBUG
CXXFLAGS += -DSWDEBUG
endif

ifdef LPLANES
CFLAGS   += -DLPLANES
CXXFLAGS += -DLPLANES
ifdef LEMITDIET
$(error LPLANES=1 and LEMITDIET=1 are incompatible: LEMITDIET pre-groups the face payload into banks and would bypass the per-face cull)
endif
endif

ifdef LEMITDIET
CFLAGS   += -DLEMITDIET
CXXFLAGS += -DLEMITDIET
ASFLAGS  += -DLEMITDIET
endif

# FLIPASM=1 (2026-07-27): move the PUBLISH half of the flip protocol out of
# video.c and into cpu68k.S, so the order (wait -> rotate -> precompute ->
# BLITTER BARRIER -> publish) is fixed in the instruction stream instead of
# resting on a compiler keeping two volatile stores either side of a
# non-volatile poll.  The ISR half is already assembler (startup.S).  LOWRES
# only -- that is the path the asm implements.
ifdef FLIPASM
# (2026-08-01) FLIPASM used to require LOWRES=1. It no longer does. The asm
# publish path touches only fb0/1/2, draw_buf, front_fb, op_link, pend_fix0 and
# pending_fb -- nothing resolution-specific; RENDER_H and the scale factor live
# in OP phrase fields written once at init. The one thing the UNSCALED path
# needs on top is pend_fix1, and that is constant after init (op_link,
# DISPLAY_H, BASE_Y), so video.c now seeds it in the OP-list builder instead of
# recomputing it per flip. The unscaled path also GAINS the Blitter completion
# barrier, which the C version of it never had.
ifdef HALFRES
$(error FLIPASM=1 and HALFRES=1 are incompatible (HALFRES publishes via blit_double))
endif
CFLAGS   += -DFLIPASM
CXXFLAGS += -DFLIPASM
ASFLAGS  += -DFLIPASM
endif

# make MOVESET=1 (or MV_ROLL=1 / MV_SIDE=1 individually): Lara's roll
# (Y/PAUSE) and sidestep (C+LEFT/RIGHT).  Split into two flags on 2026-07-29
# because the combined build BLACK-SCREENS silicon from t=0 while rendering
# identically to the control in jagemu — the bisect has to be a build flag,
# not an edit, or the arms are not reproducible.
ifdef MOVESET
MV_ROLL := 1
MV_SIDE := 1
endif
# make ENTITIES=1: real doors + switches driven by the level's OWN entity
# table and FloorData triggers (mrt_spawn.h), replacing the single
# hand-placed door that sat at a fixed offset from Lara's spawn.
# LEVEL1 has 3 SWITCH entities and 8 doors; two of the three switch
# triggers carry a CAMERA_SWITCH command whose extra word must be
# skipped, or the doors they open are silently dropped.
ifdef ENTITIES
CFLAGS   += -DENTITIES
CXXFLAGS += -DENTITIES
endif
ifdef MV_ROLL
CFLAGS   += -DMV_ROLL
CXXFLAGS += -DMV_ROLL
endif
ifdef MV_SIDE
CFLAGS   += -DMV_SIDE
CXXFLAGS += -DMV_SIDE
endif

# make PADBYTES=N: link N bytes of dead, never-referenced .rodata into the
# image.  Pure LAYOUT/SIZE control — it changes nothing the 68k executes, so
# if a build that boots starts black-screening when padded, the fault is the
# image size or the addresses everything lands at, NOT the added logic.
ifdef PADBYTES
CFLAGS   += -DPADBYTES=$(PADBYTES)
CXXFLAGS += -DPADBYTES=$(PADBYTES)
endif

# make BOOTMARK=1: a single store in startup.S that reddens the border just
# before jsr main.  The minimum-perturbation probe for the boot hang.
ifdef BOOTMARK
ASFLAGS  += -DBOOTMARK
endif

# make GDSTUB=1: gd_install reports failure immediately instead of probing the
# cart.  Diagnostic for the boot hang: it changes ONLY gdbios.o, so main.o stays
# byte-identical to the build under test.
ifdef PASS2ALIAS
ASFLAGS  += -DPASS2ALIAS
endif

# IRQREARM: ON by default - it is the A10 mitigation.
# ☠️ A jagemu measurement said it HALVED throughput (GPU instret 689M vs 1312M,
# launches 22M vs 42M over a matched in-game window, both arms verified in-game).
# SILICON SAYS NO: median 8 fields = 7.50 fps either way. The instret-over-window
# proxy does NOT model 68k/Tom bus contention and cannot compare sync strategies.
# The claim is retracted; the flag stays default ON for the A10 protection.
# What IS real from that investigation, and is kept:
#   - gpu_sync now SLEEPS (cpu_stop_unless) instead of spinning on the DRAM
#     mailbox, and only re-arms after 200 wakes. The 68k PC histogram showed the
#     spin left the 68k 0% asleep / 100% awake in gpu_sync, and gpu_sync's own
#     comment warns a DRAM poll there steals bus from Tom all frame.
#   - measured: gpu_sync takes ~8 STOP wakes per frame normally, so any re-arm
#     threshold near 8 fires every frame; re-arming writes VMODE and disturbs
#     the video timing.
#   - IRQREARM was missing from ASFLAGS, so its cpu68k.S arm never compiled.
ifndef IRQREARM_OFF
IRQREARM := 1
endif
ifdef IRQREARM
CFLAGS   += -DIRQREARM
CXXFLAGS += -DIRQREARM
ASFLAGS  += -DIRQREARM   # cpu68k.S's flip wait is guarded by it too - it was
                         # missing here, so that code was compiled OUT of every
                         # build and the original STOP path always ran.
endif

ifdef GDSTUB
ASFLAGS  += -DGDSTUB
endif

# make HANGDIAG=1: bound every unbounded 68k wait and, on timeout, paint a
# distinct border colour and halt.  RED = the FLIPASM Blitter barrier,
# YELLOW = blit_wait, CYAN = the flip's pending_fb wait, MAGENTA = no vblank
# ISR.  Touches only cpu68k.S / blit.c / video.c, never main.c, so main.o stays
# byte-identical to the build being diagnosed.
ifdef HANGDIAG
CFLAGS   += -DHANGDIAG
CXXFLAGS += -DHANGDIAG
ASFLAGS  += -DHANGDIAG
endif

# make PASS_X=/PASS_Y=/PASS_Z=: title-screen passport placement (world units).
# Swept offline against jagemu; see the note by mp2 in main.c for the metric.
ifdef PASS_X
CFLAGS   += -DPASS_X=$(PASS_X)
endif
ifdef PASS_Y
CFLAGS   += -DPASS_Y=$(PASS_Y)
endif
ifdef PASS_Z
CFLAGS   += -DPASS_Z=$(PASS_Z)
endif
ifdef PASS_OPEN_X
CFLAGS   += -DPASS_OPEN_X=$(PASS_OPEN_X)
endif
ifdef PASS_OPEN_Y
CFLAGS   += -DPASS_OPEN_Y=$(PASS_OPEN_Y)
endif
ifdef PASS_OPEN_Z
CFLAGS   += -DPASS_OPEN_Z=$(PASS_OPEN_Z)
endif
ifdef CTRL_YAW
CFLAGS   += -DCTRL_YAW=$(CTRL_YAW)
endif
ifdef CTRL_PITCH
CFLAGS   += -DCTRL_PITCH=$(CTRL_PITCH)
endif
ifdef CTRL_ROLL
CFLAGS   += -DCTRL_ROLL=$(CTRL_ROLL)
endif
ifdef PASS_YAW
CFLAGS   += -DPASS_YAW=$(PASS_YAW)
endif
ifdef PASS_PITCH
CFLAGS   += -DPASS_PITCH=$(PASS_PITCH)
endif
ifdef PASS_OPEN_PITCH
CFLAGS   += -DPASS_OPEN_PITCH=$(PASS_OPEN_PITCH)
endif
ifdef PASS_ROLL
CFLAGS   += -DPASS_ROLL=$(PASS_ROLL)
endif
ifdef PASS_OPEN_ROLL
CFLAGS   += -DPASS_OPEN_ROLL=$(PASS_OPEN_ROLL)
endif
ifdef PASS_OPEN_YAW
CFLAGS   += -DPASS_OPEN_YAW=$(PASS_OPEN_YAW)
endif

# make PADTEXT=N: the same idea one section earlier -- N dead bytes in .TEXT,
# which shifts .rodata/.data/.bss exactly the way real added code does.
ifdef PADTEXT
CFLAGS   += -DPADTEXT=$(PADTEXT)
CXXFLAGS += -DPADTEXT=$(PADTEXT)
endif

# make VRES60=1 (with LOWRES=1): render 60 lines instead of 120 and let the OP
# scaler stretch 4.0x.  Spans are per-scanline, so this halves the span count -
# the one cost that every perf measurement points at.  Requires LOWRES=1 (it is
# the OP-scaler display path).
ifdef VRES60
ifndef LOWRES
$(error VRES60=1 requires LOWRES=1 (it is the OP-scaler display path))
endif
CFLAGS   += -DVRES60
CXXFLAGS += -DVRES60
endif

# VRES240 / full 320x240 — STILL BLOCKED.  2026-08-02 UPDATE, READ THIS FIRST:
# the note below blames the KERNEL's 120-line constants.  THAT IS WRONG and it
# sent a whole session to the wrong file.  gpu_geotex.gas already has a correct
# full-height branch (.if LOWRES ... .else RENDER_H 240 / CENTER_Y 120 /
# FOCAL_Y 190).  The blocker is the DISPLAY PATH in video.c + startup.S, which
# has been rebuilt entirely around the LOWRES *scaled* (TYPE-1) object.
# Progress made 2026-08-02 (all committed, all necessary, none sufficient):
#   * op_list was `static` outside LOWRES while startup.S's exc_catch
#     references it unconditionally -> 8 undefined refs.  Now exported always.
#   * fs_ph1/2/3/5 were declared only in the LOWRES branch, same problem.
#     Mirrored into the full-height branch at 1.0x scale (0x2020).
#   -> a full-height ROM now LINKS and BUILDS.
# Still BLACK on silicon, and these are ruled OUT as the cause:
#   * A10 lottery      - black at PADTEXT 544 AND 272 (two layouts)
#   * OPDBL            - byte-identical size without it, still black, so it
#                        was never active in a non-LOWRES build
#   * OPPLAIN=1        - the ISR does clobber [4]/[5] (a plain BITMAP object is
#                        2 phrases/4 longs, so [4]/[5] are the STOP object, not
#                        REMAINDER/SCALE).  OPPLAIN writes a real STOP there.
#                        Still black, so this is necessary but not sufficient.
# NEXT SUSPECTS, in order: DISPLAY_H/BASE_Y for an unscaled object; the
# pitch/depth/dwidth bits in op_list[3]; and video_flip's pending_fb path
# (video.c ~423-452), which is written around the scaled object and OPDBL.
#
# VRES240 — REVERTED 2026-08-01, do not re-enable without reading this.
# It rendered the full 240 lines through the scaled display path at 1.0x. It
# was retracted twice over: the world does not draw in-game (only Lara and a
# few wall fragments; the room is black), because the KERNEL (gpu_geotex.gas,
# GEOMDIRECT) still carries 120-line projection/clip constants while FOCAL_Y is
# switched in main.c only. Fixing the kernel constants is the actual work.
# Worse, its edits to video.c/video.h/startup.S broke the SHIPPING 120-line
# path too: a plain LOWRES=1 build put the picture in half the vertical window
# (measured off the capture: aspect 2.10 where 4:3 = 1.33). Those three files
# are now reverted to df00e96; the 240 work is recoverable from f3de566,
# bdcd434 and af2e67b when someone fixes the kernel constants first.
ifdef VRES240
$(error VRES240 was REVERTED - it broke the 120-line shipping display path too. \
        See the comment above this error in the Makefile, and PLAY_BUILD.md)
endif

# make ... LOWRES=1: render a HALF-HEIGHT (320x120) framebuffer and let the
# Object Processor's hardware VERTICAL scaler (2.0x) display it as 320x240 -
# halves the Blitter fill (the bottleneck) with no per-frame cost.  The C side
# keys off -DLOWRES (RENDER_H=120, TYPE-1 scaled OP object).  The geomdirect
# kernel needs matching constants (CENTER_Y/FOCAL_Y/Y-clamp) chosen at assemble
# time: rmac has no .ifdef, so LOWRES is ALWAYS passed to it as -dLOWRES=0 or 1
# and the kernel selects via `.if LOWRES`.  Combine with GEOMDIRECT.
# NB: the kernel .bin rule can't see the -D change, so `make clean` when toggling.
# make ... HALFRES=1: render half-height (320x120) like LOWRES (half the fill)
# but DISPLAY at 320x240 by Blitter LINE-DOUBLING through the UNSCALED OP path.
# The OP hardware scaler blacks out under heavy multiroom fill; the unscaled
# path survives it. HALFRES turns on the LOWRES render/kernel side + adds the
# line-double display. `make clean` when toggling (kernel .bin caches -D).
ifdef HALFRES
LOWRES     := 1
CFLAGS     += -DHALFRES
CXXFLAGS   += -DHALFRES
ASFLAGS    += -DHALFRES
endif

# make ... LOWRES=1 OPPLAIN=1: SCALER-BISECT PROBE (2026-07-25).  Displays the
# 320x120 framebuffer through a PLAIN unscaled OP object (top 120 lines, black
# below) instead of the TYPE-1 scaled object — everything else identical.
# Splits "the OP scaler cannot survive render-time bus pressure" from "the
# renderer and the scan-out share a buffer"; see video.c build_object_list.
ifdef OPPLAIN
CFLAGS     += -DOPPLAIN
CXXFLAGS   += -DOPPLAIN
ASFLAGS    += -DOPPLAIN
endif

ifdef LOWRES
CFLAGS     += -DLOWRES
CXXFLAGS   += -DLOWRES
# startup.S's vblank stub carries the deadline-critical scaled-object repair
# (LOWRES && !HALFRES); it needs both flags to select that block.
ASFLAGS    += -DLOWRES
LOWRES_DEF := -dLOWRES=1
else
LOWRES_DEF := -dLOWRES=0
endif

# perf experiment: make ... NOFILL=1 assembles the geotex kernel WITHOUT the
# Blitter span launch (all transform/edge/span math still runs) — isolates
# GPU compute cost vs Blitter fill+wait cost. `make clean` when toggling.
ifdef NOSPAN
NOFILL := 1
endif
NOSPAN_DEF := -dNOSPAN=$(if $(NOSPAN),1,0)

ifdef NOFILL
NOFILL_DEF := -dNOFILL=1 -dHALFSPAN=$(if $(HALFSPAN),1,0)
else
NOFILL_DEF := -dNOFILL=0 -dHALFSPAN=$(if $(HALFSPAN),1,0)
endif

# M2 object set: room renderer + video + Blitter + input + room data.
ifdef SPAWNAT_ROOM
CFLAGS   += -DSPAWNAT_ROOM=$(SPAWNAT_ROOM) -DSPAWNAT_X=$(SPAWNAT_X) -DSPAWNAT_Y=$(SPAWNAT_Y) -DSPAWNAT_Z=$(SPAWNAT_Z) -DSPAWNAT_YAW=$(SPAWNAT_YAW)
CXXFLAGS += -DSPAWNAT_ROOM=$(SPAWNAT_ROOM) -DSPAWNAT_X=$(SPAWNAT_X) -DSPAWNAT_Y=$(SPAWNAT_Y) -DSPAWNAT_Z=$(SPAWNAT_Z) -DSPAWNAT_YAW=$(SPAWNAT_YAW)
endif

ifdef NOPCLIP
CFLAGS   += -DNOPCLIP
CXXFLAGS += -DNOPCLIP
endif

ifdef A10BG
CFLAGS   += -DA10BG
CXXFLAGS += -DA10BG
ASFLAGS  += -DA10BG
endif

ifdef OPMOVEM
CFLAGS   += -DOPMOVEM
CXXFLAGS += -DOPMOVEM
ASFLAGS  += -DOPMOVEM
endif

ifdef AUTOSPIN
CFLAGS   += -DAUTOSPIN
CXXFLAGS += -DAUTOSPIN
endif

ifdef CNTDUMP
CFLAGS   += -DCNTDUMP
CXXFLAGS += -DCNTDUMP
endif

ifdef POSDUMP
CFLAGS   += -DPOSDUMP
CXXFLAGS += -DPOSDUMP
endif

ifdef CURONLY
CFLAGS   += -DCURONLY
CXXFLAGS += -DCURONLY
endif

ifdef EARLYCON
CFLAGS   += -DEARLYCON
CXXFLAGS += -DEARLYCON
endif

ifdef VECDIAG
CFLAGS   += -DVECDIAG
CXXFLAGS += -DVECDIAG
endif

ifneq ($(BEACON_AT)$(VECDIAG),)
OBJS_BEACON := $(BUILD)/hangbeacon.o
endif
ifdef BEACON_AT
CFLAGS   += -DBEACON_AT=$(BEACON_AT)
CXXFLAGS += -DBEACON_AT=$(BEACON_AT)
ASFLAGS  += -DBEACON_AT=$(BEACON_AT)
endif
# ☠️ MUST be defined BEFORE the OBJS := below - OBJS is immediately expanded,
# so a definition further down the file reaches it EMPTY (the same trap that
# made a late `AUTOSTART := 1` never reach the compiler).
OBJS_OVL := $(if $(JOVL),$(BUILD)/dsp_ovl_blob.o)
OBJS := $(BUILD)/startup.o $(BUILD)/cpu68k.o $(BUILD)/main.o $(BUILD)/video.o \
        $(BUILD)/blit.o $(BUILD)/joypad.o $(BUILD)/vidpanel.o \
        $(BUILD)/gd_input.o $(BUILD)/gdbios.o \
        $(BUILD)/gpu.o $(BUILD)/gpu_blob.o \
        $(BUILD)/jerry.o $(BUILD)/dsp_blob.o $(OBJS_OVL) $(OBJS_BEACON)
# The console objects are needed by SKCONONLY too, not just NOGD: SKCONONLY
# defines SKUNK_CONSOLE (dbg_kv/skunk_init) WITHOUT compiling out the
# GameDrive path, which is what you want when the console is the instrument
# and the game must otherwise behave normally.
ifneq ($(NOGD)$(SKCONONLY),)
OBJS += $(BUILD)/skunk.o $(BUILD)/skunkglue.o $(BUILD)/skunkdbg.o
endif
ifdef MULTIROOM
OBJS += $(BUILD)/mrt_data.o
else
# single-room render paths embed the old rooms.bin / laramesh.bin
# (tools/tr2jag.cpp); MULTIROOM doesn't reference them, so it links neither.
OBJS += $(BUILD)/roomsdata.o $(BUILD)/laradata.o
ifdef GEOTEX
OBJS += $(BUILD)/room0data.o
else ifdef TEXROOM
OBJS += $(BUILD)/room0data.o
else ifdef TEXTURED
OBJS += $(BUILD)/texdata.o
endif
endif

TARGET := $(BUILD)/openlara.cof
BIN     := $(BUILD)/openlara.bin

all: $(TARGET)

# C is compiled by COBWEB's jcc68k -> jas --elf-obj (GNU-linkable ELF; the
# link, jaguar.ld and every .S object are untouched — docs/gnu-interop.md).
# The 32-bit mul/div runtime jcc68k calls (__mulsi3 & co) is the SAME
# divmod68k.S that served gcc. `make CCVERIFY=1` builds with gcc instead —
# the verification oracle (both images must behave identically in jagemu).
ifdef CCVERIFY
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
else
# ALL translation units on jcc68k since cobweb 73ac9dd (round-2 fixes: the
# libgcc helper ABI, .bss/dead-static emission, inline-asm passthrough,
# power-of-2 strength reduction). main.c's two long-long sites were
# restructured to 32-bit math (jcc68k makes 64-bit a hard error rather
# than silently truncating). Remaining known gap: text ~1.9x gcc -O2
# (their register-allocator follow-up).
$(BUILD)/%.o: %.c | $(BUILD)
	$(JCC68K) $< -o $(BUILD)/$*.jcc.s -I. -Idisc $(filter -D%,$(CFLAGS))
	$(JAS) $(BUILD)/$*.jcc.s --68000 --elf-obj -o $@
# main.c stays on gcc for PERFORMANCE (not correctness): all-jcc renders
# correctly but the game crawls — main.c is the per-frame 68k logic and
# jcc68k's ~1.9x text + __mulsi3 for non-power-of-2 scaling (y*320) is a
# per-frame tax there. Flips when cobweb's register-allocator follow-up
# lands. (Their own end-to-end verification was also 7-of-8-except-main.)
# JCCMAIN=1: build main.c with COBWEB's jcc68k instead of gcc.
# ☠️ The pin below says "for PERFORMANCE ... the game crawls", and that was
# true when jcc68k emitted ~1.9x gcc's text on the per-frame path. IT NO LONGER
# DOES: after the address-mode/peephole work (cobweb 9ed8042) main.c measures
# 97,392 B of .text against gcc's 98,689 - 0.99x, i.e. slightly SMALLER.
# ☠️☠️☠️ AND IT STILL DOES NOT WORK - THE SIZE WAS THE WRONG MEASUREMENT.
# The jcc68k build compiles, links, and comes out 1% SMALLER than gcc, and then
# CRASHES: run in jsim it executes 936,874 ILLEGAL INSTRUCTIONS with the 68k PC
# off at 0x1638E2 and 2 DSP divide-by-zeros, against 0 illegal / PC 0x00E2AC
# for the identical gcc build (2026-08-16, cobweb 9ed8042). So the pin STAYS,
# and the reason recorded above ("~1.9x text") is now the wrong reason: the gap
# is closed, correctness is not.
# ★★★★★ Size, and even a clean link, say nothing about whether code RUNS. The
# emulator's illegal-instruction counter answered in one shot what a byte
# comparison could not.
# ⬜ This flag is kept as the REPRODUCTION for cobweb: build with JCCMAIN=1 and
# run the ROM in jagemu to see it. Narrowing it means bisecting a 33,587-line
# asm output - start by diffing which TUs jcc68k already passes (7 of 9) for
# the construct main.c uses that they do not.
ifdef JCCMAIN
$(BUILD)/main.o: main.c | $(BUILD)
	$(JCC68K) $< -o $(BUILD)/main.jcc.s -I. -Idisc $(filter -D%,$(CFLAGS))
	$(JAS) $(BUILD)/main.jcc.s --68000 --elf-obj -o $@
else
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) $(MAINCFLAGS) -c $< -o $@
endif
# joypad.c/gd_input.c pinned to gcc: controls went DEAD on hardware with
# them on jcc68k (user 2026-07-20; jagemu's injected input can't see the
# real strobe-scan timing). Suspect MMIO access width/ordering in the pad
# strobe or GD BIOS calls — narrow with single-TU A/B flashes, then report.
$(BUILD)/hangbeacon.o: hangbeacon.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/joypad.o: joypad.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gd_input.o: gd_input.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# jerry.c pinned to gcc: under jcc68k Lara's HEAD renders turned (last angle
# of the pose marshal corrupted — reproduced in jagemu, fix1 vs oracle
# 2026-07-20). Repro asm saved; narrowing for the cobweb report.
$(BUILD)/jerry.o: jerry.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# make ... GCCHOT=1 : build blit.c, gpu.c and video.c with gcc instead of jcc68k.
#
# ☠☠ THE PERFORMANCE RATIONALE IS REVOKED (2026-09-05). Kept as a DIFFERENTIAL
# INSTRUMENT, which is the only thing it is now good for. Three independent
# refutations, all of which were already in this repo and none of which had been
# joined up:
#
#  1. The number it rested on is dead. The original rationale said "blit.c is
#     where the 68k pc-histogram puts ~60% of awake time" -- a 2026-07-24,
#     320x60-era measurement. ENGINE_HEATMAP.md:5 retracts that whole class
#     ("every previous conclusion about the 68000 was measured at 320x60"), and
#     the 160x120 histogram puts blit_band at 5.78% of awake, not 60%. The
#     largest category is software mul/div helpers at 13.6%, which the old
#     rationale does not mention at all.
#  2. The predicted benefit was already measured at ZERO. ENGINE_HEATMAP.md:51
#     and JAGUAR_FINDINGS.md:56, independently: GCCHOT cut 68k instructions 22%
#     for 0.00% fps. 68k CYCLES are nearly free; only 68k DRAM traffic during
#     Tom's render costs anything, and this moves none.
#  3. ☠ The only historical USE was not a codegen workaround. AUTORUN_STATE.md:347
#     -- `ship_p136` used GCCHOT=1 to dodge the JAS destination-relocation bug
#     (24 sites in video.c storing into the 68000 exception vectors, black on
#     every pad), fixed in cobweb `ba9c680`. demo33 at the 9da2f99 pin scans 0
#     `23f9` stores below $4000, so that bug is not present.
#     ⚠ DO NOT read "ship_p136 shipped with GCCHOT=1" as evidence that jcc68k
#     miscompiles video.c. It reads exactly like that and it is not what happened.
#
# ⇒ Do not enable this for speed. It exists so a gcc-vs-jcc68k differential can
# be run on the three hot TUs as a group (see CCVERIFY). And note that offline
# can FIND a codegen difference but never CLEAR one: two of the four historical
# per-TU pins were silicon-only and would have passed any offline oracle.
#
# ★ Lesson, and the reason this comment is long: the retraction that killed the
# ~60% reached every document that QUOTED a 68000 number and not the build rule
# whose COMMENT carried one. Date the number inside the decision -- written as
# "~60% (2026-07-24, 320x60)" it would have refuted itself the moment the
# regime changed.
ifdef GCCHOT
$(BUILD)/blit.o: blit.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gpu.o: gpu.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/video.o: video.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
endif
endif

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# skunk console lib is rmac/smac syntax (Tursi's skunk_jcp), not GNU as -
# assemble with rmac -fe to a linkable ELF object.
$(BUILD)/skunk.o: skunk.s | $(BUILD)
	$(RMAC) -fe $< -o $@
$(BUILD)/skunkglue.o: skunkglue.s | $(BUILD)
	$(RMAC) -fe $< -o $@

# GPU kernel: rmac assembles at SRAM origin $F03000 -> raw blob,
# embedded by gpu_blob.S via .incbin. The blob is an explicit
# prerequisite so a kernel edit relinks (make won't see the .incbin).
$(BUILD)/gpu_spanfill.bin: gpu_spanfill.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/gpu_jvdec.bin: gpu_jvdec.gas | $(BUILD)
	$(JAS) -d JVMIN=$(if $(JVMIN),1,0) $< -o $@ --gpu

$(BUILD)/gpu_geomwalk.bin: gpu_geomwalk.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/gpu_geomxform.bin: gpu_geomxform.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

# ☠️ THESE THREE USED TO NEED rmac. jas refused them with four bug-13
# write-after-write errors each - a FALSE POSITIVE: its hazard pass was
# straight-line and treated the two arms of a branch as sequential, so
# r24..r27 loaded in one arm looked like a race with the same registers
# loaded in the other. Fixed upstream (cobweb 86413ca: an unconditional jump
# ends the shadow window), and jas now emits BYTE-IDENTICAL output to rmac for
# all three - verified by cmp, not assumed. That makes the whole kernel side
# cobweb's, and drops rmac from the build entirely.
# ★ It matters beyond tidiness: rmac's upstream repo (ggnkua/rmac) has
# VANISHED - it 404s - so every container build was already reaching for a
# mirror to fetch an assembler that no longer has a home.
$(BUILD)/gpu_geomdirect.bin: gpu_geomdirect.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu $(call jasd,$(LOWRES_DEF))

$(BUILD)/gpu_textured.bin: gpu_textured.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/gpu_bltex.bin: gpu_bltex.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/dsp_pose.bin: dsp_pose.das | $(BUILD)
	$(JAS) $< -o $@ --dsp $(call jasd,$(LOWRES_DEF) -d NOSOUND=$(if $(NOSOUND),1,0) -d AUDIOLITE=$(if $(AUDIOLITE),1,0) -d JDRAIN=$(if $(JDRAIN),1,0) -d NEARLOW=$(if $(NEARLOW),1,0) -d JCENT=$(if $(JCENT),1,0) -d JOVL=$(if $(JOVL),1,0))
	@sz=$$(stat -c%s $@); if [ $$sz -gt 4192 ]; then \
	  echo "!!! dsp_pose.bin $$sz bytes OVERLAPS MBLK at F1C060 (max 4192 = F1C060-F1B000; OUT_D is dead since direct-DRAM pose)"; \
	  rm -f $@; exit 1; fi

$(BUILD)/dsp_blob.o: dsp_blob.S $(BUILD)/dsp_pose.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c dsp_blob.S -o $@

# JERRY OVERLAY (JOVL): assembled to run at `ovl_area` - the first free byte
# after the resident kernel - so its base address depends on how big
# dsp_pose.bin came out.  Compute it here and prepend it as an .equ; jas -d
# only feeds .if conditionals, it does not create symbols usable by .org.
$(BUILD)/dsp_ovl_ent.bin: dsp_ovl_ent.das $(BUILD)/dsp_pose.bin | $(BUILD)
	@base=$$(printf '%X' $$(( 0xF1B000 + $$(stat -c%s $(BUILD)/dsp_pose.bin) ))); 	 printf 'OVLBASE\t.equ\t$$%s\n' "$$base" > $(BUILD)/ovlgen.das; 	 cat dsp_ovl_ent.das >> $(BUILD)/ovlgen.das; 	 echo "overlay base = \$$$$base (resident $$(stat -c%s $(BUILD)/dsp_pose.bin) bytes)"
	$(JAS) $(BUILD)/ovlgen.das -o $@ --dsp
	@rsz=$$(stat -c%s $(BUILD)/dsp_pose.bin); osz=$$(stat -c%s $@); 	 if [ $$(( rsz + osz )) -gt 4192 ]; then 	   echo "!!! resident $$rsz + overlay $$osz > 4192 - overruns MBLK at F1C060"; 	   rm -f $@; exit 1; fi; 	 echo "overlay $$osz bytes; $$(( 4192 - rsz - osz )) bytes still free"

$(BUILD)/dsp_ovl_blob.o: dsp_ovl_blob.S $(BUILD)/dsp_ovl_ent.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c dsp_ovl_blob.S -o $@

NOMUL_DEF := -dNOMUL=$(if $(NOMUL),1,0)
NODIV_DEF := -dNODIV=$(if $(NODIV),1,0)
NOSTORE_DEF := -dNOSTORE=$(if $(NOSTORE),1,0)
SHADEPASS_DEF := -dSHADEPASS=$(if $(SHADEPASS),1,0)
NOCULL_DEF := -dNOCULL=$(if $(NOCULL),1,0)
# STAGEDIET: early backface cull from a 12B per-face plane prefix. Needs bins
# built with FACE_PLANES=1 (extractor) AND -DSTAGEDIET in CFLAGS (main.c
# runtime blob builders + record strides). CAMLOC reuses the PROFGPU var
# block, so the pair is forbidden.
# make TIMESTEP=1 [TURNDIV=n]: decouple game logic from the RENDER rate.
# WALK_SPEED is "units per frame" and lanim_step advances per frame, so at
# 7.50 fps Lara ran at 25% of the speed the 30 Hz logic intends.  TURNDIV
# divides ONLY the turn scaling if a fully-scaled turn feels twitchy.
ifdef OPDBL
CFLAGS   += -DOPDBL
CXXFLAGS += -DOPDBL
endif

ifdef OPMIN
CFLAGS   += -DOPMIN
CXXFLAGS += -DOPMIN
endif

# make ANIMRATE=1: drive Lara's animation cadence from the anim record's own
# frameRate byte (rt) instead of hand-picked step= arguments.  Needs TIMESTEP.
# TURNRUN / TURNWALK: override Lara's turn rate. ☠️ UNITS CHANGED 2026-08-02:
# these are now EIGHTHS of a SINTAB unit per 30 Hz tick (SINTAB = 256 per full
# turn), not whole units, so the rate can be tuned finely. 34 = TR1-authentic
# 180 deg/s, 24 = the old default (126 deg/s), 12 = current default (63 deg/s).
# TURNRAMP = ticks to ease in to full rate (default 4).
# (old comment: units per 30 Hz tick, 256 per
# full circle).  TR1 is 4.27 running / 2.13 walking; lower reads calmer at a
# low frame rate because each displayed frame carries several ticks of turn.
ifdef TURNRUN
CFLAGS   += -DTURN_RUN_TR1=$(TURNRUN)
CXXFLAGS += -DTURN_RUN_TR1=$(TURNRUN)
endif
ifdef TURNWALK
CFLAGS   += -DTURN_WALK_TR1=$(TURNWALK)
CXXFLAGS += -DTURN_WALK_TR1=$(TURNWALK)
endif
ifdef TURNRAMP
CFLAGS   += -DTURN_RAMP=$(TURNRAMP)
CXXFLAGS += -DTURN_RAMP=$(TURNRAMP)
endif

ifdef FPSBEACON
CFLAGS   += -DFPSBEACON
CXXFLAGS += -DFPSBEACON
endif

# PADMUTE=1: force the in-game pad word to 0 (menus/boot unaffected).  An fps
# A/B is only valid when both arms render the SAME SCENE, and a pad resting on
# a direction silently walks Lara into a different room - which invalidated a
# whole SYNCDRAIN A/B on 2026-08-10.  Measurement arm only, never ship.
ifdef PADMUTE
CFLAGS   += -DPADMUTE
CXXFLAGS += -DPADMUTE
endif

# GPUSPLIT=1: paint Tom's transform/raster split ON SCREEN (X% R% halflines/frame
# scanlines/frame).  Needs PROFGPU=1, which FORBIDS STAGEDIET=1 - so this is a
# profiling arm, not the ship build; read RATIOS, never absolute counters.
# ☠️ The rig is a GameDrive, not a Skunkboard: dbg_kv output goes nowhere, the
# screen is the only instrument.
ifdef GPUSPLIT
CFLAGS   += -DGPUSPLIT
CXXFLAGS += -DGPUSPLIT
endif

# ODRAWS=1: SAFE overdraw probe - kernel totals span pixels + span count into
# GPU SRAM, the 68k paints them.  Answers "how many of the Blitter's pixels are
# redundant".  ☠️ Uses the PROFGPU block, so it needs STAGEDIET off; measurement
# arm only.  ☠️ The older ODRAW=1 CANNOT answer this - it is a DRAM
# read-modify-write (emulator-only) and the offline harness never rasterises.
ifdef ODRAWS
CFLAGS   += -DODRAWS
CXXFLAGS += -DODRAWS
endif

# HOLEVIS=1: paint the per-frame clear WHITE so uncovered pixels are glaring.
# A coverage gap normally reads as "shadow" against a dark cave - that is how
# the r30 hole hid for the whole project. Pair with ROOMTOUR=1 DBGROOM=1 to map
# all 38 rooms in one recording. Diagnostic arm, never ship.
ifdef HOLEVIS
CFLAGS   += -DHOLEVIS
CXXFLAGS += -DHOLEVIS
endif

# HW_TESTCARD=1: paint a static colour test card straight after video_init and
# stop there. It exists to tell "the CONSOLE is broken" apart from "the VIDEO
# CHAIN is broken" on the rig - a dead capture once faked NINE black boots,
# including a control that was known-lit, and `OK!` + black means the video
# path, not the Jaguar. A card whose pixels are ASSERTABLE settles that in one
# upload instead of a conversation about what the TV looks like.
# ☠️ The green ramp is 6 bits and the red/blue ramps are 5, deliberately
# ASYMMETRIC: Jaguar RGB16 is R<<11 | B<<6 | G with GREEN SIX BITS UNSHIFTED at
# 5-0 (measured in cobweb 8d09c43). Feeding 0..31 to all three would never
# exercise green's sixth bit, so a 5-bit-green packing would pass every check.
ifdef HW_TESTCARD
CFLAGS   += -DHW_TESTCARD
CXXFLAGS += -DHW_TESTCARD
endif

# ALLVIS=1: a room whose portal window computes EMPTY is dropped entirely and
# leaves a doorway-shaped hole. Draw it full-screen instead - the same
# conservative rule that made NOPCLIP free. Pair with HOLEVIS to verify.
ifdef ALLVIS
CFLAGS   += -DALLVIS
CXXFLAGS += -DALLVIS
endif

# ENEMIES=1: the bat/wolf/bear models, their homing AI, Lara's health and the
# fire button. ☠️ THIS WAS NEVER PLUMBED - main.c says so in its own comment
# ("ENEMIES itself is not plumbed in the Makefile at all, so that block has
# never been built"), which is why no enemy has ever appeared in any build even
# though the models, the AI and the draw loops are all written. Needs ENTITIES.
ifdef ENEMIES
CFLAGS   += -DENEMIES
CXXFLAGS += -DENEMIES
endif

# AUTOFIRE=1: hold the fire button down for us so a capture can verify Lara's
# pistols with nobody on the pad. Test arm only, never ship.
ifdef AUTOFIRE
CFLAGS   += -DAUTOFIRE
CXXFLAGS += -DAUTOFIRE
endif

# VIEWH=N: draw the world into the TOP N lines only and leave the rest of the
# buffer black (where a Doom-style status bar would go). The buffer, the OP
# object and the display window do not move. A crop, not a squash - see the
# comment in video.h. ☠️ The value must reach BOTH the C projection and the
# kernel's own copy of the constants, hence the GEOTEX_DEFS entry below too.
# HUDTEXT=1: draw the health/kills numbers ("1000 K00") in the top-left.
# ☠️ OFF BY DEFAULT because menu_text is 68000 pixels, one byte per store with
# four bounds compares, EVERY FRAME - measured at ~5 fps per 25 glyphs. Turn it
# on for a test arm that needs to see the kill count; do not ship it as text.
# NOBRIDGEDRAW=1: diagnostic - do not submit the 12 bridge models in room 22.
# Prices what the bridges cost, which is the ceiling on any bridge-culling work.
# ENTVIEWCULL=1: skip entity models that are behind the camera.
ifdef ENTVIEWCULL
CFLAGS   += -DENTVIEWCULL
CXXFLAGS += -DENTVIEWCULL
endif
ifdef NOBRIDGEDRAW
CFLAGS   += -DNOBRIDGEDRAW
CXXFLAGS += -DNOBRIDGEDRAW
endif
ifdef HUDTEXT
CFLAGS   += -DHUDTEXT
CXXFLAGS += -DHUDTEXT
endif

# VRESN=N: the RESOLUTION DIAL - render N lines and let the OP scaler stretch
# them over the same 240-line window (VRES60 generalised). Unlike VIEWH this
# keeps the FULL SCREEN and the full field of view; it spends vertical
# resolution instead of screen area, and it measures FASTER for it.
# ☠️ The ladder is fixed by HARDWARE, not by taste: the OP's VSCALE is 3.5
# fixed point, so 240/N must be a multiple of 1/32 <=> 7680/N whole. 72 is NOT
# expressible - that is why the VIEWH ladder and this one differ.
VRESN_LADDER := 96 80 64 60
ifdef VRESN
ifeq ($(filter $(VRESN),$(VRESN_LADDER)),)
$(error VRESN=$(VRESN) cannot fill the 240-line window: VSCALE=7680/N must be a \
whole number (OP VSCALE is 3.5 fixed point). Available: $(VRESN_LADDER))
endif
ifndef LOWRES
$(error VRESN=$(VRESN) requires LOWRES=1 (it is the OP-scaler display path))
endif
CFLAGS   += -DVRESN=$(VRESN)
CXXFLAGS += -DVRESN=$(VRESN)

endif

# ☠☠ THIS BLOCK MUST LIVE OUTSIDE `ifdef VRESN`.  It was first written just
# after the VRESN CFLAGS append, which put it INSIDE that conditional: with
# QUALITY=pretty (no VRESN) the CFLAGS line never ran, so -DHRESN reached jas
# and jcc68k but NOT gcc, and main.c kept VIEW_W==320 while the kernel used
# CENTER_X=80.  The ROM still differed from its control, so "the flag landed"
# looked true.  ★ A flag is landed only when EVERY define path has it -- this
# Makefile has three (CFLAGS/gcc, JCCDEFS/jcc68k, GEOTEX_DEFS/jas).
# HRESN=N: the HORIZONTAL dial.  Render only the leftmost N columns of each
# RENDER_W-byte row and widen the PIXEL CLOCK (VMODE PWIDTH) so they fill the
# active line.  The STRIDE does not change, so the title/ring/loading/FMV paths
# (all of which assume a 320-byte row) are untouched and BSS stays put -- which
# is why this does NOT re-roll the A10 boot lottery.
# ☠ NOT the OP horizontal scaler: that holds the bus ~15-20x and blacks the
# display.  PWIDTH is the opposite and is a bus WIN.  [HW] jag_quake ships
# VMODE=$0EC7 (PWIDTH 7) since 2026-08-12.
# ☠ Ladder is closed on purpose: the Blitter width field encodes only
# 2^e x (4+m)/4, so 160 and 80 are expressible and e.g. 72 is NOT -- and it
# fails SILENTLY.  Refuse anything off it rather than emit a wrong picture.
HRESN_LADDER := 160 80
ifdef HRESN
ifeq ($(filter $(HRESN),$(HRESN_LADDER)),)
$(error HRESN=$(HRESN) is not expressible: the Blitter width field encodes only \
2^e x (4+m)/4 and PWIDTH must divide the line cleanly. Available: $(HRESN_LADDER))
endif
CFLAGS   += -DHRESN=$(HRESN)
CXXFLAGS += -DHRESN=$(HRESN)
endif

# TRAPFLOOR=1: TR1's collapsing tiles (the 2 in r19). VERIFIED on silicon with
# enemies compiled OUT: she walks on, the tile lets go, she falls through and
# LANDS (frame-to-frame change settles to 3.0).
# ☠️ The "continuous fall damage" this was first gated for was a MISDIAGNOSIS -
# it was the r24 wolf hunting her. The control that settled it: stand still on
# solid ground two sectors from any tile and health still ran 0200 -> 0000.
# Isolate the subsystem before blaming it.
ifdef TRAPFLOOR
CFLAGS   += -DTRAPFLOOR
CXXFLAGS += -DTRAPFLOOR
endif

# FITSTEP=1: apply TR1's headroom rule to the AUTOMATIC 256 STEP-UP as well.
# run 48 put climb_fits() on the three CLIMB entries (vault / jump-reach arm /
# airborne grab), which left the one path that never asks to climb: a <=256 step
# is taken by the walk itself, so she still walked up into gaps a third of her
# height. Behind a flag because unlike run 48's change this one sits in the MOVE
# GATE, on every walking frame and both axes, so it has to be A/B'd rather than
# reasoned about.
ifdef FITSTEP
CFLAGS   += -DFITSTEP
CXXFLAGS += -DFITSTEP
endif

# DREWVIS=1: expose g_visrooms / g_drewrooms, bitmasks of the rooms that passed
# portal visibility and that were actually submitted this frame. Diagnostic for
# coverage holes - peek them with tools/probe_spot.py instead of A/B-ing culls.
ifdef DREWVIS
CFLAGS   += -DDREWVIS
CXXFLAGS += -DDREWVIS
endif

# HOPDEPTH=N: portal-visibility depth in hops (default 3). Value-carrying.
ifdef HOPDEPTH
CFLAGS   += -DHOPDEPTH=$(HOPDEPTH)
CXXFLAGS += -DHOPDEPTH=$(HOPDEPTH)
endif

# MVDIAG=1: expose g_mvveto/g_mvnf - which move-gate clause refused a blocked
# step. Diagnostic; the shipping gate is untouched (the clauses are re-evaluated
# afterwards, never restructured).
ifdef MVDIAG
CFLAGS   += -DMVDIAG
CXXFLAGS += -DMVDIAG
endif

# GYMTEST=1 / CAVETEST=1: boot straight into Lara's Home / the Caves, skipping
# the title ring. ☠️☠️ BOTH #ifdefs HAVE EXISTED IN main.c FOR AGES AND NEITHER
# WAS EVER PLUMBED HERE - so -DGYMTEST never reached the compiler and a GYMTEST
# arm silently rendered THE CAVES. It was caught by diffing the arm against a
# caves build and getting ZERO differing pixels out of 25,600. Same class of
# bug as ENEMIES, which was unplumbed for months: never test "is X wired" by
# grepping for the flag NAME - check that it reaches the compile line.
# AUTOGYM=1: auto-select Lara's Home from the ring, the way a real A-press on
# page 4 does. Unlike GYMTEST this goes through the whole menu exit, so it is
# the only offline test that covers the path the USER actually reported broken.
# Needs a build WITHOUT GYMSD (GYMSD makes the menu refuse the item).
ifdef AUTOGYM
CFLAGS   += -DAUTOGYM
CXXFLAGS += -DAUTOGYM
endif

# PHRASECLEAR=1: fill the per-frame screen clear in PHRASE mode (8 bytes/tick)
# instead of pixel mode (1 byte/tick). The clear is the biggest single Blitter
# shape in the level - 21.2% of all transfer ticks by --blit-histogram.
ifdef PHRASECLEAR
CFLAGS   += -DPHRASECLEAR
CXXFLAGS += -DPHRASECLEAR
endif

# NOENTDRAW=1: MEASUREMENT ARM. Skip DRAWING bats, wolves and the bear - the
# blob build and the displist entry both - while leaving their AI, movement and
# collision running untouched (those live in the entity update, not the draw
# pass). This is the CEILING of "turn every enemy into a sprite": a billboard
# costs 1 face against the wolf's 251, so no sprite scheme can beat the frame
# time this arm reports, and it costs no art pipeline to find out.
# ☠️ Two priors say do NOT skip this measurement and go straight to sprites:
#   1. Flat-merge cut room 34's faces by 36% for ZERO fps - face count alone is
#      not automatically the lever here.
#   2. The believed "25% enemy cost" turned out to be EIGHT GLYPHS of 68k HUD
#      text hiding behind the same `#ifdef ENEMIES`, so no enemy A/B ever
#      separated them. Removing that text alone was worth +56%.
# ☠️ NEVER SHIP IT. Enemies are invisible but still bite.
ifdef NOENTDRAW
CFLAGS   += -DNOENTDRAW
CXXFLAGS += -DNOENTDRAW
endif

ifdef GYMTEST
CFLAGS   += -DGYMTEST
CXXFLAGS += -DGYMTEST
endif
ifdef CAVETEST
CFLAGS   += -DCAVETEST
CXXFLAGS += -DCAVETEST
endif

# GYMSD=1: leave Lara's Home OUT of the image (it belongs on the card).
# ☠️ 316,336 bytes of gym_* blobs were linked into every CAVES ROM for a level
# that can never be resident at the same time as the caves. Selecting Lara's
# Home in a GYMSD build is refused rather than crashing - the pointers are
# NULL stubs - until the loader that streams it from the SD exists.
ifdef GYMSD
CFLAGS   += -DGYMSD
CXXFLAGS += -DGYMSD
ASFLAGS  += -DGYMSD
endif

# LEVSD=1: BOTH level sets on the card, sharing one arena. This is the one that
# actually frees DRAM.
# ☠️ GYMSD ALONE IS NET-ZERO. Deleting the mansion's 254 KB of rodata means
# needing a 254 KB buffer to load it into, and the Caves cannot serve as that
# buffer because they are ROM-resident and cannot be reclaimed. Only when both
# sets are on the card can one arena, sized for the LARGER (754,400 B), hold
# whichever is live - which frees the smaller, 254,400 B, and takes ~1,008,800 B
# out of the ROM image.
# ★ The ROM half stands on its own: `jaggd -ux` was measured failing at about
# 1.6 MB (0 of 6 at or above, 203 of 327 below) and this ROM is 1.47 MB, 92% of
# that cliff - which is why the GameDrive wedges on roughly every other flash
# and needs a power cycle. ~0.5 MB clears it.
# ☠️ ASFLAGS IS LOAD-BEARING: the stubs live in mrt_data.S, which is ASSEMBLED.
# Without it the C would take the arena path while the blobs stayed linked -
# a build that is both bigger AND broken, and whose ROM would still differ, so
# "the flag landed" would read true.
ifdef LEVSD
CFLAGS   += -DLEVSD
CXXFLAGS += -DLEVSD
ASFLAGS  += -DLEVSD
endif

# GUNS=1: Lara draws pistols (PAD_Z). The gun HANDS are LARA_PISTOLS' meshes
# 10/13, hung off the hand matrices the skinner already computes - her skin,
# face grouping and baked LPLANES are untouched. Assets: MRT_GUNPATCH=1
# python3 tools/tr2jag_multiroom.py  (appends the pistol objtex to the atlas
# and writes mrt_gun.h).
# RUNSPEED=<n>: Lara's run velocity, units per 30Hz frame. Default 47 = TR1's
# own anim-0 record. The old (wrong) constant was 140; kept reachable so the
# feel can be A/B'd on a TV instead of argued from a table.
ifdef RUNSPEED
CFLAGS   += -DRUNSPEED=$(RUNSPEED)
CXXFLAGS += -DRUNSPEED=$(RUNSPEED)
endif

# PADPROBE=1: print the RAW controller matrix word on screen. Press each key on
# a REAL pad and read which bit it asserts - GDPAD cannot answer this, it
# injects the final mask and never touches the matrix. This is how the X/Y/Z
# and keypad bit table stops being the standard layout and becomes measured.
ifdef PADPROBE
CFLAGS   += -DPADPROBE
CXXFLAGS += -DPADPROBE
endif

ifdef GUNWIND
CFLAGS   += -DGUNWIND
CXXFLAGS += -DGUNWIND
endif
ifdef GUNDIAG
CFLAGS   += -DGUNDIAG
CXXFLAGS += -DGUNDIAG
endif
# GUNDBG=1: start with the pistols ALREADY DRAWN, so a plain capture shows them
# without driving the pad. (It began life as a winding discriminator; winding
# turned out to be innocent - see gun_pose_hands in main.c.)
ifdef GUNDBG
CFLAGS   += -DGUNDBG
CXXFLAGS += -DGUNDBG
endif
ifdef GUNS
CFLAGS   += -DGUNS
CXXFLAGS += -DGUNS
endif

VIEWH_LADDER := 108 100 96 90 84 80 72 60
ifdef VIEWH
ifeq ($(filter $(VIEWH),$(VIEWH_LADDER)),)
$(error VIEWH=$(VIEWH) is not on the ladder ($(VIEWH_LADDER)). jas takes -d only \
for .if comparisons - a define is NOT a symbol - so gpu_geotex.gas enumerates \
every band height. Add a branch there and extend VIEWH_LADDER here. \
Falling through would build a full-height kernel against a short-band C side)
endif
CFLAGS   += -DVIEW_H=$(VIEWH)
CXXFLAGS += -DVIEW_H=$(VIEWH)
endif

# ENEMYTEX=1: real per-face enemy skins instead of the flat tone swatch.
# Half-res packing (MRT_ENEMYTEX_STEP=2) is what made it fit; verified on
# silicon 2026-08-11. Assets: MRT_ENEMYTEX=1 python3 tools/tr2jag_multiroom.py
ifdef ENEMYTEX
CFLAGS   += -DENEMYTEX
CXXFLAGS += -DENEMYTEX
endif

# AUTOMENU=1: drive the pause menu from the frame counter so a capture can see
# it with nobody on the pad. Test arm only.
ifdef AUTOMENU
CFLAGS   += -DAUTOMENU
CXXFLAGS += -DAUTOMENU
endif

ifdef ANIMRATE
CFLAGS   += -DANIMRATE
CXXFLAGS += -DANIMRATE
endif

ifdef TIMESTEP
CFLAGS   += -DTIMESTEP $(if $(TURNDIV),-DTURNDIV=$(TURNDIV)) $(if $(SPEEDDIV),-DSPEEDDIV=$(SPEEDDIV)) $(if $(ANIMDIV),-DANIMDIV=$(ANIMDIV))
CXXFLAGS += -DTIMESTEP $(if $(TURNDIV),-DTURNDIV=$(TURNDIV)) $(if $(SPEEDDIV),-DSPEEDDIV=$(SPEEDDIV)) $(if $(ANIMDIV),-DANIMDIV=$(ANIMDIV))
endif

ifdef HOPDIAL
CFLAGS   += -DHOPDIAL $(if $(HOPBOOT),-DHOPBOOT=$(HOPBOOT))
CXXFLAGS += -DHOPDIAL $(if $(HOPBOOT),-DHOPBOOT=$(HOPBOOT))
endif
ifdef FARDIAL
$(error FARDIAL retired 2026-07-21: measured NULL on silicon; the kernel far-cull block + FARD var were removed to fund XCULL alongside SHADEPASS (see BUCKET_CAMPAIGN.md))
endif
ifdef QUIETFPS
CFLAGS   += -DQUIETFPS
CXXFLAGS += -DQUIETFPS
endif
ifdef ABLADDER
CFLAGS   += -DABLADDER $(if $(ABBOOT),-DABBOOT=$(ABBOOT))
CXXFLAGS += -DABLADDER $(if $(ABBOOT),-DABBOOT=$(ABBOOT))
endif
# PIPELINE: frame N's last Tom batch overlaps frame N+1's 68k logic
# (deferred sync+flip; see g_tominflight in main.c).
ifdef PIPELINE
PIPESTAGE ?= 2
CFLAGS   += -DPIPELINE -DPIPESTAGE=$(PIPESTAGE)
CXXFLAGS += -DPIPELINE -DPIPESTAGE=$(PIPESTAGE)
endif
# STATICS: bins built with the extractor's STATICS=1 (static meshes baked into
# room geometry) push rooms 13/26 past the 512-vert kernel vertex cache —
# -DSTATICS grows gpu.c's vtxcache to 768 verts. Data-only otherwise.
# PACEPROBE: pacing-campaign discriminator counters (pp_wait/span/safe/blit/
# mid + synk) streamed with the hl_* block. PROFILE-only instrumentation.
ifdef PACEPROBE
CFLAGS   += -DPACEPROBE
CXXFLAGS += -DPACEPROBE
endif

ifdef STATICS
CFLAGS   += -DSTATICS
CXXFLAGS += -DSTATICS
endif

# JLOOPS=1 (2026-07-25): stream Jerry's main_loop heartbeat (LOOP_COUNT in DSP
# SRAM at $F1C32C) alongside fpsT, with the block's render count, so silicon
# can answer whether Jerry keeps up.  jagemu cannot: its DSP accounting is
# self-contradictory (see the Jerry commit).  Needs SKUNK_CONSOLE (NOGD=1).
# SLIVERW/SLIVERH=N (2026-07-25): portal-sliver cull thresholds (defaults
# 16 px wide / 6 px tall).  A neighbour room seen through a doorway smaller
# than this is skipped entirely.  The LAST draw-distance-family knob that
# trades picture for Tom cycles — the room far clip is inert at hopcap=1.
ifdef SLIVERW
CFLAGS   += -DSLIVERW=$(SLIVERW)
CXXFLAGS += -DSLIVERW=$(SLIVERW)
endif
ifdef SLIVERH
CFLAGS   += -DSLIVERH=$(SLIVERH)
CXXFLAGS += -DSLIVERH=$(SLIVERH)
endif

# FARCLIP=N (2026-07-25): room-level far-cull distance (default 9000).
# Draw-distance experiment now that Tom is 90.3% saturated — the portal-hop
# dial (HOPBOOT) is already at its tight end, so this is what is left.
ifdef FARCLIP
CFLAGS   += -DFARCLIP=$(FARCLIP)
CXXFLAGS += -DFARCLIP=$(FARCLIP)
endif

ifdef JLOOPS
CFLAGS   += -DJLOOPS
CXXFLAGS += -DJLOOPS
endif
STAGEDIET_DEF := -dSTAGEDIET=$(if $(STAGEDIET),1,0)
ifdef STAGEDIET
ifdef PROFGPU
$(error STAGEDIET=1 and PROFGPU=1 are mutually exclusive: CAMLOC reuses $$F03EF0-FF)
endif
CFLAGS   += -DSTAGEDIET
CXXFLAGS += -DSTAGEDIET
endif
ifdef TRAPEZOID
ifdef SHADEPASS
$(error TRAPEZOID=1 and SHADEPASS=1 are mutually exclusive (kernel byte budget, first-proof rule — see TRAPEZOID_CAMPAIGN.md))
endif
ifdef RUNHIST
$(error TRAPEZOID=1 and RUNHIST=1 are mutually exclusive (kernel byte budget))
endif
endif
# BUCKET campaign face guards (see BUCKET_CAMPAIGN.md):
#   XCULL=1  reject faces fully left/right of the clip window before the
#            y-walk (they otherwise walk up to 240 empty-span rows).
#            Fits alongside SHADEPASS since 2026-07-21 (FARDIAL retirement
#            + XCULL-gated u/v-init indexed-store diet paid the ~44B gap;
#            SHADEPASS=1 STAGEDIET=1 XCULL=1 BEXIT=1 kernel = 3668/3680).
#   BEXIT=1  abort face staging at the first behind/far-sentinel vertex
#            (saves the rest of the stage; frees ~14B net). Fits any config.
#   ROWDIET=1 (PERFHUNT 2026-07-22): row-loop diet — per-RUN DIVCTRL
#            toggles + clip window in r4/r21 + off==0 fall-through
#            (multiply path out-of-line).  Requires TRAPEZOID=0 (r4/r21).
#
# KERNEL BYTE BUDGET — size it against the SHIPPED PLAY FLAG SET, not a subset.
# The play config is MULTIROOM GEOMDIRECT SHADEPASS JERRYPOSE STAGEDIET PIPELINE
# PIPESTAGE=2 HOPDIAL HOPBOOT XCULL BEXIT ROWDIET STATICS BANKDIET, and BANKDIET
# + ROWDIET together are worth 136 bytes (movefa vs movei residents + the row
# loop diet).  Measured 2026-07-25:
#     play set            = 3532 / 3680     (148 B free)
#     play set + UVNEG    = 3548 / 3680     (132 B free)
#     same set, BANKDIET and ROWDIET OFF    = 3668, and +UVNEG = 3684 = OVER.
# The 3668 figure above and the "UVNEG is 4 B over the ceiling" note in 53f5d84
# both came from that BANKDIET/ROWDIET-off kernel.  ==> UVNEG is NOT byte-blocked
# on the shipping config; it has 132 B of headroom.  Rebuild before believing any
# "N bytes over" claim, and record which flags the number was measured under.
ifdef RUNBATCH
ifndef BANKDIET
$(error RUNBATCH=1 requires BANKDIET=1 (RUNC/BATCHC live in the alternate bank))
endif
ifdef TRAPEZOID
$(error RUNBATCH=1 and TRAPEZOID=1 are incompatible (same batching machinery))
endif
ifdef DIVHIDE
$(error RUNBATCH=1 and DIVHIDE=1 are incompatible (imul32 r27/r28 WAW vs cooking divides))
endif
ifdef NOFILL
$(error RUNBATCH=1 and NOFILL=1 are incompatible (batch path owns the launch))
endif
endif

ifdef BANKDIET
ifdef TRAPEZOID
$(error BANKDIET=1 and TRAPEZOID=1 are incompatible (tz reads RUNC from SRAM; BANKDIET moves it to the alternate bank))
endif
endif

ifdef DIVHIDE
ifndef ROWDIET
$(error DIVHIDE=1 requires ROWDIET=1 (its register plan assumes the ROWDIET clamp arms))
endif
ifdef TRAPEZOID
$(error DIVHIDE=1 and TRAPEZOID=1 are incompatible (both claim r5/r27/r28 in the span path))
endif
endif

ifdef ROWDIET
ifdef TRAPEZOID
$(error ROWDIET=1 and TRAPEZOID=1 are mutually exclusive (both claim r4/r21 in the span run))
endif
endif
GEOTEX_DEFS := $(LOWRES_DEF) -d VRES60=$(if $(VRES60),1,0) $(NOFILL_DEF) $(NOSPAN_DEF) $(PROFGPU_DEF) $(NOMUL_DEF) $(NODIV_DEF) $(NOSTORE_DEF) $(SHADEPASS_DEF) $(NOCULL_DEF) $(STAGEDIET_DEF) -d NOBLIT=$(if $(NOBLIT),1,0) -d ALLCULL=$(if $(ALLCULL),1,0) -d RUNHIST=$(if $(RUNHIST),1,0) -d TRAPEZOID=$(if $(TRAPEZOID),1,0) -d DRIFTLOOSE=$(if $(DRIFTLOOSE),1,0) -d XCULL=$(if $(XCULL),1,0) -d BEXIT=$(if $(BEXIT),1,0) -d ROWDIET=$(if $(ROWDIET),1,0) -d PHRASESHADE=$(if $(PHRASESHADE),1,0) -d DIVHIDE=$(if $(DIVHIDE),1,0) -d BANKDIET=$(if $(BANKDIET),1,0) -d RUNBATCH=$(if $(RUNBATCH),1,0) -d RBNOUV=$(if $(RBNOUV),1,0) -d CULLCOUNT=$(if $(CULLCOUNT),1,0) -d NOBFCULL=$(if $(NOBFCULL),1,0) -d BEXCNT=$(if $(BEXCNT),1,0) -d SDPROBE=$(if $(SDPROBE),1,0) -d NOSDCULL=$(if $(NOSDCULL),1,0) -d WCCNT=$(if $(WCCNT),1,0) -d NOEMPTYY=$(if $(NOEMPTYY),1,0) -d GPUBG=$(if $(GPUBG),1,0) -d SHADEEXCL=$(if $(SHADEEXCL),1,0) -d RCLIPFIX=$(if $(RCLIPFIX),1,0) -d NEARLOW=$(if $(NEARLOW),1,0) -d PREPASSONLY=$(if $(PREPASSONLY),1,0) -d MMULTX=$(if $(MMULTX),1,0) -d MMXDIAG=$(if $(MMXDIAG),1,0) -d UVCLAMP=$(if $(UVCLAMP),1,0) -d UVPROBE=$(if $(UVPROBE),1,0) -d UVFIX=$(if $(UVFIX),1,0) -d UVNEG=$(if $(UVNEG),1,0) -d TINYCULL=$(if $(TINYCULL),$(TINYCULL),0) -d JMPDIET=$(if $(JMPDIET),1,0) -d LARACOUNT=$(if $(LARACOUNT),1,0) -d KEEPDEGEN=$(if $(KEEPDEGEN),1,0) -d DIVZGUARD=$(if $(DIVZGUARD),1,0) -d TINYKEEP=$(if $(TINYKEEP),$(TINYKEEP),0) -d BWOVER=$(if $(BWOVER),1,0) -d ODRAW=$(if $(ODRAW),1,0) -d PHRASEDST=$(if $(PHRASEDST),1,0) -d IMULPROBE=$(if $(IMULPROBE),1,0) -d SPANSHADE=$(if $(SPANSHADE),$(SPANSHADE),0) -d FOURBPP=$(if $(FOURBPP),1,0) -d INLINEMUL=$(if $(INLINEMUL),1,0) -d OFFHOIST=$(if $(OFFHOIST),1,0) -d VPACK=$(if $(VPACK),1,0) -d VCJDIET=$(if $(VCJDIET),1,0) -d NEARCLIP=$(if $(NEARCLIP),1,0) -d SYNCDRAIN=$(if $(SYNCDRAIN),1,0) -d ODRAWS=$(if $(ODRAWS),1,0) -d GPUHALT=$(if $(GPUHALT),1,0) -d VIEWH=$(if $(VIEWH),$(VIEWH),0) -d VRESN=$(if $(VRESN),$(VRESN),0) -d VCDRAIN=$(if $(VCDRAIN),1,0) -d MICROOPT=$(if $(MICROOPT),1,0) -d REVFACE=$(if $(REVFACE),1,0) -d HALFW=$(if $(HALFW),1,0) -d HRESN=$(if $(HRESN),$(HRESN),0) -d DIVSAFE=$(if $(DIVSAFE),1,0)
$(BUILD)/gpu_geotex.bin: gpu_geotex.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu $(call jasd,$(GEOTEX_DEFS))
	@# ☠☠ THIS GUARD MUST SIT IN THIS RECIPE.  It used to live after the
	@# gpu_geotex_hq.bin rule, separated only by a BLANK LINE -- which does
	@# NOT end a make recipe.  So both guards were commands of the _hq_ rule
	@# and both stat'd $@ = the HQ file, leaving the SHIPPING kernel with no
	@# ceiling check at all.  A kernel-growing change would have silently
	@# overlapped the SRAM vars at F03E60 and died on silicon only.
	@sz=$$(stat -c%s $@); if [ $$sz -gt 3680 ]; then 	  echo "!!! gpu_geotex.bin $$sz bytes OVERLAPS SRAM vars at F03E60 (max 3680; AU/BU at F03FA8+; SY/U/V relocated to F03FCC+; SX_BUF at F03F24; F03F74+ = DISPATCH LIST, not free)"; 	  rm -f $@; exit 1; fi

# TITLE-HQ kernel (task #4): SAME source/defs but LOWRES=0 -> 240-line
# projection constants (CENTER_Y 120, FOCAL_Y 190, YMAX 239). Uploaded to Tom
# only during the TITLE phase; the game phase re-uploads the normal kernel.
# ☠☠ HRESN MUST BE SUBSTITUTED OUT HERE TOO, EXACTLY AS LOWRES IS.
# The TITLE always displays FULL WIDTH -- IWIDTH and VMODE are phase-dependent
# on g_disp240 (video.c), so the title keeps the 320 fetch and the standard
# pixel clock while only the GAME narrows. The HQ kernel draws the title's 3D
# (the ring and its items), so it must project for 320: CENTER_X 160, FOCAL 190.
# Leaving HRESN=160 in gave it CENTER_X 80 / FOCAL 95 -- ring items drawn at
# half scale and shifted left, reported from the TV as "the menu isn't right".
# The LOWRES subst was already doing exactly this job for the vertical axis;
# the horizontal dial simply was not added to it.
GEOTEX_HQ_DEFS := $(subst -dLOWRES=1,-dLOWRES=0,$(GEOTEX_DEFS))
ifdef HRESN
GEOTEX_HQ_DEFS := $(subst -d HRESN=$(HRESN),-d HRESN=0,$(GEOTEX_HQ_DEFS))
endif

$(BUILD)/gpu_geotex_hq.bin: gpu_geotex.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu $(call jasd,$(GEOTEX_HQ_DEFS))
	@sz=$$(stat -c%s $@); if [ $$sz -gt 3680 ]; then echo "!!! gpu_geotex_hq.bin $$sz > 3680"; rm -f $@; exit 1; fi


$(BUILD)/gpu_blitprobe.bin: gpu_blitprobe.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

# gpu_blob.S .incbin's whichever kernel is selected; depend on all so a
# toggle rebuilds cleanly.
$(BUILD)/gpu_blob.o: gpu_blob.S $(BUILD)/gpu_jvdec.bin $(BUILD)/gpu_spanfill.bin $(BUILD)/gpu_geomwalk.bin $(BUILD)/gpu_geomxform.bin $(BUILD)/gpu_geomdirect.bin $(BUILD)/gpu_textured.bin $(BUILD)/gpu_bltex.bin $(BUILD)/gpu_blitprobe.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# gpu_geotex.gas is written by an agent; only depend on its blob for GEOTEX
# builds (so other builds don't fail when the kernel file isn't there yet).
ifdef GEOTEX
$(BUILD)/gpu_blob.o: $(BUILD)/gpu_geotex.bin $(BUILD)/gpu_geotex_hq.bin
endif
ifdef MULTIROOM
$(BUILD)/gpu_blob.o: $(BUILD)/gpu_geotex.bin $(BUILD)/gpu_geotex_hq.bin
endif

$(BUILD)/texdata.o: texdata.S tex_test.bin texpal.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/room0data.o: room0data.S room0_tex.bin room0_atlas.bin room0_pal.bin room0_sect.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/mrt_data.o: mrt_data.S disc/mrt.bin disc/mrt_geom.bin disc/mrt_sect.bin disc/mrt_atlas.bin disc/mrt_pal.bin disc/mrt_lara.bin disc/gym.bin disc/gym_geom.bin disc/gym_sect.bin disc/gym_atlas.bin disc/gym_pal.bin disc/gym_lara.bin pass_geom.bin pass_atlas.bin pass2_geom.bin pass2_atlas.bin ctrl_geom.bin ctrl_atlas.bin photo_geom.bin photo_atlas.bin font_load.bin sfx.bin music.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# room geometry blob is an explicit prerequisite (make won't see the
# .incbin), so regenerating room.bin relinks.
$(BUILD)/roomdata.o: roomdata.S room.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/laradata.o: laradata.S laramesh.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/roomsdata.o: roomsdata.S rooms.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/openlara.elf: $(OBJS) jaguar.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@if $(OBJDUMP) -d $@ | grep -q '61ff.*bsrs'; then \
	    echo 'ERROR: 68020 bsr.l leaked from libgcc (decodes as bsr.s -1'; \
	    echo 'on a 68000). Add the missing helper to divmod68k.S.'; \
	    rm $@; exit 1; \
	fi

$(BIN): $(BUILD)/openlara.elf
	@# ☠☠ strtonum() IS A GAWK EXTENSION.  Ubuntu 24.04 (the container base)
	@# ships mawk, where it does not exist -- awk printed nothing, $$end came
	@# out EMPTY, and the test errored with "[: -gt: unexpected operator" on
	@# stderr while the build carried on.  This guard has been DEAD in every
	@# container ROM.  $$((0x...)) is POSIX and works in dash, bash and sh.
	@# ★ It now PRINTS the headroom on success: a guard that is silent when it
	@# passes is indistinguishable from one that is not running, which is
	@# exactly how this survived.
	@hex=$$(m68k-neogeo-elf-nm $< | awk '/ __bss_end$$/{print $$1}'); \
	if [ -z "$$hex" ]; then \
	  echo "!!! __bss_end not found in $< - the DRAM budget guard CANNOT RUN"; \
	  exit 1; fi; \
	end=$$((0x$$hex)); \
	if [ $$end -gt 2080768 ]; then \
	  echo "!!! __bss_end $$end > 0x1FC000: <16KB 68k stack headroom (sp=0x200000 grows DOWN)"; \
	  exit 1; fi; \
	echo "   DRAM budget OK: __bss_end 0x$$hex = $$end / 2080768 ($$(( 2080768 - $$end )) B headroom)"
	$(OBJCOPY) -O binary $< $@

$(TARGET): $(BIN) makecof.py
	$(PYTHON) makecof.py $< $@ --addr $(LOAD_ADDR) --entry $(LOAD_ADDR)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean

# make SYNCPOLL=1: gpu_sync busy-polls instead of STOP-sleeping (diagnostic)
ifdef SYNCPOLL
CFLAGS   += -DSYNCPOLL
endif

# Jerry's room co-transform is RETIRED by default (measured: zero fps benefit,
# zero pixel difference, and it carried a cross-chip ordering hazard). Tom
# self-transforms every room. `make JERRYX=1` restores the old behaviour.
ifdef JERRYX
CFLAGS   += -DJERRYX
endif

# make FLIPSPIN=1: restore video_flip's old busy-wait (diagnostic A/B against the
# STOP-sleep). The spin stole DRAM bus from Tom while Tom rendered.
ifdef FLIPSPIN
CFLAGS   += -DFLIPSPIN
endif

# ---- verify-asm: byte-compare the jas-built kernels against rmac ----------
# (gpu_bltex / gpu_textured / gpu_geomdirect are legacy museum kernels still
#  owned by rmac: the first two contain real jas-flagged hazards nobody will
#  fix, the third predates the current data path.)
verify-asm: $(BUILD)/gpu_geotex.bin $(BUILD)/gpu_spanfill.bin $(BUILD)/gpu_geomwalk.bin $(BUILD)/gpu_geomxform.bin $(BUILD)/gpu_blitprobe.bin $(BUILD)/dsp_pose.bin
	@ok=1; \
	vfy() { $(RMAC) $$3 -fe $$1 -o $(BUILD)/vfy.elf && $(OBJCOPY) -O binary $(BUILD)/vfy.elf $(BUILD)/vfy.bin && \
	  { cmp -s $(BUILD)/vfy.bin $$2 && echo "verify-asm: $$1 IDENTICAL" || { echo "verify-asm: $$1 DIFFERS"; ok=0; }; }; }; \
	vfy gpu_geotex.gas   $(BUILD)/gpu_geotex.bin   "$(GEOTEX_DEFS)"; \
	vfy gpu_spanfill.gas $(BUILD)/gpu_spanfill.bin ""; \
	vfy gpu_geomwalk.gas $(BUILD)/gpu_geomwalk.bin ""; \
	vfy gpu_geomxform.gas $(BUILD)/gpu_geomxform.bin ""; \
	vfy gpu_blitprobe.gas $(BUILD)/gpu_blitprobe.bin ""; \
	vfy dsp_pose.das     $(BUILD)/dsp_pose.bin     "$(LOWRES_DEF)"; \
	[ $$ok -eq 1 ]
.PHONY: verify-asm

# ---- verify-c: run every C translation unit through jcc68k (cobweb) -------
# jcc68k is the VERIFICATION front-end for the C side today, not the code
# generator: it compiles 6/7 TUs (main.c blocked on a mis-attributed
# diagnostic — see COBWEB_REQ_jcc68k_adoption.md, which also lists what full
# adoption needs: GNU-ELF interop or a jln linker-script story, leaf-function
# prologue elision, a soft-mul/div runtime). A second front-end catches
# portability/UB the same way a second assembler catches encoding bugs.
JCC68K ?= $(if $(wildcard $(COBWEB_BIN)/jcc68k),$(COBWEB_BIN)/jcc68k,$(HOME)/Documents/Git/cobweb/sim/target/release/jcc68k)
JCCDEFS := -DMULTIROOM -DFB8 $(if $(HRESN),-DHRESN=$(HRESN)) $(if $(JERRYPOSE),-DJERRYPOSE) $(if $(AUTOSTART),-DAUTOSTART) $(if $(PROFILE),-DPROFILE)
verify-c:
	@ok=1; for f in video.c blit.c gpu.c jerry.c joypad.c gd_input.c; do \
	  if $(JCC68K) $$f -o $(BUILD)/jcc_$$f.s $(JCCDEFS) >/dev/null 2>&1 && [ -s $(BUILD)/jcc_$$f.s ]; then \
	    echo "verify-c: $$f OK"; else echo "verify-c: $$f FAILED"; ok=0; fi; done; \
	$(JCC68K) main.c -o $(BUILD)/jcc_main.s $(JCCDEFS) >/dev/null 2>&1 && [ -s $(BUILD)/jcc_main.s ] \
	  && echo "verify-c: main.c OK (cobweb fixed it — update the docs!)" \
	  || echo "verify-c: main.c KNOWN-FAIL (mis-attributed diagnostic, reported)"; \
	[ $$ok -eq 1 ]
.PHONY: verify-c

# make ASSETSUM=1: boot + periodic skunk-console checksums of the embedded
# assets (DRAM-decay / upload-corruption diagnostic, 2026-07-20)
ifdef ASSETSUM
CFLAGS += -DASSETSUM
endif

# make CLUTGUARD=1: per-frame CLUT rewrite + on-screen palette ramp strip
# (rows 190-198) — CLUT corruption diagnostic (2026-07-20)
ifdef CLUTGUARD
CFLAGS += -DCLUTGUARD
endif

# make SLITDISPLAY=1: OP presents only the top 64 lines (bus-contention
# probe: render work unchanged, OP fetch -73%; fps bar stays visible)
ifdef SLITDISPLAY
CFLAGS += -DSLITDISPLAY
endif

# CTRLOPEN=1: boot with the Controls page already open, so its layout can be
# verified offline in jagemu without driving the ring. Diagnostic only.
ifdef CTRLOPEN
CFLAGS   += -DCTRLOPEN=$(CTRLOPEN)
CXXFLAGS += -DCTRLOPEN=$(CTRLOPEN)
endif
# PASSSWEEP=1: cycle the open-book roll every 30 frames, index on screen
ifdef PASSSWEEP
CFLAGS   += -DPASSSWEEP=$(PASSSWEEP)
CXXFLAGS += -DPASSSWEEP=$(PASSSWEEP)
endif
# PADVID=N: shift the ISR handler within video.o (A10 internal-position roll)
ifdef PADVID
CFLAGS   += -DPADVID=$(PADVID)
CXXFLAGS += -DPADVID=$(PADVID)
endif
# PADMAIN=N: intra-main.o layout roll (A10 shifts PADTEXT cannot reach)
ifdef PADMAIN
CFLAGS   += -DPADMAIN=$(PADMAIN)
CXXFLAGS += -DPADMAIN=$(PADMAIN)
endif
# STEADYREAD=1: experimental 7KB/frame video reads (13-black mystery, OFF)
ifdef STEADYREAD
CFLAGS   += -DSTEADYREAD=$(STEADYREAD) -DSRGUARD=$(if $(SRRUN),1,0)
CXXFLAGS += -DSTEADYREAD=$(STEADYREAD) -DSRGUARD=$(if $(SRRUN),1,0)
endif
# GDPROBE=1: measure gd_fread cost vs size, draw bars, park (rig only)
ifdef GDPROBE
CFLAGS   += -DGDPROBE=$(GDPROBE)
CXXFLAGS += -DGDPROBE=$(GDPROBE)
endif
# VIDCAD=1: per-frame parity block in the FMV for cadence measurement
ifdef VIDCAD
CFLAGS   += -DVIDCAD=$(VIDCAD)
CXXFLAGS += -DVIDCAD=$(VIDCAD)
endif
# PASSTART=N: press A on the open book N frames later (captures)
ifdef PASSTART
CFLAGS   += -DPASSTART=$(PASSTART)
CXXFLAGS += -DPASSTART=$(PASSTART)
endif
# NOBOOTCLIPS=1: skip the boot logo chain (fast start-flow iteration)
ifdef NOBOOTCLIPS
CFLAGS   += -DNOBOOTCLIPS=$(NOBOOTCLIPS)
CXXFLAGS += -DNOBOOTCLIPS=$(NOBOOTCLIPS)
endif
# PASSOPEN=N: boot then auto-open the passport after N title frames (captures)
ifdef PASSOPEN
CFLAGS   += -DPASSOPEN=$(PASSOPEN)
CXXFLAGS += -DPASSOPEN=$(PASSOPEN)
endif
# TITLESEL=P: auto-rotate the title ring to page P after 90 frames (captures)
ifdef TITLESEL
CFLAGS   += -DTITLESEL=$(TITLESEL)
CXXFLAGS += -DTITLESEL=$(TITLESEL)
endif
# RINGBG=1: bright diagnostic panel behind the select slot (hole detector)
ifdef RINGBG
CFLAGS   += -DRINGBG
CXXFLAGS += -DRINGBG
endif

# LARA_FOOT1/2=N: footfall frames as EIGHTHS of the run/walk cycle. The
# footstep SFX is phase-locked to the animation (it used to fire off a
# free-running tick counter, so it drifted against her feet). Tune by ear.
ifdef LARA_FOOT1
CFLAGS   += -DLARA_FOOT1=$(LARA_FOOT1)
endif
ifdef LARA_FOOT2
CFLAGS   += -DLARA_FOOT2=$(LARA_FOOT2)
endif

# JVTESTCARD: FMV grain DISCRIMINATOR (run 3, 2026-08-18). Replaces the
# decoded clip frame with four flat bands in the shadow, immediately before
# the phrase copy - so anything speckled on screen was added by the COPY or
# the DISPLAY, and anything clean acquits them and indicts the decode. Never
# ship it; it makes the clips into a colour chart.
ifdef JVTESTCARD
CFLAGS   += -DJVTESTCARD
CXXFLAGS += -DJVTESTCARD
endif

# JVTCONLY: the QUIET-BUS arm of the same hunt (run 4). Loads the palette and
# codebook, then loops fill/copy/flip forever with no gd_fread, no audio, no
# decode. Splits "the OP is starved" from "the copy or the object is wrong".
# It never returns - a scope, not a build.
# JVDECDIAG / JVDECMASK: the run-7 decode discriminator. 1 = force the 68k
# token walk instead of Tom's kick, 2 = zero the shadow before each decode.
# The mask is a `volatile const` in main.c, so an arm is ONE IMMEDIATE and the
# layout never moves - one A10 pad roll covers all of them.
# BLITSET: the false-idle settle window in blit_copy_phrase (blit.c). 0 ships
# the historic "believe the first IDLE" behaviour; N requires N consecutive
# IDLE reads. It is a `volatile const`, so an arm is ONE IMMEDIATE and one A10
# pad roll covers every value.
BLITSET  ?= 0
CFLAGS   += -DBLITSET=$(BLITSET)
CXXFLAGS += -DBLITSET=$(BLITSET)

ifdef JVDECDIAG
JVDECMASK ?= 0
CFLAGS   += -DJVDECDIAG -DJVDECMASK=$(JVDECMASK)
CXXFLAGS += -DJVDECDIAG -DJVDECMASK=$(JVDECMASK)
endif

ifdef JVTESTCARD
JVTCMASK ?= 0
CFLAGS   += -DJVTCMASK=$(JVTCMASK)
CXXFLAGS += -DJVTCMASK=$(JVTCMASK)
endif

ifdef JVTCONLY
CFLAGS   += -DJVTCONLY
CXXFLAGS += -DJVTCONLY
# JVTCMASK: which consumer to add back to the quiet-bus baseline.
#   1 = gd_fread   2 = audio ring   4 = decode kick   (combine freely)
# ★ It is a `volatile const` in main.c, so changing it changes ONE IMMEDIATE
#   and NOT the code layout - one A10 pad roll covers every arm. Do not turn
#   this back into #ifdefs; that is what cost four rig turns in run 4.
endif

# VIDDIAG: FMV decode-status marker (ghost hunt 2026-08-07)
ifdef VIDDIAG
CFLAGS   += -DVIDDIAG
CXXFLAGS += -DVIDDIAG
endif

# ASVID: auto-Start-Game after the title settles (Tom campaign self-test)
ifdef ASVID
CFLAGS   += -DASVID
CXXFLAGS += -DASVID
endif

# ─────────────────────────────────────────────────────────────────────────
# vidrom — STANDALONE BOOT-TO-VIDEO ROM (vidmain.c)
#
# `make vidrom` links a tiny image that boots straight into ONE .JV clip
# from the GameDrive SD, decoded on Tom. No level data, no title, no game:
# it builds in seconds and boots in ~5, which is what makes bisecting the
# "Tom's jvdec kernel never starts from the boot context" mystery tractable
# (each full-game probe cost a build + an A10 roll + a 3-minute boot chain).
#
#   make vidrom MULTIROOM=1 LOWRES=1 OPDBL=1 FLIPASM=1 VR_CLIP=EIDOS.JV
#
# Context switches (default = the barest context that can play a clip):
#   VR_GPUINIT VR_JERRY VR_AUDIO VR_GDINPUT VR_EARLYLOAD VR_NO68K
# Add them one at a time to find which ingredient poisons the kernel start.
# ☠️ `rm -rf build` when switching between `make` and `make vidrom` — make
# tracks timestamps, not flags, and the two share every object but main.
ifdef VR_CLIP
CFLAGS   += -DVR_CLIP=\"$(VR_CLIP)\"
endif
ifdef VR_GPUINIT
CFLAGS   += -DVR_GPUINIT
endif
ifdef VR_JERRY
CFLAGS   += -DVR_JERRY
endif
ifdef VR_AUDIO
CFLAGS   += -DVR_AUDIO
endif
ifdef VR_GDINPUT
CFLAGS   += -DVR_GDINPUT
endif
ifdef VR_EARLYLOAD
CFLAGS   += -DVR_EARLYLOAD
endif
ifdef VR_NO68K
CFLAGS   += -DVR_NO68K
endif

# VR_CHAIN=1: vidrom hands the machine to the game when the clips finish, so
# the video decoder costs the game NOTHING (it is gone from RAM before the
# game's _start runs). chain.o is the position-independent handover blob.
ifdef VR_CHAIN
VIDCFLAGS += -DVR_CHAIN
VIDCHAIN  := $(BUILD)/chain.o
endif
VIDOBJS := $(BUILD)/startup.o $(BUILD)/cpu68k.o $(BUILD)/vidmain.o $(BUILD)/vidpanel.o \
           $(BUILD)/video.o $(BUILD)/blit.o $(BUILD)/joypad.o \
           $(BUILD)/gd_input.o $(BUILD)/gdbios.o \
           $(BUILD)/gpu.o $(BUILD)/gpu_blob.o \
           $(BUILD)/jerry.o $(BUILD)/dsp_blob.o $(VIDCHAIN)

# vidmain.c is the boot code the user asked to keep on the 68k, and it is on
# gcc for the same reason main.c is: the frame loop copies 76800 bytes.
$(BUILD)/vidmain.o: vidmain.c | $(BUILD)
	$(CC) $(CFLAGS) $(VIDCFLAGS) -c $< -o $@

$(BUILD)/vidrom.elf: $(VIDOBJS) jaguar.ld
	$(CC) -nostdlib -T jaguar.ld -Wl,-Map=$(BUILD)/vidrom.map \
	      -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,-z,noexecstack \
	      -o $@ $(VIDOBJS) -lgcc

$(BUILD)/vidrom.bin: $(BUILD)/vidrom.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/vidrom.cof: $(BUILD)/vidrom.bin makecof.py
	$(PYTHON) makecof.py $< $@ --addr $(LOAD_ADDR) --entry $(LOAD_ADDR)

vidrom: $(BUILD)/vidrom.cof
.PHONY: vidrom

# VR_PAD=N: A10 layout roll for vidrom (see vidmain.c). Build 0/136/272/408/
# 544/816 and keep whichever boot - small ROMs are NOT lottery-immune.
ifdef VR_PAD
CFLAGS   += -DVR_PAD=$(VR_PAD)
endif

# VR_NOPANEL=1: drop the on-screen read-out (measure the player, not the
# instrument - the panel itself cost 62ms/frame before it was made cheap).
ifdef VR_NOPANEL
CFLAGS   += -DVR_NOPANEL
endif

# JVFASTKICK=1: drop the 2000-iteration 68k drain loop in gpu_jvdec_kick.
# It was added during the blind Tom campaign as a "stop-settle" fix and was
# already falsified there; now that Tom demonstrably runs, it is ~12ms of
# pure 68k spin per video frame. Verify the hello lamp still lights.
ifdef JVFASTKICK
CFLAGS   += -DJVFASTKICK
endif

# VR_PHRASECOPY=1: move the video frame with the Blitter in PHRASE mode
# (8 bytes/step) instead of per-pixel. Falls back per frame if the Blitter
# never idles, counted in the high bits of the stage row.
ifdef VR_PHRASECOPY
CFLAGS   += -DVR_PHRASECOPY
endif

# VR_LIVEPANEL=1: also paint the read-out DURING playback. Off by default -
# the panel costs ~35ms/frame, so leaving it out of the clip loop makes the
# measurement the real ship cost. The counters accumulate either way and the
# hold screen after the clip shows the whole-clip totals.
ifdef VR_LIVEPANEL
CFLAGS   += -DVR_LIVEPANEL
endif

# VR_MAXF=N: play only the first N frames of the clip, then hold. INTRO and
# CAVES are ~104s each and a roll should not cost two minutes of rig time.
ifdef VR_MAXF
CFLAGS   += -DVR_MAXF=$(VR_MAXF)
endif

# VR_BOOTTRACE=1: BG crumbs + a ramp through the boot sequence, so a held
# colour can be told from a crawling 68k (they look identical otherwise).
ifdef VR_BOOTTRACE
CFLAGS   += -DVR_BOOTTRACE
endif

# VR_SHOW=1: the viewing build - EIDOS, CORE, INTRO, CAVES back to back with
# sound, nothing painted over them, ending on black. Not an instrument.
ifdef VR_SHOW
CFLAGS   += -DVR_SHOW
endif

# VIDPANEL=1: the self-calibrating read-out in the GAME's boot video block
# (same geometry as vidrom, same host decoder). The old VIDDIAG squares are
# unreadable off a capture - they cost two wrong diagnoses.
# BANDPROBE=1: paint the two candidate owners of the bottom scanlines in flat
# values just before the flip, so one capture says which region the frozen
# band at the bottom of the screen comes from.  Diagnostic only.
ifdef BANDPROBE
CFLAGS   += -DBANDPROBE
CXXFLAGS += -DBANDPROBE
endif

# DARTS=1: TRAP_DART_EMITTER entities fire darts across the corridor and hurt
# Lara on contact (TR1 damage 50/1000).  Room-0 pass, 2026-08-09.
ifdef DARTS
CFLAGS   += -DDARTS
CXXFLAGS += -DDARTS
endif

# make BLOBCACHE=1: stop the 68000 rebuilding static entity blobs every frame.
# Bridges/doors/levers were re-derived vertex-by-vertex on every frame even
# though a bridge never moves and a shut door never moves; cache per draw slot
# and rebuild only when the entity's mutable state changes.
ifdef BLOBCACHE
CFLAGS   += -DBLOBCACHE
CXXFLAGS += -DBLOBCACHE
endif

# make JCENT=1: Jerry emits each Lara mesh's CENTROID SUMS while it poses the
# verts, so the 68000 stops walking every posed vertex of every mesh in
# lara_finish just to re-derive them.  Bit-identical sums => identical painter
# order (unlike the dormant T-export, which flipped the order and lost fps).
ifdef JCENT
CFLAGS   += -DJCENT
CXXFLAGS += -DJCENT
endif

# make NOLARA=1: ABLATION ONLY (never ship) - drop every bit of Lara's render
# work from the 68000's frame (lara_finish + her display-list entry) to size
# what she actually costs, now that entities have measured at ~0.
ifdef NOLARA
CFLAGS   += -DNOLARA
CXXFLAGS += -DNOLARA
endif

# make NOMUSIC=1: ABLATION - skip the in-frame 4KB music gd_fread only.
ifdef NOMUSIC
CFLAGS   += -DNOMUSIC
CXXFLAGS += -DNOMUSIC
endif

# make JOVL=1: Jerry CODE OVERLAY loader (CMD=3).  Jerry's code window is only
# 4192 bytes and the resident kernel uses 2664, so the game loop cannot be
# ported into it wholesale - it has to arrive one phase at a time, copied from
# DRAM into the free tail.  Loader costs 96 bytes, leaving ~1432 per overlay.
ifdef JOVL
CFLAGS   += -DJOVL
CXXFLAGS += -DJOVL
endif

# make SECTLONG=1: build a LONG-ALIGNED mirror of the sector data once at level
# load.  Jerry cannot reliably byte-read DRAM, so collision cannot move to it
# while the data is byte-packed - and repacking in the EXTRACTOR would mean an
# asset regen, the step this project has been burned by.  ~56KB of BSS.
ifdef SECTLONG
CFLAGS   += -DSECTLONG
CXXFLAGS += -DSECTLONG
endif

# STAGECHK=1: checksum the display list the 68k stages each frame and count
# how often it changes while the CAMERA is unchanged.  Answers whether the
# flickering faces are lost 68k-side or inside Tom's render.
ifdef STAGECHK
CFLAGS   += -DSTAGECHK
CXXFLAGS += -DSTAGECHK
endif

# BUSPROBE=1: on-screen split of the 68000's frame - halflines ACTIVE during
# Tom's render (the flicker exposure window) vs halflines ASLEEP in the collect.
ifdef BUSPROBE
CFLAGS   += -DBUSPROBE
CXXFLAGS += -DBUSPROBE
endif

# COLLECTEARLY=1: take the PIPELINE collect after the light head of the frame
# instead of after all the logic, so Lara's collision + the portal walk do not
# run on top of Tom's render.  Trades a little frame time for a quiet bus.
ifdef COLLECTEARLY
CFLAGS   += -DCOLLECTEARLY
CXXFLAGS += -DCOLLECTEARLY
endif

# FASTBOOT=1: boot STRAIGHT into the level - no logos, no title splash, no
# Start Game cinematic, no loading art or dwell.  Implies AUTOSTART.  For test
# rolls: the boot chain is ~130s and every capture was mostly boot.
ifdef FASTBOOT
# ☠️ define AUTOSTART here, not by setting the variable: the ifdef that turns
# AUTOSTART into -DAUTOSTART is parsed EARLIER in this file, so a late
# assignment would never reach the compiler.
CFLAGS   += -DFASTBOOT -DAUTOSTART
CXXFLAGS += -DFASTBOOT -DAUTOSTART
endif

# TRAVDIAG=1: print what the ledge in front of Lara allows (rise, verdict,
# grab reach, state) so a screencast says WHICH traversal rule refuses.
# WORLDCOUNT=1: paint the WORLD face counters (staged vs rastered) as bars, so
# the black-geometry split can be read off a capture instead of a console.
# Needs the kernel counters: pass CULLCOUNT=1 too.
ifdef WORLDCOUNT
CFLAGS   += -DWORLDCOUNT
CXXFLAGS += -DWORLDCOUNT
endif
ifdef TRAVDIAG
CFLAGS   += -DTRAVDIAG
CXXFLAGS += -DTRAVDIAG
endif

# GDPAD=1: drive the game remotely - gd_input reads INPUT.BIN off the SD as a
# pad mask, written by the host with `jaggd -wf`.  TEST BUILDS ONLY (an SD
# open/read/close every 4th frame).
# JXWAIT=1: A1 experiment - 68k waits for every Jerry room transform before
# Tom's first dispatch (see main.c). Measured with the static-camera racing-
# pixel metric; if it holds it becomes the default.
ifdef JXWAIT
CFLAGS   += -DJXWAIT
CXXFLAGS += -DJXWAIT
endif
ifdef GDPAD
CFLAGS   += -DGDPAD
CXXFLAGS += -DGDPAD
endif

# PROBE_AHEAD=N: how far in front the ledge probes sample (default WALK_SPEED*2
# = 94 units, against a 1024-unit sector).
ifdef PROBE_AHEAD
CFLAGS   += -DPROBE_AHEAD=$(PROBE_AHEAD)
CXXFLAGS += -DPROBE_AHEAD=$(PROBE_AHEAD)
endif

ifdef VIDPANEL
CFLAGS   += -DVIDPANEL
CXXFLAGS += -DVIDPANEL
endif
