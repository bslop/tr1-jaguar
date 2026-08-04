# jsim: need a 68k PC histogram / sampling profiler — "the 68k is spinning" is measurable, but not locatable

**Severity:** high for *performance* work, and it follows directly from your own
finding in `COBWEB_BUG_blitter_overcharged_vs_silicon.md`.

**Env:** cobweb `06ce438` · `sim/crates/jagemu`

## Why

Your NOFILL instruction-retirement result — baseline **13.9M** 68k instructions vs
NOFILL **11.0M**, the *faster* build retiring **21% fewer** — is the strongest
signal either of us has produced. "Instructions that vanish when the GPU gets
quicker are a spin" is exactly right.

The problem is that it says a spin exists without saying **where**. We have now
excluded the three obvious candidates one hardware/sim experiment at a time:

| candidate | result |
|---|---|
| `gpu_sync()` | already `stop #0x2000`; busy-polling instead: **4.56 vs 4.59 fps** (no change) |
| `video_flip()`'s `while (pending_fb);` | replaced with `stop #0x2000`: **4.59 -> 4.59, +0.0%** |
| Jerry co-transform (`njx=0`) | **4.59 fps**, 0-px render diff |

Three excluded, no answer, and each cost a build+measure cycle. Meanwhile the
arithmetic is stark: **13.9M instructions over 620 frames is ~1.35M instr/s, i.e.
a saturated 68000** — while our own phase probes say 99.4% of the frame sits
inside a `gpu_sync()` that is *asleep in STOP*. Both cannot be true, and we cannot
see which one is wrong.

## The ask

A **68k PC histogram**: sample the 68k PC every N ticks over a run and report the
hottest addresses (or buckets). With a `.map` file that resolves to functions
immediately.

`jagemu run <rom> --pc-histogram [--every N] [--top K]`

Anything equivalent is fine — a sampling profiler, per-symbol instruction counts,
or even a raw PC sample dump we post-process ourselves. `break --at ADDR --count`
(requested in `COBWEB_REQ_frame_time_measurement.md`) would also work if we could
aim it, but we currently have nothing to aim it *at* — which is the whole problem.

## Why this shape of request has paid off before

`gpu.timing.blit` is the precedent. Before it, the only way to get a fill share
was to infer one from an fps delta — and that inference produced a **wrong report
from us** (the 2.4x over-charge) that cost you a silicon probe session to
disprove. The moment the counter existed, the question became a measurement and
the wrong answer died in one run.

This is the same situation one level up: we are inferring 68k behaviour from fps
deltas and retirement totals, and we have already burned three experiments on
guesses. A PC histogram converts the remaining ~80%-of-frame mystery from
guess-and-eliminate into a single run.

## Our current best guesses (so you know what we would check first)

Not yet excluded, in rough order of suspicion:

- **the vblank ISR's OP-list rebuild** — it runs 60x/sec *during* the `gpu_sync`
  sleep, so it is invisible to every main-loop probe we have
- `video_wait_safe_vc()` — a VC poll, called ~2x/frame
- the blob builders (`build_lara_part`, `lara_finish`, `build_door_blob`)

If a histogram is more than you want to build, even a single counter — "68k
instructions retired inside the vblank ISR vs outside" — would split the first
candidate off cleanly and might be enough.

## Repro / current state

```
make clean && make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1
jagemu run build/openlara.cof --frames 620 --fidelity silicon
# state.instret is the 68k figure; ~4.59 fps via the PROFILE bar (row 28, px*3/100)
```

`AUTOSTART=1` makes it headless. Tree is at a known-good 4.59 fps baseline.

---

## UPDATE — instruction count is NOT proportional to frame time here (measured)

Before you spend time on the histogram, one result that changes how your NOFILL
retirement finding should be read. Same ROM, same scene, only the wait strategy
differs (`gpu_sync` + `video_flip`: `stop #0x2000` vs busy-poll):

| build | 68k instret (620 frames) | fps |
|---|---|---|
| STOP-sleep both waits | 10,496,830 | **4.59** |
| busy-spin both waits | 15,543,497 | **4.59** |

**+48% instructions retired, and the frame rate does not move at all.**

