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

## ★★★ 2026-07-26 — BISECT COMPLETE + MECHANISM
### The culprit faces (all REAR of the skull, all pale hair-highlight texels)
| blob face | tex | z vs head centre | patch px |
|---|---|---|---|
| **quad 150** | 490 | −63.7 | **8** |
| **tri 374** | 526 | −56.9 | **3** |
| **tri 373** | 524 | −56.9 | **2** |
| tri 369 | 522 | −47.2 | ~1 |
| tri 370 / 371 | 523 / 525 | −54.5 | 1 (edge) |
Total 15/15 accounted for. Neighbours 148/149 (tex 489) are dark-brown HAIR and
are correct. The pale tiles are **hair-strand highlight textures** — bold
(247,219,132) zigzags on dark brown — not a face. At 320x120 a cluster of them
on the back of her skull READS as a face. That is the artifact.

### ⇒ MECHANISM: Lara is rendered COMPLETELY UNSHADED
**All 375 of her faces carry shade level k=0** (max `u`=246, so the k bits in
`u[0]` 13-15 are never set), while room geometry carries **k=1..7**. Her hair
highlights therefore render at FULL BRIGHTNESS in a shaded world.

### ⚠️ BUT `LARA_SHADE=k` IS NOT THE FIX — IT BREAKS HER COLOURS
Packing k into her `u[0]` makes the patch vanish (**0/15**) but her limbs turn
GREY-GREEN at k=3 and BLACK with white blotches at k=4. The atlas stores each
ramp's BRIGHTEST entry and k steps down inside an 8-slot ramp
(`palette[bi*RAMP_M + s]`, `idx_of[c] = bi*RAMP_M`); Lara's tiles are not
ramp-aligned that way, so k walks into slots her textures do not own.
**Flag kept, default 0, as the proof of mechanism — do NOT ship it.**

### THE ACTUAL FIX
Make Lara's textures participate in the ramp palette like room tiles, then give
her a real per-face k (TR1 stores per-vertex normals/intensity in the mesh; the
extractor currently SKIPS them: `p += vAbs*8 if vCount>0 else vAbs*2`). Then she
shades with the world and the highlights stop blazing.

## ❌ 2026-07-26 — LARA_SHADE REJECTED ON SILICON (user, same session)
Flashed `LARA_SHADE=3` (textured faces only). User reports:
"weird shading on her butt", "discoloration on her shorts and backpack",
"a layer of shading over her head", and **the head still glitches**.

**Why: shading only her TEXTURED faces makes her INTERNALLY INCONSISTENT.**
The pixel diff is clean proportional darkening (206,146,90 -> 123,81,49, ~0.6x)
applied to just **16.3% of her pixels** — the textured ones. Her shorts,
backpack and hair darkened; her SKIN did not, because skin is drawn by COLOURED
faces which I skipped to avoid the 242..253 swatch slots walking into 254/255.
So half of her is lit and half is not. That is worse than the original artifact.

**And the head still glitches**, so full-brightness highlights were at most part
of the story — not the whole cause.

⇒ `LARA_SHADE` is a DEAD END as built. Shading Lara at all requires her COLOURED
faces to be shadeable too, i.e. the reserved swatch band (242..253) must become
ramp-aligned like the texel ramps (`bi*RAMP_M`). That is an asset-pipeline
change to the palette layout, not a face-record tweak. Flag kept DEFAULT 0.

## ★★★ 2026-07-26 — IT IS A REGRESSION, AND THE CAUSE IS **LOWRES ALIASING**
User: "this happened before and was fixed." Correct, and the proof is in `probes/`.

### The known-good reference ROM
**`probes/RAMP_larafix.cof` (2026-07-20)** renders her head as dark brown hair
with a FEW TINY PALE SPECKS. Today's build renders a big solid pale blob in the
same place. Same 151/224 faces, same textures. **Confirmed regression.**
Its Lara blob is at file offset 576552 (find it by scanning for the header
vcount=300, framecount=66, atlasW=256).

### What differs, tested one at a time
| difference | result |
|---|---|
| 7 head faces wound differently (`LARA_WINDSKIP=`) | **null** (15 -> 16) |
| STATICS textures (atlas 1096 vs 1032) | **null** — blob remains |
| `LARA_TEXSCALE=1` (drop her full-res exemption) | **WORSE** (15 -> 21): TEXSCALE POINT-SAMPLES, which aliases harder |
| **full-res render (no LOWRES)** | **much better** — the pale area becomes fine detail instead of a blob |

