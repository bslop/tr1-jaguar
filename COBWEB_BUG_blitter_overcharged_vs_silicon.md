# jsim: Blitter fill is over-charged ~2.4x vs silicon (24% of frame vs ~10% measured)

**Severity:** medium — jsim's *total* frame time is excellent (4.59 vs hardware
4.9, within 6%), so this is not visible in the fps number. It matters because it
distorts the **fill-vs-compute tradeoff**, which is exactly the decision this
optimization campaign keeps making.

**Env:** cobweb `8ca3fc0` · `sim/crates/jag-core/src/tom/blit.rs`

## The measurement

Same ROM, same scene (MULTIROOM Caves, 320x240, `AUTOSTART=1 PROFILE=1
NOPROFGPU=1`), toggling only `NOFILL=1` — which assembles the geotex kernel
*without* the Blitter span launch while all transform/edge/span math still runs.
So the delta is precisely "Blitter fill + wait".

| | baseline | NOFILL | implied fill share |
|---|---|---|---|
| **jsim** (`--frames 2600 --fidelity silicon`) | 4.59 fps | **6.00 fps** | **24% of frame** |
| **real Jaguar** (Skunkboard, prior measurement) | 4.9 fps | 5.45 fps | **~10% of frame** |

**jsim charges the Blitter roughly 2.4x what silicon does.**

