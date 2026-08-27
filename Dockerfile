# Reproducible Tomb Raider 1 -> Atari Jaguar build.
#
# Ships NO game data.  You mount your OWN Tomb Raider PSX disc at run time; the
# assets are extracted, converted and compiled entirely inside the container,
# and only the finished GameDrive files (the ROM, the videos, the music and the
# loading art) leave it.
# Everyone uses the same pinned toolchain, so the same disc yields the same ROM.
#
#   docker build -t tr-jaguar .
#   docker run --rm -v "$PWD/disc:/disc:ro" -v "$PWD/out:/out" \
#              -e DISC_NAME="Tomb Raider (USA) (v1.6).cue" tr-jaguar
#
# Two builds come out of the same image and the same disc; pick with QUALITY:
#   -e QUALITY=pretty     (default) 120 render lines, full vertical resolution
#   -e QUALITY=playable             80 render lines through the OP 3.0x scaler -
#                                   full screen and FOV, coarser, ~12% faster
#
# The build also converts the FRONT END off the same disc - the Eidos and Core
# logos, the attract cinematic and the trek that plays before the Caves - and
# stages them beside the ROM. That is the slow part; add -e VIDEO=0 to skip it.
#
# ...or just use ../convert.sh, which wraps both steps.
# ── stage 1: COBWEB, the Jaguar toolchain that builds the GPU/DSP kernels ────
# jas assembles gpu_geotex.gas (the renderer), dsp_pose.gas and the rest; jcc68k
# compiles most of the C. Both are from github.com/bslop/cobweb (MIT), pinned.
# ☠️ rmac CANNOT stand in for jas here - the .gas sources use jas define/.if
# semantics and rmac rejects them outright. rmac still builds the older kernels.
FROM rust:1-slim-bookworm AS cobweb
# ☠️ PIN IT. `main` gets cached by Docker, so the image silently keeps whatever
# cobweb was current when the layer was first built - this build failed on four
# hazard errors that had already been fixed upstream. A real revision both busts
# the cache and makes the image reproducible.
# 2026-08-26: 59e5896 -> 9da2f99. Two jas fixes the shipping ROM needs:
#   ba9c680  a memory-to-memory MOVE dropped its DESTINATION relocation
#            (every global-to-global copy in a jcc68k TU stored into the 68000
#            exception vectors; 4 sites in the last container ROM, 24 in a
#            default local build)
#   9da2f99  object-mode .align is SECTION-relative and raises sh_addralign
#            (the A10 boot lottery; jaguar.ld places op_list regardless)
# Every ROM measured on silicon on 2026-08-26 was built with this revision.
ARG COBWEB_REV=9da2f99
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/bslop/cobweb.git /cobweb \
    && git -C /cobweb checkout $COBWEB_REV \
    && cargo build --release --manifest-path /cobweb/sim/Cargo.toml \
    && strip /cobweb/sim/target/release/jas /cobweb/sim/target/release/jcc68k || true

# ── stage 2: the build environment ──────────────────────────────────────────
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
COPY --from=cobweb /cobweb/sim/target/release/jas    /usr/local/bin/jas
COPY --from=cobweb /cobweb/sim/target/release/jcc68k /usr/local/bin/jcc68k
ENV JAS=/usr/local/bin/jas JCC68K=/usr/local/bin/jcc68k

# Toolchain + asset/disc tooling:
#   ngdevkit m68k-neogeo-elf gcc  - 68000 cross-compiler (its libgcc has no
#                                   68020 bsr.l leak, unlike gcc-m68k-linux-gnu)
#   python3 + Pillow + numpy + ffmpeg - asset converters and the music encoder
#   ☠️ numpy: tr2jag_video.py's JV_VQ=1 encoder imports it; without it every clip
#   "failed to convert" (stderr discarded) and stale disc/*.JV shipped (2026-08-26)
#   p7zip / bchunk / xorriso / mame-tools(chdman) - handle .7z/.bin+.cue/.chd
RUN apt-get update && apt-get install -y --no-install-recommends \
        software-properties-common ca-certificates git build-essential make \
        python3 python3-pil python3-numpy ffmpeg \
        p7zip-full bchunk xorriso mame-tools \
    && add-apt-repository -y ppa:dciabrin/ngdevkit \
    && apt-get update && apt-get install -y --no-install-recommends ngdevkit-toolchain \
    && rm -rf /var/lib/apt/lists/*

# ☠️ rmac IS GONE FROM THIS BUILD. It used to assemble three kernels, and its
# upstream (github.com/ggnkua/rmac) has VANISHED - it 404s - so the image was
# fetching a critical tool from mirrors of a repo with no home. jas now builds
# every GPU/DSP kernel and emits BYTE-IDENTICAL output for all three (verified
# with cmp), so the whole toolchain is cobweb and one supply-chain risk is out.

WORKDIR /src
COPY . .

# Overridable at run time: docker run -e QUALITY=playable ...
ENV QUALITY=pretty

ENTRYPOINT ["bash", "tools/docker-entrypoint.sh"]
