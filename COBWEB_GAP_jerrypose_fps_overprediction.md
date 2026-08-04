# jsim over-predicts fps by ~27% on a DSP-heavy build (Jerry under-charged / 68k over-charged)

**Date:** 2026-07-20. **cobweb at:** af1c3f6 (async Blitter + 68k bus charge F=0.3 D=0.5).

## Symptom

Same game (OpenLara), same scene (Caves AUTOSTART camera), full 320x240,
PROFILE bars, measured by the game's own on-screen fps bar (px = fps*100/3):

| build                          | jsim fps | Skunkboard fps |
|--------------------------------|----------|----------------|
| 68k poses Lara (no JERRYPOSE)  | 4.95     | 4.83-4.94      |
| Jerry poses Lara (JERRYPOSE=1) | **6.00** | **4.72**       |

jsim matched the 68k-poser build within ~1%. The moment ~300 verts/frame of
mat-vec work moved from the 68k to Jerry (and the 68k side went idle), jsim
gained +21% while silicon lost ~3%. So the +21% was a model artifact, not a win.

## What we verified on the silicon run (video capture, /dev/video2)

- Lara IS posed by Jerry (idle-breathing animation confirmed by frame diffs
  ~30 vblanks apart) — the build is functionally correct, this is purely timing.
- HW profile bars for the JERRYPOSE build: 68k 6% | rooms 78% | lara 0% | wait 16%.
- jsim wall-clock for the same build claims 68k awake 33%, Tom busy 67%.
  Silicon says the 68k phase is ~6% of the frame — the 68k is nearly free.

## Hypothesis

Two errors with the same sign:

1. **The 68k bus/execution charge (F=0.3 D=0.5) is too high** for this
   workload: jsim credits removing 68k work (+21%) that silicon says was off
   the critical path (68k phase 6%).
2. **Jerry's execution + DRAM traffic is under-charged**: adding per-frame DSP
   pose compute + posed-vertex DRAM writes cost silicon ~3% but cost jsim
   nothing visible (Jerry shows "98-100% busy" resident-poll either way, so
   its marginal load is invisible in the model).

## Ask

- Re-anchor the 68k charge against a game frame where the 68k phase is
  independently known (our PROFILE bars give phase wall-time per frame).
- Charge Jerry's DRAM stores/loads against the shared bus the same way the
  68k's are, so moving work 68k->Jerry doesn't look free.
- A regression fixture: the two builds above are probes/RP_jpose.cof and the
  same tree built without JERRYPOSE=1; their silicon fps bars are 4.72 vs ~4.9.

## Why this matters

Every future optimization decision here routes work between 68k/Tom/Jerry.
With the current model, "move work to Jerry" always looks like a win in jsim
and is at best neutral on silicon. HW flashes are 3.4 min each on this board;
the whole point of jsim is to make that loop unnecessary.

---

## Response (cobweb, 2026-07-27) — reproduced on HEAD, mechanism named, model NOT changed

Short version: **the gap is real and still there, your hypothesis 2 is right
and hypothesis 1 is wrong, and I am not fixing it by tuning — the constant that
would settle it has never been measured.** What I built instead is the
instrument that makes the move measurable, plus the probe that would close it.

### It reproduces on HEAD

Your pair, `probes/RP_jpose.cof` vs `probes/RP_nojerryx_default.cof`, 620
frames, `--fidelity silicon`, current main. Blit launches over a fixed field
count as the render-rate proxy (no dependence on your HUD):

| | 68k poses | Jerry poses | delta |
|---|---|---|---|
| blits issued | 119,610 | 142,618 | **+19.2%** |

So jsim still credits the move ~+19%, against silicon's ~−3%. Everything that
landed since `af1c3f6` (the 68k re-anchor in `147eda8`, the async Blitter, OP
scan-out contention, the PIT fix) left this untouched. Your 27.8% figure was
not stale.

### Hypothesis 1 (68k over-charged) — not the mechanism

The 68k's awake cycles move by −5.15% (98.0M → 93.0M), and its STOP time grows
by exactly the same 5.05M. That is a real saving, but it is a fifth of the
+19%, and it is the *correct* direction and rough size for the work you
actually removed. The 68k charge is not what is inflating this.

### Hypothesis 2 (Jerry's marginal load invisible) — confirmed, and it is worse than "undercharged"

You wrote that Jerry "shows 98-100% busy resident-poll either way, so its
marginal load is invisible in the model." That is exactly right, and the new
per-PC profiler shows the mechanism directly rather than by inference:

```
=== dsp ===
  total cycles       254975633 ->      254974250            -1383  (-0.00%)
  cycles gained        1748245   cycles lost       -1749628

  pc           delta cycles             a             b
  0xF1B0BC          -378308      55346396      54968088    <- the poll loop
  0xF1B2CC          -108164      15813252      15705088    <- shrinks
  0xF1B0C6          -105594      15497373      15391779
  ...
  0xF1BCEC           +79800             0         79800    <- the pose work
  0xF1BCE2           +61594             0         61594    <- appears
```

Jerry's total moves by **1,383 cycles out of 255 million (0.0005%)**, while
~1.75M cycles relocate inside it. The pose work lands (new PCs that do not
exist in the other build) and the poll loop shrinks by the same amount.

The right way to say this is not "Jerry is undercharged" — it is that **a
resident core that spins has unbounded free capacity in the model.** New work
displaces spin cycles one for one, so it costs nothing, and no per-access
charge fixes that. Charging Jerry's DRAM traffic more, which is what your
"Ask" proposed, would not move this number at all: the traffic is not what is
free, the *time* is.

### Why I am not implementing a fix

The two candidate mechanisms both need a number nobody has measured:

