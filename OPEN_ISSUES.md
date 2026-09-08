# OPEN ISSUES — consolidated 2026-07-27

Status audited from the tracking docs in this directory, not from memory.

---
## A. VISUAL BUGS (game side)

| # | issue | status | notes |
|---|---|---|---|
| A1a | ~~Whole-face flicker / racing pixels~~ | **✅ CLOSED 2026-08-26 by `VCDRAIN=1`** | A GAME-SIDE kernel drain, now in `build_cof.sh` — **not** a cobweb fix; the cobweb fix that same session was A10, which is an easy conflation because both landed on 2026-08-26. jagq #1998, 38-room tour, rooms paired by CONTENT (`tools/tour_pair.py`): racing px **9797 → 6219 (−37%)**, the whole-wall face in PSX room 20 **2698 → 12**, rooms 3/10 832→93 and 484→35. Mechanism: Tom's face loop read back vertex-cache words the self-transform pre-pass had just stored (jsim: 118-cycle minimum gap) and silicon returned the STALE word — one wrong vertex toggles a whole face. Only rooms dispatching >8 rooms (Jerry's cache cap) self-transform, which is why it looked room-specific. ✅ The suspected "second mechanism" in PSX room 32 was a WOLF walking through the patch (luma constant with `ENEMIES` off), worth ~4300 of the remainder. ⇒ ~1900 racing px level-wide; **the whole-face class is gone and no room stands out.** |
| A1b | **Lara's head: two notches / see-through** | **OPEN — this is the ORIGINAL A1, and it is NOT what VCDRAIN fixed.** Front faces wrongly DROPPED from her crown; silicon-only (jagemu renders the same ROM's head solid). ☠ THE EVIDENCE CELL TO THE RIGHT IS FROM 2026-07-27, **BEFORE** VCDRAIN — its "next test = PIPELINE race" plan predates that fix, so do not re-run it as written. ⭐ CURRENT LEAD (2026-09-06): **`DIVSAFE=1`** fixes five DIVIDE-SHADOW hazards `jas` reports on every build, three of them the per-span U/V gradient and edge-walk divides — a divide's late write can discard a `neg` of its own destination, flipping a negative slope positive on some faces and not others. That is the drop/misplace class, and it is invisible in emulation. Built, ROM byte-identical when off, silicon-verified FREE — but **not** yet shown to touch the notches. ☠☠ A VALID TEST NEEDS ALL THREE: (1) the crown against a genuinely BRIGHT backdrop (<10% of the band below luma 40 — the level-start spawn rotated 180° still measures 49%); (2) a SILHOUETTE / convex-deficiency metric, because a notch is a boundary concavity — three metrics have already read ~0 regardless of the truth, and a whole-frame pixel diff (noise floor 585..1182 px) is a fourth; (3) both arms in ONE rig turn. Build (1) and (2) OFFLINE first: jagemu cannot give the verdict but validates the composition and the plumbing free. | ☠️ **THE CULL-SIGN THEORY IS DEAD IN BOTH DIRECTIONS.** `TINYCULL` (drop coin-flip faces) was already null; `TINYKEEP=4` (the inverse — KEEP faces whose \|ar\| is inside the rounding noise, added this session, 0 bytes) was flashed and the user reports the crown **unchanged**. That was the pre-declared kill criterion. Do not spend further flashes on sub-pixel cull-sign ideas. **NEW LEAD, not yet tested: the PIPELINE race.** `Makefile:488` — *"PIPELINE: frame N's last Tom batch overlaps frame N+1's 68k logic."* If Tom is still rasterising Lara while the 68k re-poses her for the next frame, a face built from mixed old/new vertices lands in the WRONG PLACE and is ABSENT from the right one — which would make **A1 and A2 the same bug** (notch here, floating chunk there), and would be timing-dependent, i.e. silicon-only and invisible to jagemu. That matches every constraint. ☠️ **BUT BOTH BISECT KNOBS ARE DEAD ON SILICON — the race is UNTESTED, not refuted.** `PLAY_NOPIPE` (ship flags minus `PIPELINE`, `-DPIPELINE` verified absent): **black, 55 s, 0.00% non-black** — and it contains **NO kernel change**, which breaks the "gpu_geotex is at a fragility cliff" framing that explained the earlier four black screens. `PLAY_PIPESTAGE0` (`main.c:4053`'s own documented mechanism test, "0 = collect immediately, null overlap window"): **green on the first flash, black on a clean re-flash — both dead, 50 s.** The re-flash was run specifically to tell a flaky RAM load from a broken build; it settled it. Both render **99.0% non-black in jagemu**, making these cases 5 and 6 of the emulator-passes/silicon-dies class. Structurally the two C paths are symmetric (same `gpu_sync`+`video_flip` pair, moved from loop-top to loop-bottom), so nothing in the source explains it — read as **bit-rot in paths nobody has built in a long time**. **Next: repair `PIPESTAGE=0` offline in C until it boots, THEN test the race.** Do not spend more flashes on these knobs as-is. ⚠️ Rig note: one flash aborted at 2 s with "can't connect with skunkboard" and wedged the board (needed a physical power cycle); it re-enumerated 052→055→056. Not a jcp timeout — the link itself is flaky. **DIVZGUARD FLASHED AND REFUTED:** **Silicon verdict: the notches are STILL THERE with the zero-divide eliminated** (user, direct observation, `PLAY_DIVZGUARD.cof` running, her idle on snow). So the divide-by-zero is NOT A1's cause. The guard is still correct and still ships — it removes 104,642 undefined-behaviour events per 1500 frames for 6 bytes at zero cycle cost — it is just not this bug. ⚠️ **AND MY MEASUREMENT WAS INVALID:** I scored "interior gaps" with a fill-holes test, which by construction only counts background **fully enclosed** by hair. The reported artifact is a **bite out of the OUTLINE** of her skull, which is a boundary concavity and can never be an enclosed hole — so the metric read 0.06 px whatever the truth was. Any future A1 metric must be a **silhouette/convex-deficiency** measure, not a hole count. (Third metric failure on this bug: cream-threshold, neck-confound, now this.) **I have still never seen the notches in a capture I took myself** — next step is a reference image from the user showing them clearly, before any more theories. Background detail below is unchanged and still valid: **the shipping kernel divides by zero ~45x per frame, on purpose.** `--pc-histogram --core gpu` says `waw_hazards`=0 and `stall_div_busy`=0, so BOTH remaining hazard candidates are dead — but `div_by_zero`=104,642 per 1500 frames. Attributed to **one site**: `sp_dvp`, the per-span `du`/`dv` divide by `denom = xr_raw - xl_raw`, which is 0 on every single-column span. The kernel comments this as deliberate ("garbage du/dv, which one-pixel spans never consume") — but the quotient **is** consumed: it feeds `A1_INC` and, on left-clamped spans, `u0 = uL + du*off`. The other four divide sites are all guarded (`NEAR=64` before `vcf9`/`vcf10`; `dy<0`/`dy==0` before `gw_div` and the chain divides), which is why `DIVZGUARD=1` takes the count to **exactly 0**. Fix = force `denom` to 1: **6 bytes** (3532→3538, 142 spare), jas-clean, cycles −428 per 1500 frames (noise), and **pixel-identical in jagemu** — as expected, since jsim's zero-divide returns a benign `$FFFFFFFF` and every affected span is 1 px wide. **Silicon is the only oracle.** Staged: `probes/PLAY_DIVZGUARD.cof`; its guard-off control is byte-identical to `PLAY_JERRYFIX.cof`. Faces are DROPPED (proved with a flat-tinted head: silicon 35 px interior gaps, jagemu 0). **Silicon-only.** Present with Jerry or 68k posing. Judge it against SNOW; the cave wall hides it. **The divide-rounding hypothesis is REFUTED** (silicon 2026-07-27: Tom TRUNCATES, both integer and 16.16 — jsim is bit-faithful, so the two dividers cannot be disagreeing about which sub-pixel faces survive). Your own caveat was why it could not hold: the 9.4-of-85 cull-sign figure was integer-vs-exact, not silicon-vs-jagemu. Remaining candidates are hazards, not arithmetic — bug 25 (DIV re-issued while busy, `stall_div_busy`) and bug 13 (WAW, `waw_hazards`), both now attributable to a **specific PC** via `--pc-histogram --core gpu`. That is a read-only instrument, so it works even though every guard is load-bearing and cannot be disabled to bisect. See `LARA_HEAD_CAMPAIGN.md`. |
| A2 | **Floating shaded poly chunks** "rising up into the player's view" | **OPEN, not investigated** | User-reported 2026-07-26. Awaiting a capture. First things to check: `SHADEPASS` paints a RECT shade over each face's bbox rows (`SH_Y0` armed at the cull, `pkt_done` shades y0..y1) — a bbox armed against the wrong face would paint exactly this shape. |
| A3 | **Grey outline around Lara's model** | **OPEN, not investigated** | User-reported 2026-07-26. First check: `inset_uv()` pulls every face corner 1 texel toward the UV centroid (anti-bleed); atlas cell borders are the other candidate. |
| A4 | **Texture streaking** | **OPEN** | `UVNEG` fixes a real unsigned-pack bug at the span start and is correct + free, but measured only 0.0068% of pixels on a walking strip — **does not close this**. Default OFF. |
| A5 | **Black polygon at the cave mouth** | **OPEN** | Separate from A1. World-anchored, first ~2 frames of a forward walk. Hopcap and shade-pass both eliminated by measurement; remaining suspect is a face sampling black atlas texels (extractor UV issue). |
| A6 | **Passport renders wrong** (pose/scale/position) on the title screen | **OPEN** | Visible in every title capture this session. |
| A7 | Mansion renders nearly black under `GYMTEST` | **UNVERIFIED** | May be an artifact of the test hook (`goto menu_done` skips the menu). Not asserted as a bug. |
| A8 | Loading screen (DIVY aspect + progress bar) | **AWAITING REVIEW** | Built and shipping in the default build (commit 3828a93). User asked to review before switching; it is currently ON. |