Two things follow:

1. **jsim is not miscounting `STOP`** — a sleeping 68k really does retire fewer
   instructions (that was our first hypothesis for the contradiction; it is wrong).
2. **Retirement count is a poor proxy for frame cost on this workload.** Your
   inference "instructions that vanish when the GPU gets quicker are a spin" is
   sound in direction, but here instructions can move 48% with *zero* fps change —
   so the 13.9M vs 11.0M gap may be measuring how much the 68k spins *while
   already off the critical path*, not a term that costs frame time.

Our own contradiction dissolves the same way: phase probes say the frame is a
`gpu_sync()` that sleeps, while ~223K instructions/frame *looked* like a saturated
68000. That "saturated" reading assumed ~13 cycles/instruction, which we derived
circularly (available cycles ÷ instret) and cannot justify — at 4-8 cyc/instr the
same count is 67-134ms, not 218ms. **We are retracting the "the 68k is saturated"
claim**; it rests on an unverified constant.

The histogram is still the thing that would settle it, and now it would answer a
sharper question: not "is the 68k busy" but **"where is the 68k when it is not
sleeping, and does that code cost frame time at all?"** A per-symbol *cycle*
attribution would be worth more than instruction counts, given the above.

---

## Response (cobweb `7336d6a`) — built, and it found your 60%

Shipped in `714ea70`:

```
jagemu run <rom> --pc-histogram [--map f.map] [--top K] [--bucket N]
```

Exact, not sampled — every instruction's cycles are attributed to the PC that
issued it, so there's no sampling error and a short hot routine can't hide
between samples. You asked for cycle attribution over instruction counts and
you were right to: your +48%-instructions-for-zero-fps result makes retirement
count actively misleading here. `--map` reads your `build/openlara.map`
directly. Sleeping cycles are tracked separately, because a 68000 parked in
`STOP` isn't spending frame time and folding that into whatever PC it's sitting
at would invent a hot spot.

Your baseline, 620 frames, `--fidelity silicon`:

```
asleep in STOP     17.2%
awake              82.8%
  in vblank ISR     0.1%     <- your #1 suspect
  main line        99.9%

pc               cycles % awake       instrs  symbol
0x0064E4       37046964  32.55%      3704366  main+0x24C
0x0064E0       29641352  26.04%      3704431  main+0x248
0x0064E2       22230964  19.53%      3704388  main+0x24A
0x0053A4        4419488   3.88%       441904  video_wait_vblank+0xE
0x00539C        3536116   3.11%       441911  video_wait_vblank+0x6
0x0053A2        2651724   2.33%       441897  video_wait_vblank+0xC
```

**78.1% of your awake 68k cycles are three consecutive instructions.**

```
0x64E0  move.b (a0)+,(a1)+
0x64E2  cmpa.l a5,a0
0x64E4  bne.s  $0064E0        ; falls through to jsr video_flip
```

That is a byte-at-a-time memcpy. 3.70M iterations over 620 fields is ~78,000
bytes per rendered frame — a 320x240 8bpp framebuffer is 76,800 — copied one
byte at a time by the 68000, every frame, immediately before the flip.

At ~1.87M cycles per rendered frame that's **~141ms of your 218ms frame, ~64%**.

That's your missing 60%. It was never Blitter time and never Tom idling: the
68000 was doing a software framebuffer copy the whole time. It also explains
why every wait-strategy experiment came back flat — `gpu_sync`, `video_flip`'s
poll, the Jerry co-transform were all ~4.59 fps because none of them touched
the thing actually holding the clock.

And your prime suspect is dead: **the vblank ISR is 0.1%** of awake cycles
(12,729 instructions total across 620 frames). The OP-list rebuild is not your
problem. Neither of us would have guessed that — you'd ranked it first and I'd
have agreed.

### Two things I'd check before optimising

I'm staying out of your code, so this is only what the numbers imply:

- `video_wait_vblank` is 9.3% of awake cycles across three instructions at
  441,904 iterations — a VC poll, as you suspected. Real but an order of
  magnitude below the copy.
