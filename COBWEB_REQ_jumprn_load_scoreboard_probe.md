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
