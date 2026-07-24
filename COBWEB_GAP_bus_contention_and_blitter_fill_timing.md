# jsim over-predicts fps by ~35%: unmodeled DRAM bus contention + free (synchronous) Blitter fill

> **STATUS (audited 2026-07-19):** PARTLY RESOLVED — Blitter half fixed in `674767d` (7.50->5.43 fps, ~2.1 of the 2.6 gap); LOADP latency in `6f1fd08`. Contention half DECLINED with hardware evidence — see COBWEB_GAP_tom_jerry_contention.md, which we WITHDREW. Closed.

**Severity:** high for *performance* work. jsim renders correctly and its
per-core issue/stall model is good, but its **frame-time / fps prediction is
systematically optimistic**, so it cannot be used to locate the bottleneck of a
real, bus-bound game — which is the entire OpenLara framerate effort right now.

**Env:** cobweb `f5197f3` · `sim/crates/jag-core/src/tom/blit.rs`,
`sim/crates/jag-core/src/scheduler.rs`

## The hard number

Same ROM (OpenLara MULTIROOM 320×240, `HWH_clean.cof`), same scene (Caves),
measured off the on-screen PROFILE fps bar:

| | fps |
|---|---|
| **jsim** (`--fidelity silicon`, 2600 frames) | **~7.5** |
| **real Atari Jaguar** (Skunkboard, video capture) | **~4.9** |

**jsim is ~53% fast / the real frame is ~35% longer than jsim charges for.** That
missing 35% is where the game actually lives, and jsim is blind to it.

## What jsim isn't charging for

### 1. The Blitter is synchronous — fill costs 0 cycles
`tom/blit.rs`: `tom_read32(B_CMD)` always returns `BLIT_IDLE`, and `blit::run`
does the whole span instantly. So the GPU's `bwait` never spins and the pixel
fill is free. On silicon the fill is ~12% of the frame (measured: a `NOFILL`
build — Blitter launch suppressed — runs 4.9→5.45 fps). **Request:** charge the
Blitter a realistic cycle cost per launch (phrase writes × count, page/refresh),
and have `B_CMD` read back *busy* until that many cycles elapse so `bwait` spins
as it does on hardware.

### 2. No shared-DRAM bus contention between masters (the big one)
`scheduler.rs` gives Tom and Jerry independent per-slice budgets, and the OP
scan-out / Blitter fills don't compete with them for DRAM. On the real Jaguar
there is **one 64-bit DRAM bus** shared by: the 68k, Tom (GPU), Jerry (DSP), the
Blitter, and the Object Processor (which reads the framebuffer every displayed
line). When two masters want DRAM in the same cycle, one **stalls**. In this
game all of them are hammering DRAM concurrently — Tom's per-face vertex-cache
reads/writes, Jerry's pose, the Blitter's fill, the OP's continuous scan-out — so
contention is a first-order cost. The engine's own source is built around it
(`gpu_sync` uses `stop` to "release the bus completely"; comments about
"flag-dance livelock under bus load" and "Jerry's DRAM write storm vs the vblank
OP-list rebuild").

**Evidence it's unmodeled:** in the clean in-render profile jsim reports GPU
`mem_external` ≈ 6% and `contention` ≈ 0 — i.e. Tom almost never waits on the
bus. That is not credible for a game this DRAM-heavy, and the 7.5-vs-4.9 gap is
the proof. **Request:** a DRAM arbitration model where concurrent external
accesses from the active masters serialize and charge each other bus-occupancy
(you already have `bus.m68k_on_bus`; extend it to Tom↔Jerry↔Blitter↔OP), with the
cost surfaced in the existing `contention` / `mem_external` stall counters.

## Why it matters (and the payoff)

With these two, jsim's fps should converge on the ~4.9 the hardware shows, and —
critically — its **per-cause stall attribution would then reveal *which* master's
DRAM traffic dominates** (Tom staging vs Jerry vs OP scan-out). That turns the
framerate hunt from blind hardware-probing (each probe a ~3.4-min Skunkboard
flash, and confounded by geometry-dependent fill) into offline analysis. Right
now the emulator actively misleads: it says Tom's *compute* is the cost
(imul32 23%), but on silicon cutting Tom's compute does nothing — because the
real limiter is bus contention jsim can't see.

## Corroborating divergence (same root: memory-timing model is optimistic)

`LOADP`/`G_HIDATA` are treated as **zero-latency**. A kernel that does
`loadp (r1),Rd` then reads `G_HIDATA` two instructions later renders correctly in
jsim but **garbage on silicon** (stale coordinates — the load result hadn't
settled). Real JRISC loads have multi-cycle latency before the destination (and
`G_HIDATA`) are valid; jsim makes them instant. Worth modeling load-use latency
for `loadp`/`load` (and reflecting it in `stall_load`) alongside the above.

## Repro
```
# jsim predicts ~7.5 fps (read the on-screen fps bar, render row 28):
jagemu screenshot HWH_clean.cof --frames 2600 --fidelity silicon -o j.png
# hardware: ~4.9 fps on the same scene.
```

---

## Response (cobweb `7336d6a`) — modelled, hardware-calibrated, and one null result

Four separate pieces, all measured on a Skunkboard rather than estimated:

**Blitter DRAM cost** (`674767d`) — jsim modelled the Blitter as free. Now
charged its real bus time, calibrated against hardware probes
(`calib/probes.s`: `p_blitsm`/`p_blitbg`). Took OpenLara 7.50 -> 5.43 fps
against hardware's 4.9.

**OP scan-out contention** (`92a00bf`, probe `a5e5f68`) — the Object Processor
*does* steal Tom's bus: +11.1% measured, reproduced in jsim.

**Textured-span validation** (`d384217`) — added `p_blittex1`/`p_blittexq` to
check the fill cost in the XADDINC config you actually use, at two fractional
step rates. Silicon matches jsim within 1%, and there is **no source-phrase
coalescing** — `du=0.25` costs the same as `du=1.0`. See the reply on
`COBWEB_BUG_blitter_overcharged_vs_silicon.md`; the short version is that the
fill model is right and the 2.4x lives in the NOFILL delta, not the cost.

**Tom<->Jerry contention: null.** Measured, deliberately *not* modelled — 656
vs 656 ticks in mode B, twice, with DSPMARK proving the DSP was actually
running. See `COBWEB_GAP_tom_jerry_contention.md`.

The one thing this report asked for that stayed open — where the frame time
actually goes — is now answered, and it wasn't bus contention at all. It's a
bytewise framebuffer memcpy on the 68000: see the reply on
`COBWEB_REQ_68k_pc_histogram.md`.