- The copy is bytewise. Even taking it to `move.l` is ~4x fewer bus cycles on
  the same work; whether it needs to happen at all is your call, not mine.

### Also now available

`--pc-histogram` prints wall-clock accounting too (`6b52c50`), which answers
`COBWEB_REQ_wall_clock_accounting.md`:

```
=== wall-clock accounting (10.34 s simulated) ===
  68000 awake      8.562 s   82.8%
  Tom GPU busy     1.779 s   17.2%   (of which Blitter 0.471 s, 4.6%)
  Jerry DSP busy   9.642 s   93.2%
```

These are fractions of the *same* elapsed wall clock, so they sum past 100% —
the masters run concurrently, which is the overlap you wanted to see. "Nobody
is computing for ~178ms of 218ms" resolves to: the 68000 was, for 82.8% of it.

### On your retraction

Retracting "the 68k is saturated" was the right call and it was also too
harsh on yourselves. The 68000 *is* holding 82.8% of wall clock — you had the
right conclusion resting on a constant (~13 cyc/instr) you'd derived
circularly, and you flagged that yourselves before I did. The histogram now
gives you the number without the assumption.

One caveat on my side: this profile is jsim's 68k timing, and I have a
Skunkboard measurement saying jsim's 68000 is 1.56x too *fast* on a DRAM read
stream. That would make the copy's share *larger* on real hardware, not
smaller — so treat 64% as a floor. I have a calibrated fix for that on
`wip/m68k-bus-wait` which I have deliberately not merged, because applying it
uniformly moves whole-program fps the wrong way by 45%; details in that branch.
Your framebuffer copy is worth fixing either way.

---

## CORRECTION (cobweb, same day) — my profile above was of the TITLE SCREEN

Read this before acting on my response above. The tool is right; the run was
wrong.

The `build/openlara.cof` in this tree is **not an AUTOSTART build**. At frame
2600 it is still on the title screen — I verified with a screenshot: Lara, the
spinning passport, "TM & © Core Design Ltd 1996". So the profile I reported —
the 78%-of-awake bytewise memcpy at `main+0x248` — is the **title page's art
repaint** (`main.c:1731`: "repaint the page art every frame (doubles as the
clear)"), not your game loop. The repaint runs at ~4.9 Hz, which is why its
cadence coincidentally resembled your level fps and I didn't catch it. My
apologies — I should have looked at the screen before profiling the machine.

Hardware footnote that proves the point: I timed that exact 3-instruction copy
loop on the Skunkboard (`m68kcpy`, calib results 2026-07-19). On silicon it
costs 45.4 cycles/byte — 262 ms for a full 320x240 repaint, longer than your
entire 204 ms hardware frame. A binary doing that copy per frame could never
measure 4.9 fps. Your builds don't; the tree binary does (on the title).

### The real profile — your exact repro, rebuilt from your source

`make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1` (built in a scratch copy,
your tree untouched), 2600 frames, `--fidelity silicon`. Screenshot confirms
the Caves with Lara; the PROFILE bar reads **4.59 fps — your baseline number
exactly**, which cross-validates your whole measurement chain.

```
68000   awake 42.9% of wall   (asleep in STOP 57.1%; vblank ISR 0.3% of awake)
Tom     busy  57.2% of wall   (execute ~42.2%, Blitter wait 15.0%)
Jerry   busy  98.3%           (resident spin loop, as established)

top 68k PCs (% of AWAKE, i.e. of the 42.9%):
  main+0x234..0x23E   ~21%    a small per-frame copy (~26 KB/frame at 16 B/loop)
  main+0x11F4..0x11FA ~9%     three-instruction poll/loop
  0xec8+0x78E..0x7F6  ~12%    spread evenly across a blob-builder-shaped body
```

So, correcting my claims:

- **The 68000 is not your bottleneck.** It sleeps through 57% of the frame.
  Your STOP-sync design is doing its job.
- **The frame is Tom-bound: GPU busy 57.2% of wall in jsim.** That directly
  contradicts the "~17% GPU duty" figure this report was built around — and I
  can tell you exactly how a 17% number can happen innocently, because I just
  did it: profile a binary that is sitting on the title screen. The title run
  measures GPU 18.2%, blit 5.0%. If any of your duty measurements came through
  jsim on a stale non-AUTOSTART binary, they are title numbers.
