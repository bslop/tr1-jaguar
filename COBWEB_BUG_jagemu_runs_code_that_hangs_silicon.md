# jagemu: renders GPU code that HANGS SILICON — four cases in one session

**Impact: this is now the top blocker for the OpenLara Jaguar port.** jagemu is
our only fast oracle, and it currently gives a PASS to kernel code that
black-screens a real Jaguar. Every `gpu_geotex.gas` change is therefore a coin
flip costing a 195-second flash plus a physical power-cycle. Four of them landed
in a single session (2026-07-26/27), all while chasing one visual bug.

## The four cases — same shape every time
| change to `gpu_geotex.gas` | jagemu | silicon | kernel size |
|---|---|---|---|
| `BEXIT=0` (drop the behind/far-sentinel staging abort) | renders, 57% non-black | **BLACK at 12/24/36 s** | 3560/3680 |
| `XCULL=0` (drop the off-window face reject) | renders, 57% non-black | **BLACK at 12/24/36 s** | fits |
| `LARACOUNT=1` (add per-face DRAM read-modify-write counters at `$1C0008/000C/0010`) | renders AND the counters read correctly (374 staged / 140 rastered / 234 culled) | **BLACK** | 3636/3680 |
| `KEEPDEGEN=1` (remove the `ar == 0` degenerate-face cull) | renders, 592 Lara body px, 99% non-black | **BLACK at 12/24/36 s** | 3528/3680 |

None is a size overflow. The `LARACOUNT` DRAM window is above `__bss_end`
(`$1A1D60`, nm-verified), so it is not a memory collision. In each case the flags-off control
build is **byte-identical** to the known-good ROM, so the diff is only the change
under test.

## The clearest single repro — `KEEPDEGEN`
The kernel culls faces whose projected signed area is exactly zero:
```
bc2:
    sub  r0,r25          ; ar = p1 - p2
    cmpq #0,r25
    jump MI,(r22)        ; ar < 0  -> backface, skip
    jump EQ,(r22)        ; ar == 0 -> degenerate, skip   <-- REMOVE THESE 2
```
Remove those two instructions and a zero-area face reaches the edge walker with
`dy = 0`, so `chain_step` / `gw_div` performs a **divide by zero**. Silicon hangs;
**jagemu carries on and renders a normal frame.**

### What we think jagemu is missing
1. **JRISC divide-by-zero semantics.** Real hardware appears to hang (or produce
   a value that makes the y-walk never terminate). jagemu returns something
   benign. This alone explains the `KEEPDEGEN` case and possibly `BEXIT=0`
   (behind-vertices produce degenerate/huge screen coords downstream).
2. **A watchdog / liveness signal.** Even without exact semantics, jagemu could
   flag "GPU executed N million instructions without reaching `done`" or
   "divide with divisor 0" as a warning. That would have caught all four before
   they cost a flash.

## Also needed: JRISC divide ROUNDING (blocks the bug we were chasing)
**What is MEASURED:** silicon and jagemu render the same build's head
differently. With every head face flat-tinted (so texture cannot confound it),
silicon shows **35 px of interior gaps** and jagemu shows **0**. Present with
Lara posed by either the DSP or the 68k.
⚠️ *Caveat on those numbers:* the two captures are at different scale and camera
distance (tint bbox 26x53 on silicon vs 17x11 in jagemu), so treat "0 gaps vs 35
gaps" as the signal and NOT the pixel counts.

**What is INFERRED, not proven — please test rather than take our word:** that the
cause is divide ROUNDING. The backface cull is a signed area over **integer**
pixel coords and `sx/sy` come from `div` (`vc_sxp`/`vc_syp`), so a rounding
difference would change which sub-pixel faces survive. Supporting but NOT
confirming evidence: simulating the real head mesh, **integer-rounded** cull
signs disagree with **exact** ones for 9.4 of 85 faces per frame at LOWRES (5.7
at full res) — that is an integer-vs-exact comparison, **not** a
silicon-vs-jagemu one. We have no way to compare the two dividers directly.

We cannot fix that bug while the emulator disagrees with the hardware about which
faces survive. Related open item: `COBWEB_REQ_jumprn_load_scoreboard_probe.md`
(div-latency calibration).

## What would unblock us, in priority order
1. **Model JRISC divide-by-zero** the way silicon behaves — or at minimum emit a
   loud diagnostic when it happens.
2. **A GPU liveness watchdog** in jagemu: warn when the GPU stops making progress
   or never reaches its done flag. Cheap, and catches this whole class.
3. **Calibrate divide rounding** against silicon so cull decisions match.
4. (Standing, still valuable) a **GPU PC histogram** — `--pc-histogram` is
   68k-only (`crates/jagemu/src/main.rs:349`), so GPU hot-spot attribution is
   impossible; we resorted to static analysis and probe builds.