⇒ **The renderer went to LOWRES (320x120) on 2026-07-25. Her hair is fine
alternating dark/pale STRANDS, and she is deliberately EXEMPT from TEXSCALE so
her tiles stay full-res** ("holster/boot 1px details mangled at half-res" — an
exemption made when the renderer was 320x240). At half vertical resolution the
sampler lands on the pale strand and the back of her skull turns into a solid
bright blob. **It is ALIASING, not culling, not shading, not winding.**

### THE FIX THAT WORKS: `LARA_VBLUR=1`
Box-filter Lara's tiles along the VERTICAL axis only (3-tap, full width kept) —
the correct low-pass for a 2x vertical decimation. The blob breaks up into
scattered specks, close to the 07-20 look. Bright px on the skull 79 -> 71, and
visually it is the difference between "a face" and "highlights". Body pixels
580 -> 565, i.e. she is otherwise unchanged. Atlas size unchanged (274 KB).
**Point-sampling (LARA_TEXSCALE) is the WRONG tool and makes it worse — the
filter must AVERAGE.**

## ★★★★ 2026-07-26 — THE REGRESSION, PROVEN AT THE PALETTE LEVEL
### The decisive comparison (do this FIRST next time — it took me all session)
`probes/RAMP_larafix.cof` (2026-07-20) renders her head CORRECTLY. Extract its
atlas+palette straight out of the ROM and compare the same tile:
```
lara blob @ 576552 (scan for vcount=300, framecount=66, atlasW=256)
mrt_pal   = lara_off - 512        mrt_atlas = pal_off - 256*atlasH   (atlasH 1032)
```
**quad 150's tile is PIXEL-IDENTICAL in both** (183 pale / 111 dark / 14 mid).
Only the COLOUR changed:
| | atlas byte | pale texel | dark texel |
|---|---|---|---|
| 2026-07-20 | **0** | **(222,170,107)** | (57,16,0) |
| today | **224** | **(247,219,132)** | (41,8,0) |
Her hair HIGHLIGHT got much brighter and yellower; the darks got darker. The
contrast blew up, and that is the "face on the back of her head".

### Cause: she is baked FULL BRIGHT and never shaded
`used_pairs.add((f['tex'],3))` — lvl 3 = `SHADE_FACT[3]` = 256 = full bright.
RAMP_PAL bakes every tile full-bright ON PURPOSE and moves darkening to the
runtime shade pass... but **all 375 of Lara's faces carry k=0**, so she is the
ONE object that never gets darkened. The 07-20 build effectively had her a
shade step down: 222/247, 170/219, 107/132 = 0.90/0.78/0.81 ~=
`SHADE_FACT[2]/SHADE_FACT[3]` = 200/256 = **0.78**.

### Tested, with results
| knob | result |
|---|---|
| `LARA_BASES` 8 -> 12 -> 16 (more palette seats) | **worse** (247->255): the quantiser is faithful, the texel really is that bright |
| `LARA_LVL=2` (bake her tiles a shade darker) | **partial**: bright px on skull 79 -> 64, but shifts other colours (red in her top/backpack) |
| `LARA_VBLUR=1` (vertical low-pass) | **partial**: blob breaks into specks; user said colouring looked "a little more correct" but head still wrong |
| `LARA_SHADE=k` (runtime k) | **rejected on silicon** — half-shaded Lara (textured shaded, coloured not) |

### ⇒ THE PROPER FIX (fully specified, not yet built)
Do what RAMP_PAL's own design intends — **give Lara a real per-face runtime k**:
1. **Make her swatch band shadeable.** Her COLOURED faces use reserved slots
   242..253, which are NOT ramp-aligned, so k walks into 254/255 (UI
   black/white). Those tones need their own ramps (`base*RAMP_M`) like texels.
2. **Emit per-face k for her.** TR1 stores per-vertex normals/intensity in the
   mesh and the extractor SKIPS them (`p += vAbs*8 if vCount>0 else vAbs*2`).
   Decode them, convert to k the same way room faces do, pack into `u[0]` 13-15.
Then she shades with the world, the highlights stop blazing, and it is correct
at every light level instead of one hardcoded compromise.

## ✅✅ 2026-07-26 — THE FIX: `LARA_COLRAMP=1 LARA_SHADE=k`
The two-part repair the RAMP_PAL design always implied.

