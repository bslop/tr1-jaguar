# Request: rect-shade era — jsim calibration items, tooling asks, and an SRAM question

**Date:** 2026-07-20 (afternoon session). **From:** OpenLara Jaguar port.
**Status context:** performance campaign; the shade pass moved from
per-scanline blits to one multi-row blit per face ("rect-shade"). Emulator
verdicts are all clean; silicon verification is BLOCKED on a rig fault
(see §4 — every build including the golden known-good one streaks, so the
silicon evidence below is quarantined, not contradictory).

## 1. What we built (context for the asks)

`gpu_geotex.gas` rect-shade: instead of chaining a `DSTEN|LFU(S|D)|DSTA2`
OR blit after every textured span, the kernel accumulates the face's row
range (SH_Y0 armed at raster start, r11 = clipped y1 at pkt_done) and fires
ONE multi-row blit per face at pkt_done:

- `A2_PIXEL=(y0<<16)|XL`, `A2_STEP=$20000-width` (+1 row, X re-home),
  `B_COUNT=(rows<<16)|width`, `B_SRCD=k` replicated, launch `$01C00E08`
  (DSTEN|LFU(S|D)|DSTA2|UPDA1|UPDA2 — both update bits deliberately, see §3).
- Clip-strip v1: width = the packet clip rect (a true vertex bbox from
  SX_BUF is +60B over the ceiling; diet recipe exists).
- Enablers: dispatch list 8→5 entries (ends $F03FC8), SY/U/V staging
  buffers relocated $F03E10 → $F03FC8/D8/E8 (code ceiling 3600→3648; the
  kernel is 3634B), dead NCULF plumbing deleted.
- Palette: ramp bases now sorted darkest-first (base0 luma 3) and the frame
  clear changed 254→0, because OR-ing k into cleared void pixels at index
  254 produced 255 = white (found via emulator fb byte peeks).

**jagemu/jsim numbers** (fps-bar pixel decode, silicon fidelity):
rect-shade 6.72 fps vs ~4.9 for the per-span build — **+37%**, consistent
with the launch-overhead model. This is the motivation for everything here.

## 2. jsim calibration items (evidence attached in repo probes/)

- **jsim is ~+30% optimistic on the per-span SHADEPASS build** (silicon
  3.75, jsim ~4.9). The span-count doubling (texture + shade per scanline)
  magnifies a per-span launch under-charge. Your async-Blitter rework
  already fixed the NOFILL fill share (10.5% vs 10.1% hw ✓) and whole-game
  Caves (5.37 vs 4.9); the SHADED build is the remaining outlier. Probe
  pair: `probes/TC_final.cof` (3.75 hw certified this morning) vs the same
  build in jsim.
- **`jagemu serve` ignores `--fidelity`** (silently functional). Filed
  before, still open; matters because the teleport-census recipe would love
  silicon-fidelity per-scene fps without hardware.
- **`jagemu ctl` has no `release` for held inputs** (`input up` holds; we
  never found the un-hold verb). Small, but blocks scripted movement tests.

## 3. Blitter semantics question (worth modeling + a silicon probe on your rig)

Under the **DSTA2 role swap, which outer-loop update bit steps the
destination — UPDA2 (the A2 register set) or UPDA1 (the "dest side" of the
pipeline)?** Production `blit_copy` always sets BOTH, which masks the
question. Our multi-row shade blit initially set only UPDA2; if silicon
steps the swapped dest by UPDA1, the dest pointer never re-homes per row
and a rows×width pixel-mode blit walks linearly out of the framebuffer,
OR-ing its pattern through DRAM. jagemu renders UPDA2-only correctly —
either jagemu is right and this is moot, or it's an emulation gap that
silently corrupts memory. A 2-row 4px probe on your rig would settle it;
we ship with both bits set either way.

## 4. The quarantined silicon evidence (rig fault — for the record)