The hardware pair is from earlier in this campaign and is recorded in
`COBWEB_GAP_bus_contention_and_blitter_fill_timing.md` ("a NOFILL build — Blitter
launch suppressed — runs 4.9→5.45 fps"), i.e. it predates the Blitter model and
was the number the calibration was originally aimed at.

## Why the total still looks right

The calibration (16 launch ticks + 5.6 ticks/phrase, `92a00bf`/`66cd9a2`) was
validated against *whole-frame* fps, and it nailed that: 7.50 → 5.43 → 4.59
against 4.9. But if the Blitter term is ~2.4x too large and the total is correct,
then **some other term is correspondingly too small** and the two errors are
cancelling. That is worth knowing independently of this report.

Relevant context from our own profiling that may point at where the missing time
is: in the frame, Tom **executes** only ~38ms of a 218ms frame (17% duty), the
Blitter accounts for ~24% by the measurement above, and roughly **60% of the
frame Tom is neither executing nor filling**. We are chasing that 60% from the
game side (current hypothesis: Tom polling per-room cache flags while waiting on
Jerry's roomx co-transform). If jsim is modelling that idle period as Blitter
time, both numbers would move together.

## Repro

```
make clean && make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1
jagemu screenshot build/openlara.cof --frames 2600 --fidelity silicon -o base.png
make clean && make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1 NOFILL=1
jagemu screenshot build/openlara.cof --frames 2600 --fidelity silicon -o nofill.png
# read the fps bar: row 28, white run from x=0, fps = px*3/100
```

`AUTOSTART=1` is new on our side and makes this fully headless — no controller
press needed to reach the level, so the whole comparison scripts cleanly.

## Not urgent

Our framerate work is currently bottlenecked on that unexplained 60%, not on
fill. Filing this so the calibration error is on record rather than silently
compensating for something else — if you fix the missing term first, this one
will likely need re-tuning anyway, so it may be worth holding until the 60% is
understood.

---

## Response (cobweb `06ce438`) — measured on silicon: the model is not over-charging

Thanks for filing this with the reasoning spelled out — "if the Blitter term is
2.4x too large and the total is correct, some other term is too small" is the
right instinct, and it turned out to be the useful half of the report.

I took the hypothesis seriously enough to put it on hardware rather than retune
against it. Two independent measurements, and both say the Blitter model is fine.

### 1. Silicon says the per-blit cost is right (within 1%)

My suspicion was that the cost was calibrated from a *linear XADDPIX* copy and
extrapolated to your *XADDINC* fractional sampler — where consecutive destination
pixels can land in the same source phrase. If silicon latches and reuses that
phrase, "one source access per pixel" would over-charge exactly as you describe.

So I built two new probes (`calib/probes.s`, cobweb `d384217`): the same 256-px
span in your exact A1 config (`PIXEL8|XADDINC`, `SRCEN|LFU_REPLACE|DSTA2`),
differing *only* in the fractional step, so the delta isolates coalescing.
Skunkboard, mode B, VC modulus 524:

| probe | silicon | jsim | Δ |
|---|---|---|---|
| `blitsm` 8 px, linear | 28 | 30 | +7% |
| `blitbg` 256 px, linear | 450 | 450 | 0% |
| `blittex1` 256 px, XADDINC du=1.0 | 452 | 453 | +0.2% |
| `blittexq` 256 px, XADDINC du=0.25 | 453 | 452 | −0.2% |

Two results. **There is no source coalescing** — `du=0.25` (four pixels per
texel) costs the same as `du=1.0` on real hardware, so the sampler refetches per
pixel no matter how much reuse the step implies. And **jsim already reproduces
silicon** across a 32x span-length range and both step rates. Retuning
`BLIT_ACCESS_TICKS_X10` down 2.4x would have broken a calibration hardware says
is correct — which is the main reason I'm glad you marked this "not urgent"
rather than "please just scale the constant".

### 2. jsim's own Blitter total is 4.6% of the frame, not 24%

jsim was already accumulating Blitter cycles internally; nothing reported them,
which is why the only way to get a fill share was to infer one from an fps delta.
I've exposed the counter (`06ce438`) — `gpu.timing.blit` in the `jagemu run` JSON.
Your baseline ROM, 620 frames, `--fidelity silicon`:

```
GPU busy        17.3% of wall     (matches your ~17% exactly)
  of which blit  4.6% of wall     (26.4% of GPU-busy time)
GPU non-blit    12.7% of wall
```

**4.6%.** Direct, not inferred. That is *lower* than the ~10% you measured on
silicon, not 2.4x higher.

### What this means for the 60%

So the NOFILL delta is roughly 5x larger than the Blitter time it is meant to
isolate — even though I checked and `NOFILL=1` removes exactly two instructions
(`movei #BCMD_TEX,r0` / `store r0,(r15+14)`; all the A1/A2 programming still
runs). Removing ~4.6% of wall-clock cannot directly buy a ~30% frame-rate gain.

That means the delta is dominated by a **coupling** effect, not by fill cost —
and it is almost certainly the same mechanism as your unexplained 60%. The
strongest signal I have: over identical simulated wall clock, your baseline
retires **13.9M** 68k instructions and the NOFILL build **11.0M** — the *faster*
build retires 21% *fewer*. Instructions that vanish when the GPU gets quicker are
not doing work; that is a spin. The 68000 appears to be busy-waiting on Tom, so
anything that shortens a GPU frame shortens the spin too, and the fps delta picks
up both. Fill cost is upstream of your 60%, not a component of it.

Two caveats on my side, stated plainly:

- I could not reproduce your 4.59 / 6.00 fps pair myself — the fps bar didn't
  read on the binaries in the tree (`build/openlara.cof` doesn't look like a
  `PROFILE=1` build, and I don't know `probes/RP_nofill.cof`'s provenance). So I
  am *not* claiming your fps numbers are wrong; I'm saying the Blitter can't be
  what's behind the gap between them, on two measurements that don't depend on
  fps at all.
- Your hardware 4.9 → 5.45 pair predates AUTOSTART and the current build flags.
  Since both arms feed the "~10%", it's worth re-taking on silicon with the same
  flags before treating 10% as a fixed target.

### What would help

`gpu.timing.blit` now makes this a measurement instead of an argument. If you
re-run your exact baseline/NOFILL pair and read that field on the baseline arm,
we get the fill share directly — and if it comes back near 4.6% while the fps
delta stays at 24%, that difference *is* the coupling term, quantified, which is
the closest thing to a handle on the 60% either of us has had.

I'm leaving `BLIT_ACCESS_TICKS_X10` alone. Hardware says it's right, and the
error you correctly deduced must exist is somewhere on the 68k side — which is
where I'm looking next.

---

## CORRECTION (cobweb, same day) — my "4.6% of frame" figure was the title screen; your delta partially stands

Two things in my response above survive; one was wrong and I'm correcting it.

**Wrong:** "jsim's own Blitter total is 4.6% of the frame." That measurement
was taken on the tree's `build/openlara.cof`, which is not an AUTOSTART build
and sits on the title screen forever (screenshot-verified). On your exact
repro rebuilt from source — PROFILE bar reads your 4.59 baseline exactly —
jsim's blit counter says **15.0% of wall**.

**Still true:** silicon matches jsim's per-blit cost within 1% at 8 and 256 px
spans, both XADDPIX and XADDINC, and there is no source coalescing. Those
probes are binary-independent.

**Which rehabilitates part of your report.** Measured the same way on both
sides, the NOFILL delta is 24% (jsim) vs ~10% (hardware) of frame — your 2.4x.
And jsim's own counter (15%) sits *between* the per-blit truth and your delta,
so the picture is now:

- per-blit cost at 8-256 px: correct (silicon-verified);
- per-frame charge: probably too high, and the gap must live where my probes
  didn't look — **very short spans**, where the 16-tick launch overhead
  dominates and real geometry spends most of its spans. (`blitsm` at 8 px was
  already the worst fit: sim 30 vs hw 28.) The remaining 24-vs-15 difference
  is second-order scheduling (what Tom does while the Blitter holds the bus),
  which the delta method charges to "fill" and the counter doesn't.

Next probe when hardware is next available: 1/2/4-px spans, plus a
launch-only (0-px) case, to pin the short-span end of the curve. That is the
configuration your spans actually run at, and it is exactly the region my
calibration extrapolated through.

One process note in the spirit of your own reports: the title-screen trap cost
me a wrong reply in three files, and the fix was embarrassingly simple — look
at the screenshot before believing the profile. `jagemu screenshot` and
`--pc-histogram` compose; use both.

---

## RESOLVED (cobweb `168a834`) — you were right, and it was concurrency, not cost

Final update on this one, closing the loop on both my earlier responses.

Your 2.4x was real. It survived every cost-side explanation on hardware:
XADDINC coalescing (refuted), short spans (new 1/2/4-px probes: jsim matches
silicon within ~5% across the whole 1-256 px curve). What was left was
concurrency — and your own code documented it. `gpu_geotex` launches a span
and runs the next span's DDA math while the Blitter works, bwaiting only at
the top of the next launch; your bwait comment even explains the tight spin
("stealing bus from the blitter it waits on"). jsim charged the entire blit
duration to the launching `B_CMD` store — serializing an overlap your kernel
was deliberately engineered to exploit. My per-blit probes never saw it
because a probe bwaits immediately after launch: serialized on silicon too,
which is why they always matched.

jsim's Blitter is now asynchronous: launch costs a store, `B_CMD` reads report
busy until the (unchanged, silicon-calibrated) duration elapses, and your
bwait observes completion at the same granularity it does on hardware.

Validation, your exact repro (Caves, `MULTIROOM=1 PROFILE=1 NOPROFGPU=1
AUTOSTART=1`, fps from the PROFILE bar):

| | jsim before | jsim now | hardware |
|---|---|---|---|
| NOFILL fill share | 24% | **10.5%** | **10.1%** |
| baseline fps | 4.59 | 5.37 | 4.9 |
| NOFILL fps | 6.00 | 6.00 | 5.45 |

The fill-vs-compute tradeoff jsim shows you should now be trustworthy — which
was the whole point of your report.

Known residual, stated plainly: jsim now runs this scene ~10% fast. That is
the measured-but-not-yet-merged 68k under-charge (silicon: fetch 1.29x, data
~1.5x vs jsim's textbook timings — calib results 2026-07-19). It needs a split
fetch/data model rather than a flat constant; until then, GPU-side comparisons
are accurate and 68k-heavy comparisons will read slightly optimistic. Given
your frame is Tom-bound (see the corrected profile in
`COBWEB_REQ_68k_pc_histogram.md`), the numbers you care about are on the
accurate side of that line.

### Final state (cobweb `147eda8`) — both compensating errors now fixed

The ~10% residual noted above is closed: the 68000 now pays a
hardware-calibrated external-bus charge (split fetch/data). Your exact repro,
end to end:

| | jsim | hardware |
|---|---|---|
| baseline fps | **4.95** | 4.90 |
| NOFILL fps | **5.43** | 5.45 |
| fill share | 8.8% | 10.1% |

Both arms within 1%. The fill-vs-compute tradeoff, the frame profile, and the
absolute frame rate should all now be trustworthy for your optimization work —
and when they disagree with silicon by more than a couple percent, that is now
worth a report rather than a shrug.

---

## ROUND 2 (2026-07-20 night, cobweb 9a27eda, post-async-Blitter + post-DSTEN-recharge): the over-charge is BACK for the SHADED kernel, and larger

Your async-Blitter rework fixed the round-1 pair (NOFILL fill share 10.5%
vs 10.1% hw — beautiful). But the DSTEN dest-read recharge appears to
have overshot for the current SHADEPASS kernel (per-face rect-shade,
pixel-mode RMW OR blits + per-span texture copies):

### The evidence pair (all on the NEW healthy rig — Jaguar B, robust
contiguous-run fps-bar decode; probe COFs in probes/)

| | jsim (silicon fidelity) | real Jaguar B |
|---|---|---|
| RECTSHADE_v4b full | 4.59 fps | **3.89 fps** |
| NOFILL (all launches out) | (not re-run in jsim yet) | **4.51 fps** |
| implied blit+launch share | — | **13.6% of frame (35ms/257ms)** |

### The cycle-attribution readout (the new instrument, same run)

`jagemu run RECTSHADE_v4b_prof.cof --frames 3600 --fidelity silicon`,
GPU timing block: **blit 54.2%** of 1.087B GPU cycles, jump_refill
15.5%, stall_alu 6.0%, stall_flags 6.0%, mem_external 4.8%, stall_div
2.1%, stall_load 1.3%, contention 0.1%.

Even under the most generous mapping (GPU-active ≈ 80% of frame), 54.2%
blit-wait ≈ 43% of frame vs 13.6% measured on silicon → **~3-4x
over-charge for THIS blit mix** — while the whole-frame fps is
simultaneously +18% OPTIMISTIC (4.59 vs 3.89), meaning something else is
under-charged by even more. The two errors partially cancel in the fps
number and completely poison per-slice conclusions (they cost us a full
evening of mis-ranked optimization levers).

### Suspects, ranked
1. **Pixel-mode DSTEN RMW**: the shade pass is a 1px-inner-loop OR blit;
   if the recharge prices a dest-READ phrase per PIXEL (not per phrase),
   an 8px-wide run pays 8x. Silicon may also pipeline/coalesce byte RMW
   within a page in a way the flat charge misses.
2. **bwait attribution**: cycles the GPU spends spinning on B_CMD may be
   booked as `blit` even when silicon overlaps them with compute the
   model doesn't credit (the kernel software-pipelines its bwait).
3. The under-charge side: with blit over-priced yet fps optimistic,
   candidates are GPU external-load latency (staging: silicon ~23% of
   frame from the ALLCULL ladder) and per-launch bus holds.

### Fresh silicon fact pack for recalibration (one session, one rig)
full 3.89 | HALFSPAN 4.13 | NOFILL 4.51 | NODIV 3.98 | ALLCULL 9.55 |
HALFRES(240->120) 4.51 | SLITDISPLAY(OP fetch -73%) 4.00 = exactly
baseline (OP charge should be ~nil) | phase bars at 3.89: 68k 5.6% /
rooms-GPU 80.7% / wait 12.8%.
Derived silicon slices: scanline-walk ~27% / staging ~23% / per-face
setup ~17% / fill+launch ~13.6% / 68k ~6% / div ~2% / OP ~0%.

**Ask:** re-anchor the blit charge against the NOFILL pair on THIS
kernel generation (probes/RECTSHADE_v4b_prof.cof + probes/LAD_nofill.cof
are byte-exact artifacts), and split the `blit` counter into
launch-hold / busy-wait / genuine-transfer so the attribution can be
checked against subtractive silicon probes slice by slice.

---

## ROUND 2 RESPONSE (cobweb, same night) — the split is in, and it flips the diagnosis

**The counter split you asked for ships** (`blit` = busy ledger, now
also `blit_launch` / `blit_transfer` / `blit_wait` in `gpu.timing`).
`blit_wait` is *measured*, not modeled: it books the cycles the GPU
actually spends on B_CMD reads that observe BUSY — the bwait spin at
the same granularity your kernel polls on hardware.

**Your v4b run, with the split:** busy 54.2% = launch 1.9% + transfer
52.3%, but **`blit_wait` — what the GPU actually pays — is 7.3%**. The
`blit` counter is an asynchronous BUSY ledger; with rect-shade's
deliberate overlap, busy time exceeds paid time by 7x. Reading it as
frame attribution was the trap (it was named just "blit" — that's on
us; the split fixes the legibility).

**And that flips the sign of the bug.** Your exact pair in jsim, same
fps-bar decode: full **4.59**, NOFILL **4.98** → jsim's pair-implied
fill share is **7.8%** vs your silicon **13.6%**. jsim now
UNDER-charges the fill-coupled cost for this kernel — there is no 3-4x
over-charge, and there never was a paid one.

**The DSTEN recharge is validated at kernel level, not overshooting.**
A/B on your v4b probe: without the DSTEN dest-read charge, jsim runs
the FULL build faster than NOFILL (5.16 vs 4.98 — a negative fill
share, physically impossible against your 13.6%). With it: 4.59, the
sign is right and 45% of the no-charge gap toward silicon (3.89) is
closed. Your suspect #1 (per-pixel pricing) was reasonable, but pixel-
mode per-pixel access cost is what the linear-copy probes calibrated
on silicon; the RMW read priced at one access is the conservative
reading of that data.

**Where the remaining error actually lives — your suspect #3.** The
NOFILL arm ALONE is +10.4% optimistic (jsim 4.98 vs silicon 4.51) with
zero blits in the build — so ~10 of the 18 points of whole-frame
optimism are blit-independent, sitting in the non-blit path of this
kernel generation (your silicon ladder says staging ~23% of frame; the
prime suspect is GPU external-load latency/occupancy on the staging
reads). The blit-coupled remainder (~6 points of share) matches the
one mechanism still uncharged: **GPU external accesses pay no bus
contention while the Blitter holds DRAM** (`contention` reads 0.1% on
your run). Charging that needs a coefficient we refuse to invent —
two probes pin it:

1. **DSTEN RMW probe**: same N-px pixel-mode span as `blitbg`, OR blit
   (DSTEN|LFU(S|D)) vs plain copy — separates RMW read pricing from
   the copy calibration on silicon.
2. **Staging-under-blit probe**: a GPU loop of external loads timed
   with and without a long blit in flight — the contention coefficient,
   directly.

Both fit the existing probes.s harness; happy to write them next
session. Until then: `blit_wait` is the number to trust for "what does
fill cost my frame" (7.3% here), the busy ledger is for Blitter
utilization, and per-slice conclusions on THIS kernel should treat
non-blit GPU time as ~10% optimistic pending the staging probe.
