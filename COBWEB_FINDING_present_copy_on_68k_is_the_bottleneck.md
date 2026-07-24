# The frame-time bottleneck is a bytewise framebuffer copy on the 68000 (~64% of frame)

**From:** cobweb/jsim session, 2026-07-23. **For:** OpenLara Jaguar port.
**TL;DR:** it was never the Blitter and never Tom idling — the 68000 is doing a
software framebuffer *present* copy, one byte at a time, every frame, right
before the flip. Move it to the Blitter and the frame roughly halves.

## The measurement (exact, not sampled, not inferred)

`--pc-histogram` (exact per-instruction cycle attribution, resolved through
`build/openlara.map`), MULTIROOM baseline, 620 frames, `--fidelity silicon`:

```
asleep in STOP     17.2%
awake              82.8%
  in vblank ISR     0.1%     <- the OP-list rebuild is NOT the problem
  main line        99.9%

pc               cycles % awake       instrs  symbol
0x0064E4       37046964  32.55%      3704366  main+0x24C
0x0064E0       29641352  26.04%      3704431  main+0x248
0x0064E2       22230964  19.53%      3704388  main+0x24A   <- 78.1% in 3 instrs
```

Those three instructions are:

```
0x64E0  move.b (a0)+,(a1)+
0x64E2  cmpa.l a5,a0
0x64E4  bne.s  $0064E0        ; then falls through to jsr video_flip
```

3.70M iterations / 620 fields = **~76,800 bytes per rendered frame** — exactly a
320x240 8bpp framebuffer, copied **one byte at a time by the 68000**, immediately
before `video_flip`. At ~1.87M cycles/frame that is **~141ms of the 218ms frame,
~64%**.

This also explains why every wait-strategy experiment came back flat (`gpu_sync`
STOP-vs-spin, `video_flip`'s poll, the Jerry co-transform all ~4.59 fps): none of
them touched the thing actually holding the clock. And retirement count is a red
herring here — you already saw +48% instructions for 0% fps change; this copy is
where the *cycles* are, not where the *instructions* are.

## Why the copy exists (and why the two fast paths don't cover this build)

The port has three present strategies. The two cheap ones are real and correct:

| path    | present mechanism                                   | 68k cost |
|---------|-----------------------------------------------------|----------|
| LOWRES  | OP hardware vertical scaler stretches 120->240      | zero     |
| HALFRES | `blit_double()` — Blitter line-doubles 120->240     | ~zero    |
| **MULTIROOM full-res (profiled)** | **68k `move.b` copy of the whole FB** | **~64%** |

The full-res MULTIROOM path evidently isn't using the LOWRES OP scaler — your
own `video.h` documents that the OP hardware vertical scaler blanks the display
under heavy multiroom fill, which is presumably why the full-res path doesn't
lean on it. But the baseline is *also* not routing its present through
`blit_double()` / the Blitter — it falls back to a plain byte copy on the 68k.
That's the defect: not that a copy happens, but that it's the slowest possible
copy on the slowest master.

## The fix — get the present off the 68k

In rough order of payoff:

1. **Blitter present.** You already drive the Blitter for the HALFRES line-double
   (`blit_double(rbuf, db, RENDER_H)` in `video.c:video_flip`). A full-res present
   is even simpler — a straight phrase copy (or a pointer swap if the display and
   render buffers can be made identical in layout; `fb0`/`fb1` are already
   `RENDER_W*DISPLAY_H`, so a true double-buffer *ping-pong flip with zero copy*
   may be reachable here and is the ideal). Route the MULTIROOM present through the
   Blitter instead of the 68k loop.
2. **If a copy is genuinely required** (format/layout differs so a pointer swap
   won't do), at minimum make it phrase-wide: `move.b` -> `move.l` is 4x fewer bus
   cycles for the same bytes, and a Blitter copy is far better still — it's a DMA
   engine built for exactly this.
3. **OP hardware scaler for full-res** is the other theoretical route (it's how
   LOWRES presents for zero cost), but your `video.h` notes it blanks under heavy
   multiroom fill — so if you go this way it needs the bus-starvation solved
   first. The Blitter present sidesteps that entirely, which is why it's ranked
   first here — but this isn't ruled out, it's just the harder path.

## Why this is safe to do now (corroborating silicon work, this session)

I ran a dedicated silicon probe this session (`calib/probes.s` `p_ovlap` /
`p_serial`, flashed on the Skunkboard) that isolates blit/compute concurrency:

| pattern                                         | silicon | jsim   |
|-------------------------------------------------|---------|--------|
| `ovlap`  (launch -> compute -> bwait)           | 118/117 | 118/118 |
| `serial` (launch -> bwait -> compute)           | 227/227 | 228/227 |

jsim matches silicon to **within 1 half-line tick**, and the blit-hidden delta
(`serial - ovlap`) is 109 on silicon vs 110 in jsim — identical. Two consequences
for you:

- **The Blitter is validated as NOT your bottleneck.** jsim's Blitter-cost and
  async-overlap models are silicon-exact. When jsim says the fill is cheap, it's
  telling the truth.
- **A Blitter present will overlap with GPU compute** rather than stall the 68k
  serially — the exact pattern the `ovlap` probe confirms silicon credits
  correctly. Moving the copy off the 68k onto the Blitter isn't just faster in
  raw bytes; it also stops serializing the present against everything else.

## Re-pin the exact source line on your current tree

Addresses above are from the profiled build; your current `main.o` has moved
(`video_flip` is at `0x5bde` in today's `build/openlara.map`). Re-run against the
current tree to get today's hot line:

```
make MULTIROOM=1 PROFILE=1 NOPROFGPU=1 AUTOSTART=1
jagemu run build/openlara.cof --pc-histogram --map build/openlara.map \
      --top 12 --fidelity silicon
```

The hot 3-instruction `move.b (a0)+,(a1)+` cluster before `jsr video_flip` is the
copy. (In `main.c` the present sites are the `video_flip()` calls around lines
1768/1848/1976/1992/2103/2356 and the async "present the PREVIOUS frame" block at
~1955 with its `for (xx=0; xx<RENDER_W; xx++)` row loop — that inner loop is the
prime candidate for what the compiler lowered to the byte copy.)

## Status of the broader fps campaign (so you know what's already closed)

The old "jsim over-predicts fps ~35%" gap is **closed** and was NOT bus
contention:

- Async Blitter charge: jsim 7.50 -> 5.43 fps vs hardware 4.9 (**the dominant
  term** — the free Blitter, now charged and silicon-validated as above).
- OP scan-out contention: measured and modeled (+11.1%).
- Tom<->Jerry DRAM contention: measured **null** on silicon (656 vs 656 ticks);
  that report was withdrawn (its 42% counter was the PIT-readback artifact, since
  fixed). Correctly *not* modeled — don't ask for it back.
- The residual frame time: **this 68k present copy.** Game-side, not a jsim gap.

Still possibly open on the jsim side, separate issue: `jerrypose` over-prediction
(+27% when pose work moves 68k->Jerry). The 68k charge was since re-anchored
(whole-program within 1%); the Jerry-marginal-load half may still be soft. That's
a jsim question, independent of the present copy — flag it if it bites you, but
the present copy is the win that's sitting right here.
