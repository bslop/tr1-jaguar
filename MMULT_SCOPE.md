# MMULT vertex-transform — SCOPE (2026-07-23)

**Target:** the vertex pre-pass = 65% of Tom = ~49% of frame (CULLWALK_SCOPE.md),
run in software imul32 while Tom's hardware MMULT unit is used zero times.

## What MMULT does (sim isa.rs `mmult`, opcode 54)

One instruction = ONE length-`width` dot product, accumulated in `mac`:
- `width = mtxc & 0xF` (MWIDTH 3..15) — set once via a control-reg write.
- **Vector operand:** bank-1 registers from r1, packed 2× s16 per reg.
- **Matrix operand:** `width` s16 values from LOCAL SRAM at `mtxa`
  (stride 4 row-major, or width*4 if `mtxc & 0x10` column-major).
- `acc = Σ vec[i]·mat[i]` (s16×s16 → i64 mac), low 32 bits → r2.
- Cost model: `width` cycles (one multiply/tick) → **3 cyc for a 3-vec.**

So a 3×3 matrix × vector = 3 MMULTs (one per output row), each with `mtxa`
pointed at that row.

## The mapping IS clean — but needs a precomposed 3×3

Current per-vert math (vc_loop, ~7 imul32 for the rotate):
```
rx = (dx·cY − dz·sY) >> 12                 ; yaw
rz = (dx·sY + dz·cY) >> 12
ry = (dy·cP − rz·sP) >> 12                  ; pitch (uses yaw's rz — INTERLEAVED)
rz'= (dy·sP + rz·cP) >> 12
```
The interleave (pitch consumes yaw's rz) is why it's 7 sequential imul32.
**Precompose yaw·pitch into a flat 3×3 once per frame** and every output
becomes an independent dot product of (dx,dy,dz):
```
R = | cY       0     −sY     |   ; from cY,sY,cP,sP (already in CAM_BUF)
    | −sY·sP   cP    −cY·sP  |   ; 4 extra products, computed ONCE/frame
    | sY·cP    sP     cY·cP  |
(rx,ry,rz) = R·(dx,dy,dz)  →  3 MMULTs/vert, no interleave.
```

## Win estimate

- Current rotate: ~7 imul32 × ~15 cyc = **~105 cyc/vert.**
- MMULT rotate: pack (dx,dy,dz)→bank-1 (~3) + 3×(set mtxa + MMULT) (~18) +
  read/>>12 (~6) = **~27 cyc/vert. ~4× on the rotate.**
- Rotate is the bulk of the pre-pass (rest = vert loads + the projection
  divide, unchanged). If rotate ≈ 75% of the 65%-of-Tom pre-pass:
  **~4× on ~49% of pre-pass ≈ −37% of Tom ≈ −28% of FRAME.** Largest single
  lever in the campaign by far (vs 4-5% for span/cull work). ~5 fps → ~7.

## Risks / unknowns (ordered)

1. **MMULT silicon-fidelity UNVALIDATED.** The sim models it; real Tom's
   systolic array (latency, mac semantics, the width-cycle pipeline) has
   NOT been silicon-checked in this project. Every complex instruction has
   surprised us (div interlock, blit cadence). **Phase 0 = a calib probe:
   known M×v via MMULT vs imul32, on the rig. Do this FIRST — do not build
   on an unvalidated instruction.** (cobweb may already have an mmult probe;
   check calib/ before writing one.)
2. **s16 range of (dx,dy,dz).** MMULT operands are s16; the current imul32 is
   32-bit. dx = wx − camx can exceed ±32767 in large rooms / distant verts
   (the vert is s16 but +roomOffset −camera is not bounded to s16). Out-of-
   range → silent truncation → warped geometry. Needs a per-vert range guard
   (fall back to imul32, or pre-scale). STAGEDIET's existing "camera exceeds
   s16" ok-flag is a related precedent. **This is the binding correctness
   risk.** (Accumulator is fine: 3×32767×4096 < 2^31, and cY..cP ≤4096 fit
   s16 trivially.)
3. **SRAM for the matrix.** Row-major needs 9 s16 at 4-byte stride = ~36 B in
   Tom's SRAM (packed already — the scraps are $F03F20/4B, $F03F38-3F/8B).
   Needs a byte diet or a repurposed buffer; the per-frame matrix write is
   cheap (once/frame, not per vert).
4. **mtxa reset per row** (3 control-reg writes/vert) — folded into the ~27
   cyc estimate; if it dominates, column-major layout or a single-issue
   full-matrix MMULT mode (check TRM) trims it.
5. **Bank-1 operand collision.** MMULT reads the vector from bank-1; BANKDIET
   residents occupy r1-r12. Use free r13-r15 for the packed (dx,dy,dz);
   ~2 moveta/vert (in the estimate).

## Phasing

- **Phase 0 (rig, ~30 min):** silicon-validate MMULT (calib probe). GATE.
- **Phase 1:** precompose R (9 coef) per frame — 68k-side or a few kernel
  instr at frame start; store to the SRAM matrix region; set mtxc=3.
- **Phase 2:** replace vc_loop rotate with pack + 3 MMULT + >>12, plus the
  s16 range guard. Keep imul32 path under a flag for the fallback + A/B.
- **Phase 3:** emu pixel-identity gate (geometry must match to ±1px class),
  then silicon fps + visual A/B. Flags-off byte-identity as always.

## Recommendation

Phase 0 first — one short rig probe. MMULT is a 28%-of-frame prize but it's
the most complex instruction we'd depend on, and unvalidated. Validate, then
build. If MMULT diverges on silicon, the fallback is a tighter software
transform (fewer imul32 via the precomposed 3×3 = 9 straight muls, no
interleave — a smaller but safe win on its own).

**Extra Phase-0 flag:** the sim (isa.rs `mmult`) and the TRM spec
(RISC_ISA.md §7.2) DISAGREE on operand placement — sim reads the matrix from
local SRAM + vector from bank-1; the spec calls the bank-1 operand the matrix.
Functionally it's a dot product of (bank-1 packed)·(SRAM) either way and we
control both operands — but the probe must pin which layout silicon uses
before Phase 1 commits. No existing mmult probe in cobweb calib/ (checked) —
Phase 0 authors one.