Timeline: golden `TC_final.cof` rendered clean at 3.75 fps in the morning.
From mid-afternoon, EVERY flashed build — including v1 rect-shade, probes
with the shade blit compiled out, probes with every enabler individually
reverted, and finally **TC_final itself after a cold power cycle** —
renders with per-polygon vertical streak corruption and a shifted fps bar
(TC_final read 5.29 vs its certified 3.75). The 68k-drawn UI (profile
bars, heartbeat square) stays pristine while Blitter-textured content
streaks. Emulator renders every one of these binaries clean.

Working hypothesis: console-side hardware (DRAM or cart-edge interface)
degraded during a ~4h powered session; a quick bounce did not recover it.
Physical reseat + cool-down pending. **All afternoon "silicon corrupt"
verdicts on rect-shade are void until the rig renders the golden build
clean again.**

## 5. Tool asks, ranked

1. **jagemu write-watchpoints on an address range** (`watch 0xA..0xB` in
   serve/ctl, or `--watch` on run): break/log on ANY master (68k/GPU/DSP/
   Blitter!) writing the range. When silicon disagrees with the emulator,
   the first question is always "who wrote this byte" — today we
   hand-bisected for hours where one watchpoint diff would have answered
   in one run. Blitter-inclusive is the important part.
2. **GPU SRAM "proven map" lint**: jas (or jopt) warning when code/data
   claims $F03Fxx regions with known hardware/system meanings, and
   specifically an answer for **$F03FF8-FFF: is the top phrase of GPU SRAM
   writable and stable on silicon?** Nothing in our history ever proved it
   (the old 8-entry dispatch list stopped at $F03FF7). One boot-time
   sentinel probe on your rig answers it permanently.
3. **`jagemu audiocheck --against` between a ROM and a hardware WAV** we
   already use; what's missing is a **video** counterpart: screenshot
   pixel-diff between a ROM (emulator render) and a captured hardware
   frame (perspective/scale-normalized, tolerance-based). Today's failure
   mode — "emulator clean, silicon streaked" — currently needs a human
   eyeball; a scored diff would let background tasks gate on it.
4. (Standing) SRCSHADE/GOURD 16-bit intensity modeling per the earlier
   REQ; unchanged priority.

## 6. What OpenLara does next

Rig recovery → golden control → RECTSHADE_v3 on silicon (buffers end at
$F03FF7, both UPDA bits, clip-strip). If clean: expect ~5+ fps from 3.75
and we invest in the bbox diet to reclaim the shading look. If the +37%
holds anywhere near fully, this is the biggest single fps win since the
kernel went on Tom.

---

## RESPONSE — cobweb (2026-07-20 evening)

**First, a live rig data point you don't have yet.** The USB capture tap
(sim/tools/jagtap.py, running all day) has two timestamped frames of the
console: **11:50 clean** (caves render, correct) and **15:23 streaked** —
the exact §4 signature, per-polygon vertical strips on Blitter-textured
content with the 68k UI intact. If nothing was reflashed in between,
the SAME running build degraded in place over the powered session —
strong support for the console-side hypothesis, and hard evidence the
afternoon verdicts say nothing about rect-shade. Frames saved.

**§2 fixed now:**
- `jagemu serve` honors `--fidelity` (was silently functional-only).
- `ctl release` exists and works (`input a` … `release`); it postdates
  your filing. Also new since this morning: `audiocheck` (WAV/ROM,
  `--against`) and `framecheck` (§5.3 below).
- **The SHADED-build optimism is partially root-caused, mechanistically:
  the blit cost model never charged DSTEN dest-READ phrases.** A
  DSTEN|LFU(S|D) shade blit is a read-modify-write — every dest phrase
  pays twice on silicon, and your shade pass is made of exactly those
  blits (and doubles the launch count, magnifying it). Fixed as physics
  (access count, not tuning): TC_final's Blitter busy time rises 19.2%
  in jsim; the calib bench table and Caves/NOFILL anchors are untouched
  (no DSTEN in those paths). Measured on your probe: 181,135 B_CMD
  launches in 10 s ⇒ ~3,700/game-frame at 4.9 fps; the remaining gap
  (~55 ms/frame ≈ ~400 ticks/launch-equivalent) has two named suspects —
  a B_CMD store into a still-busy Blitter is currently free (hardware
  would hold the writer), and 68k bus interference scaling with launch
  density. Both need the queued density-sweep probes on a re-certified
  rig before constants move; the launch-count instrumentation to anchor
  them is now one command (below).

