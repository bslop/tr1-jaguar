# OPEN ISSUES — consolidated 2026-07-27

Status audited from the tracking docs in this directory, not from memory.

---
## A. VISUAL BUGS (game side)

| # | issue | status | notes |
|---|---|---|---|
| A1 | **Lara's head: two notches / see-through** | **OPEN** | Faces are DROPPED (proved with a flat-tinted head: silicon 35 px interior gaps, jagemu 0). **Silicon-only.** Present with Jerry or 68k posing. Judge it against SNOW; the cave wall hides it. **The divide-rounding hypothesis is REFUTED** (silicon 2026-07-27: Tom TRUNCATES, both integer and 16.16 — jsim is bit-faithful, so the two dividers cannot be disagreeing about which sub-pixel faces survive). Your own caveat was why it could not hold: the 9.4-of-85 cull-sign figure was integer-vs-exact, not silicon-vs-jagemu. Remaining candidates are hazards, not arithmetic — bug 25 (DIV re-issued while busy, `stall_div_busy`) and bug 13 (WAW, `waw_hazards`), both now attributable to a **specific PC** via `--pc-histogram --core gpu`. That is a read-only instrument, so it works even though every guard is load-bearing and cannot be disabled to bisect. See `LARA_HEAD_CAMPAIGN.md`. |
| A2 | **Floating shaded poly chunks** "rising up into the player's view" | **OPEN, not investigated** | User-reported 2026-07-26. Awaiting a capture. First things to check: `SHADEPASS` paints a RECT shade over each face's bbox rows (`SH_Y0` armed at the cull, `pkt_done` shades y0..y1) — a bbox armed against the wrong face would paint exactly this shape. |
| A3 | **Grey outline around Lara's model** | **OPEN, not investigated** | User-reported 2026-07-26. First check: `inset_uv()` pulls every face corner 1 texel toward the UV centroid (anti-bleed); atlas cell borders are the other candidate. |
| A4 | **Texture streaking** | **OPEN** | `UVNEG` fixes a real unsigned-pack bug at the span start and is correct + free, but measured only 0.0068% of pixels on a walking strip — **does not close this**. Default OFF. |
| A5 | **Black polygon at the cave mouth** | **OPEN** | Separate from A1. World-anchored, first ~2 frames of a forward walk. Hopcap and shade-pass both eliminated by measurement; remaining suspect is a face sampling black atlas texels (extractor UV issue). |
| A6 | **Passport renders wrong** (pose/scale/position) on the title screen | **OPEN** | Visible in every title capture this session. |
| A7 | Mansion renders nearly black under `GYMTEST` | **UNVERIFIED** | May be an artifact of the test hook (`goto menu_done` skips the menu). Not asserted as a bug. |
| A8 | Loading screen (DIVY aspect + progress bar) | **AWAITING REVIEW** | Built and shipping in the default build (commit 3828a93). User asked to review before switching; it is currently ON. |

---
## B. PERFORMANCE

| # | issue | status | notes |
|---|---|---|---|
| B1 | **Next vsync rung needs −12.5% frame time** | **OPEN** | Sitting on the 8-field rung (7.50 fps); 7 fields = 8.57. Anything smaller is banked and invisible. |
| B2 | `jump_refill` = 16.9% of Tom's cycles | **RETRACTED 2026-07-27** | Silicon says refill is **correctly priced**: gap flat across 0/16/48 taken branches, per-branch cost silicon 0.79 vs jsim 0.81 (`facennb1`/`facennb3`). The 17,987 branches/frame are real and they cost what jsim says. `JMPDIET`'s -0.086% was not a measurement failure — there was nothing to win. Do not spend more on branch-shaving. |
| B3 | `LEMITDIET` | **INERT while Lara is 68k-posed** | Measured EXACTLY 0.00% — it optimises `lara_finish`, which only runs under JERRYPOSE. |
| B4 | `M68DIET` | **QUARANTINED — convicted toxic** | −62% on silicon. Do not re-land without the discriminator kit. |
| B5 | `RUNBATCH` | **OPEN, weakest option** | +4% as built (not the −25% once projected) and has a racy walking-only silicon death. |
| B6 | Draw distance / far clip / sliver cull | **CLOSED — exhausted** | Measured bit-identical. Do not re-run. |
| B7 | Wall-texture quality (`TEXSCALE=4`) | **CLOSED — null** | Identical cycles/spans/blit to the digit. Saves 175 KB of ROM, not frame time. |

