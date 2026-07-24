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