**1. `LARA_COLRAMP=1` — put her flat tones on RAMP BASES.**
Her COLOURED faces used reserved flat slots 246..253, which are NOT ramp-aligned,
so a runtime k walked into 254/255 (UI black/white) and turned her limbs
grey-green/black. Now each tone is matched to the nearest ramp base
(`bi*RAMP_M`), so k just steps down the ramp like every other surface:
`6->ramp23  8->ramp0  10->ramp18  13->ramp23  14->ramp23  21->ramp19  25->ramp8  34->ramp22`

**2. `LARA_SHADE=k` — now applies to ALL her faces, textured AND coloured.**
Shading only half of her is exactly what the user rejected on silicon ("weird
shading on her butt", discoloured shorts/backpack). With the tones ramp-aligned,
the whole character darkens together.

### MEASURED (idle screenshot, the scene that shows the bug)
| build | bright px on skull | Lara mean luma |
|---|---|---|
| baseline | **79** | 95 |
| `LARA_COLRAMP=1 LARA_SHADE=1` | **15** | 88 |
| `LARA_COLRAMP=1 LARA_SHADE=2` | **0** | 82 |
No grey-green, no black blotches, no white leak — she darkens UNIFORMLY.
k=1 keeps her skin warmest; k=2 removes the blob completely. **Needs the user's
eye on silicon to choose.**

### WHY THIS IS THE RIGHT SHAPE OF FIX
RAMP_PAL bakes every tile full-bright ON PURPOSE and moves darkening to the
runtime shade pass. Lara was the ONE object that never got a k, so she alone
stayed full-bright — her hair highlight blew out to (247,219,132) and read as a
face. This gives her the k the design always intended.

### STILL A COMPROMISE — the last step
k is UNIFORM here, not per-face. The complete version decodes the per-vertex
normals/intensity TR1 stores in her mesh (the extractor SKIPS them:
`p += vAbs*8 if vCount>0 else vAbs*2`) and emits a real per-face k, so she
tracks the room light instead of one hardcoded step. `build_lara_part` could
even OR a room-derived k in as it copies each face record, giving her per-ROOM
lighting for ~nothing.

## ★★★★★ 2026-07-26 — THE SYMPTOM IS **TEMPORAL**: her head TWITCHES
User: "her head keeps twitching" + "I can see through part of it". That is a
MOTION symptom, and every metric in this campaign was a STILL-FRAME metric.

### Measured on silicon (20 s capture, she is standing still)
| region | mean frame-to-frame change | p90 |
|---|---|---|
| **her head** | **1.419** | **4.409** |
| whole screen | 0.205 | 0.297 |
**Her head changes ~7x more than the entire rest of the screen**, on EVERY
rendered frame (6 events/sec at ~6 fps), while the scene around her is static.
⚠️ Capture was 720x**480** here, not 576 — check `Image.open(...).shape` before
decoding raw frames or the numbers are garbage.

### jagemu REPRODUCES it — a proper temporal metric at last
`jagemu video <rom> --count 16 --every 8 --start 1400`, then mean |frame N+1 -
frame N| over a HEAD box vs a BODY box:
| build | head | body | ratio |
|---|---|---|---|
| baseline | **3.468** | 0.657 | 5.3x |
| `LPLANES=1` | 4.748 | 0.657 | 7.2x |
| `TINYCULL=2` | 4.804 | 0.623 | 7.7x |

### ⇒ BOTH CULL FLAGS MAKE THE TWITCH WORSE — TURN THEM OFF
`TINYCULL` and `LPLANES` each destabilise the head further, and `TINYCULL` culls
sub-pixel faces, which is the likely source of "I can see through part of it".
**They were mitigations for a theory that is dead; they must not ship.**
`probes/PLAY_LARAFIX_noTC.cof` = COLRAMP + SHADE=1, both culls OFF.

### NEXT: why does the HEAD alone move every frame?
She is mesh 14 — the LAST mesh, deepest in the matrix-stack chain — so it carries
the MOST accumulated fixed-point (.12) error from `sk_rot`/`sk_translate`. A
small joint jitter at the end of a 15-deep chain moves the head further than the
torso, and at LOWRES (half vertical resolution) that lands as whole-pixel snap.
**Test next:** freeze `g_lframe` (constant pose) and re-measure the temporal
metric. If the head still moves with a frozen pose the instability is numerical,
not animation; if it stops, it is the idle animation being amplified down the
chain and the fix is precision in the pose math (or rounding the head's final
vertices consistently).

## ★★★★★★ 2026-07-26 — ROOT CAUSE: **JERRYPOSE**. Her head is posed by the DSP.
The frozen-pose test split it in one shot. `LFREEZE=1` pins `g_lframe` to one
animation frame, so the pose, the camera and every input are CONSTANT:

| build (frozen pose) | head | body |
|---|---|---|
| ship (JERRYPOSE) | **1.086** | 0.000 |
| PIPELINE off | 1.269 | 0.000 |
| PIPESTAGE=1 | 1.086 | 0.000 |
| **JERRYPOSE OFF (68k poses her)** | **0.000** | **0.000** |

**With identical inputs the 68k path is BIT-STABLE and the Jerry path is not.**
Her body is 0.000 in every arm — only the head moves, and her head is the LAST
mesh Jerry writes. It is not culling, not shading, not texture, not aliasing,
not the pipeline. **It is the DSP pose path.**

### Why every previous fix failed
They all operated on the wrong layer. The vertices themselves are unstable
before anything downstream sees them, so cull flags, planes, winding, palette
and filtering could never touch it — and two of them (`TINYCULL`, `LPLANES`)
made the twitch measurably WORSE (3.47 -> 4.75/4.80) and must not ship.

### The immediate lever: `probes/PLAY_NOJERRY.cof` (JERRYPOSE off)
⚠️ **PERF CAVEAT, UNRESOLVED.** jagemu says moving her pose back to the 68k costs
**-27.8% spans / -26.8% GPU cycles** (68k instret +5.5%) — i.e. the 68k becomes
the limiter. BUT the ledger is explicit that **jsim cannot price 68k<->Jerry work
moves and silicon is the only oracle**, and silicon previously measured
JERRYPOSE as ~neutral-to-slightly-slower (4.72 vs 4.83-4.94, 2026-07-20).
**Do not trust the -27% without a silicon A/B.**

### The REAL fix (keeps both the frame rate and the head)
Find the bug in `dsp_pose.das` rather than disabling it. It hits the LAST mesh
written, which is exactly the signature of a write/read boundary: Tom reading
the tail of Jerry's vertex buffer before Jerry has finished it, or Jerry's final
mesh write racing the completion flag. Note `dsp_pose.das` already has a history
of exactly this class — "REAL dsp_pose races (AUDIO_PUMP restore, indexed-store
root)" were found and fixed 2026-07-20.
**Next: instrument Jerry's per-mesh write completion vs Tom's read of mesh 14.**

