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
# rmac is retained for (a) the legacy museum kernels it still owns and
# (b) `make verify-asm`, which byte-compares jas output against rmac.
JAS     := $(HOME)/Documents/Git/cobweb/sim/target/release/jas
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
ifndef LOWRES
$(error FLIPASM=1 requires LOWRES=1 (the asm implements the LOWRES publish path))
endif
ifdef HALFRES
$(error FLIPASM=1 and HALFRES=1 are incompatible (HALFRES publishes via blit_double))
endif
CFLAGS   += -DFLIPASM
CXXFLAGS += -DFLIPASM
ASFLAGS  += -DFLIPASM
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
OBJS := $(BUILD)/startup.o $(BUILD)/cpu68k.o $(BUILD)/main.o $(BUILD)/video.o \
        $(BUILD)/blit.o $(BUILD)/joypad.o \
        $(BUILD)/gd_input.o $(BUILD)/gdbios.o \
        $(BUILD)/gpu.o $(BUILD)/gpu_blob.o \
        $(BUILD)/jerry.o $(BUILD)/dsp_blob.o
ifdef NOGD
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
	$(JCC68K) $< -o $(BUILD)/$*.jcc.s -I. $(filter -D%,$(CFLAGS))
	$(JAS) $(BUILD)/$*.jcc.s --68000 --elf-obj -o $@
# main.c stays on gcc for PERFORMANCE (not correctness): all-jcc renders
# correctly but the game crawls — main.c is the per-frame 68k logic and
# jcc68k's ~1.9x text + __mulsi3 for non-power-of-2 scaling (y*320) is a
# per-frame tax there. Flips when cobweb's register-allocator follow-up
# lands. (Their own end-to-end verification was also 7-of-8-except-main.)
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# joypad.c/gd_input.c pinned to gcc: controls went DEAD on hardware with
# them on jcc68k (user 2026-07-20; jagemu's injected input can't see the
# real strobe-scan timing). Suspect MMIO access width/ordering in the pad
# strobe or GD BIOS calls — narrow with single-TU A/B flashes, then report.
$(BUILD)/joypad.o: joypad.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gd_input.o: gd_input.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# jerry.c pinned to gcc: under jcc68k Lara's HEAD renders turned (last angle
# of the pose marshal corrupted — reproduced in jagemu, fix1 vs oracle
# 2026-07-20). Repro asm saved; narrowing for the cobweb report.
$(BUILD)/jerry.o: jerry.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# make ... GCCHOT=1 : pin the remaining PER-FRAME TUs to gcc as well.
# Rationale (2026-07-24): jcc68k emits ~1.9x gcc -O2's text (note above), and
# main.c was already pinned back here "for PERFORMANCE ... the game crawls".
# blit.c is where the 68k pc-histogram puts ~60% of awake time and gpu.c runs
# every kick, so both were still paying that 1.9x on the critical path.
# video.c carries the vblank ISR, where jcc68k costs ~10 instructions per store.
# Correctness risk is nil (gcc is the reference compiler); this only opts out of
# dogfooding cobweb's codegen on the hot path.
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

$(BUILD)/gpu_geomwalk.bin: gpu_geomwalk.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/gpu_geomxform.bin: gpu_geomxform.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

$(BUILD)/gpu_geomdirect.bin: gpu_geomdirect.gas | $(BUILD)
	$(RMAC) $(LOWRES_DEF) -fe $< -o $(BUILD)/gpu_geomdirect.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_geomdirect.elf $@

$(BUILD)/gpu_textured.bin: gpu_textured.gas | $(BUILD)
	$(RMAC) -fe $< -o $(BUILD)/gpu_textured.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_textured.elf $@

$(BUILD)/gpu_bltex.bin: gpu_bltex.gas | $(BUILD)
	$(RMAC) -fe $< -o $(BUILD)/gpu_bltex.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_bltex.elf $@

$(BUILD)/dsp_pose.bin: dsp_pose.das | $(BUILD)
	$(JAS) $< -o $@ --dsp $(call jasd,$(LOWRES_DEF) -d NOSOUND=$(if $(NOSOUND),1,0) -d AUDIOLITE=$(if $(AUDIOLITE),1,0) -d JDRAIN=$(if $(JDRAIN),1,0))
	@sz=$$(stat -c%s $@); if [ $$sz -gt 4192 ]; then \
	  echo "!!! dsp_pose.bin $$sz bytes OVERLAPS MBLK at F1C060 (max 4192 = F1C060-F1B000; OUT_D is dead since direct-DRAM pose)"; \
	  rm -f $@; exit 1; fi

$(BUILD)/dsp_blob.o: dsp_blob.S $(BUILD)/dsp_pose.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c dsp_blob.S -o $@

