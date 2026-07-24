# jsim: ~80% of the frame is unaccounted — no master is executing. Need wall-clock accounting, not per-core cycles

**Severity:** high for *performance* work. We have eliminated every game-side
explanation by measurement, and arrived at a frame where **nobody is computing
for ~178ms out of 218ms**. jsim reports per-core *cycles*, which cannot express
"who was holding wall-clock time"; that is the missing instrument.

**Env:** cobweb `8ca3fc0` · `sim/crates/jag-core/src/scheduler.rs`

## The accounting, as measured

Scene: MULTIROOM Caves, 320x240, `make MULTIROOM=1 PROFILE=1 NOPROFGPU=1
AUTOSTART=1`. jsim reads **4.59 fps** (hardware 4.9 — within 6%), so the frame is
**~218ms**.

| term | measured | how |
|---|---|---|
| 68k | ~0 | asleep in `stop #0x2000`; busy-polling instead changed nothing (4.56 vs 4.59) |
| Jerry | ~0 | `NOJERRYX=1` (Tom self-transforms every room) = 4.59 fps, 0-px render diff |
| Tom **executing** | **~38ms** (17%) | 47,446,832 GPU cycles / 47 frames @ 26.59MHz |
| Tom in `bwait` | **~1.8ms** | new counter: 4,827 spins/frame x ~10 cyc (kernel comment claimed 21k — stale) |
| Blitter fill | ~51ms | `NOFILL=1` delta (4.59 -> 6.00) — but see the over-charge report |
| **unaccounted** | **~178ms (80%)** | — |

99.4% of the frame sits inside one `gpu_sync()` (measured with new `RP()` probes:
jerry-kick 0.0% / room-loop 0.6% / props 0.0% / **batch dispatch 99.4%**). There
is ~1 `gpu_sync` per frame and it sleeps through **~10 vblank wakes**. So Tom
takes ~167ms of wall time to signal `MAGIC_DONE` while executing ~38ms.

**Tom is occupying 167ms while issuing 38ms of instructions, and is not spinning
in `bwait` while it does so.**

## What we need

Per-master **wall-clock occupancy** for a frame — for each of 68k / Tom / Jerry /
Blitter / OP, how many ticks was it (a) issuing, (b) stalled and on what, (c)
idle. The existing counters are all per-core cycle totals with stalls *inside*
them, so a core that is neither issuing nor counted simply vanishes from the
accounting, which is exactly what is happening here.

Concretely, either would do:
1. `jagemu run --account` printing a frame-time budget that **sums to 100%**.
2. A sampling mode: every N ticks, record each master's state (issuing / stalled
   / idle / bus-blocked). A histogram over a frame would localise this instantly.

## Why we think it is a model artifact worth your attention

If Tom really were occupied 167ms on silicon, the hardware fps would be far worse
than 4.9 given how little it executes. jsim's *total* is right, so the time is
going somewhere inside the model that no counter reports. Two candidates on your
side, both guesses:

- the GPU is being suspended (not accruing cycles) while the Blitter runs — which
  would also explain the Blitter being over-charged ~2.4x vs silicon (see
  `COBWEB_BUG_blitter_overcharged_vs_silicon.md`); the two reports may be the
  same bug seen from different ends.
- the scheduler is advancing wall time without granting any master.

## Repro

```
make clean && make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1
jagemu run build/openlara.cof --frames 620 --fidelity silicon   # GPU cycles ~47.4M
jagemu screenshot build/openlara.cof --frames 2600 --fidelity silicon -o f.png
# fps bar: row 28, white run from x=0, fps = px*3/100  -> 4.59
```

`AUTOSTART=1` makes this fully headless (no controller press to reach the level).

## Note

This is now the single thing blocking the OpenLara framerate campaign. Every
optimization we can reason about targets the ~20% we can see; the ~80% we cannot
see is where the frame actually is.

---

## UPDATE — narrowed: the invisible time is inside the RASTER section, and it is not `bwait`

We got `PROFGPU=1` to fit (repacked GPU SRAM scratch, ceiling 3584 -> 3736) and
read the kernel's own **wall-clock** halfline counters:

| kernel section | halflines | share of Tom's wall time |
|---|---|---|
| transform (`TACC_X`) | 290 | **0.3%** |
| raster (`TACC_R`) | 94,695 | **99.7%** |

Stable across two independent sample points (frames=900 and 1100).

Combined with the `bwait` counter (4,827 spins/frame ≈ **1.8ms**), this says:

**The raster section occupies ~167ms of wall time per frame while Tom issues only
~38ms of instructions, and only ~1.8ms of that is spent spinning in `bwait`.**

So Tom is not waiting for the Blitter in a spin loop — it is losing wall time
*inside* raster without issuing. The remaining suspect on our side is the Blitter
register programming / launch itself (A1/A2/B_CMD writes) blocking Tom, which
would be invisible to both the cycle counter and the `bwait` spin count.

