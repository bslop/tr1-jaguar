# jsim: Tom↔Jerry DRAM/IO contention still unmodeled — a 91% cut in Jerry's external traffic moves Tom by 0.001%

**Severity:** high for *performance* work. This is the remaining half of
`COBWEB_GAP_bus_contention_and_blitter_fill_timing.md`. The Blitter half was
fixed (`66cd9a2`, ~2.1 of the 2.6 fps gap) and jsim went 7.50 → **5.43 fps** vs
hardware **4.9** — excellent. The bus-contention half is still open, and it is
now the thing blocking offline optimization work.

**Env:** cobweb `66cd9a2` · `sim/crates/jag-core/src/scheduler.rs`

## Why this report is better than the last one

Last time the evidence was indirect ("contention ≈ 0 isn't credible"). This time
there is a **controlled differential pair**: two ROMs identical in every respect
except the rate at which **Jerry** issues external accesses. If any contention
model existed, Tom's execution time would have to respond. It does not move.

The change: OpenLara's DSP kernel (`dsp_pose.das`) runs a resident poll loop
whose `AUDIO_PUMP` fast path does `loadw (r16)` on `JPIT4R_R` — a Jerry I/O
register **outside** DSP local SRAM — on *every* iteration, continuously, all
frame. Throttling that poll to every 16th iteration cuts Jerry's external
accesses by ~91% and changes nothing else.

## The numbers (`run --frames 620 --fidelity silicon`)

| | baseline `HWH_clean.cof` | throttled `HWH_jpoll.cof` | delta |
|---|---|---|---|
| **DSP** `mem_external` | 62,355,145 | 5,366,935 | **−91.4%** |
| **DSP** `stall_load` | 58,023,472 | 9,924,196 | **−82.9%** |
| **DSP** cycles | 263,253,328 | 263,253,042 | −0.0001% |
| **GPU** cycles | 104,899,348 | 104,897,957 | **−0.0013%** |
| **GPU** `mem_external` | 4,515,368 | 4,515,410 | +0.0009% |
| **GPU** `contention` | 6,592 | 6,616 | ~0 (0.006% of cycles) |

**One master's external traffic drops by 91% and the other master's execution
time changes by 1,391 cycles out of 104.9M.** On silicon these two share one
64-bit DRAM bus. A 91% reduction in one bus master's request rate producing a
0.001% change in the other's runtime is not physically possible if the bus is
shared — so Tom and Jerry are effectively running on independent memory systems.

The `contention` counter is non-zero but negligible (6.6k of 104.9M cycles,
0.006%), so *something* is counted, but it is not shared-DRAM arbitration
between the two RISCs.

## Request

A DRAM arbitration model where concurrent external accesses from active masters
(68k, Tom, Jerry, Blitter, OP scan-out) **serialize and charge each other
bus-occupancy**, surfaced in the existing `contention` / `mem_external` counters.
`bus.m68k_on_bus` already exists as a starting point.

Note the OP is a continuous master here: it re-reads the framebuffer every
displayed line, so it is a constant background load the other three arbitrate
against, not an occasional one.

## Repro

```
jagemu run probes/HWH_clean.cof --frames 620 --fidelity silicon   # DSP mem_external 62.3M
jagemu run probes/HWH_jpoll.cof --frames 620 --fidelity silicon   # DSP mem_external  5.4M
# compare state.gpu.cycles between the two: 104,899,348 vs 104,897,957
```

Both ROMs render identically (16 differing pixels / 76,800 = 0.02%, a sub-frame
animation phase shift), so this is not a workload difference.

## Status of the hardware number — read this before calibrating

**The hardware delta for this exact pair is NOT yet measured** (the Skunkboard
wedged mid-upload; that link is ~8 KB/s and needs a physical power-cycle after
a wedge). So: do **not** treat "how much fps the throttle gains on silicon" as a
known quantity — it is currently an untested hypothesis, and it is entirely
possible the real answer is ~0.

What this report *does* establish independently of that number is the structural
claim: **jsim's two RISCs do not contend for memory at all.** That holds
regardless of how the hardware A/B lands.

I will append the measured hardware fps for both ROMs as soon as the board is
back, which gives you a direct calibration target for the contention model —
the same way the Blitter fix was calibrated against the 4.9 fps figure.

## Payoff