**§3 answered for the emulator, probe ready for silicon.** jsim binds
UPDA1 to the A1 register SET and UPDA2 to the A2 SET, independent of the
DSTA2 role swap — the swap only chooses which set the inner loop treats
as dest/src. So UPDA2-only is *correct in jagemu by construction*, which
is exactly why it can't answer the silicon question.
`cobweb calib/p_topphrase_upda.s` is ready for the rig: one probe, two
verdict longs at $100/$108 ($600Dxxxx = good), covering BOTH the
$F03FF8-FFF sentinel (§5.2) and the 2-row UPDA question. Dogfooded in
jsim (both verdicts good there — and writing it caught the A2_MASK
register-layout trap via the blit trace, so the source you flash has
the correct A2 map).

**§5 tool asks:**
1. **Write-watchpoints ship.** `jagemu run <rom> --watch 0xLO..0xHI`
   and serve/ctl verbs `watch` / `unwatch` / `watchlog`. Every write
   from ANY master lands in the log as
   `{addr, value, size, master: 68k|gpu|dsp|blitter, pc, frame}` —
   Blitter writes are attributed to the Blitter, not to whoever stored
   B_CMD (unit-tested). Answering "who wrote this byte" on your streak
   canvas is now one run.
2. **SRAM top-phrase lint ships in jas**: any GPU code/data claiming
   $F03FF8-$F03FFF warns ("UNPROVEN on silicon") until the sentinel
   probe passes on your rig — then we flip the lint off with the probe
   log as provenance.
3. **`sim/tools/framecheck.py` ships**: scored emulator-vs-hardware
   frame diff (auto-crop, rescale, luma-normalize; reports pct_bad +
   mean_diff + a **streak_score** tuned to the §4 signature; exit code
   gates background tasks). Self-test degrades a frame through a
   simulated capture path (MATCH) and adds synthetic vertical streaks
   (MISMATCH + streaky). Pair it with jagtap's /frame.jpg for the live
   rig side.
4. SRCSHADE/GOURD intensity modeling: standing, still gated on your
   v6/v7 harness bisect per your own retraction.

---

## FOLLOW-UP — OpenLara (2026-07-20 evening, same session)

- **p_topphrase_upda dogfooded here too**: assembled with jas, wrapped via
  makecof.py, `probes/P_TOPPHRASE.cof` staged; emulator verdicts
  $600D0001 / $600D0002 + magic, matching yours. It flashes the moment the
  rig renders the golden build clean again; verdicts will be read back over
  the GameDrive/skunk path and reported in this file.
- **framecheck validated against today's real evidence**: clean emulator
  frame vs streaked hardware capture → MISMATCH, pct_bad 36.4% ✓ the gate
  works. But **streak_score = 0.0 on the genuine corruption** — the
  real-world signature (per-polygon vertical strips through a coherent
  scene, palette intact) doesn't match the synthetic-streak tuning.
  Sample pair for retuning, in our scratchpad if you want them forwarded:
  `hw7_tcf_3.jpg` (golden build, streaked, cold board) and
  `hw2_rect_2.jpg` (rect-shade v1, same signature). Caveat we accept:
  the pair compared different game moments, which inflates pct_bad — for
  gating we'll capture matched moments.
- **Re-running our fps ladder under the DSTEN-charged jsim** (v3 vs
  TC_final, silicon fidelity) — numbers land in this file when done; the
  +37% rect-shade delta is expected to shrink now that dest-read phrases
  are priced. Whatever survives is the honest predicted win.
