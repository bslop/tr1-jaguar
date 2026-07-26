# LARA'S HEAD — "her face moves all around her head"

User-reported, demonstrated on silicon 2026-07-25 (PLAY_LOADBAR, 45 s capture).
Zoomed on the head: a **skin-toned patch sits on the BACK of her skull and moves
every frame**. So it is a HIDDEN-SURFACE failure, not a texture sliding across
geometry.

## ⚠️ READ THIS FIRST — THE METRIC TRAP THAT COST A ROUND
I first scored the artifact as "cream" pixels: `r>190 AND g>170 AND 110<b<200`.
**Lara's skin renders as (222,162,99) — g=162 is just BELOW that threshold.**
The column therefore counted **7 px out of 1368 — pure noise — in every arm**,
which made every candidate fix look like a null.

Two things caught it:
- the head mesh samples **no cream texel at all** (only palette entries 216/233
  are cream; zero of the 85 head faces touch them), and
- a colour histogram of the box showed the bright pixels are **(222,162,99)**.

The box was also 36×38 and included her **shoulders**. The skull alone is 22×24
at `y 42..64, x 144..168` in the 322-px LOWRES tile.

**Rule: when every arm of an A/B moves by nothing, suspect the metric before
concluding the change does nothing. Dump the histogram of what you are counting.**

## CORRECT METRIC
Skin-tone pixels **on the skull box**, per frame, over a 12-frame walking strip
(`--press up --press-after 1000`, `--start 1300 --every 12`):
```python
(r>180) & (g>120) & (g<190) & (b<140) & ((r-b)>80)
```

## RESULTS (clean bins, matched arms)
| arm | skin px /12 frames | per frame | flicker |
|---|---|---|---|
| current (head skipped) | 331 | 27.6 | 13.17 |
| **TINYCULL=2** | **252** | **21.0** | **10.05** |
| TINYCULL=4 | 248 | 20.7 | 10.13 |
| head winding fixed (`LARA_WINDSKIP=`) | 341 | 28.4 | 13.30 |

**TINYCULL=2 = −24% of the artifact and −24% flicker for 6 bytes**, and it
removes the worst frame entirely (76 px → 17). TINYCULL=4 buys nothing more, so
**2 (area ≤ 1 px²) is the right threshold**. Kept **DEFAULT OFF** pending the
user's eyes on silicon. **~76% of the artifact remains unexplained.**

## MECHANISM THAT IS PROVEN
The backface cull is a **signed area over INTEGER pixel coords** (`sx/sy` are
stored post-divide, `vc_sxp`/`vc_syp`), so on a 2–6 px triangle the SIGN is
decided by rounding. Simulating the real head geometry through a full turn:
| render path | faces/frame with the WRONG cull sign | worst |
|---|---|---|
| **LOWRES** (FOCAL_Y=95, CY=60) | **9.4 of 85** | 20 |
| full-res (FOCAL_Y=190, CY=120) | 5.7 of 85 | 17 |
LOWRES halves vertical resolution ⇒ **64% worse**, which is why this became
prominent right after LOWRES shipped. Lara cannot be caught by the earlier
cull: runtime blobs carry a **dummy plane** that the STAGEDIET N·C test always
passes, so the screen-space test is her only hidden-surface mechanism.

## ELIMINATED — DO NOT RE-RUN
1. **The head mesh data is fine.** 85 faces / 46 verts; **closed manifold** (all
   129 edges shared by exactly 2 faces); **already consistently oriented**
   (0 of 258 directed edges lack an opposite twin). A ray-parity re-orientation
   (concavity-proof, unlike the centroid test) flipped **nothing**.
2. **Winding-fixing the head is a null** — measured twice, on contaminated AND
   clean bins.
3. **Runtime UV assignment is not it.** `build_lara_part` copies face records
   VERBATIM (`main.c:781,786`); the kernel keeps u/v paired to the vertex by
   index (`stage_loop`). Her UVs are static extractor data.
4. The static `_fzkey` front/back ordering cannot fix it — it is baked in T-pose,
   so it stops being true the moment she turns.

## NEXT — ISOLATION, NOT ANOTHER HYPOTHESIS
Identify WHICH faces emit the skin texels on the skull: render mesh 14 alone from
a fixed rear camera and colour-code or disable faces individually. 85 faces, so a
bisect is ~7 builds. That names the culprit instead of guessing at it.