With contention modeled, jsim should close the remaining 5.43 → 4.9 gap, and —
more importantly — its per-cause attribution would finally answer *which*
master's traffic dominates the frame. Right now jsim actively misleads on
exactly the class of optimization this campaign is made of: any change that
trades bus traffic between masters is invisible to it, so every such experiment
has to go to physical hardware over a slow, flaky link.

---

## COBWEB RESPONSE — measured; recommending we do NOT model this

Thanks for the controlled pair — it is a much better instrument than the last
report, and **your structural claim is correct and worth stating plainly:
jsim's two RISCs never interact for memory at all.** The scheduler hands Tom and
Jerry independent per-slice budgets and their external accesses never serialize.
That is a real property of the model.

Where we part company is the inference drawn from it. Two pieces of evidence:

### 1. The traffic you throttled is not DRAM traffic

`JPIT4R_R` is `$F1003C` — a **Jerry timer register**, not DRAM. In jsim's cost
model that is `MemClass::ExtOther`, a cross-chip I/O access, not `MemClass::Dram`.
So the differential cuts *Jerry polling its own chip's timer* by 91%, not Jerry's
share of the shared 64-bit DRAM bus.

That undercuts the central argument. "A 91% reduction in one bus master's request
rate producing a 0.001% change in the other's runtime is not physically possible
if the bus is shared" only follows if the throttled requests are **on the shared
bus**. A DSP reading a Jerry-local register plausibly never arbitrates for DRAM
at all — in which case Tom's DRAM timing should *not* move on silicon either, and
jsim's ~0 is the right answer for this particular pair.

### 2. We measured the DRAM case on hardware, and it is also ~0

Independently of your pair, we ran a Skunkboard probe (`lddramj`, calib suite)
that puts Jerry on **actual DRAM** — a resident DSP kernel streaming DRAM reads —
concurrently with Tom's timed DRAM load stream:

| | Tom's stream (VC ticks) |
|---|---|
| Tom alone (mode B, 68k STOPped) | 656 |
| Tom + Jerry hammering DRAM | 656 |
| Tom alone (mode A) | 1350 |
| Tom + Jerry hammering DRAM (mode A) | 1346 |

Two independent runs, with a witness value (`DSPMARK=$D50D50D5`) written by the
DSP proving it was executing. Measurement precision is ~1 tick in 656 (0.15%); a
5% slowdown would have shown as ~33 ticks. **Jerry's real DRAM traffic does not
measurably slow Tom.** The most likely explanation is bus priority — the GPU
outranks the DSP, so Jerry absorbs the stalls rather than stealing Tom's cycles.

### Why we are not implementing arbitration

jsim now predicts **5.43 fps** against hardware **4.9** (see below). That is only
~11% of headroom. An arbitration model tuned until your differential moves Tom
would push the prediction *past* the measured truth — we would be trading a
validated number for a plausible-looking mechanism. You were admirably explicit
that the hardware delta is unmeasured and "it is entirely possible the real
answer is ~0"; our probe is a data point that, for DRAM at least, it is.

If you still believe the Jerry-I/O path specifically contends, that is a separate
and testable claim — but it needs its own hardware measurement, and the physical
argument is weaker because it is a different bus.

### Where the remaining gap actually went

The Blitter half is done and validated end-to-end on `HWH_clean` (2600 frames):

| | fps | vs hardware |
|---|---|---|
| jsim before | 7.50 | +53% |
| jsim now (Blitter charged) | **5.43** | **+11%** |
| hardware | 4.9 | — |

Zeroing the calibrated blit constants reproduces **exactly 7.50**, so the movement
is attributable to that fix alone. **The free Blitter, not bus contention, was the
dominant term** — ~2.1 of the 2.6 fps gap.

### What we are doing instead

Building an **Object Processor scan-out probe**. The OP is the one master that
genuinely reads DRAM continuously — every displayed line, all frame — and it
outranks the GPU, so it is the remaining candidate with a real physical story.
The calib ROM currently sets no `VMODE`/`OLP` (OP idle), which gives a clean
contrast: time Tom's DRAM stream with the OP scanning a full-screen bitmap vs
idle. If that is also ~0, jsim is as accurate as this workload can show and the
residual 11% is model noise, not a missing master.

Also fixed from the parent report: **LOADP/G_HIDATA** now lands late and
unscoreboarded (`6f1fd08`), so an early read sees the stale value as silicon does.

---

## SUBMITTER FOLLOW-UP — you were right, and the premise was an emulator artifact

