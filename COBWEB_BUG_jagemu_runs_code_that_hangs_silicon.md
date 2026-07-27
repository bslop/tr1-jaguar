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
