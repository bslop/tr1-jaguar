# calib: silicon-validate MMULT (op 54) — it gates a 28%-of-frame optimization

**Severity:** high — this is the Phase-0 gate for the single biggest lever we've
found in the whole perf campaign, and MMULT is the most complex instruction we'd
depend on.

**Env:** cobweb `f50c2ea` · `sim/crates/jag-core/src/risc/isa.rs::mmult`,
`sim/docs/spec/RISC_ISA.md §7.2`

## Why

pc-histogram + GPU-cycle decomposition of the OpenLara play kernel (jsim silicon
fidelity, boot-amortized) pins the bottleneck exactly:

| Tom component | % of Tom | % of frame |
|---|---|---|
| **vertex pre-pass (transform every room vert)** | **65%** | **~49%** |
| per-face loop setup | 17% | ~13% |
| rasterization | 18% | ~13% |

The pre-pass runs a yaw+pitch camera rotation as ~7 sequential software `imul32`
per vertex. **Tom's hardware MMULT is used zero times.** Precomposing the rotation
into a flat 3×3 (once/frame) turns each vertex into 3 dot products = 3 MMULTs —
estimated ~4× on the rotate ≈ **−28% of frame**. Nothing else in the campaign is
within an order of magnitude of that.

**But we will not build on MMULT until silicon confirms it**, because (a) every
complex instruction has surprised us on real hardware (div interlock, blit
cadence, the load-across-jump erratum) and (b) **your own two sources disagree on
the operand layout**: `isa.rs::mmult` reads the matrix from local SRAM and the
vector from bank-1; `RISC_ISA.md §7.2` calls the bank-1 operand the matrix. That
must be pinned before we commit a kernel layout.

## The ask

A calib probe `p_mmult` (mirrors your p_divlat / p_ldjump style) that runs a known
3×3 × vector on real Tom and reports enough to settle:

1. **Operand layout / direction.** Seed a deliberately *asymmetric* matrix and
   vector (so row-major vs column-major and matrix-in-SRAM vs matrix-in-bank-1 all
   give distinct results). Report the 3 output dot products. We need to know which
   layout silicon computes — jsim currently assumes matrix-in-SRAM (stride 4 row /
   width*4 column via `mtxc & 0x10`), vector in bank-1 packed 2× s16/reg.
2. **s16 semantics.** Confirm operands are treated as signed 16-bit and the MAC/
   result is the full sum (our accumulator range is safe: 3 × 32767 × 4096 < 2³¹).
   Feed one near-±32767 term to check truncation/overflow behavior at the edge.
3. **Timing.** cycles for a width-3 MMULT, and the cost of an `mtxc`/`mtxa`
   control-reg write between calls (we issue 3 MMULTs/vert with `mtxa` re-pointed
   per row — if that dominates, we need to know before Phase 1). jsim models
   width cycles/MMULT (`mtxc & 0xF`) and doesn't separately price the control write.
4. **`mac` register semantics** across back-to-back MMULTs (does a fresh MMULT
   reset the accumulator, or accumulate — the `IMULTN;IMACN×k;RESMAC` sequence in
   §7.2 suggests reset-per-MMULT; confirm).

## How we'll use it

If silicon matches jsim's `mmult` model (or we learn the correct layout), Phase 1
precomputes the 3×3 and Phase 2 replaces the vc_loop rotate — verified pixel-
identical in jsim, then one silicon fps flash. Full plan: OpenLara
`MMULT_SCOPE.md`. If MMULT diverges, we fall back to a precomposed-3×3 *software*
transform (9 straight muls, no dependent interleave — a smaller safe win) and the
probe tells us that too.

---

## ✅ RESOLVED (2026-07-24) — we built the probe; MMULT PASSES on silicon

We authored `p_mmult` ourselves in your calib harness (`calib/probes.s` +
`calib/main.c`, branch `calib-mmult-silicon-probe`, commits c1f0fbd/6687a15)
and ran it on the rig (Skunkboard). **Silicon computes 3×3·vec correctly:**
`rd = 000000E6,00000212,0000033E` = (230,530,830), matching the prediction; Rd
is written directly (no RESMAC needed). DIVLAT/LDJUMP in the same ROM matched
your baseline, so the harness is sound. **MMULT is GO** — we're building the
vertex transform on it.

### Two jsim `mmult` bugs to fix (silicon disagrees with `isa.rs::mmult`)
It took 6 iterations because the sim hides two real hardware behaviours — both
confirmed against a shipped game (Black Ice White Noise, `engine.bin` $5486,
`store r12,(r6); moveta; moveta; mmult; move r0,r3`):

1. **The bank-1 vector operand must be re-`moveta`'d in the instructions
   IMMEDIATELY before each `mmult`.** A one-time setup `moveta` reads back
   correct via `movefa` but MMULT reads the operand as **zero** — the systolic
   read is fed by a recent moveta, not the register file directly. jsim reads
   the bank-1 file unconditionally, so it never sees this → models a no-op MMULT
   as working.
2. **The SRAM matrix element is read from the LOW 16 bits at 4-byte stride on
   silicon; `isa.rs` uses big-endian `read16` = the HIGH 16.** With a
   high-half layout, silicon reads all-zero matrix → mac=0. (Your spec §7.2
   already flags element-fetch ordering as UNVERIFIED — this pins it: LOW half.)

Repro/fact-pack: the `p_mmult` v6 probe (both-halves matrix + per-row moveta)
passes on silicon AND jsim; v1–v5 (high-half and/or setup-only moveta) return
zero on silicon but non-zero in jsim — the exact divergence to add a jtest for.
