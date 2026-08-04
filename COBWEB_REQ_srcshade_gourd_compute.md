# Request: implement SRCSHADE / GOURD per-pixel intensity compute in blit.rs

> **RETRACTION / STATUS 2026-07-20 (v5): every "silicon writes zeros" claim
> below is CONTAMINATED by a probe-harness bug.** v5 (order-controlled) showed
> `copy+DSTEN` works PERFECTLY as the first blit of the harness — and then
> even plain control copies write zeros. Pattern across v1-v5: only the
> harness's FIRST blit ever works; every subsequent blit writes zeros
> regardless of command. Production code chains 68k blits fine (blit_copy /
> blit_double), so the harness differs in some detail under bisection (v6:
> seven identical controls; v7: bisect vs blit.c idiom). Until that lands,
> the only clean silicon facts are: control copy OK as first blit, and
> **DSTEN RMW copy OK as first blit** (which actually *raises* hope for the
> LFU/ADDDSEL shading path). The feature request for blit.rs (model the §4.3
> intensity math) stands regardless.

**Date:** 2026-07-20. **Blocking:** OpenLara Gouraud-shading work (the next
visual-quality milestone). **blit.rs today:** `gourd` selects *static* `B_PATD`
("computation deferred", blit.rs:350); `SRCSHADE` (B_CMD bit 30) is not
referenced at all — a SRCSHADE copy silently behaves as a plain copy.

## What we need modeled

Per BLITTER.md §4.3 / TRM p.74-81:

1. **GOURD (bit 12) + PATDSEL:** per inner-loop pass, add `B_IINC` fraction
   (16b) to the `B_SRCD` intensity-fraction lanes, add `B_IINC` integer (8b)
   with carry to the `B_PATD` intensity-integer lanes, saturate (no wrap),
   carry blocked from intensity->colour unless TOPNEN/TOPBEN. Write `B_PATD`.
2. **SRCSHADE (bit 30):** same intensity DDA, but the computed intensity
   modulates the *source* data (LFU must select source). Used with GOURZ.
3. **B_I0..B_I3 ($F02288/84/80/7C)** convenience scatter into the
   B_PATD(integer)/B_SRCD(fraction) lanes — we program intensities through
   these in the probe below.

## The open semantics question we need answered (HW probe running)

The TRM scopes the intensity path to "16-bit pixel mode". Our framebuffer is
**8bpp indexed**, and our spans are **pixel-mode dest** (XADDPIX, 1 px/pass) —
so the 4-lane/8-px phrase mismatch doesn't apply, but what the intensity adder
does to an 8-bit pixel is undocumented. We built a silicon probe
(`probes/BP_srcshade.cof`, `make BLITPROBE=1`) that runs 7 test blits
(control copy / SRCSHADE +1 / SRCSHADE -1 / GOURD+PATDSEL ramp / GOURD
saturation / SRCSHADE|GOURD / SRCSHADE|GOURZ) on a 16-byte 8bpp span with the
exact A-gen config of gpu_geotex's textured span, and displays the result
bytes as on-screen bit-cells (byte-exact through video capture).

jagemu today returns for those tests: copy=correct, SRCSHADE*=source
passthrough, GOURD=static PATD (zeros). Whatever silicon says, please match it
— we will report the HW byte patterns in this file once captured.

## HW RESULTS v1 (2026-07-20, Skunkboard, video-capture decode, calibration 0 bit-errors)

16-px blits, src = 8bpp bytes, dest 8bpp pixel-mode (XADDPIX), first 8 result
bytes shown. Source constant 0x40 for SRCSHADE rows, ramp 00..77 for control.

| test                                   | silicon result      | jagemu today  |
|----------------------------------------|---------------------|---------------|
| control copy (SRCEN\|LFU_REP\|DSTA2)   | 00 11 22 ... 77 OK  | same (OK)     |
| SRCSHADE, B_IINC=+1int                 | **00 x8**           | 40 x8 (noop)  |
| SRCSHADE, B_IINC=-1int                 | **00 x8**           | 40 x8 (noop)  |
| GOURD\|PATDSEL, B_I0..3=0x20, IINC=+2  | **00 x8**           | 00 x8 (patd)  |
| GOURD\|PATDSEL, B_I0..3=0xF0, IINC=+8  | **00 x8**           | 00 x8 (patd)  |
| SRCSHADE\|GOURD copy                   | **00 x8**           | 00 x8 (patd)  |
| SRCSHADE\|GOURZ copy                   | **00 x8**           | 40 x8 (noop)  |

**Silicon verdict: every intensity-path mode writes ZEROS to an 8bpp
destination.** The TRM's "16-bit pixel mode only" is literal — in 8bpp the
intensity ALU contributes nothing and the written data is 0. So for modeling:
SRCSHADE/GOURD with an 8bpp dest should write 0x00, NOT pass source through.
(jagemu currently diverges on the SRCSHADE rows: it passes source unchanged.)