---
## C. COBWEB / TOOLCHAIN

**Audited against cobweb `84d9fc2` on 2026-07-27.** Most of this section was
stale: five of the nine items were already fixed, and C2 — listed as blocking
B2 — had shipped. Statuses below are verified against the code, not the docs.

| # | item | status |
|---|---|---|
| C1 | `BUG_jagemu_runs_code_that_hangs_silicon` | **RESOLVED (all four asks).** ① divide-by-zero is now COUNTED (`gpu.timing.div_by_zero` + unconditional stderr warning) — deliberately not *modelled*, since silicon's actual behaviour is unmeasured and guessing would make jsim confidently wrong instead of merely silent. ② `--watchdog N` warns when a core runs N frames without clearing RISCGO (frame-anchored, so a resident DSP does not trip it every run). ③ divide ROUNDING measured on silicon: **Tom TRUNCATES**, both integer and 16.16 — jsim is bit-faithful. ④ GPU PC histogram shipped. **Found immediately: `probes/RP_jpose.cof` does 888 GPU divide-by-zeros in 120 frames.** |
| C2 | `REQ_68k_pc_histogram` | **RESOLVED.** `--pc-histogram --core 68k\|gpu\|dsp\|all`, exact per-PC cycles with stall categories sliced per instruction; `jas --map` for symbols; `--prof-json` + `sim/tools/profdiff.py` to diff two runs. **Unblocks B2** — and see the B2 note below, because the answer changed. |
| C3 | `GAP_tom_jerry_contention` | **RESOLVED — measured null both directions.** Reads were already null; the write side (`p_dsphammerw`, witness-confirmed) is 655 vs 656 ticks, +0.15%. jsim charging zero is correct. |
| C4 | `REQ_wall_clock_accounting` | **RESOLVED.** Per-master wall clock, plus a per-core breakdown that partitions to exactly 100%. |
| C5 | `REQ_pchistogram_warmup_start` | **RESOLVED** — `--start`. |
| C6 | `BUG_bus_oob_panic` | **RESOLVED** — no panic, covered by `bus.rs` `oob_tests`. The requested `oob_access` counter was never added; still a fair ask. |
| C7 | `GAP_gamedrive_sd_bios` | **OPEN** (jsim has a GameDrive BIOS block with trap thunks; the specific gap is yours to re-check). |
| C8 | `REQ_srcshade_gourd_compute` | **OPEN — now the top toolchain item.** `tom/blit.rs` takes the GOURD lane from a static `B_PATD`; `BC_SRCSHADE` is defined but referenced nowhere, so a shaded surface renders at one flat intensity. With CRY16 fixed (below) the base hue is right but the gradient still is not — this is the last thing between you and judging a shaded frame offline. |
| C9 | `REQ_rectshade_and_calibration` | **PARTLY RESOLVED** — answered 2026-07-20; re-check what remains. |

### Also fixed since this list was written (not previously tracked here)
- **CRY16 scan-out decoded to the wrong colours.** jsim indexed the chroma byte
  transposed and scaled by `/255` instead of `>>8`. Both fixed; the four
  silicon-captured face colours from bubsy3d are now jsim regression tests.
  Any CRY screenshot you took before `8e78a1d` was wrong — and CRY is not
  niche, the Blitter's Gouraud path is CRY-only on silicon.