## 2026-07-26 — SILICON VERDICT ON `JERRYPOSE=OFF`, AND WHERE THE FPS WENT
**User on hardware: "Head seems a bit more stable though there is a chunk
missing" + "Framerate is slower."** So disabling JERRYPOSE is CONFIRMED as
addressing the twitch, at a real frame-rate cost, and it introduces a NEW
geometry gap.

### The chunk is reproducible in jagemu
Idle head pixels: **JERRYPOSE on 450 → off 443**, and her hair mass is visibly
narrower on one side. Small in this frame, clearly visible to the user on
silicon. **Chase it in the 68k pose path (`build_lara_part`) — it is NOT present
when Jerry poses her.**

### Where the frame rate went — the bottleneck MOVED back to the 68k
| | JERRYPOSE on | off |
|---|---|---|
| 68000 awake | 17.6% (old ledger) | **52.7%** |
| Tom GPU busy | 90.3% | 88.4% |
| spans (1500 fields) | 8,399,792 | 6,066,592 (**−27.8%**) |
| 68k instret | 17,242,812 | 18,193,062 (+5.5%) |

### ❌ `LEMITDIET` CANNOT HELP HERE — measured EXACTLY 0.00%
spans/instret/cycles identical to the digit. **It optimises `lara_finish`, which
is the JERRYPOSE FINISH path — with Jerry off that code never executes.** Do not
spend time on it while Lara is 68k-posed.