For 16-bit dests, the per-pixel computation per BLITTER.md §4.3 is still wanted
(that is the path a future CRY16 renderer would use — SRCSHADE on CRY Y was
HW-proven here 2026-07-07, commit a340f02, so 16-bit intensity works on
silicon), but it is no longer the gating item for OpenLara — the ramp-palette
plan now rides on ADDDSEL/LFU behavior instead.

## HW RESULTS v2-v4 — a deeper anomaly: D-path blits appear to WEDGE the Blitter

v2 (LFU-OR, ADDDSEL pixel+phrase, CLIP_A1, GOURD-with-nonzero-PATD): control
copy perfect, EVERY other row wrote 0x00 over the 0xEE prefill. jagemu (same
binary) computes them all correctly (0x45s, ramp+3, patd 0x37s...).

v3 (isolation): even `control copy + DSTEN` (ONE bit different from the
working control) wrote zeros. So did LFU-OR without DSTEN, and single-operand
OR/ADDDSEL in both pixel and phrase mode.

v4 (minimal-delta, no extraneous register writes): same zeros — **including
row 6, the PRODUCTION-PROVEN single-op REPLACE-from-B_SRCD idiom (blit_span /
blit_band shape) that runs in the shipped game every frame.** A standalone-
broken proven idiom is not credible => the current hypothesis is that the
FIRST D-path blit (DSTEN / D-selecting LFU / ADDDSEL / PATDSEL / intensity
ops) wedges the Blitter data path and every subsequent blit writes zeros
until reset. In every probe so far, all zero rows sit after the first D-path
row — order was never controlled for until v5 (control blits interleaved
after each suspect; DSTEN as the very first blit) — results below.

Probe harness caveat for interpreting v1 rows 2+: v1's SRCSHADE rows also sat
after... no — v1 row 2 (SRCSHADE) was itself the first non-control blit and
wrote zeros, so SRCSHADE-writes-zeros stands on its own, but the later v1
GOURD rows are contaminated by the same order effect and need re-running in
isolation before being treated as ground truth.

**Ask (updated):** cobweb has its own silicon rig (the Blitter timing probes).
Please cross-check on your Jaguar: (1) does `SRCEN|DSTEN|LFU_REPLACE|DSTA2`
8bpp pixel-mode write source or zeros as the FIRST blit after boot? (2) does
a D-path blit poison subsequent plain copies? Our unit may be damaged, or
this may be a real chip erratum the emulator should model.

## FINALE (2026-07-20): the answer came from testing IN THE PRODUCTION KERNEL

Probe rounds v13-v16 moved to a GPU-driven harness (gpu_blitprobe.gas):
same signature — only the first blit per BOOT ever landed, across processors
(68k and GPU), kick boundaries, parameter-passing paths, and environments
(pre-init / post-gpu_init / hot-swapped mid-game after 15s of the game's own
flawless chained spans). **Sixteen rounds; the standalone-probe mystery is
UNSOLVED and is yours if you want it** (all probes preserved:
probes/BP_srcshade1..16b.cof; harness in main.c `blitprobe_run()` +
gpu_blitprobe.gas, `make ... BLITPROBE=1`).

**But the shipping question is ANSWERED. `make ... SHADEPASS=1` patches
gpu_geotex.gas to chain a SECOND blit per span — `DSTEN|LFU(S|D)|DSTA2`,
B_SRCD=0x02020202, i.e. a dest-READ-modify-write OR over the span just
written. On real silicon the game renders every frame with the expected
texel|2 palette shift across the whole scene (probes/SHADEPASS_or2.cof,
shade_hw.png): chained per-span RMW blits + D-dependent LFU compute work
PERFECTLY in the production kernel context.** jagemu agrees pixel-for-pixel
in character. Measured cost of the naive always-on pixel-mode pass:
4.72 -> 3.89 fps (-18%); the real ramp shade pass will skip k=0 faces and
can go phrase-mode.

So for blit.rs modeling priorities: the LFU/D-path is already right; the
GOURD/SRCSHADE intensity compute request stands (16-bit CRY path, for a
future renderer); and the standalone first-blit-only anomaly is a genuine
silicon puzzle worth your logic analyzer.

---

## POSTSCRIPT (2026-07-20 night): phrase-mode RMW dest-read failure — jsim MATCHES silicon

A phrase-mode variant of the shade pass (DSTA2 single-op, DSTEN|LFU-OR,
phrase-aligned bounds) intermittently writes the RAW B_SRCD pattern instead
of S|D — dest reads failing — as bright phrase blocks in dark areas. The
SAME corruption appears in jsim and on the console (PHRASE_shade.cof,
frame region: cave mouth). So (a) your phrase-RMW model faithfully captures
a real silicon behavior — nice; (b) the underlying mechanism (when/why the
phrase dest read yields zeros) is now dual-reproducible if anyone wants to
chase it; (c) for us it is moot: phrase-mode also gave ZERO fps gain on
silicon (3.65 vs 3.65) — short spans are launch-bound. We reverted to the
pixel-mode pass and closed the line of work.