| A10 | ~~A build can black-screen silicon from t=0~~ | **✅ RESOLVED 2026-08-26 — IT WAS THE TOOLCHAIN, and it is fixed IN THE COMPILER.** Two bugs, both closed. **(1) `jas` dropped the DESTINATION relocation of a memory-to-memory `move.l sym,sym2`**, so every global-to-global copy in a jcc68k TU stored into the 68000 exception vectors — 24 sites in video.c alone, black on every pad. Fixed in cobweb `ba9c680`; the build pins `COBWEB_REV=9da2f99`, which contains it. **(2) The OP fetches a SCALED object as one 32-BYTE burst**, so a 16-mod-32 `op_list` poisoned the display; `op_list` now sits under `ALIGN(32)` in `jaguar.ld:47` (jas drops a bare `.align 32`). ✅ CONFIRMED ON SILICON (#1988), and demo31/demo33 boot on ANY pad. ☠ **PADTEXT IS NO LONGER A BOOT LOTTERY** — it was never size or layout; do not roll pads for a black screen without first checking `op.scaled_misaligned_hits` and scanning for opcode `23f9` with a destination below `$4000`. Current ROM: **0** and **0 mod 32**. ★ The transferable lesson: BISECT THE TOOLCHAIN when every commit fails. A semantics-preserving register shuffle (`-frename-registers`, zero source change) was sufficient to trigger it, which is what finally pointed away from the source. The investigation below is kept because it is the useful part — but everything in it that says "OPEN" or proposes a mitigation is superseded by this row. |
| A9 | ~~`front_fb` is written by the vblank ISR but is NOT `volatile`~~ | **✅ RESOLVED 2026-09-06 — and the fix is FREE.** `video.c` now declares it `volatile`, joining its own immediate neighbours `frame_count` (:145) and `pending_fb` (:149), both of which were already volatile — this one was simply missed. ★ **The rebuilt ROM is BYTE-IDENTICAL to the pre-fix build**, which proves the diagnosis rather than merely fixing it: jcc68k was already keeping the variable in memory, which is exactly why the latent bug never bit. Zero cost, zero behaviour change, risk removed. The sharp site was `video_two_buf()`: `while (pending_fb);` spins on a volatile and the next line read the NON-volatile `front_fb`, so a compiler was free to hoist that read above the spin and use the value from before the ISR update the spin exists to wait for. ⭐ **This was also a CONSTRAINT ON THE TOOLCHAIN, which is the real reason to fix it:** any improvement to jcc68k register allocation would have broken this project. The source is now correct independent of codegen, so cobweb is free to get better — which is what makes it more useful to the other ports. |
| A11 | **Lara's Home: a solid WHITE BAR across the top of the picture, and Lara stands in an ARMS-OUT bind pose** | **OPEN — user-reported 2026-09-07 while playing demo34 (job #2783), confirmed by a live grab off the capture card in the same minute** | Evidence frame (game-derived, deliberately NOT in this repo): `/home/jvilla/Documents/Git/_openlara_evidence/A11_larashome_2026-09-07_job2783.png`. TWO defects in one frame, and they should not be assumed to share a cause. **(1) The white bar** spans roughly the middle third of the top edge and is solid, unshaded white — the shape of a blit that ran with the wrong width or the wrong destination row, not of a texture error. ⭐ First place to look is the HRESN split: `RENDER_W` is still the 320-byte row STRIDE while `VIEW_W` is 160 rendered columns, and every HUD/text writer has to clip to `VIEW_W` while indexing by `RENDER_W`. `menu_text` was given exactly that split (`CW` clip vs `W` stride) but the health/air BAR is a separate writer and was NOT audited when 160x120 landed — a bar laid out for 320 columns would run off the visible band and paint the row above. ☠ Do not assume it is the same bug as the in-game menu (A6-adjacent) until the bar's own writer has been read. **(2) The arms-out pose** is a bind/T pose, i.e. no animation matrix applied — the same *signature* as the closed `g_handm` bug (JERRY posed Lara and never wrote the matrix, so the arms drew from a zero matrix), so check whether Lara's Home takes a path that skips the pose write rather than assuming the model is wrong. ⚠ This is the GYM level, which is the least-travelled code path in the build and the one `GYMTEST`/A7 already flags. |
| A12 | **Audio pops and STOPS when changing selection in the main menu** | **OPEN — user-reported twice: as "a skip in the music when moving between menu options", and again 2026-09-07 on demo34 as "pops and stops", which is worse than a skip** | Reported against two different builds, so it is not a one-build regression. ⭐ The mechanism to check first is a bus/starvation one, not a mixer one: `MUSIC.PCM` **streams off the SD card** while the ring renderer redraws over a TRIPLE-buffered display, and changing the selected item is the moment that does the most work per frame — the item model changes and the dirty rect covers the ring. If the redraw (or an SD read triggered by it) starves the DSP's sample feed, the symptom is exactly a pop followed by a gap. That predicts the defect scales with how much the selection change redraws, which is testable offline. ☠ "Pops AND stops" is two symptoms: a discontinuity (a sample-buffer underrun or a pointer jump) and a dropout (the feed not being refilled at all). A fix that removes one and not the other is not a fix — say which of the two any measurement is about. |
| A13 | **Loud STATIC burst when a front-end clip is skipped**, then the level's loading screen sits with no progress | **OPEN — user-reported 2026-09-07 while playing on the rig.** The static is the real bug and is independent of anything this session changed. ⚠ The stalled loading screen in the SAME report is NOT this bug and is not a game defect at all: the board was running the `LEVSD=1` streaming test build whose `OLCAVES.LEV` push had FAILED (`LIBUSB_ERROR_TIMEOUT`, job #2806), so the Caves data genuinely was not on the card and `lev_load()` took its deliberate park rather than dereference the null stubs. Two symptoms, one report, entirely different causes — do not chase them together. | ☠️☠️ **THE MECHANISM RECORDED HERE FIRST WAS BACKWARDS — kept because the refutation is the useful part.** I wrote that the skip probably exits the playback loop *without* stopping voice 0, leaving a live voice on a ring nobody refills. Reading the code killed it: the skip's `break` (`main.c:6399`) leaves the loop and the end-of-clip flush **runs anyway**, because that flush sits AFTER the loop. The bug is that it RUNS, not that it is skipped. The flush drains the ring and says why — *"blocking is fine here, the clip is over"* — which is true when the clip ENDED (ring nearly empty, plays out a fragment) and false on a SKIP, where the ring is FULL: six 4KB slots read ahead, **~2.2 seconds of audio**, dumped into voice 0 as fast as the DSP accepts it. ✅ **FIXED 2026-09-08**: a skip discards instead of draining — un-arm the queued slot first (the DSP can promote it between two writes), CNT=0 to silence voice 0, then `jerry_audio_stale()` so no reader re-arms what was just killed. Natural end untouched. ⭐ Only a human could have found it: ending a clip and skipping one are different exits, the rig has no hands, and every automated capture ever taken exercised the graceful one. ⬜ Superseded detail below.

⭐ ORIGINAL (REFUTED) REASONING, and the code already states the law it breaks. The clip decoder feeds DSP **voice 0** from a 6-slot 4 KB ring (`main.c:5882-5911`) and carries this note: *"a queue NEVER restarts an idle voice (music-engine law): after any stall > the DSP's ~744ms of buffered audio (clip transitions!), the voice dies and stays dead - RE-ARM when V0_CNT reads 0"*. The skip is *"any fire button skips the current clip"* (`main.c:5473`). So the first thing to read is whether the skip path breaks out of the playback loop **without stopping voice 0** — a live voice left pointed at a ring buffer that is no longer being refilled will keep playing whatever bytes are in it, and stale PCM at full volume is exactly a loud static burst. ☠️ Note the asymmetry that makes this easy to miss in testing: letting a clip END is a graceful path that runs the transition code; SKIPPING it is a different exit, and only one of the two was ever exercised by the automated rig captures, which have no hands to press skip. ★ A fix must silence/halt the voice on the skip exit specifically, then let the normal re-arm law bring it back — not merely re-arm, which is what the code already does and is not enough when the buffer beneath it is stale. |

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
  silicon-captured face colours from a peer project are now jsim regression tests.
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
## C2. ☠☠ INBOX FROM `jaguar-shared` (2026-09-04) — the kernel uploader drops the last instruction of 6 of 12 blobs

⚠ **Filed by the shared-brain session because no session was running this project.
Read it before the next kernel edit — it is currently harmless for a reason
nobody chose.**

`gpu_upload()`-style loops here size the kernel `(end - start) / 4` and copy
**32-bit words**. ☠ **JRISC instructions are 2 bytes**, so any blob whose byte
length is `≡ 2 (mod 4)` loses its **final instruction** — and adding or removing
one instruction anywhere flips it. It assembles, links, uploads and runs.

**Five call sites**: `gpu.c:64-65`, `gpu.c:112`, `gpu.c:261`, `jerry.c:31`,
`jerry.c:258`, `main.c:4398`.

☠ **And every `.balign 4` in `gpu_blob.S` / `dsp_blob.S` sits BEFORE the
`.incbin`** — that aligns the blob's *start* and does nothing for its *length*.
`gpu_kernel_end:` follows the blob immediately, so `end - start` is the raw byte
count.

| blob | bytes | `% 4` |
|---|---|---|
| `gpu_spanfill.bin` | 186 | ☠ **2** |
| `gpu_jvdec.bin` (boot FMV) | 386 | ☠ **2** |
| `gpu_geomwalk.bin` | 1034 | ☠ **2** |
| `gpu_textured.bin` | 1858 | ☠ **2** |
| `gpu_bltex.bin` | 2042 | ☠ **2** |
| `gpu_geomdirect.bin` | 2850 | ☠ **2** |
| `gpu_blitprobe` 560 · `gpu_geomxform` 1588 · `gpu_geotex` 3480/3572 · `dsp_pose` 2876 · `dsp_ovl_ent` 596 | | ✅ 0 |

✅ **Why the game is nonetheless correct today.** All six end with the same three
words — `d7c0` (jump) `e400` (nop, the delay slot) `e400` (**a redundant trailing
nop**). The truncation eats that last nop. It has been harmless since the day it
was written, by luck.

### ☠☠☠ THE HAZARD IS THE COMBINATION — do not delete the second nops first

`jaguar-shared` now actively recommends removing the redundant second nop after
every jump (**one delay slot, not two** — worth 184 bytes in `jag_resident`, 64 in
`jag_s3k`, both silicon-verified). ☠ **Take that advice here before fixing the
uploader and the dropped word becomes the DELAY SLOT**, and the failure will look
like anything at all. `jag_aerodagger` is already in that state.

✅ **Fix, two halves, both needed:**
1. `.balign 4` **after** each `.incbin` in `gpu_blob.S` and `dsp_blob.S`.
2. `(n + 3) / 4` at all five call sites.

`jag_s3k` gates it (`make blobcheck`: compare each blob's file size against its
`end - start` span from the link map, and assert the `(span>>2)*4` the uploader
moves covers the whole file). Its negative control is the good one — removing
*only* one blob's pad must fail while the other stays clean.

**Reproduce:** `stat -c%s build/*.bin` and `xxd -s -6 -g2 build/gpu_spanfill.bin`.
**Full write-up incl. a nine-project census:** `jaguar-shared`
`techniques/coprocessor-offload.md` §"A KERNEL UPLOAD SIZED IN THE WRONG UNIT".

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
