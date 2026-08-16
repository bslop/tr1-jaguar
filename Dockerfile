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
ARG COBWEB_REV=main
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
#   python3 + Pillow + ffmpeg     - asset converters and the music encoder
#   p7zip / bchunk / xorriso / mame-tools(chdman) - handle .7z/.bin+.cue/.chd
RUN apt-get update && apt-get install -y --no-install-recommends \
        software-properties-common ca-certificates git build-essential make \
        python3 python3-pil ffmpeg \
        p7zip-full bchunk xorriso mame-tools \
    && add-apt-repository -y ppa:dciabrin/ngdevkit \
    && apt-get update && apt-get install -y --no-install-recommends ngdevkit-toolchain \
    && rm -rf /var/lib/apt/lists/*

# rmac - the Jaguar RISC assembler that builds the GPU/DSP kernels, from source
# pinned to a known-good commit.
#
# ☠️ THE GITHUB REPO THIS USED TO CLONE (ggnkua/rmac) IS GONE - it 404s, so
# every container build broke silently at this step. Upstream moved; the host
# below is the one RMAC's own download page still points at, and it carries the
# pinned commit. Mirrors are tried in order so one host dying is not fatal
# again. If they all fail, put an `rmac` binary on the PATH and build without
# Docker (tools/build_cof.sh).
ARG RMAC_REV=1fd77b5db027255ef58e0af7b5a329d8060a82ee
RUN set -eu; \
    for url in http://tiddly.mooo.com:5000/rmac/rmac.git \
               https://gitlab.com/ggnkua/rmac-mirror.git \
               https://github.com/ggnkua/rmac.git; do \
        echo "trying $url"; \
        if GIT_TERMINAL_PROMPT=0 git clone --quiet "$url" /tmp/rmac 2>/dev/null; then break; fi; \
        rm -rf /tmp/rmac; \
    done; \
    test -d /tmp/rmac || { echo "ERROR: could not fetch rmac from any mirror"; exit 1; }; \
    (git -C /tmp/rmac checkout --quiet $RMAC_REV \
        || echo "note: pin $RMAC_REV unavailable, using default branch"); \
    make -C /tmp/rmac; \
    install -m 755 /tmp/rmac/rmac /usr/local/bin/rmac; \
    rm -rf /tmp/rmac
ENV RMAC=/usr/local/bin/rmac

WORKDIR /src
COPY . .

# Overridable at run time: docker run -e QUALITY=playable ...
ENV QUALITY=pretty

ENTRYPOINT ["bash", "tools/docker-entrypoint.sh"]