NOMUL_DEF := -dNOMUL=$(if $(NOMUL),1,0)
NODIV_DEF := -dNODIV=$(if $(NODIV),1,0)
NOSTORE_DEF := -dNOSTORE=$(if $(NOSTORE),1,0)
SHADEPASS_DEF := -dSHADEPASS=$(if $(SHADEPASS),1,0)
NOCULL_DEF := -dNOCULL=$(if $(NOCULL),1,0)
# STAGEDIET: early backface cull from a 12B per-face plane prefix. Needs bins
# built with FACE_PLANES=1 (extractor) AND -DSTAGEDIET in CFLAGS (main.c
# runtime blob builders + record strides). CAMLOC reuses the PROFGPU var
# block, so the pair is forbidden.
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
GEOTEX_DEFS := $(LOWRES_DEF) $(NOFILL_DEF) $(NOSPAN_DEF) $(PROFGPU_DEF) $(NOMUL_DEF) $(NODIV_DEF) $(NOSTORE_DEF) $(SHADEPASS_DEF) $(NOCULL_DEF) $(STAGEDIET_DEF) -d NOBLIT=$(if $(NOBLIT),1,0) -d ALLCULL=$(if $(ALLCULL),1,0) -d RUNHIST=$(if $(RUNHIST),1,0) -d TRAPEZOID=$(if $(TRAPEZOID),1,0) -d DRIFTLOOSE=$(if $(DRIFTLOOSE),1,0) -d XCULL=$(if $(XCULL),1,0) -d BEXIT=$(if $(BEXIT),1,0) -d ROWDIET=$(if $(ROWDIET),1,0) -d PHRASESHADE=$(if $(PHRASESHADE),1,0) -d DIVHIDE=$(if $(DIVHIDE),1,0) -d BANKDIET=$(if $(BANKDIET),1,0) -d RUNBATCH=$(if $(RUNBATCH),1,0) -d RBNOUV=$(if $(RBNOUV),1,0) -d CULLCOUNT=$(if $(CULLCOUNT),1,0) -d PREPASSONLY=$(if $(PREPASSONLY),1,0) -d MMULTX=$(if $(MMULTX),1,0) -d MMXDIAG=$(if $(MMXDIAG),1,0) -d UVCLAMP=$(if $(UVCLAMP),1,0) -d UVPROBE=$(if $(UVPROBE),1,0) -d UVFIX=$(if $(UVFIX),1,0) -d UVNEG=$(if $(UVNEG),1,0) -d TINYCULL=$(if $(TINYCULL),$(TINYCULL),0) -d JMPDIET=$(if $(JMPDIET),1,0) -d LARACOUNT=$(if $(LARACOUNT),1,0) -d KEEPDEGEN=$(if $(KEEPDEGEN),1,0) -d DIVZGUARD=$(if $(DIVZGUARD),1,0) -d TINYKEEP=$(if $(TINYKEEP),$(TINYKEEP),0) -d BWOVER=$(if $(BWOVER),1,0) -d ODRAW=$(if $(ODRAW),1,0) -d PHRASEDST=$(if $(PHRASEDST),1,0) -d IMULPROBE=$(if $(IMULPROBE),1,0) -d SPANSHADE=$(if $(SPANSHADE),$(SPANSHADE),0)
$(BUILD)/gpu_geotex.bin: gpu_geotex.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu $(call jasd,$(GEOTEX_DEFS))
	@sz=$$(stat -c%s $@); if [ $$sz -gt 3680 ]; then 	  echo "!!! gpu_geotex.bin $$sz bytes OVERLAPS SRAM vars at F03E60 (max 3680; AU/BU at F03FA8+; SY/U/V relocated to F03FCC+; SX_BUF at F03F24; F03F74+ = DISPATCH LIST, not free)"; 	  rm -f $@; exit 1; fi

$(BUILD)/gpu_blitprobe.bin: gpu_blitprobe.gas | $(BUILD)
	$(JAS) $< -o $@ --gpu

# gpu_blob.S .incbin's whichever kernel is selected; depend on all so a
# toggle rebuilds cleanly.
$(BUILD)/gpu_blob.o: gpu_blob.S $(BUILD)/gpu_spanfill.bin $(BUILD)/gpu_geomwalk.bin $(BUILD)/gpu_geomxform.bin $(BUILD)/gpu_geomdirect.bin $(BUILD)/gpu_textured.bin $(BUILD)/gpu_bltex.bin $(BUILD)/gpu_blitprobe.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

# gpu_geotex.gas is written by an agent; only depend on its blob for GEOTEX
# builds (so other builds don't fail when the kernel file isn't there yet).
ifdef GEOTEX
$(BUILD)/gpu_blob.o: $(BUILD)/gpu_geotex.bin
endif
ifdef MULTIROOM
$(BUILD)/gpu_blob.o: $(BUILD)/gpu_geotex.bin
endif

$(BUILD)/texdata.o: texdata.S tex_test.bin texpal.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/room0data.o: room0data.S room0_tex.bin room0_atlas.bin room0_pal.bin room0_sect.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/mrt_data.o: mrt_data.S mrt.bin mrt_geom.bin mrt_sect.bin mrt_atlas.bin mrt_pal.bin mrt_lara.bin gym.bin gym_geom.bin gym_sect.bin gym_atlas.bin gym_pal.bin gym_lara.bin pass_geom.bin pass_atlas.bin photo_geom.bin photo_atlas.bin font_load.bin sfx.bin music.bin | $(BUILD)
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
	@end=$$(m68k-neogeo-elf-nm $< | awk '/__bss_end/{print strtonum("0x"$$1)}'); \
	if [ $$end -gt 2080768 ]; then \
	  echo "!!! __bss_end $$end > 0x1FC000: <16KB 68k stack headroom (sp=0x200000 grows DOWN)"; \
	  exit 1; fi
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
JCC68K := $(HOME)/Documents/Git/cobweb/sim/target/release/jcc68k
JCCDEFS := -DMULTIROOM -DFB8 $(if $(JERRYPOSE),-DJERRYPOSE) $(if $(AUTOSTART),-DAUTOSTART) $(if $(PROFILE),-DPROFILE)
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
