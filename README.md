# Tomb Raider 1 for the Atari Jaguar

A native Atari Jaguar port of Tomb Raider 1, running the real first level on
real hardware: the full Caves, textured and lit, with a runtime-skinned Lara
carrying her complete animation set. Wolves and bats hunt her, she draws and
fires the pistols, collapsing floors give way, switches open doors rooms
away, and medikits are the game's own sprites.

The whole front end is here too, converted off the same disc: the Eidos and
Core logos, the attract cinematic, the 3D passport ring menu with the title
theme streamed from SD, and the trek that plays before the Caves begin.

**This is a development release.** It is a demo of a first level, not a
finished game - expect rough edges and a frame rate that is the point of the
exercise rather than a boast.

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

When it finishes, `./TombRaider-Jaguar/` holds **`OPENLARA.COF`**, the four
front-end clips (**`EIDOS.JV`**, **`CORE.JV`**, **`INTRO.JV`**, **`CAVES.JV`**),
**`MUSIC.PCM`**, **`GYMLOAD.DAT`** and a copy-instructions note. Copy **every**
file onto the **root** of your GameDrive SD card — the game *streams* the music
and the video from the card, so a missing file means a hiss or a skipped clip —
then boot `OPENLARA.COF`. (Level loads take 10–15 seconds — that's the 68000
earning its keep.)

Converting the video is most of the build time. `-e VIDEO=0` (or
`VIDEO=0 ./convert.sh …`) skips it and boots straight to the title screen.

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

It extracts the disc, runs every converter, converts the front-end video, and
builds the whole `./out/` payload. After editing engine code you can rebuild just the ROM with
`make MULTIROOM=1 CFLAGS_EXTRA="-DJERRYPOSE"`.

Rendering is **native 320×240** (no `HALFRES`). The frame is DSP/transform-bound,
not fill-bound, so the full-height framebuffer costs <1% fps versus the old
half-height + line-double path (hardware-measured: 4.79 vs 4.83 fps in Caves) —
i.e. full resolution is effectively free. Add `HALFRES=1` only to reclaim that
last ~1% on the most fill-heavy scenes.

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

- D-pad: move/turn · A: jump · B: grab/action · C: walk
- **OPTION: draw / holster the pistols** — then B fires, exactly as TR1 does it
  (there is no separate fire button). X/Y/Z and the keypad are also mapped:
  1 action · 2 jump · 3 walk · 4 draw · 5 roll · 6 look
- Title screen: LEFT/RIGHT flips the passport/photograph, any fire button selects

## Credits

- **Code: written by Claude (Anthropic)** — the engine, the Jaguar port, the
  GPU/DSP kernels, the asset pipeline and the tooling in this repository.
- **Direction, hardware bring-up and testing: beautifulslop** — what to build,
  what to fix, and every judgement call about how it should look and feel, made
  against real silicon.
- TR1 data formats: [OpenLara](https://github.com/XProger/OpenLara) by
  Timur "XProger" Gagiev (BSD-2-Clause — see LICENSE)
- Title/loading art is RNC ProPack-compressed on the disc; `tools/rnc.py`
  implements the documented method-2 algorithm (cross-checked against
  [ScummVM](https://www.scummvm.org)'s `rnc_deco` — see that file's note)
- Toolchain: [ngdevkit](https://github.com/dciabrin/ngdevkit) · `rmac`/`rln` by
  the Removers · GameDrive BIOS (RetroHQ) · Skunkboard console lib by Tursi
- Tomb Raider is the property of its rights holders. Buy the game; bring your own disc.
