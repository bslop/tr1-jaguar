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

## ☠️☠️ 2026-07-25 LATE — THE SKIN-PIXEL METRIC IS CONFOUNDED BY HER NECK
**`LARA_DROPFACE=<all 85 head faces>` (whole head deleted) makes the skull-box
skin count GO UP: 360 vs 331.** Deleting the head exposes the NECK STUMP, which
is a larger skin area than the artifact. So the counter was never measuring the
artifact — **the number said "worse" while the picture said "fixed".**
**Judge this bug from IMAGES, not from a skin-pixel count.** Side-by-side zooms
of the skull box are the only reliable instrument found so far.

## ✅ CONFIRMED BY CONSTRUCTION: the artifact IS the head mesh
With mesh 14 deleted the yellow face-patch is **completely gone** from the
render. So the source is a head face — not another mesh, not the background.

## ❌ THE 10-FACE DROP DID NOT FIX IT (silicon, user-verified)
`LARA_DROPFACE=316,317,322,323,325,326,331,332,333,334` flashed to hardware:
**user still sees the face rotating around her head.** Those 10 were chosen from
**T-POSE normals with a fixed ±z front/back assumption**, which does not survive
the real pose and camera. The selection method is wrong, not just the set.

## ☠️ FLASH BURNED: dropping faces WITHOUT fixing the header counts
The extractor writes `MRT_LARA_QCOUNT/TCOUNT` from `len(lquads)/len(ltris)` —
the PRE-drop lengths — while `LARA_DROPFACE` shrinks the blob. The C side then
reads **10 tris past the end of Lara's blob**: tolerated in jagemu (it just
reads neighbouring ROM), **BLACK SCREEN on silicon**. Fixed by emitting `_nqk`/
`_ntk`. **Verify `blob header == mrt_lara.h` before every flash.**
**And verify a ROM at a frame where LARA IS ON SCREEN (~1300 with AUTOSTART),
not at frame 400 — that is the TITLE, where she isn't loaded and the bug
cannot show.**

## ALSO ELIMINATED (measured, don't re-run)
- **Every mesh is wound the same way.** Signed volume is NEGATIVE for all 15
  meshes including the head ⇒ the head is NOT inside-out relative to the body,
  and the cull convention is globally consistent.

## NEXT — ISOLATION, NOT ANOTHER HYPOTHESIS
Identify WHICH faces emit the skin texels on the skull: render mesh 14 alone from
a fixed rear camera and colour-code or disable faces individually. 85 faces, so a
bisect is ~7 builds. That names the culprit instead of guessing at it.

## ☠️ 2026-07-25 LATER — WHY THE FACE BISECT DID NOT CONVERGE
Two independent confounds, both discovered the hard way. **Read this before
attempting a per-face hunt again.**

### 1. DROPPING a face perturbs the whole draw order
`LARA_DROPFACE` removes the face from the blob, which shifts every LATER face's
position. Since the kernel has no depth buffer and relies on emit order, that
changes overdraw globally. Symptom: dropping face 311 alone and face 312 alone
BOTH scored 5, with the signal jumping to a different frame each time. A bisect
built on dropping is measuring order changes as much as the culprit.

### 2. TINTING (all four UVs to one texel) makes the face INVISIBLE, not coloured
`LARA_TINTFACE` was written to be order-preserving — same face count, same
positions, only the UVs changed to a distinctive olive texel (idx 136 =
(115,138,0) at atlas u=0,v=128). Result: **ZERO olive pixels anywhere**, while
frame 0's bright-skin count still fell 114 → 54. A degenerate UV span produces a
zero-width blit, so the face disappears instead of rendering flat olive.
Useful as an order-preserving HIDE; useless as a LABEL.
**To label a face, give it a small but NON-DEGENERATE uv rect on a flat-colour
atlas cell — never a single repeated texel.**