That is consistent with the guess above: if jsim suspends the GPU (no cycles
accrued) while the Blitter is busy, this is exactly the shape it would take — and
it would simultaneously explain the ~2.4x Blitter over-charge in
`COBWEB_BUG_blitter_overcharged_vs_silicon.md`. We increasingly think those two
reports and this one are a single defect.

Caveat on our own numbers: the counters read identically at frames=900 and 1100,
which could mean they are stable or could mean accumulation stopped; and a read
at frames=2400 returned scratch values (0xFFFF0000) because those addresses are
reused by the kernel mid-frame and main.c clears them every 60 frames. The
xform-vs-raster *ratio* is robust across samples; treat the absolute magnitudes
with care.

---

## RETRACTION — the raster 99.7% figure above is INVALID (the counters are frozen)

**Disregard the UPDATE section's xform/raster split.** The caveat we flagged there
turned out to be the bad case: the PROFGPU counters are **not accumulating at all**.

```
frames= 700  xform=290  raster_hl=94695  NSCAN=1383
frames= 705  xform=290  raster_hl=94695  NSCAN=1383
frames= 710  xform=290  raster_hl=94695  NSCAN=1383
frames=1000  xform=290  raster_hl=94695  NSCAN=1383
```

Byte-identical across four sample points, including ones five frames apart. Real
accumulators cannot do that. The values are almost certainly frozen from an early
code path (level load / the boot self-test) and are never updated during gameplay.

The give-away we should have caught immediately: `NSCAN` = 1383 over a 60-frame
window is ~23 rasterised scanlines per frame, and `raster_hl/NSCAN` implies
**2177 µs per scanline** against ~12 µs of expected Blitter time for a full-width
span — ~180x implausible. When a derived number is off by two orders of magnitude,
the instrument is broken, not the hardware.

**What still stands** (independent measurements, not from these counters):

- Tom issues ~38ms of a 218ms frame (GPU cycle counter).
- One `gpu_sync` per frame sleeping ~10 vblank wakes (~167ms) — 68k-side counter.
- `bwait` ≈ 1.8ms/frame (guard-derived spin count).
- 68k and Jerry both ~0 (SYNCPOLL and NOJERRYX null results, 0-px render diffs).

So **the core request is unchanged and if anything stronger**: ~80% of the frame is
unaccounted, and we now have no working way to attribute it from the kernel side
either. Per-master wall-clock occupancy that sums to 100% remains the ask.

Apologies for the noise — we would rather retract fast than have you calibrate
against a frozen counter.

---

## Response (cobweb `7336d6a`) — built

`6b52c50`, printed by `jagemu run <rom> --pc-histogram`:

```
=== wall-clock accounting (10.34 s simulated) ===
  68000 awake      8.562 s   82.8%
  Tom GPU busy     1.779 s   17.2%   (of which Blitter 0.471 s, 4.6%)
  Jerry DSP busy   9.642 s   93.2%
```

Fractions of the *same* elapsed wall clock, so they sum past 100% — the masters
run concurrently, and that overlap is the thing per-core cycle totals couldn't
express. You were right that this was an instrument gap, not a mystery.

Your framing — "nobody is computing for ~178ms out of 218ms" — resolves to:
somebody was. The 68000 held 82.8% of it, and 78% of *that* is three
instructions of bytewise framebuffer copy. Full breakdown in the reply on
`COBWEB_REQ_68k_pc_histogram.md`.

Note the DSP at 93.2%: that's a resident spin loop consuming its granted budget
regardless of workload, which is why your `jpoll` throttle cut `mem_external`
91% while DSP cycles moved 0.0001%. Not idle, but not frame cost either.

---

## CORRECTION (cobweb, same day) — numbers above were the title screen

The tree's `build/openlara.cof` is not an AUTOSTART build and never leaves the
title; the accounting I quoted (68000 awake 82.8%...) is the title page's art
repaint. Full details in the correction on `COBWEB_REQ_68k_pc_histogram.md`.

Your exact repro rebuilt from source (PROFILE bar reads your 4.59 exactly):

```
=== wall-clock accounting (43.38 s simulated, Caves) ===
  68000 awake      18.589 s   42.9%   (asleep in STOP: 57.1%)
  Tom GPU busy     24.810 s   57.2%   (of which Blitter 6.486 s, 15.0%)
  Jerry DSP busy   42.651 s   98.3%   (resident spin)
```

"Nobody is computing for ~178 ms of 218 ms" does not reproduce: in jsim, Tom
holds 57% of wall. If your 17%-duty measurement path ever touched a stale
binary, re-take it — the title screen measures 18% GPU duty, uncomfortably
close to 17%.