### ⇒ THE FRAME RATE IS IN JERRY. FIX `dsp_pose.das`, DO NOT WORK AROUND IT.
Every alternative is worse: LEMITDIET is inert, `M68DIET` is CONVICTED TOXIC
(−62% on silicon), and dieting her faces re-opens the gaps that `LARA_MINAREA=0`
exists to prevent. The one lever that recovers the fps AND keeps the head steady
is repairing the DSP pose.
**The lead is precise: it corrupts the LAST MESH JERRY WRITES (her head), while
every earlier mesh is bit-stable.** That is a write/read boundary — Tom reading
the tail of Jerry's vertex buffer before Jerry has finished it, or the final
mesh write racing the completion flag. `dsp_pose.das` has a documented history
of exactly this class (races fixed 2026-07-20: AUDIO_PUMP restore, indexed-store
root). Instrument Jerry's per-mesh write completion against Tom's read of mesh 14.

---
# ⬜ SEPARATE BUG (user, 2026-07-26) — FLOATING SHADED POLY CHUNKS
**"Chunks of shaded polys that seem to rise up into the player's view. Looks like
CRY mode shading that is wrongly placed."** Awaiting a video capture from the
user — DO NOT theorise before seeing it (this campaign lost a day to exactly that).

Context worth having ready, NOT a diagnosis:
- The framebuffer is **8bpp indexed + RGB16 CLUT**, not CRY (`VMODE $06C7`), so
  "CRY-like" here likely means the RAMP shade steps, not a CRY-mode pixel.
- `SHADEPASS` applies a **RECT shade over each face's bounding-box rows**:
  `SH_Y0` is armed at the cull ("this face rasters") and `pkt_done` shades rows
  y0..y1. A bbox that is armed but not matched to the face it belongs to would
  paint exactly this — **rectangular shaded chunks in the wrong place**.
- Related known-good/bad references: `PHRASE_shade.cof`, `RECTSHADE_v1..v3.cof`,
  `RAMP_fullbright.cof` in `probes/` are all shade-pass experiments from 07-20.
- The user is on `probes/PLAY_NOJERRY.cof` (COLRAMP + SHADE=1, JERRYPOSE OFF,
  no TINYCULL, no LPLANES).

## ★★★★★★★ 2026-07-26 — PROOF: JERRY COMPUTES WRONG VERTICES (not a read race)
`lara_blob` is a static: `nm build/openlara.elf | grep lara_blob` -> `0017ab50 b
lara_blob.8`. Head verts (mesh 14 = verts 254..299) start at
`lara_blob + 16 + 254*8`. With **LFREEZE=1 (pose frozen)** they MUST be identical
every frame. `jagemu peek <rom> --at 0x17b350 --len 32 --frames N`:
```
frame 1400: [  0,  0, 9,189, 0,35,0,255, ...]
frame 1408: [255,225, 9,190, 0,35,0,255, ...]   <- DIFFERENT
frame 1416: [  0,  0, 9,189, ...]               <- back to 1400's value
frame 1424: [255,197, 9,192, 0,15,0,255, ...]   <- different again
```
X swings 0 -> -31 -> 0 -> -59 **with identical inputs**. So the DATA Jerry writes
is wrong — this is NOT Tom reading a half-written buffer, and no amount of
synchronisation will fix it. Note the pattern is **PERIODIC** (1400 == 1416),
which points at a periodic event, i.e. **the DSP ISR**.

### Eliminated (measured, do not re-run)
| suspect | result |
|---|---|
| posted-write drain too weak (`JDRAIN`, read back 8 longs) | **WORSE** 1.086 -> 2.048 — reverted. (It DID change the number, which proves the bug is timing-sensitive.) |
| pipelined single-buffer race (`PIPELINE` off) | still 1.269 |
| `PIPESTAGE=1` | still 1.086 |
| matrix-stack underflow | **balanced** — traced all 15 meshes from `mrt_lskin.bin`: depth never goes below 0 (PUSH at 1/8, POP at 7/14, min 0) |

### ⇒ NEXT SUSPECT: the DSP ISR clobbering pose state
The periodicity is the tell. `dsp_pose.das:996` claims the ISR "stack frame and
fully RESTORED (last one in the return delay slot), so pose/roomx keep every
register" — **audit that claim register by register.** This file has a documented
history of exactly this bug class: "REAL dsp_pose races (AUDIO_PUMP restore,
indexed-store root)" were found and fixed 2026-07-20. Check especially any
register the ISR touches that the vertex loop holds live across the AUDIO_PUMP
site inside `vert_loop` (r10 write cursor, r13 counter, r18/r22/r25 operands).
**Method that works: `LFREEZE=1` + `peek` the head verts across frames — a fix is
correct iff those bytes stop changing.**