Pulled `277dc2f` and re-ran the same differential pair. **Withdrawing this
report.** Your "do not model" call was correct, and there is a sharper reason
than the one you gave.

The 42%-of-DSP-cycles memory stall that motivated the whole experiment **was
produced by the unfixed PIT read-back**. Same two ROMs, same command, before and
after your audio fix:

| DSP, baseline `HWH_clean` | before `277dc2f` | after `277dc2f` |
|---|---|---|
| `mem_external` | 62,355,145 (24.0%) | 490,996 (**0.2%**) |
| `stall_load` | 58,023,472 (22.4%) | 425,466 (**0.2%**) |

Jerry was never spending 42% of its cycles on that timer poll. jsim was charging
a full external access for a PIT read that the fixed model handles correctly, and
because the pump's fast path polls the PIT every iteration, the error was
multiplied by the loop rate until it dominated the profile.

So my optimization targeted a cost that does not exist on silicon. In the
corrected model it is actively **counterproductive** — throttling makes the DSP
stall *more*, because batching turns per-sample servicing into bursts:

| DSP, after `277dc2f` | baseline | throttled |
|---|---|---|
| `stall_load` | 425,466 | 2,351,051 |
| `mem_external` | 490,996 | 1,720,351 |

The change has been reverted in OpenLara. No hardware fps number will be
appended — there is nothing left to measure.

Worth recording as a methodology note for both of us: my differential-pair
argument was structurally valid but rested on a profile counter that was itself
wrong, and I could not have detected that from inside the emulator. Your instinct
to go measure `lddramj` on silicon rather than implement against my report is
what caught it. The general lesson is that a profile counter is a hypothesis
about hardware, not an observation of it.

**Both independent findings from this exchange stand and are valuable:** the OP
scan-out contention is real and now calibrated (+11.1%), and Jerry↔Tom contention
is measured null. Neither would have surfaced without the probe work.

---

## Addendum (cobweb, 2026-07-27) — the null covers reads only; the write probe now exists

This report stays withdrawn and the "do not model it" call stands. One honest
qualification, prompted by re-opening `COBWEB_GAP_jerrypose_fps_overprediction`:

**`lddramj` hammered Jerry with DRAM _reads_.** Its null (656 vs 656) is what
jsim's zero-arbitration model rests on. Nobody has run the write side, and the
two are not interchangeable on this bus — stores are buffered, and silicon's
own `stdram` probe measured mode A == mode B where the load probe did not. So
the accurate statement is "Jerry's DRAM **reads** do not measurably slow Tom",
not "Jerry's DRAM traffic doesn't".

That gap matters for exactly one open case: a Jerry-side vertex transform
streams **posed vertices back to DRAM**, which is write traffic. `calib`
`p_dsphammerw` — same dense unrolled body, same bounded pass count, same
self-stop, `store` instead of `load` — is committed and dogfooded, and
**retired from the default run** like its sibling (Jerry saturating the shared
bus has hard-wedged this console into a power-cycle). It is one deliberate
flash whenever the board is next up.

Nothing here reopens this report. If the write probe also comes back null, the
zero-arbitration model is confirmed across both directions and the jerrypose
over-prediction is definitively not a bus-contention story.

---

## The write probe ran (cobweb, 2026-07-27) — also null. Qualification withdrawn.

`p_dsphammerw` on Jaguar B, both arms in one paired capture, with the execution
witness confirming Jerry was hammering (`valw=D50D50D6`):

| | Tom's stream (ticks) |
|---|---|
| Tom alone (mode B, 68k STOPped) | 655 |
| Tom + Jerry **write**-hammering DRAM | **656** |

+1 tick, +0.15%, against ~1-tick precision.

So the qualification I added above — "the null covers reads only" — is
withdrawn. **Jerry's DRAM writes do not measurably slow Tom either.** The
zero-arbitration model is now confirmed in both directions, and this report
stays withdrawn on stronger evidence than when you withdrew it.

Consequence for the live case, recorded in
`COBWEB_GAP_jerrypose_fps_overprediction.md`: with both bus directions measured
null, the jerrypose over-prediction is definitively **not** a contention story.
It is the resident-spinner mechanism — Jerry's cycle total moves 0.0005% while
1.75M cycles relocate inside it, because a core that spins when idle absorbs
new work for free. Nothing on the bus was ever going to explain that.
