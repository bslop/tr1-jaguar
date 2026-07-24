# jsim: STOREP / LOADP swap the two 32-bit longs (wrong big-endian phrase order)

> **STATUS (audited 2026-07-19):** RESOLVED in cobweb `f5197f3` (phrase long order) + `6f1fd08` (LOADP/G_HIDATA late+unscoreboarded). Verified by us on hardware. No reply needed.

**Severity:** correctness — silent. Code that round-trips through `STOREP`/`LOADP`
"works" in jsim but renders/computes **wrong on real silicon**, or vice-versa.
Found by flashing to a real Jaguar: a kernel that assembles a vertex `(sx,sy)`
with `STOREP` rendered **correctly in jsim but transposed (sx↔sy swapped) on
hardware**.

**Env:** cobweb `31dc918` · `sim/crates/jag-core/src/risc/isa.rs`

## The bug

The Jaguar is **big-endian**. A phrase register pair is `{ G_HIDATA (high 32),
Rn (low 32) }`; the 64-bit value is `(hidata << 32) | Rn`. Stored big-endian at a
phrase-aligned address `A`, the **high** long lands at the **lower** address:

```
mem_long[A]   = hidata   (bits 63..32)
mem_long[A+4] = Rn       (bits 31..0)
```

jsim does the opposite:

```rust
// isa.rs ~385, STOREP (opcode 48, GPU)
bus.write32(s,               d);        // Rn   -> A     (should be hidata)
bus.write32(s.wrapping_add(4), core.hidata); // hidata -> A+4 (should be Rn)

// isa.rs ~350, LOADP (opcode 42, GPU)
core.set_reg(b, r2, bus.read32(s));            // Rd  <- A     (should be A+4)
core.hidata = bus.read32(s.wrapping_add(4));   // hid <- A+4   (should be A)
```

So both ops have the two longs **swapped** relative to real hardware.

## Proof (hardware)

OpenLara's `gpu_geotex.gas` writes each transformed vertex's screen `(sx,sy)` to a
DRAM cache. Replacing the two 32-bit `store`s with one `storep` (`hidata=sy`,
`Rn=sx`, expecting jsim's documented low@A/high@A+4):

- **jsim:** renders the Caves correctly (self-consistent: it wrote `sx@A, sy@A+4`
  and the raster reads `sx@A, sy@A+4`).
- **real Jaguar (Skunkboard):** the image is **transposed — every vertex's x and
  y are swapped** (scene on its side, characters facing the wrong way). Because HW
  wrote `hidata(sy)@A, Rn(sx)@A+4`, and the raster read `sx@A` (got sy), `sy@A+4`
  (got sx).

Swapping the operands (`hidata=sx`, `Rn=sy`) fixes it on hardware — and then jsim
shows it wrong. Definitive: jsim's phrase long-order is reversed.

## Fix

```rust
// STOREP
bus.write32(s,               core.hidata);  // high long -> lower addr
bus.write32(s.wrapping_add(4), d);          // low long  -> higher addr

// LOADP
core.hidata = bus.read32(s);                // high long <- lower addr
core.set_reg(b, r2, bus.read32(s.wrapping_add(4))); // low long <- higher addr
```

## Regression test

Round-trip check: set `G_HIDATA=0xAAAAAAAA`, `Rn=0xBBBBBBBB`, `storep Rn,(A)`;
assert `mem_long[A]==0xAAAAAAAA` and `mem_long[A+4]==0xBBBBBBBB`. Then `loadp
(A),Rd`; assert `Rd==0xBBBBBBBB` and `G_HIDATA==0xAAAAAAAA`. (Both currently
produce the swapped result.)

## Impact / note

This blocks using jsim to verify *any* phrase-transaction optimization (the exact
bus-traffic win we're pursuing — `storep` for the vertex cache, `loadp` for
phrase vertex reads). Until fixed, those have to be verified on hardware. Once
fixed, jsim's render-diff can gate them again. Also worth auditing the DSP MMULT
`MAC` / `hidata` paths for the same high/low assumption.

---

## Response (cobweb `7336d6a`) — fixed, in two parts

Your diagnosis was right and there was a second bug behind it.

**Byte order** (`f5197f3`): STOREP/LOADP are big-endian across the phrase — the
HIGH long lives at the LOWER address. jsim had them swapped.

**G_HIDATA timing** (`6f1fd08`): LOADP's high half lands *late and
unscoreboarded*, matching silicon. So a STOREP issued in a LOADP's shadow reads
a stale G_HIDATA. jsim was making the write visible immediately, which hid a
class of real hazard — code that works in jsim and corrupts on hardware, the
worst failure mode an emulator has. `hidata_now()`/`hidata_next` model the
delay explicitly.

The second one only surfaced because fixing the first made the remaining
divergence visible.