---
# ⬜ SEPARATE BUG (user, 2026-07-26) — GREY OUTLINE AROUND LARA'S MODEL
"There's a grey outline around Lara's model." Logged, not yet investigated.
Context to check first, NOT a diagnosis: `inset_uv()` pulls every face corner one
texel toward the UV centroid (anti-bleed), and the atlas cell borders / the 1-px
inset are the obvious candidates for a uniform edge fringe. Also worth ruling in
or out: the `SHADEPASS` rect shade over each face's bbox rows.

### 2026-07-26 — the Jerry HANDSHAKE is sound; four more suspects eliminated
| suspect | verdict |
|---|---|
| **DSP ISR clobbering pose registers** | **DEAD — no ISR runs.** `dsp_pose.das:350`: the I2S interrupt is **PARKED** (disabled 2026-07-12, "save/restore tears it"), and `NOSOUND=1` compiles the sample service out entirely (`dsp_pose.das:21`). |
| **stale done-flag** (sync returns on the previous frame's flag) | **DEAD** — `jerry_pose_kick` does `dsp_mailbox[0] = 0;` *before* `D_CMD = 1` (jerry.c). |
| **magic mismatch** between the two sides | **DEAD** — `jerry.c:85 MAGIC_POSE_DONE 0x0D5BD05E` == `dsp_pose.das:238 MAGIC_DONE $0D5BD05E`. |
| **Tom drawing the blob while Jerry rewrites it** | **guarded** — `main.c:3521` has an explicit PIPELINE collect point: "Present it before any blitter (clear) or pose (lara_blob) work — both would collide with a live render." And PIPELINE-off still shows the bug. |

### ⚠️ CAVEAT BEFORE MORE DSP WORK IN JAGEMU
This ledger already records **"'Jerry 100.0% busy' is a MODEL ARTIFACT"** with
three irreconcilable DSP readings of the same ROM, and a standing request to
cobweb about the run-vs-serve DSP divergence. **The frozen-pose instability is
measured IN JAGEMU.** The user does see the twitch on silicon and does see it
improve with JERRYPOSE off, so the effect is real — but the exact per-frame
vertex values from `peek` may be partly a DSP-model artifact.
**Confirm on silicon before trusting a jagemu-only DSP diagnosis.**

### NEXT STEP (localises it inside Jerry, one measurement)
Peek Jerry's WORKING MATRIX for mesh 14 across frames (frozen pose), not just the
output verts:
- matrix stack `MSTACK = $F1C0C0` (20 x 48 B); the live matrix block is the one
  `mesh_loop` writes each iteration.
- **If the MATRIX differs frame to frame** the error is upstream, in the
  node/rotation chain feeding the last mesh.
- **If the matrix is IDENTICAL but the VERTS differ**, the fault is in
  `vert_loop`'s store path (r10 cursor / packing), not the maths.
That splits the remaining space cleanly and is one `peek` run per arm.

# ✅✅✅ 2026-07-26 — **ROOT CAUSE FOUND AND FIXED**
## Jerry's counters were allocated ON TOP OF Lara's head rotation angles.
`D_PARAMS = $F1C240`. The 68k copies `mcount*3 = 45` angle longs into the window
`D_PARAMS+0x40 .. +0xF4` ($F1C280..$F1C334). Mesh 14 — **her head** — uses angle
longs **42, 43, 44**, which land at **+0xE8/+0xEC/+0xF0**:
| angle long | address | what else was declared there |
|---|---|---|
| 42 (head ax) | $F1C328 | `ISR_COUNT` |
| 43 (head ay) | $F1C32C | **`LOOP_COUNT`** — bumped EVERY `main_loop` pass |
| 44 (head az) | $F1C330 | `WAKE_D` |
`CMD_D` at +0xF8 was correctly OUTSIDE the window; these three never were.
**Jerry overwrote her head's own Y rotation angle after the 68k had copied the
pose in** — so her head, and only her head, was re-rotated every frame.

## How it was found (the method that finally worked)
1. `LFREEZE=1` pins the animation frame ⇒ every input constant.
2. Temporal metric: mean |frame N+1 − frame N| over a HEAD box vs a BODY box.
   Body 0.000, head 1.086 ⇒ only the head is unstable.
3. `peek` the posed head verts ⇒ they change with frozen inputs ⇒ DATA is wrong,
   not a read race.
4. `peek MBLK ($F1C060)` = mesh 14's matrix ⇒ **varies**; `peek MSTACK` = the
   matrix it POPs ⇒ **constant** ⇒ the fault is in mesh 14's own rotate.
5. Its rotate reads angles 42/43/44 ⇒ address arithmetic ⇒ collision.

## The fix (commit below)
Relocate `ISR_COUNT`/`LOOP_COUNT`/`WAKE_D` into the free 5-long gap between
`CAMB_D` (ends $F1C0AB) and `MSTACK` ($F1C0C0): now $F1C0AC/$F1C0B0/$F1C0B4.
The 68k's 6 probe reads in `main.c` were moved to match.

| build (frozen pose) | head | body |
|---|---|---|
| JERRYPOSE, buggy | 1.086 | 0.000 |
| **JERRYPOSE, FIXED** | **0.000** | **0.000** |
| 68k pose (reference) | 0.000 | 0.000 |

**And the frame rate comes back** — spans over 1500 fields:
JERRYPOSE buggy 8,399,792 · **FIXED 8,050,208 (−4.2%)** · JERRYPOSE OFF
6,066,592 (−27.8%). So the fix keeps Jerry doing the work AND steadies her head.
`probes/PLAY_JERRYFIX.cof` is staged.

## Why two sessions of fixes did nothing
Every earlier attempt operated downstream of vertices that were already wrong:
winding, plane culls, sub-pixel guards, palette ramps, shading, vertical
filtering, face deletion. Two of them (`TINYCULL`, `LPLANES`) measurably made the
twitch WORSE. **The lesson: the symptom was TEMPORAL and every metric used for
two sessions was a STILL-FRAME metric.** "Twitching" was the word that cracked it.

# ⬜ REMAINING: the SEE-THROUGH is a SEPARATE, SILICON-ONLY defect
The twitch is fixed (user: "starting to look better") and the frame rate is back.
What is left is a hole in the **SCREEN-RIGHT side of her head**.

### Evidence that it is its own bug, not a leftover of the twitch
| observation | value |
|---|---|
| silicon hair coverage, screen-LEFT vs screen-RIGHT, 5 moments over 25 s | **−29%, −29%, −21%, −16%, −26%** — always the same side |
| **jagemu, same build** | **+4% (symmetric) — the emulator does NOT reproduce it** |
| present with JERRYPOSE **on** and **off** (user saw the chunk on PLAY_NOJERRY) | ⇒ NOT the pose path |
| `jas` hazard check, both kernels | **clean** (only benign wasted-delay-slot warnings) |

⇒ **Silicon-only geometry loss** — the category this project has hit three times
already (div early-read, r22 clobber, load-consumed-across-taken-jump). jagemu
cannot see it, so **the bisect must run on hardware.**

### Staged bisect — the two guards that can drop geometry on ONE side
- `probes/HOLE_noBEXIT.cof` — **BEXIT off**. BEXIT aborts face staging at the
  first behind/far-sentinel vertex; a head vertex misjudged "behind" would drop
  the rest of that face.
- `probes/HOLE_noXCULL.cof` — **XCULL off**. XCULL rejects faces wholly left/right
  of the clip window — a one-sided reject by construction.
Flash each, look at the same spot; whichever closes the hole names the guard.
If NEITHER does, the next suspects are `STAGEDIET`'s early N·C plane cull and the
screen-space winding test itself.

### Useful measurement for judging it
Hair pixels per column across the head box, split left/right half — the number
above. It is objective, works from a single still, and does not depend on pose.

### ☠️ BEXIT=0 BLACK-SCREENS ON SILICON (but renders in jagemu) — arm unusable
Flashed `probes/HOLE_noBEXIT.cof`: **black at 12 s, 24 s and 36 s** on hardware.
Same ROM in jagemu renders normally (57% non-black at frame 900), and the kernel
is 3560/3680 so it is not a size overflow. **`BEXIT` is LOAD-BEARING on silicon**
— without it the stage keeps behind/far-sentinel vertices and something in that
path kills the real Blitter/GPU. Yet another silicon-only divergence in this
kernel, and it means the BEXIT arm of the hole bisect cannot be run this way.
**Do not ship or test BEXIT=0 on hardware again.**
Next arm: `probes/HOLE_noXCULL.cof`.
