# jagemu bug #2: JRISC indexed STORE `(R14+n)/(R15+n)` uses the wrong field for the offset (and data)

> **STATUS (audited 2026-07-19):** RESOLVED — verified directly on cobweb `8ca3fc0`: `store r6,(r14+4)` lands at base+16 (correct size-scaled offset), `store r5,(r14)` at base+0. Test: jtest golden --capture 0x1E0000:32. Closed.

**Severity:** blocker for GPU/DSP kernels that program hardware registers or SRAM
via indexed stores — which is nearly all of them. Concretely: the OpenLara GPU
kernel programs the Blitter with `store rN,(R15+n)`, so **no textured span ever
launches and the 3D scene renders black** even though transform/rasterization run.

**Env:** jagemu 0.1.0 · cobweb `c102187` (after the GPU-restart fix).
**Depends on:** the restart fix (`c102187`); without it you never reach a render.

---

## Symptom

The room render runs (GPU reaches `render_pkt`, `yloop`, `sp_fill`) but the
framebuffer stays at the clear color. A blit trace (`JAGEMU_BLIT_TRACE=1`) over
300 frames shows **only** full-screen clears (`cmd=01800200`, launched by the
68k) and **zero** textured spans (`cmd=01800801`, launched by the GPU).

Peeking the Blitter register file `0xF02200` mid-render:

| reg | addr | value | set by | ok? |
|---|---|---|---|---|
| A1_BASE | F02200 | `0004FAB8` (atlas) | GPU **non-indexed** store | ✅ |
| A1_FLAGS| F02204 | `00034018` | GPU non-indexed | ✅ |
| A1_PIXEL| F0220C | `00000000` | GPU `store r0,(R15+3)` | ❌ never written |
| A1_INC  | F0221C | `00000000` | GPU `store …,(R15+7)` | ❌ |
| A2_BASE | F02224 | `001CD460` (fb) | GPU non-indexed | ✅ |
| A2_FLAGS| F02228 | `00014218` | GPU non-indexed | ✅ |
| A2_PIXEL| F02230 | `00000000` | GPU `store …,(R15+12)` | ❌ |
| B_CMD   | F02238 | `01800200` (68k clear) | GPU `store r0,(R15+14)` | ❌ never `01800801` |

Non-indexed stores land; **every `store rN,(R15+offset)` misses.** A stray SRAM
value (`0xF03E40`) was found at **`0xF02280`** — i.e. `R15 + 32·4`.

## Root cause (exact)

`sim/crates/jag-core/src/risc.rs`, effective-address decode (~line 479):

```rust
let q1 = if r1 == 0 { 32u32 } else { r1 as u32 };   // quick-imm of reg1 field
let q2 = if r2 == 0 { 32u32 } else { r2 as u32 };   // quick-imm of reg2 field
...
43 => Some(self.reg(b, 14).wrapping_add(q1 * 4)),   // LOAD (R14+n)  — uses q1 ✅
44 => Some(self.reg(b, 15).wrapping_add(q1 * 4)),   // LOAD (R15+n)  — uses q1 ✅
49 => Some(self.reg(b, 14).wrapping_add(q2 * 4)),   // STORE (R14+n) — uses q2 ❌
50 => Some(self.reg(b, 15).wrapping_add(q2 * 4)),   // STORE (R15+n) — uses q2 ❌
```

JRISC encodes both indexed **load and store** with the offset `n` in the **reg1
field** (bits 9–5) and the GPR in the **reg2 field** (bits 4–0). The loads read
the offset from `q1` (correct); the stores read it from `q2` (the *data*
register's field) — the offset and data fields are swapped for stores.

For `store r0,(R15+14)` rmac emits reg1=14 (offset), reg2=0 (data r0):
* hardware / intended: `EA = R15 + q1·4 = R15 + 14·4 = R15+56 = B_CMD (F02238)`
* jagemu: `EA = R15 + q2·4 = R15 + quick(0)·4 = R15 + 32·4 = R15+128 = F02280`

which is exactly the misplaced write observed. The **stored data** is wrong too:
the value comes from `reg(r1)` (=reg 14 here) instead of `reg(r2)` (=r0) — the
disassembler confirms it, rendering `store r0,(r15+14)` as `store r14,(r15+32)`
(so `isa.rs` decode has the same swap; please fix both exec and disasm).

## Fix

Indexed stores must take the offset from `q1` and the data from `reg(r2)`, exactly
like the loads take offset from `q1` and dest = `r2`:

```rust
49 => Some(self.reg(b, 14).wrapping_add(q1 * 4)),   // STORE (R14+n)
50 => Some(self.reg(b, 15).wrapping_add(q1 * 4)),   // STORE (R15+n)
```
and ensure the stored value is `reg(b, r2)` (not `reg(b, r1)`). Mirror the same
field assignment in the `isa.rs` disassembler.

Also check the register-indirect stores (45/46/47) and indexed loads (58–61) for
the same reg1/reg2 convention against rmac's encoder (`jas` in-tree is the
reference).

## Why the test suite missed it

`risc.rs:798 timed_indexed_store_erratum()` uses `enc(49, 2, 1) // store r2,(r14+1)`
— it hand-encodes the **offset in reg2** (`b=1`) and **data in reg1** (`a=2`),
i.e. the operands in the order jagemu happens to read them, so it passes. Encode
it the way rmac does (offset in reg1, data in reg2) and it fails:

```rust
// store r2,(r14+1): rmac => reg1=1 (offset), reg2=2 (data)
enc(49, 1, 2)   // expect write of R2 to (R14 + 1*4); currently writes R1 to (R14 + 2*4)
```

Add a positive regression: assemble `store rD,(r15+14)` with **jas**, run in jsim,
assert the value lands at `R15+56`.

## Reproduction

```sh
ROM=PROBE_full240.cof   # OpenLara MULTIROOM 320x240 headless (NO_GAMEDRIVE, no NOGD)
# black scene; only clear-blits, no textured spans:
JAGEMU_BLIT_TRACE=1 jagemu run $ROM --frames 300 --press a --press-after 60 2>&1 \
  | grep -c 'cmd=01800801'          # => 0  (should be thousands)
# Blitter regs never programmed by the GPU:
jagemu peek $ROM --at 0xF02230 --len 16 --frames 250 --press a --press-after 60
#   A2_PIXEL=0, B_CMD=01800200 (never 01800801)
```

---

## Response (cobweb `7336d6a`) — fixed

`9476039`. JRISC indexed store operand fields were swapped: the offset is
**reg1** and the data register is **reg2**, not the reverse. Fixed in both jas
(assembly) and jsim (execution), which is why it was self-consistent and
therefore invisible until you hit it — assembler and emulator agreed with each
other and disagreed with silicon.