- **`stall_load` / `stall_alu` were OVERSTATED** in `gpu.timing` on load-heavy
  kernels: an instruction reading two in-flight registers stalls once, for the
  longer wait, but both waits were being counted. Any conclusion drawn from
  those counters is worth re-checking. Charged cost was always the max, so no
  timing or fps moved.

### Read this before acting on B2 or any jsim timing number
Three timing terms were measured on silicon 2026-07-27, and **two are errors of
opposite sign that cancel**:

- **mixed independent/dependent ALU streams: UNDER-charged** (marginal 1.53x).
  Invisible to pure-pattern probes — `addind` 1.01 and `adddep` 2.00 are both
  exact — because it only exists in the interleave, which is what real code is.
- **DRAM loads: OVER-charged** ~10% quiet-bus, and much more with the 68k
  active, where jsim applies a contention tax to stores, page-miss loads and
  loads-under-a-blit that silicon does not apply at all.
- **`jump_refill`: CORRECT.** Flat across 0/16/48 taken branches, per-branch
  cost silicon 0.79 vs jsim 0.81.

So **B2 is retracted, not just "partly"**: `jump_refill` at 16.9% of Tom's
cycles is real and correctly priced. Shaving branches on the theory that jsim
over-counts them would be wasted effort. Meanwhile jsim's whole-program fps
looks close (~11% fast) *because* the ALU and load errors cancel — which is
exactly why per-cause attribution has misled this campaign. Trust the totals;
distrust any single counter until the paired fix lands.

The paired fix is not applied: confirming the net needs the whole-program
ladder (TC/v4b/NOFILL) re-measured on silicon, which is an OpenLara flash
session with video capture, not a calib run.

### Partly resolved
`GAP_bus_contention_and_blitter_fill_timing` (Blitter half fixed; contention
half measured null and closed) · `ISSUE_jas_hazard_attribution` (blocking half
fixed) · `REQ_frame_time_measurement`

### Resolved (18)
`BUG_blitter_overcharged_vs_silicon` · `BUG_bus_oob_panic` ·
`BUG_cry16_decode` · `BUG_gpu_restart` · `BUG_indexed_store` ·
`BUG_jagemu_runs_code_that_hangs_silicon` · `BUG_jopt_rejects_all_transforms` ·
`BUG_jopt_sinks_donor_across_branch_target` · `BUG_storep_loadp_byteorder` ·
`GAP_jerrypose_fps_overprediction` · `GAP_tom_jerry_contention` ·
`ISSUE_dsp_dcmd_unverified` · `REQ_68k_pc_histogram` · `REQ_jcc68k_adoption` ·
`REQ_jumprn_load_scoreboard_probe` · `REQ_mmult_silicon_probe` ·
`REQ_pchistogram_warmup_start` · `REQ_wall_clock_accounting`

---
## D. FIXED THIS SESSION
- **Lara's head TWITCH** — Jerry's `LOOP_COUNT` was allocated on top of her head's
  Y rotation angle (`$F1C32C` = angle long 43 = mesh 14). Relocated. Head
  instability 1.086 → **0.000**, frame rate kept (−4.2% vs −27.8%). Commit cee6b08.
- **The LOWRES flag omission** that cost ~8 → 5 fps (build-recipe error, not code).

## E. PROCESS RULES EARNED THE HARD WAY
- **`make FOO=0` turns FOO ON.** Express "off" by OMITTING the variable; md5 both
  ROMs before believing any A/B.
- **`LOWRES` has no Makefile default** — omitting it silently drops the +25% win.
- **Build from the last known-good ROM in `probes/`, never from a flag list
  reassembled out of prose.**
- **Every guard in `gpu_geotex.gas` is load-bearing on silicon** — do not disable
  one to test a hypothesis.
- **Reproduce Lara bugs with an IDLE screenshot**, not a walking strip.
- `cp mrt_*.bin` **misses `mrt.bin`** — back up `mrt.bin`, `gym*`, pass/photo/
  font/sfx/music too (27 files).
