# Tomb Raider 1 for the Atari Jaguar

A native Atari Jaguar port of Tomb Raider 1, running real TR1 levels on real
hardware: full Caves and Lara's Home, textured + lit, runtime-skinned Lara with
her complete animation set, the authentic title screen with the 3D passport
ring menu, sound effects, and the title theme streamed from SD.

Every Jaguar processor earns its keep:

| Chip | Role |
|---|---|
| 68000 | game logic, animation state machine, dispatch, SD streaming |
| Tom (GPU) | per-face transform/project/cull + edge walk, drives the Blitter |
| Blitter | affine-textured span fill |
| Jerry (DSP) | Lara skeletal pose, room vertex transform, audio mixing |
| OP | 320×240 display of the line-doubled framebuffer |

**This repository contains no game assets.** You need your own disc image of
*Tomb Raider* (PlayStation, USA v1.6). Assets are extracted locally at build
time and never leave your machine.

## Quick start (Docker)

All you need is [Docker](https://docs.docker.com/get-docker/) and your own
*Tomb Raider 1 (USA)* PlayStation disc, in any common shape — `.iso`,
`.bin`+`.cue`, `.chd`, `.7z`, or a folder of the disc's `PSXDATA`:

```sh
./convert.sh "Tomb Raider (USA) (v1.6).cue"
```

A couple of minutes later `./TombRaider-Jaguar/` holds **`OPENLARA.COF`**,
**`MUSIC.PCM`**, and a short copy-instructions note. Copy **both** files onto the
**root** of your GameDrive SD card — the game *streams* `MUSIC.PCM` from the card,
so skipping it makes the title screen hiss — then boot `OPENLARA.COF`. (Level loads
take 10–15 seconds — that's the 68000 earning its keep.)

The container extracts the disc, converts every asset, and compiles the ROM with
a pinned toolchain, so the same disc always produces the same build. Nothing is
uploaded anywhere — it all happens locally. Works on Linux, macOS, and Windows
(run it inside WSL2 or Git Bash with Docker Desktop running).

To run the result you need a RetroHQ Jaguar GameDrive (recommended) or a Skunkboard.

## Manual build (no Docker)

Install the toolchain yourself — the ngdevkit `m68k-neogeo-elf` GCC
([ppa:dciabrin/ngdevkit](https://launchpad.net/~dciabrin/+archive/ubuntu/ngdevkit)),
`rmac` ([ggnkua/rmac](https://github.com/ggnkua/rmac), on your `PATH` or at
`~/jaguar-tools/bin/`), Python 3 + Pillow, `ffmpeg`, and `p7zip` (plus `bchunk`
and `chdman` if your disc is a single `.bin` or a `.chd`) — then one script does
everything the container does:

```sh
tools/build_cof.sh "Tomb Raider (USA) (v1.6).cue" ./out
```

It extracts the disc, runs every converter, and builds `./out/OPENLARA.COF` +
`MUSIC.PCM`. After editing engine code you can rebuild just the ROM with
`make MULTIROOM=1 HALFRES=1 CFLAGS_EXTRA="-DJERRYPOSE"`.

## How the disc becomes a ROM

The pipeline (all in `tools/`, driven by `build_cof.sh`):

1. **`extract_disc.py`** — pulls `PSXDATA/*.PSX`, the `DELDATA` title/loading
   art, and the CD-audio track out of your disc image (auto-detecting `.iso` /
   `.bin`+`.cue` / `.chd` / `.7z` / folder), and checks it's really TR1 USA
   (SLUS-00152).
2. **`tr2jag_multiroom.py`** ×2 — Caves and Lara's Home geometry, textures, and
   the runtime-skinned Lara mesh + animation set.
3. **`gen_titlebg.py`** (+ **`rnc.py`**) — RNC-decompresses the title and loading
   screens and quantizes them to the Jaguar's 8bpp + RGB16 CLUT format.
4. **`tr2jag_title.py` / `_font.py` / `_sound.py` / `_music.py`** — the 3D
   passport menu, UI font, sound effects, and the SD-streamed title theme.
5. **`make`** — compiles the 68000 / Tom / Jerry code and links the ROM.

## Controls

- D-pad: move/turn · B: grab/action · C: walk · A: jump
- Title screen: LEFT/RIGHT flips the passport/photograph, any fire button selects

## Credits

- Engine + Jaguar port: beautifulslop <beautifulslop@gmail.com>, built with Claude (Anthropic)
- TR1 data formats: [OpenLara](https://github.com/XProger/OpenLara) by
  Timur "XProger" Gagiev (BSD-2-Clause — see LICENSE)
- Title/loading art is RNC ProPack-compressed on the disc; `tools/rnc.py`
  implements the documented method-2 algorithm (cross-checked against
  [ScummVM](https://www.scummvm.org)'s `rnc_deco` — see that file's note)
- Toolchain: [ngdevkit](https://github.com/dciabrin/ngdevkit) · `rmac`/`rln` by
  the Removers · GameDrive BIOS (RetroHQ) · Skunkboard console lib by Tursi
- Tomb Raider is the property of its rights holders. Buy the game; bring your own disc.