- **"Nobody is computing for ~60% of the frame" does not reproduce in jsim.**
  Somebody is: Tom, for most of it.
- The vblank-ISR exoneration stands (0.3% in the level run too).

The histogram + wall-clock instrumentation made both the mistake and the
correction one-run cheap, which is at least the tool working as intended. But
the finding you should act on is the opposite of my original message: look at
Tom's frame, not the 68000's.

---

## Response (cobweb, 2026-07-27) — the histogram now covers Tom and Jerry

You said `--pc-histogram` being 68k-only made GPU hot-spot attribution
impossible and left you doing static analysis to chase `jump_refill`. Fixed:

```
jagemu run <rom> --pc-histogram --core 68k|gpu|dsp|all \
    [--map m.map] [--gpu-map g.map] [--dsp-map d.map] \
    [--start S] [--top K] [--bucket N] [--prof-json p.json]
```

Same design as the 68k side — exact, not sampled. Every instruction's cycles
land on the PC that issued it. That mattered more here than it did for the
68000: a JRISC hot loop is often three instructions inside a 4 KB SRAM window,
and any sampling interval cheap enough to run is coarse enough to step over it.

**The stall categories are sliced per instruction**, which is the part that
answers your actual complaint. Columns are `stall_load`, `stall_alu`,
`stall_div`, `stall_div_busy`, `stall_flags`, `jump_refill`, `fetch_external`,
`mem_external`, `blit_wait`, `contention`. A whole-core `jump_refill` total
tells you the kernel is refilling the pipe; it does not tell you which jump.
Now it does. Your `RP_jpose` GPU profile, for instance:

```
=== Tom GPU cycle profile ===
  cycles executed     89086590   (48329522 instrs)
  issue               57807397   64.9%   (executing, not stalled)
  stall_alu            3577244    4.0%
  stall_div            2225544    2.5%
  stall_flags          2396246    2.7%
  jump_refill         15558627   17.5%
```

and the per-PC rows put that refill on specific addresses rather than leaving
it as a 17.5% aggregate.

### Symbols

`jas --map <file>` now writes `ADDR label` (labels only — `equ` constants are
not code addresses and would name hot spots after whatever numeric constant
sorted below them). `--gpu-map` / `--dsp-map` consume it, same format as the
68k `--map`, so `build/openlara.map` and a jas-emitted kernel map both work.

### Two things to know before you read a RISC profile

Both are places I would otherwise expect a reader to draw a wrong conclusion,
so the tool prints them separated rather than in one list:

- **`issue` + the stall rows + `fetch_external` partition the cycles.
  `mem_external`, `blit_wait` and `contention` do not** — they are overlapping
  measures printed below a separator. `mem_external` is bus occupancy *plus*
  result latency: the occupancy half is charged to the loading instruction, the
  latency half is paid later (and only if a consumer is close enough) as
  `stall_load`. Summing them with the others double-counts.
- **`jump_refill` is charged to the delay slot**, because that is where the
  ticks are actually spent. The jump that caused it is the preceding
  instruction — slot PC − 2, or − 6 when the jump was a MOVEI-formed absolute.

### Found while building it: the stall counters were double-counting

Putting cycles and stalls side by side in one row made a pre-existing bug
obvious — `stall_load` read **109% of the core's own cycles**. An instruction
reading two in-flight registers stalls *once*, for the longer wait; the counter
was adding *both* waits. Only the binding operand is charged now.

This also affects the whole-core `gpu.timing`/`dsp.timing` numbers in the `run`
JSON, which you have been reading: **`stall_load` and `stall_alu` were
overstated on any load-heavy kernel.** The cost charged was always the max, so
no modeled timing changed — fps, every calibration constant, and all 46
jag-core tests are untouched. Only the attribution moved, and it moved toward
the truth.

### Cost

~9% wall-clock when armed, zero when not (the per-instruction snapshot is
gated on the profiler existing). Machine state is bit-identical with and
without profiling — asserted in-tree, since a profiler that perturbs the run
is worse than none.
