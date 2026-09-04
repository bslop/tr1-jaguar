# jag_openlara — Architecture

## What this is

A native Atari Jaguar port of Tomb Raider 1 that runs the real first level on
real hardware: the full Caves, textured and lit, with a runtime-skinned Lara
carrying her complete animation set — and the whole front end converted off the
same disc. It is a shipped, hardware-verified engine, which is why it is the
reference other projects here are pointed at for a full 3D pipeline at scale.

## Source material

- The original Tomb Raider 1 disc — assets extracted offline by
  `tools/tr2jag_multiroom.py`. See [`ASSET_BUILD.md`](ASSET_BUILD.md); that
  pipeline was recovered the hard way and the doc is the record of it.
- The OpenLara engine as the starting geometry/format reference. ⚠ The port no
  longer depends on the surrounding OpenLara tree: it reaches nothing outside
  its own repo root, which is what made this folder reshaping safe.

## The shape of the port

- **Renderer**: polygons with affine per-span UV through the Blitter, CRY for
  shaded work. Span shading is a separate pass over each face's bbox rows.
- **Coprocessors**: a GPU kernel does the span work; a resident DSP mixer
  handles audio with music streamed from SD.
- **Front end**: logos, attract cinematic and a 3D passport ring menu, all
  converted from the same disc.
- **Build**: `make $BUILD_FLAGS VRESN=<n>`; `tools/build_cof.sh` is the
  container entrypoint and produces the SD payload in `out/`. `PLAY_BUILD.md`
  is the release log and names the ROM of record.

## ⚠ Top risk

**A10 — a build can black-screen real silicon from t=0, with no crash and no
emulator symptom.** It is not size, not absolute addresses, not alignment, not
the GD BIOS, and not `gpu_sync`'s STOP; all were measured and eliminated. A
semantics-preserving register reallocation with zero source change
(`-frename-registers`) is sufficient on its own to trigger it, and the result is
deterministic per build rather than per boot.

That makes it a risk to the *plan*, not just to a build: the demo endpoint is
more edits to one 6,000-instruction function, and every feature re-rolls the
same lottery. A `noinline` mitigation ships and works, but it is a mitigation.

The second risk is the measurement one. This project has 8+ cases of "jagemu
passes, silicon dies", and three separate metrics have failed on bug A1 by
quantifying something other than the reported artifact. Treat an offline number
as a hypothesis about the hardware until the rig agrees.