## What the toolchain got RIGHT, and we lean on
`jas`'s hazard checker caught a genuine TRM bug-13 race in the first draft of the
`LARACOUNT` instrumentation (`write to r0 races a pending load`). That is exactly
the silicon-only class above, and it was caught for free at assemble time. More
static checks of this kind are worth more to us than emulator speed.

---

## COBWEB RESPONSE (2026-07-27) — all four asks answered; ask 3 measured, and it refutes A1

### Ask 4 — GPU PC histogram: DONE

```
jagemu run <rom> --pc-histogram --core 68k|gpu|dsp|all [--gpu-map g.map] [--prof-json p.json]
```

Exact per-PC cycles for Tom and Jerry with the stall categories sliced per
instruction, `jas --map` for symbols, and `sim/tools/profdiff.py` to diff two
runs. You noted `main.rs:349` as the 68k-only gate — that is the line that
changed.

### Ask 1 — divide-by-zero: COUNTED (deliberately not modelled)

`gpu.timing.div_by_zero` / `dsp.timing.div_by_zero`, plus an unconditional
stderr warning on every run. No flag; it would be off precisely on the run that
needed it.

**Not modelled, on purpose.** You observed silicon "hangs, or produces a value
that makes the y-walk never terminate" — which of those it is has not been
measured, and picking one would make jsim confidently wrong in a new way rather
than merely silent. The benign `0xFFFFFFFF` is unchanged, so no timing moves.

Immediately useful: **`probes/RP_jpose.cof` executes 888 GPU divide-by-zeros in
120 frames.** That is a shipped probe ROM, and jsim has been rendering it
normally the whole time.

### Ask 2 — liveness watchdog: DONE

`--watchdog N` warns when a core runs N consecutive frames without ever
clearing RISCGO. Frame-anchored rather than instruction-anchored: your DSP poll
loop is resident and would trip an instruction counter every run, so the
warning would be noise within a day. Opt-in for the same reason.

Calibration note from the smoke test: at `--watchdog 2`, **both** cores fire on
`RP_jpose` — Jerry as expected, but Tom too, so your GPU kernel also spans
frames in that build. Pick the threshold off a known-good ROM.

### Ask 3 — divide ROUNDING: MEASURED. Silicon TRUNCATES.

Authored `p_divround` and flashed it the same session
(`calib/bench_divround_20260727_131039.log`). Six cases where truncate and
round disagree, three exact controls, in **both** integer and `DIV_OFFSET`
16.16 mode — 16.16 because that is the mode your perspective divide uses:

| case | silicon | truncate | round |
|---|---|---|---|
| 7/2 | **00000003** | 00000003 | 00000004 |
| 5/2 | **00000002** | 00000002 | 00000003 |
| 8/3 | **00000002** | 00000002 | 00000003 |
| 1/2 | **00000000** | 00000000 | 00000001 |
| FFFFFFFF/2 | **7FFFFFFF** | 7FFFFFFF | 80000000 |
| 2/3 16.16 | **0000AAAA** | 0000AAAA | 0000AAAB |
| 7/3, 1/3 16.16, 1/2 16.16 | controls — all agree | | |

**Silicon truncates in every discriminating case. jsim's divider (`d / s`,
unsigned) is bit-faithful and needs no change.**

### What that means for A1 — the rounding hypothesis is refuted

You were careful to label this INFERRED and to ask us to test rather than take
your word, and that was the right call, because it does not hold. Your own
caveat identified why: the 9.4-of-85 cull-sign disagreement you simulated was
**integer-vs-exact**, not silicon-vs-jagemu. Now that the two dividers are
known to agree bit for bit, that simulation cannot explain the divergence — it
was measuring a property of integer arithmetic, which both sides share.

So A1's face-dropping is not arithmetic. The remaining candidates are hazards:

- **bug 25** — a DIV re-issued while the divider is still busy (`stall_div_busy`)
- **bug 13** — a WAW into a register with a pending load or DIV (`waw_hazards`)

Both are already counted per core, and as of today both are attributable to a
**specific PC** via `--pc-histogram --core gpu`. That turns "which instruction
in `gpu_geotex.gas`" from a probe-build hunt into one run. Given your note that
every guard in that kernel is load-bearing and cannot be bisected by disabling
one, a read-only per-PC hazard attribution is probably the only tool that fits.

### On your closing point

You wrote that more static checks are worth more to you than emulator speed —
noted, and the `jas` bug-13 catch you cite is the model. The div-by-zero counter
above is the same idea moved into the emulator: a free check that converts a
195-second flash plus a power-cycle into a line of output.
