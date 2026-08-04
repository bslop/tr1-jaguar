# jagemu: pc-histogram needs a `--start <frame>` warmup — short profiles rank boot as steady-state

**Severity:** medium — the profiler is excellent and we're using it heavily; this
is a sharp-edge fix so its output isn't a trap.

**Env:** cobweb `f50c2ea` · `sim/crates/jagemu/src/main.rs` (`--pc-histogram`),
`crates/jag-core/src/debug.rs`

## Why

The `--pc-histogram` you added is the most useful profiling tool we have — it
re-anchored our whole campaign. But it accumulates from frame 0, so a short run
ranks one-time boot/level-load loops as if they were steady-state hotspots.

Concrete bite this session, on the OpenLara play kernel:

| window | top 68k cost | 68k awake |
|---|---|---|
| 620 frames | `__mulsi3` **24%**, `video_init+0xAE` hot | **63%** |
| 1800 frames | `__mulsi3` ~15%, same | **39%** |

`video_init+0xAE` is a one-time 76,800-iteration boot loop (320×240) — its
instruction count is *flat* (76801) across both windows, proving it's boot, not
per-frame. At 620 frames it fraudulently reads as ~2% of "steady state" and
inflates `__mulsi3` and the whole 68k-awake figure. We nearly pivoted the campaign
onto a boot artifact; only running longer and noticing the flat instr-count caught
it. (This is the same class as your own move.b-present-copy finding, which was
also a non-play-path artifact — short-window ranking amplifies both.)

## The ask

A `--start <frame>` flag on `run --pc-histogram` (you already have exactly this on
`screenshot`/`video`): run to `<start>` with the profiler DISARMED, then arm it and
accumulate for the remaining `--frames`. So:

```
jagemu run rom.cof --pc-histogram --map m.map --start 400 --frames 800
```

profiles a warmed 800-frame window with boot excluded. Cheap (the profiler already
has an arm/disarm point — it's a boolean gate on the sampler) and it makes short
profiles trustworthy without forcing multi-thousand-frame runs to amortize boot.

Bonus if easy: print the armed window `[start, start+frames)` in the header so the
provenance is unambiguous.
