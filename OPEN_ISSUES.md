# OPEN ISSUES — consolidated 2026-07-27

Status audited from the tracking docs in this directory, not from memory.

---
## A. VISUAL BUGS (game side)

| # | issue | status | notes |
|---|---|---|---|
| A1 | **Lara's head: two notches / see-through** | **OPEN** | Faces are DROPPED (proved with a flat-tinted head: silicon 35 px interior gaps, jagemu 0). **Silicon-only.** Present with Jerry or 68k posing. Judge it against SNOW; the cave wall hides it. Blocked: every kernel guard is load-bearing, so it can't be bisected by disabling one, and jagemu can't reproduce it. See `LARA_HEAD_CAMPAIGN.md`. |
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
| B2 | `jump_refill` = 16.9% of Tom's cycles | **OPEN, partly retracted** | 3 ticks/taken branch ⇒ 17,987 branches/frame, ~3.35 per span. **Capturing it all is impossible**; realistic capture ≈ a third ≈ 5–6%. `JMPDIET` proved the span/chain path is NOT where they are (−0.086%, but −20 bytes). |
| B3 | `LEMITDIET` | **INERT while Lara is 68k-posed** | Measured EXACTLY 0.00% — it optimises `lara_finish`, which only runs under JERRYPOSE. |
| B4 | `M68DIET` | **QUARANTINED — convicted toxic** | −62% on silicon. Do not re-land without the discriminator kit. |
| B5 | `RUNBATCH` | **OPEN, weakest option** | +4% as built (not the −25% once projected) and has a racy walking-only silicon death. |
| B6 | Draw distance / far clip / sliver cull | **CLOSED — exhausted** | Measured bit-identical. Do not re-run. |
| B7 | Wall-texture quality (`TEXSCALE=4`) | **CLOSED — null** | Identical cycles/spans/blit to the digit. Saves 175 KB of ROM, not frame time. |

---
## C. COBWEB / TOOLCHAIN — **OPEN**

| # | item | why it matters |
|---|---|---|
| C1 | **`BUG_jagemu_runs_code_that_hangs_silicon`** (NEW) | **TOP BLOCKER.** Four kernel changes rendered fine in jagemu and black-screened silicon in one session. Asks: model JRISC divide-by-zero, add a GPU liveness watchdog, calibrate divide rounding. |
| C2 | `REQ_68k_pc_histogram` | `--pc-histogram` is **68k-only** — no GPU hot-spot attribution at all (blocks B2). |
| C3 | `GAP_tom_jerry_contention` | Bus-contention half still open. |
| C4 | `REQ_wall_clock_accounting` | |
| C5 | `REQ_pchistogram_warmup_start` | |
| C6 | `BUG_bus_oob_panic` | |
| C7 | `GAP_gamedrive_sd_bios` | |
| C8 | `REQ_srcshade_gourd_compute` | |
| C9 | `REQ_rectshade_and_calibration` | |

### Partly resolved
`GAP_bus_contention_and_blitter_fill_timing` (Blitter half fixed) ·
`ISSUE_jas_hazard_attribution` (blocking half fixed) ·
`GAP_jerrypose_fps_overprediction` · `REQ_frame_time_measurement`

### Resolved (10)
`BUG_blitter_overcharged_vs_silicon` · `BUG_gpu_restart` · `BUG_indexed_store` ·
`BUG_jopt_rejects_all_transforms` · `BUG_jopt_sinks_donor_across_branch_target` ·
`BUG_storep_loadp_byteorder` · `ISSUE_dsp_dcmd_unverified` · `REQ_jcc68k_adoption` ·
`REQ_jumprn_load_scoreboard_probe` · `REQ_mmult_silicon_probe`

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