### Bisect results so far (treat as SOFT — see the confounds above)
    drop half A (42 faces)  45 -> 1
    drop A1 (21)            45 -> 8
    drop A2 (21)            45 -> 0
    drop A2a (10, #311-320) 45 -> 0
    drop #311-315 (5)       45 -> 0
    drop #311 alone         45 -> 5     <- inconsistent with #312 alone = 5
Suggestive of the #311-320 region, NOT proven.

### THE METRIC THAT IS ACTUALLY CLEAN
Bright-skin px in the box `y40..56, x144..170` of the 322-px LOWRES tile:
full head = 45, **head deleted = 0 exactly**. Anything below y=56 is her NECK and
contaminates the count. Validate any new metric against the head-deleted control
before trusting it.

## RECOMMENDED FIX — stop hunting faces, fix the cull
The per-face hunt keeps fighting instrumentation. The root cause is already
proven: Lara's faces carry a DUMMY plane, so the exact early N·C cull never
applies to her, and her only hidden-surface test is a screen-space signed area
whose sign is quantised on sub-pixel triangles. Give her REAL per-face planes:
1. extractor bakes a MESH-LOCAL normal per Lara face (static — her mesh does not
   deform, only the per-mesh matrix changes);
2. `build_lara_part` already computes each mesh's pose matrix — rotate the baked
   normal by it and emit a real plane prefix instead of the `0xF800` dummy;
3. the kernel's STAGEDIET N·C test then culls her correctly at every angle, with
   no dependence on projected area.
### The cheap formulation — DO NOT rotate 375 normals per frame
Naively rotating a normal per face costs ~15 muls x 375 faces on a 68000 and
would dominate the frame. Use the standard trick instead: her meshes are RIGID,
so the face normal is STATIC in mesh-local space. Transform the CAMERA into each
mesh's local space once — **15 transforms per frame, not 375** — and the per-face
test collapses to `N_local . (C_local - P_local) < 0`, i.e. a dot product against
numbers the extractor can bake offline.

Concretely:
1. **Extractor**: bake the real mesh-local plane `{N_local, d_local}` into every
   Lara face record, replacing the `0xF800` dummy. ZERO runtime cost — it is the
   same 12-byte prefix the room faces already carry, and `STAGEDIET` already
   knows how to read it.
2. **Runtime**: `build_lara_part` already builds each mesh's pose matrix `m`.
   Invert-transform the camera through it (a transpose for the rotation plus a
   translate, since `m` is orthonormal) to get `C_local` for that mesh.
3. **Kernel**: `STAGEDIET`'s existing `N.C < d` test then culls her EXACTLY, with
   no dependence on projected area — so it holds at every angle and is immune to
   the sub-pixel sign flips entirely.
   The one piece of new plumbing: `CAMLOC` is a single global read per face, so
   the blob needs a per-mesh `C_local` the kernel picks up as it crosses each
   mesh group (Lara's faces are already emitted grouped by mesh via `morder`).

That is the principled repair; deleting geometry is not. It also retires
`TINYCULL` for Lara, and would fix the same class of bug on any future runtime
blob (props, pickups) rather than just her head.

## ★★★ 2026-07-26 — THE WHOLE HIDDEN-SURFACE THEORY IS WRONG
### 1. The bug lives in the IDLE scene, and jagemu reproduces it there
Every measurement in this campaign used a WALKING strip (`--press up
--press-after 1000`, start 1300), where the artifact is rare and small. **Render
her standing still instead — plain `screenshot --frames 1500` with NO input —
and the pale face-patch is plainly visible in the middle of her hair, exactly
what the user sees on silicon.** That is the reproduction case. Use it.

### 2. A CORRECT back-face cull does not remove it
`LPLANES=1` (real per-face planes, validated: 34 visible from behind + 51 from
the front = 85 exactly) and an ORDER-PRESERVING hide of the 10 rear-skin faces
both render **pixel-for-pixel the same head as the baseline** in the idle scene.
Silicon agrees: the user sees no change.

### 3. ⇒ The offending polygon is FRONT-FACING. It is a TEXTURE problem.
If an exact plane cull keeps the face, the face is genuinely visible from this
camera — so this was never a hidden-surface failure. **A legitimately-visible
rear-of-head polygon is TEXTURED WITH HER FACE.** That is a texture/UV
assignment problem in the DATA path, not a renderer problem.

**This retires the entire cull line of attack:**
- `TINYCULL` — sub-pixel sign guard. Keep for ROOM geometry; irrelevant here.
- `LPLANES` — correct and perf-neutral (spans -0.07%), but does NOT fix this.
- dropping/hiding faces — chasing the wrong mechanism.

### NEXT: which texture lands on which head polygon
The head is 85 faces; 32 are >50% skin-textured, 22 facing front and **10 facing
rear**. Those 10 are legitimately visible from behind and carry face texels.
Check the extractor's Lara face->texture assignment (`build_lara` reads
`v0..v3` then `flags`, and `tex = fl & 0x7FFF`) against what TR1 actually
intends, and compare a known-good render of TR1 Lara's head from behind. Note
the room path had EXACTLY this class of bug once — "extractor read room faces
verts-first, PSX is TEX-first" — fixed for rooms, never re-checked for Lara.

### Also eliminated 2026-07-26: the "unread COLOURED face lists" theory
OpenLara's PC-format mesh reader (`format.h:5806-5811`) reads FOUR lists —
rCount, tCount, **crCount, ctCount** — while our `build_lara` reads only the two
textured lists. That looked like the bug. It is not: auditing the words that
actually follow Lara's textured lists in LEVEL1.PSX gives **garbage**
(65528, 65529, 86, ...) for all 15 meshes, so TR1 PSX does not carry separate
coloured lists and our two-list reader is right. The `tex < 256` colour
heuristic stands.

## ★★★ 2026-07-26 — CULPRIT NAMED: QUAD 150 / objtex 490
### The instrument that finally worked
The patch is **exactly palette colour (247,219,132)** on the skull. Count THOSE
pixels in `y40..90, x130..190` of the idle screenshot:
  baseline **15** | head deleted **0** | all-head hidden **0**
Every earlier metric had a FLOOR made of her shoulder skin (222,162,99) — that
is why "wins" of 24% and 73% were invisible to the user. Use the exact colour.

### The tool that finally worked
`LARA_TINTFACE` (all 4 UVs -> one texel) is an ORDER-PRESERVING **label**: the
tinted faces render flat OLIVE (idx 136). My earlier "it makes faces invisible"
note was WRONG — it was read off the walking scene where the head is tiny.
Hiding all 85 head faces turns the whole head olive, which is the control.

### Bisect (idle scene, order-preserving, all controls clean)
    hide 42 (half A) -> 7    hide 43 (half B) -> 8
    hide A1 (21)     -> 7    hide A1a (10)    -> 7
    hide quads 148-150 -> 7  hide 148 -> 15   hide 149 -> 15
    **hide 150 alone -> 7**  (removes 8 of the 15 patch px)

### QUAD 150 IS A REAR-OF-SKULL FACE CARRYING A FACE-LIKE TEXTURE
    quad 150  verts 298,297,295,296   z = -50..-53  (centroid 64 BEHIND centre)
              tex = objtex **490**    uv (57,929)-(70,950)
    atlas tile at that uv: dominant colour **(247,219,132)** = THE PATCH COLOUR,
      with dark-brown banding — reads as face/skin, not hair.
    quads 148,149 (also rear) use objtex 489 -> dark brown HAIR. Correct.
Original disc data: objtex 489 = page 6 (144,104)-(176,144); objtex 490 = page 7
(56,136)-(72,160). So TR1 itself points this rear quad at 490; the open question
is whether our decode of 490 (tile 7, **clut 81**) is faithful. The
`tex_preview/page_07.png` crop looks structurally similar but is rendered with a
different CLUT, so colour cannot be compared from it — decode 490 with clut 81
and compare against the atlas tile.

### REMAINING
Quad 150 accounts for 8 of 15 patch px. The other 7 are in half B (43 tris) and
have NOT been bisected yet. Same method, same script (`idle_round.sh`).
