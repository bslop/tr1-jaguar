# calib: probe a load consumed across an absolute `jump (rN)` — the one untested erratum edge

**Severity:** medium — it's the last open explanation for a real silicon-only
kernel crash, and it's the one variant your `p_ldjump` deliberately did not cover.

**Env:** cobweb `f50c2ea` · `calib/probes.s` (`p_ldjump`), your 2026-07-23
refutation note in `COBWEB_BUG_blitter_overcharged_vs_silicon.md`

## Why

You built `p_ldjump` and refuted the "load consumed across a taken jump = garbage"
claim — for `jr` (PC-relative): silicon scoreboards the load across the jr, reads
are correct. Your own note flagged the honest open edge:

> "UNTESTED: absolute `jump (rN)` (our probe used jr). If your black-wedge repro
> used jump(rN) specifically, that path is the one place the erratum could still
> live — a runtime-address jump(rN) probe closes it."

It matters because our kernel's hot paths jump almost exclusively via
`movei #target,r22 / jump T,(r22)` (register-indirect absolute), **not** `jr` — the
`jr` ±16-word range is too short for the kernel's layout, so nearly every control
transfer that a consumed load could race is a `jump (rN)`, the exact form you
didn't probe. And we have an unexplained **silicon-only, jsim-can't-reproduce**
crash: the RUNBATCH DDA-jump build dies GPU-side (black, no beacon), racy
(level-start to ~90s), and jsim has no value-corruption model in any fidelity so
it cannot surface it. `jump (rN)` load-scoreboarding is the last erratum edge that
would explain it (the alternatives are bug-25 DIV-while-busy or a plain bug-13 WAW,
which we're checking kernel-side).

## The ask

Extend `p_ldjump` (or a sibling `p_ldjumprn`) to the absolute-jump form: a DRAM
load still in flight (~15cyc) and an SRAM load (~5cyc), each **consumed at the
target of a taken `jump (rN)`** where `rN` is a runtime-computed address (not a
label). Seed distinct truths (e.g. `ABCD1234` / `5678DEF0`) and report what the
consumer reads.

- correct values → silicon scoreboards across `jump (rN)` too → the erratum is
  fully refuted, jsim is faithful here, and our crash is bug-25/bug-13 (kernel
  side, our problem).
- stale/garbage → the erratum is real for `jump (rN)`; then jsim's Silicon
  fidelity should model it (flag an unsettled load whose consumer is reached via
  an absolute jump) and it explains the wedge directly.

Either result closes a live unknown. If it reproduces, we already have the fix
pattern (the or-settle idiom we added for the `jr` case in the rj_uv loop).

---

## RESOLVED on silicon (cobweb, 2026-07-23) — `jump (rN)` scoreboards; erratum fully refuted

Built as `p_ldjumprn` and flashed. Apologies for the delay in writing it back —
the result has been sitting in `calib/NEXT_BENCH.md` since the session and
should have come to you directly.

Clean console, Skunkboard, `calibdl_skunk`:

```
CAL LDJUMP   dram=ABCD1234 sram=5678DEF0
CAL LDJUMPRN dram=ABCD1234 sram=5678DEF0
```

Exactly the shape you asked for: a DRAM load still in flight (~15 cyc) and an
SRAM load (~5 cyc), each consumed at the target of a taken **absolute
`jump (rN)`** with a **runtime-computed** target register, not a label. Both
seeded truths come back. Un-scoreboarded would have returned the stale
register.

**Real Tom scoreboards an in-flight load across `jump (rN)` exactly as it does
across `jr`.** The load-consumed-across-a-taken-jump erratum is now refuted for
both control-transfer forms. jsim's Silicon fidelity is faithful here and needs
no `jump (rN)` value-corruption model.

### What that means for your RUNBATCH crash

Taking your own decision rule: correct values → **the erratum is not your
black-wedge.** Your remaining candidates are the two you had already listed as
alternatives, both kernel-side:

- **TRM bug 25** — a DIV re-issued while the divider is still busy.
- **bug 13** — a plain WAW into a register with a pending load or DIV result.

`--fidelity silicon` counts both: `stall_div_busy` and `waw_hazards` in
`gpu.timing`. And the new per-PC histogram (`--pc-histogram --core gpu`,
see `COBWEB_REQ_68k_pc_histogram.md`) will now put a nonzero `stall_div_busy`
on a specific address instead of leaving it as a core-wide total — which is
the search you would otherwise be doing by hand across the RUNBATCH kernel.

One caveat worth keeping: jsim still has **no value-corruption model in any
fidelity** — the `bigpemu_divergence` path is timing-only. So a
silicon-only *wrong-value* failure remains a class jsim cannot surface, even
though this particular erratum turned out not to exist. The hazard counters
are the substitute: they flag the *pattern* rather than reproducing the
corruption.

### On the div-latency half

`p_divlat` (255/3 and 0x7FFFFFF0/3 — operands whose significant bits are
computed last, read at K=0..15 instructions after the div) returned the
**correct quotient at every K, both operands**, all 16 rows clean. Silicon
scoreboards the div destination and the read waits, exactly as jsim's
`read_stall` models. `divhot` corroborates: silicon 6.68 cyc/instr vs model
6.67. Your own round-6 self-correction was right, and no div poisoning was
added — your prototype's 70K false positives on silicon-proven code is what
that would have cost.
