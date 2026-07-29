# The shipping build recipe (verified 2026-07-29)

Reconstructed by rebuilding until the output was **byte-identical** to the last
known-good ROM, not by reading it out of campaign prose — that mistake has cost
this project three flashes and the whole LOWRES win before (see
`feedback_build_from_artifact_not_docs`).

```
make MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 STAGEDIET=1 \
     PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
     ROWDIET=1 STATICS=1 BANKDIET=1 LOWRES=1 FLIPASM=1 NOSOUND=1 \
     DIVZGUARD=1 MOVESET=1
```

Without `MOVESET=1` this reproduces `probes/PLAY_MOVESET2.cof` exactly
(md5 `b56b103aef657e29e1cbe02aa880b9f0`, 1482088 bytes) when `MOVESET=1` is
added — that identity is what pins the list down.

## How the last two flags were found
The first guess was 992 bytes too large. Diffing the linker maps against the
known-good ROM's map isolated it to two objects: `dsp_blob` (−984 B ⇒ `NOSOUND`)
and `gpu_blob` (+8 B ⇒ `DIVZGUARD`). A single remaining byte then differed, at
`g_hopcap` in `.data` — that is `HOPBOOT`, whose value is baked in, and it was
`1`, not the Makefile default of `4`.

**`NOSOUND=1` is in this list.** The recent probe ROMs are SILENT. That is right
for measurement and wrong for a demo — drop it when building for the user, and
re-verify, because it moves ~1 KB of DSP code and re-rolls the boot lottery
below.

## Flashing
`jcp` will NOT flash over a running ROM: the upload runs to ~99% and then fails
with `can't connect with skunkboard`. **Reset first** — `jcp -r`, wait ~3 s,
then flash. That replaces the "one flash, then a physical bounce" rule in the
older notes; no power cycle is needed for the normal case. A flash is ~190 s;
never put a short timeout on it. If `jcp -s` itself starts failing, the board
has wedged and DOES need a physical power cycle.
