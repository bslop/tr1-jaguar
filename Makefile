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
endif

ifdef LOWRES
CFLAGS     += -DLOWRES
CXXFLAGS   += -DLOWRES
LOWRES_DEF := -dLOWRES=1
else
LOWRES_DEF := -dLOWRES=0
endif

# perf experiment: make ... NOFILL=1 assembles the geotex kernel WITHOUT the
# Blitter span launch (all transform/edge/span math still runs) — isolates
# GPU compute cost vs Blitter fill+wait cost. `make clean` when toggling.
ifdef NOFILL
NOFILL_DEF := -dNOFILL=1 -dHALFSPAN=$(if $(HALFSPAN),1,0)
else
NOFILL_DEF := -dNOFILL=0 -dHALFSPAN=$(if $(HALFSPAN),1,0)
endif

# M2 object set: room renderer + video + Blitter + input + room data.
OBJS := $(BUILD)/startup.o $(BUILD)/main.o $(BUILD)/video.o \
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

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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
	$(RMAC) -fe $< -o $(BUILD)/gpu_spanfill.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_spanfill.elf $@

$(BUILD)/gpu_geomwalk.bin: gpu_geomwalk.gas | $(BUILD)
	$(RMAC) -fe $< -o $(BUILD)/gpu_geomwalk.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_geomwalk.elf $@

$(BUILD)/gpu_geomxform.bin: gpu_geomxform.gas | $(BUILD)
	$(RMAC) -fe $< -o $(BUILD)/gpu_geomxform.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_geomxform.elf $@

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
	$(RMAC) $(LOWRES_DEF) -fe $< -o $(BUILD)/dsp_pose.elf
	$(OBJCOPY) -O binary $(BUILD)/dsp_pose.elf $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt 4000 ]; then \
	  echo "!!! dsp_pose.bin $$sz bytes OVERLAPS MBLK at F1C060 (max 4000; OUT_D is dead since direct-DRAM pose)"; \
	  rm -f $@; exit 1; fi

$(BUILD)/dsp_blob.o: dsp_blob.S $(BUILD)/dsp_pose.bin | $(BUILD)
	$(CC) $(ASFLAGS) -c dsp_blob.S -o $@

$(BUILD)/gpu_geotex.bin: gpu_geotex.gas | $(BUILD)
	$(RMAC) $(LOWRES_DEF) $(NOFILL_DEF) $(PROFGPU_DEF) -fe $< -o $(BUILD)/gpu_geotex.elf
	$(OBJCOPY) -O binary $(BUILD)/gpu_geotex.elf $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt 3584 ]; then 	  echo "!!! gpu_geotex.bin $$sz bytes OVERLAPS SRAM vars at F03E00 (max 3584)"; 	  rm -f $@; exit 1; fi

# gpu_blob.S .incbin's whichever kernel is selected; depend on all so a
# toggle rebuilds cleanly.
$(BUILD)/gpu_blob.o: gpu_blob.S $(BUILD)/gpu_spanfill.bin $(BUILD)/gpu_geomwalk.bin $(BUILD)/gpu_geomxform.bin $(BUILD)/gpu_geomdirect.bin $(BUILD)/gpu_textured.bin $(BUILD)/gpu_bltex.bin | $(BUILD)
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