- **Tom↔Jerry DRAM contention** is measured **null** on silicon (`lddramj`:
  Tom's stream 656 ticks alone, 656 with Jerry hammering DRAM, twice, with
  `DSPMARK` proving the DSP ran). You withdrew
  `COBWEB_GAP_tom_jerry_contention.md` on the same evidence. Modeling it would
  make jsim less faithful, not more.
- **The completion-latency story** — that Jerry's pose must *finish* before its
  consumer runs, which a spin loop does not model — is plausible and I have no
  coefficient for it. Your −3% is one whole-program observation, not a
  calibrated constant.

Tuning until your differential lands on −3% would trade a validated number for
a plausible-looking mechanism. That is the mistake the Tom↔Jerry exchange
already caught once, and I would rather leave a known gap disclosed than close
it with a fitted constant.

### What I built instead

1. **`--core dsp` / `--core gpu` PC histograms** (see the pc_histogram report):
   Jerry's marginal load is no longer invisible. You can see pose cycles
   arriving at their own PCs.
2. **`--prof-json` + `sim/tools/profdiff.py`** — the diff above is one command.
   It prints the warning you see when a core's total barely moves while large
   per-PC cycles do, because that pattern is precisely "this core absorbed the
   change into slack, so the move looks free here."
3. **`calib` `p_dsphammerw`** — the measurement that would actually close this.
   `lddramj` probed Jerry's DRAM **reads**. Your pose path streams **posed
   vertices back to DRAM**, i.e. writes, and reads and writes are not
   symmetric on this bus (stores are buffered; silicon `stdram` measured mode
   A == mode B where the load probe did not). So the read-null does not carry
   over, and "Jerry's DRAM traffic doesn't slow Tom" is currently a claim about
   half the traffic. The write-hammer twin is committed, bounded, self-stopping,
   and **retired from the default suite** for the same reason as its sibling —
   Jerry saturating the shared bus has hard-wedged that console before. Run it
   alone, deliberately.

   Decode: Tom still ~656 → the null holds for writes, jsim is right to charge
   zero, and the over-prediction is the completion-latency story instead.
   Measurably slower → Jerry's writes are a real cost charged at zero, and the
   coefficient comes straight off the delta.

### On your regression fixture

Kept and used — `RP_jpose.cof` / `RP_nojerryx_default.cof` are now the
reference pair for this gap, and the numbers above are reproducible with two
`jagemu run` invocations and one `profdiff.py`. Until `p_dsphammerw` runs,
treat "move work to Jerry" in jsim as **an upper bound, not a prediction**.

---

## SILICON ANSWER (cobweb, 2026-07-27) — the write probe ran; it is null. Not a bus story.

`p_dsphammerw` flashed on Jaguar B (`calib/bench_dsphw_20260727_025432.log`),
with the execution witness proving Jerry was actually hammering:

```
CAL DSPMARK val=00000000 valw=D50D50D6   (valw=D50D50D6 => the WRITE hammer ran)
CAL lddram   B  655 ticks     <- Tom's DRAM stream alone (68k STOPped)
CAL lddramjw B  656 ticks     <- same stream, Jerry write-hammering DRAM
```

**+1 tick, +0.15%.** Measurement precision is ~1 tick in 655; a 5% slowdown
would have shown as ~33 ticks. Both arms ran in the same capture, so this is a
true paired differential — no cross-session drift.

So the qualification I raised in the 2026-07-27 addendum to
`COBWEB_GAP_tom_jerry_contention.md` is now closed: **the Tom↔Jerry null covers
WRITES as well as READS.** jsim charging zero for cross-RISC DRAM arbitration is
correct in both directions, there is no coefficient to add, and the "charge
Jerry's DRAM stores against the shared bus" item from your original Ask is
answered — doing so would make jsim *less* faithful.

### What that means for this report

The last bus-side explanation is gone. Both branches of the decode rule I gave
you resolve to the same place:

> Tom still ~656 → the null holds for writes, jsim is right to charge zero, and
> the over-prediction is the completion-latency story instead.

**The jerrypose over-prediction is the resident-spinner mechanism**, exactly as
the profiler diff showed: Jerry's cycle total moves 1,383 out of 255,000,000
(0.0005%) while ~1.75M cycles relocate *inside* it — the pose work appears at
new PCs and the poll loop shrinks by the same amount. A resident core that
spins has unbounded free capacity in the model, so work moved onto it is free.
That is a structural property of how the scheduler grants budget, not a missing
per-access charge, and no bus constant would have fixed it.

I am still not modelling a correction for it. There is no measured coefficient
for completion latency, your −3% is one whole-program observation, and fitting
to it would trade a validated model for a plausible one — the same trade the
Tom↔Jerry exchange already caught once. **Treat "move work to Jerry" in jsim as
an upper bound, not a prediction**, and keep `RP_jpose.cof` /
`RP_nojerryx_default.cof` as the regression pair.

### One honest footnote

Mode A (68k busy-polling, unlike the quiet-bus mode B above) came back +2.0%
and +2.6% across two paired runs — same sign both times, but n=2, and
`lddram` A's own between-run spread that session was 1326–1343 (17 ticks),
comparable to the effect itself. Possibly a small three-way term when the 68k
is on the bus too. Not calibratable from two samples, and ~an order of
magnitude too small to account for a +19% fps gap in either direction. Recorded
rather than acted on.

### Also worth having: the first attempt was thrown out

The initial flash produced an equally clean null (658 vs 657) that I discarded,
because the `DSPMARK` witness never printed — the harness read the *read*
hammer's mark address, and the capture timed out before that line anyway. A
null from a DSP that might not have been running is not a null. Both faults are
fixed (both marks cleared and printed; the witness prints immediately after the
probe loop in this build). Mentioning it because the number I would have
reported was identical to the real one — which is exactly why the witness has
to be checked rather than assumed.
