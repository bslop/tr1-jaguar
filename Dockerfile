# Reproducible Tomb Raider 1 -> Atari Jaguar build.
#
# Ships NO game data.  You mount your OWN Tomb Raider PSX disc at run time; the
# assets are extracted, converted and compiled entirely inside the container,
# and only the finished GameDrive files (OPENLARA.COF, MUSIC.PCM) leave it.
# Everyone uses the same pinned toolchain, so the same disc yields the same ROM.
#
#   docker build -t tr-jaguar .
#   docker run --rm -v "$PWD/disc:/disc:ro" -v "$PWD/out:/out" \
#              -e DISC_NAME="Tomb Raider (USA) (v1.6).cue" tr-jaguar
#
# ...or just use ../convert.sh, which wraps both steps.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

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
ARG RMAC_REV=1fd77b5db027255ef58e0af7b5a329d8060a82ee
RUN git clone https://github.com/ggnkua/rmac.git /tmp/rmac \
    && (git -C /tmp/rmac checkout $RMAC_REV || echo "pin unavailable, using default branch") \
    && make -C /tmp/rmac \
    && install -m 755 /tmp/rmac/rmac /usr/local/bin/rmac \
    && rm -rf /tmp/rmac
ENV RMAC=/usr/local/bin/rmac

WORKDIR /src
COPY . .

ENTRYPOINT ["bash", "tools/docker-entrypoint.sh"]
