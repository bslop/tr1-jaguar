I verified the load-bearing facts directly. Here is the plan.

---

# CAMPAIGN PLAN — jag_openlara, `offload-campaign`
**Written 2026-09-05 against RUN 14's baseline. Read-only survey; nothing built.**

---

## 1. THE BOTTLENECK, RESTATED

Tom is the critical path and the 68000 sleeps through his render — that part of the common understanding is correct and I am not disputing it. **Three other parts of it are wrong, and two of the errors were introduced yesterday by this project's own RUN 14 measurement.**

**(a) "The Blitter is 34.6% of Tom" is a 120-line number and must not be quoted at the shipping resolution.** `AUTORUN_STATE.md:60-68` (RUN 14, VRESN=60, `--fidelity silicon`, frames 600–900) reads **blit 20.3%** — launch 1.8%, transfer 18.5% — over 408 blits/field at 182.9 cyc/blit. VRESN cut the fill and left the per-vertex/per-face work alone, exactly as §4b predicts. Every fill-lever estimate in the ledger is ~40% too large at `QUALITY=playable`.

**(b) The profile's percentages are not shares of frame time, and nobody has produced one that is.** Two independent dilutions. The `blit 20.3%` row is *asynchronous Blitter busy-time expressed against Tom's running cycles* (408 × 182.9 = 74,623 ≈ 20.3% of 367,266) — it is not a slice of Tom's own cycles. And Tom's 367,266 cycles/field are themselves ~83% spin: `gpu_geotex.gas:4348-4352` records, same ROM one immediate apart, GPU instret **11,666,506 (`moveq #3`, shipped) vs 1,969,011 (`moveq #2`)**, i.e. the `halt: nop / jr T,halt / nop` park at `gpu_geotex.gas:4364` dominates retirement. The park is *elastic* — remove N cycles of render and the park grows by N — so a saving can be real and still show as nothing in this denominator. **Every candidate in this campaign, including the one I recommend, is currently sized against a denominator that has never been validated.**

**(c) The span geometry the batching arithmetic rests on is wrong.** RUN 14: **spans are ~15 px, not 9.4** (166.9 transfer ticks / 5.6 ≈ 30 accesses), and **launch is 8.7% of blit cost, not 14%**. That kills the batching family harder than TRAPEZOID's own autopsy did, and it also means the phrase-alignment arithmetic that closes PHRASEDST (§5) should be re-derived at 15 px, not 9 — I do so below and it still closes.

**(d) Jerry has less headroom than Tom.** RUN 14: **DSP 443,304 cycles/field vs GPU 367,266**. The unit this campaign was named for offloading *to* is running closer to its ceiling than the unit it would unload. Caveated — the resident mixer spins and some of that is idle — but it inverts the campaign's founding premise and it is measured.

**What actually limits the engine, stated cleanly:** Tom transforms ~981 faces to raster ~506 (48.4% waste), then writes every destination pixel through a Blitter that holds the bus while it does so, on a machine where every texel and every pixel is a DRAM access. The two quantities that scale the whole thing are **pixel count** and **vertex count**, and both are already being attacked by the only structural lever this project ever found (VRESN). Everything else — batching, reordering, occlusion, offload, micro-opt — is closed.

---

## 2. THE STACK THAT CROSSES A FIELD

### The rung is not 12.5% at the shipping configuration. It is ~16.7%.

`tools/build_cof.sh:76,87-89`: `QUALITY="${QUALITY:-pretty}"`, `pretty` = 120 lines, `playable` = `VRESN=60`. demo32 and the RUN 14 baseline are `playable`. At ~9.4 fps that is **6.38 fields/frame** — a 6/7 mix, roughly 62% at 6 fields. Consequences:

| config | fps | fields | rung to next whole field | what a rung is worth |
|---|---|---|---|---|
| `playable` (VRESN=60) | ~9.4 | 6.38 | **6→5: −16.7%** | 9.4 → 11.3 |
| — intermediate | — | — | *all frames to 6: −6.3%* | 9.4 → 9.99 |
| `pretty` (VRESN=120) | 6.63 | 9.04 | **9→8: −11.1%** | 6.63 → 7.49 |

⚠ **The field quantum at each VRESN is not established, and this is a live unknown that changes the answer.** The 120-line ladder capture reads `8×133 10×132 (mean 9, never 9)` — a 2-field quantum, which would make the 120-line rung **20%**, not 11.1%. But `JAGUAR_FINDINGS.md:300` and `:318` record `(8×59, 9×41, 10×29)` and `(7×43, 8×33)` — odd counts, plentifully. One of these is an instrument artefact. It is free to settle (Phase 0) and it is the difference between "crosses alone" and "does not".

### Stack A — VRESN=60, the shipping config. **IT DOES NOT CROSS. Say so.**

| # | lever | contribution | bucket | status |
|---|---|---|---|---|
| 1 | `SLIVERW=56 SLIVERH=24` | **+17%?** | transform | **unverified at this baseline** (era C, 4 fps, rooms 3/10) |
| 2 | horizontal halving (PWIDTH) | +9–10% | fill | derived, 3 lenses |
| 3 | `PHRASECLEAR=1` | +1.8–2.1% | fill (clear) | built, plumbed, silicon-owed |
| 4 | `MICROOPT` + `BFSCACHE` | +0.8% | 68k/kernel | built, gated, measured flat alone |
| | **without SLIVERW** | **≈ +12.3%** | | **short of the 16.7% rung** |
| | **with SLIVERW at even half its era-C value** | **≈ +20%** | | **crosses** |

**The honest verdict: at VRESN=60 no set of levers that survives the ledger crosses a field, unless `SLIVERW=56 SLIVERH=24` survives re-measurement.** That is the whole plan in one sentence. SLIVERW is the only unspent measured win in the record (+17%, fully textured, no visible holes, `Makefile:1300-1307` plumbs it correctly as `-DSLIVERW=$(SLIVERW)`, `main.c:10803-10810` defaults 16/6, **pure runtime defines, no asset regen**), it lives in a *different bucket* from every fill lever, and it has never been re-measured since era C. It is a free arm. **Nothing in this campaign should be built before that arm is run.**

### Stack B — VRESN=120 (`QUALITY=pretty`). **One lever crosses alone.**

At 120 lines the fill bucket is **28.4% of frame** (NOFILL 6.67→9.32, silicon). Halving horizontal pixels removes ~44–47% of it = **~12.5% of frame**, against an 11.1% rung. The same change that is sub-rung at 60 lines crosses at 120.

And it is the better picture: **320×60 and 160×120 are the same pixel count and the same fill cost, but 160×120 keeps the vertical detail** — ledges, ladders, Lara's silhouette, the axis a platformer is read along. This reframes the horizontal lever from "a second quality sacrifice on top of VRESN" into **a substitute for one: trade horizontal resolution to buy vertical resolution back at constant frame rate.** That is a user judgment call, and per this project's own rule a perception verdict may close the feature but never the lever — so measure it, then let the user look at it.

### A second field is not reachable.

6→4 fields at VRESN=60 needs **−33%**. The entire fill bucket is ~20–22% and deleting all of it ships a smear (`NOCLEAR`) or a blank screen (`NOFILL`). Fill + SLIVERW + everything banked, with no quality left to trade, tops out near 35% *in the theoretical limit where fill is free*. There is no second field. Do not plan for one.

---

## 3. THE OFFLOAD MAP

**Headline: nothing should move between units. This engine has no offload left.** The campaign's own name is the hypothesis, and the evidence refutes it: the remaining lever is to make the work *smaller*, not to move it.

| work item | on which unit today | proposed unit | mechanism | est. gain | confidence |
|---|---|---|---|---|---|
| span pixel fill | **Blitter** (already) | — | already there; launch is 8.7% of blit cost, so amortising is dead (RUN 14) | **0** | measured |
| short-span fill | Blitter | Tom (GPU stores) | **NO.** Built already as `gpu_textured.gas` (see `gpu_bltex.gas:8-17`: "HW-verified but ~1.8 fps"). Peer [HW]: ~13.6 µs/px, ~360 Tom cycles/px vs the Blitter's 11.2 ticks/px. And BLITTER.md: the Blitter holds the bus for the whole operation, so the "fill during bwait" variant cannot overlap. | **0 (negative)** | measured |
| multi-row spans | Blitter (1/row) | Blitter (N/row) | **NO.** TRAPEZOID built, realizable batch 1.07–1.18 rows vs a ≥3-row bar; RUNBATCH doesn't fit (3908 vs 3680 B). RUN 14 makes it worse: launch is 8.7%, not 14%. | **0** | measured |
| screen clear | Blitter (byte) | Blitter (phrase) | `PHRASECLEAR=1`, already built and plumbed (`Makefile:1151-1153`, `blit.c:103`). Clear ≈ 26% of all transfer ticks. Bounded above by `NOCLEAR` (+2.4%). | **+1.8–2.1%** | derived; silicon-owed |
| Lara / enemies | Tom (transform+raster) | **OP sprites** | **NO — hardware-impossible on this display path.** `video.c:196`: *"QUIRK (HW-verified): nothing may follow a scaled object but STOP (a LINK to any overlay object poisons the display)"*. Two peers measured the same. And the ROM cost is 368 KB for one wolf at 8 yaws against a 102,368 B margin. | **0** | measured [HW] |
| framebuffer scan-out | OP (40 phrases/line) | OP (20) | **NO as a lever.** `SLITDISPLAY` cut OP fetch −73% and moved Tom 6147→6135 hl (0.2%); the 13.6% went to the 68k, which is asleep. jagemu *will* pay this out via `charge_op_tax` — an offline A/B reads 1–2 points optimistic here. | **0 (Tom)** | measured |
| room vertex transform | Tom | **Jerry** | **NO, three walls.** `JERRYX` measured 4.59 vs 4.59 with 0 px diff and its DSP half is deleted. Code window 4192 B, 1316 free vs 6274 B of m68k. And RUN 14: **Jerry 443k cycles/field > Tom 367k** — offloading to the busier core. | **0** | measured [HW] |
| `room_floor_mr` | 68k | Jerry | **NO.** 20 call sites, inherently serial (each query's args depend on the last result); a per-call kick+sync is the busy-poll measured at −51% (`SYNCPOLL`). | **0** | measured |
| movement/collision | 68k | Jerry | **NO.** Code-space blocked by 5–10×, in a less dense ISA. | **0** | measured |
| any work | Tom/Jerry | **68k** | **NO — and the direction is reversed.** Every 68k *removal* that cut DRAM traffic during Tom's render paid (HUDTEXT +56%, M68A2 +14–29%, ENTLISTS +4.1%); every 68k *addition* costs Tom (SYNCPOLL −51%, M68DIET −62%). The 68k must do less *bus work*, not more work. That vein is now tapped: the full remaining stack is +0.8%. | **0 (negative)** | measured |
| **destination pixels** | **Tom→Blitter, 320 wide** | **same units, 160 wide** | **THE ONLY LIVE MOVE.** VMODE PWIDTH (bits 11:9) is the *video-clock divider*, not the OP scaler — the OP never sees it and it adds zero OP bus traffic. Halves Blitter transfer, halves the clear, halves framebuffer BSS. | **+9–10% @ 60 · +12.5% @ 120** | derived, 3 lenses |
| neighbour-room transform | Tom | **nowhere — delete it** | `SLIVERW=56 SLIVERH=24`: stop *transforming* a room seen through a sliver. Deleting work dominates moving it. | **+17%?** | measured era C, **unverified now** |

**Why "nothing moves" is the right answer, mechanically:** Tom's kernel runs from GPU SRAM and fetches no instructions over the bus; every other master fetches from DRAM and pays contention. Moving work off Tom onto a DRAM-fetching master converts free instruction issue into bus traffic that steals Tom's bandwidth. That is why `SLITDISPLAY` helped the 68k and not Tom, why `SYNCPOLL` cost 51%, and why the removal sweep is uniformly negative.

---

## 4. STAGED PLAN

Rig time is a shared 10-minute batched lease via `jagq`; openlara's slot is after bubsy3d. Queue first, then work offline while it waits, and **record the job number in `AUTORUN_STATE.md` before you stop** — an unread capture is a wasted lease.

### PHASE 0 — make the offline denominator honest (no rig, ~45 min, zero ROM bytes)

**Build.** Add `--map $(BUILD)/gpu_geotex.map` to `Makefile:1398` (`$(JAS) $< -o $@ --gpu $(call jasd,$(GEOTEX_DEFS))` — verified: **no `--map` today**) and the same for `dsp_pose.das`. Fix the size-guard bug found while costing this: at `Makefile:1403-1408` **both `@sz=` guards are commands of the `gpu_geotex_hq.bin` recipe and both `stat $@`** — the blank line between them does not end the recipe. `$(BUILD)/gpu_geotex.bin` has **no ceiling check at all**; a kernel-growing change silently overlaps the SRAM vars at `$F03E60` and dies on silicon only.

**Measure.**
```
J=cobweb/sim/target/release/jagemu          # built; the "cold harness" note is stale
$J run out_auto/OPENLARA.COF --fidelity silicon --start 900 --frames 600 \
   --pc-histogram --core all --gpu-map build/gpu_geotex.map --map build/main.map \
   --blit-histogram --blit-top 0 --prof-json /tmp/base.json >/tmp/base.txt 2>/dev/null
```
Take `gpu.total_cycles` **from the JSON** (only it is windowed to `[start, start+frames)`; the state block's `cycles_per_field` is boot-contaminated — RUN 14 measured a 1.9× error, 188,754 naive vs 367,266 true). Subtract the three `halt` rows at `gpu_geotex.gas:4362-4366`. Divide by rendered frames (`60 × Δg_drawframes / Δframe_count`, needs `DREWVIS=1`).

Also emit the **field histogram at VRESN=60 and at VRESN=120** from the same runs. It has never been captured at either with today's levers.

☠ `AUTOSTART=1` is mandatory or you profile the title ring (RUN 14: a release ROM read 491 GPU cycles/field, blit_count 29 over 200 frames). Verify the window by **screenshot** before trusting a number. Redirect `>file 2>/dev/null`, never `2>&1` — hazard warnings corrupt the JSON.

**Pass/fail, declared now:** if park-subtracted productive Tom cycles/rendered-frame come out within ±15% of the naive figure, the RUN 14 percentages stand and can be used to size candidates. **If the park is >40% of Tom's cycles, every percentage in `AUTORUN_STATE.md:60-68` must be re-normalised and re-published before it is quoted again.** Abort condition: `--watchdog 3` names no park PC and the map produces bare hex — stop and fix the map, do not proceed on a guess.

*This phase changes no ROM byte. Do it while Phase 1 sits in the queue.*

### PHASE 1 — price the buckets on silicon, one batch, four arms, zero risk

Every arm is **kernel or `-D` only: no asset regen, no BSS change, no `op_list` movement, no A10 re-roll.**

| arm | flag | question it answers | pass criterion (declared in advance) |
|---|---|---|---|
| **0** control | ship recipe + `AUTOSTART=1 PADMUTE=1 FASTBOOT=1` | baseline + field histogram at VRESN=60 | bimodal fraction > 0.9; two rolls within 0.15 fps |
| **1** `NOFILL=1` | **prices the entire fill bucket in frame time at VRESN=60 — never measured** | — |
| **2** `NOBLIT=1` | forces `k=0` so `ss_none` is taken and the shade blit never fires (`gpu_geotex.gas:1457`) — prices the shade sub-bucket | — |
| **3** `SLIVERW=56 SLIVERH=24` | **re-prices the one unspent measured win at today's baseline** | — |

**The gate.** *Arm 1:* if fill at VRESN=60 is **≥18% of frame**, the horizontal lever has a pool worth building — proceed to Phase 2. If **<12%**, no fill lever can reach a rung at `playable` and the campaign either moves to `pretty` (Stack B) or pivots to the demo endpoint. *Arm 3:* if SLIVERW reads **≥+8%** at the spawn and survives a coverage check, **ship it** — it is free, textured, and needs no build system change. If it reads **<+3%**, era C's +17% was a 4-fps-era artefact and the open list loses its last unspent item.

**Instrumentation discipline.** `tools/beacon_box.py` with an **explicitly re-derived box** — the recorded `(185,148,209,180)` is the VRESN=80 layout and `FPSBEACON` paints fb x 32..63; this project has published a wrong fps from a mis-located beacon twice (6.48 for a true 7.00; 2.33 for 4.42). Second, beacon-blind instrument alongside (`framechange`, with the same motion in both arms) — these two have disagreed **in sign** here. Bounce the console through `jag_gd.sh power cycle`, never raw `jagpower`, while jagq holds the lease.

☠ **Verify each flag reached the compiler before the arm.** `make -n` and grep for `-dNOFILL=1` / `-DSLIVERW=56` on the actual command line. `make FOO=0` **turns FOO on** in this Makefile (`Makefile:1396` is `-d FOO=$(if $(FOO),1,0)`; `$(if)` returns 1 for the string "0"). ENTLISTS's phantom +2.5% was two byte-identical ROMs. `rm -rf build` between arms.

### PHASE 2 — gate the horizontal lever before building it (one arm)

Only if Phase 1 arm 1 passes. **Do not reparameterise the projection to find out whether the pool cashes.** The dominant term is separable behind one immediate:

At **`gpu_geotex.gas:2959`** (`store r0,(r15+15)`, the live texture-span `B_COUNT` — verified the `BWOVER` branch above it is dead, `BWOVER` is not in `BUILD_FLAGS`) and **`gpu_geotex.gas:3382`** (the shade `B_COUNT`), insert `shrq #1,r0` **plus a re-run of the zero guard**. The guard at `gpu_geotex.gas:2941-2948` exists and is load-bearing: *"npix == 0 (a 1px span) must skip: a 0 count blits 65536 pixels."* It tests the un-halved value; halving can produce a fresh zero.

That halves Blitter transfer with **span count, geometry, `FOCAL`, `CENTER_X`, `BASE_X`, framebuffer size, BSS layout and the OP list byte-for-byte unchanged.** It draws a half-width smear — worthless as a picture, exact as a price. **This is the KSWAP-style gate that TRAPEZOID never took, and TRAPEZOID is why it matters.**

**Pass:** ≥ +8% at VRESN=60 (or ≥ +11% at VRESN=120). **Fail:** < +5% ⇒ the fill pool does not cash through the field quantiser at this resolution; **do not build the PWIDTH reparameterisation.** That single arm saves a week.

### PHASE 3 — build it, only if gated

`HRESN` as a validated ladder (`ifdef` + membership test with `$(error)`, the `VRESN` pattern at `Makefile:1073-1083` — never a bare `-D`). The change surface, corrected against the file:

- `video.h` — derive `VMODE = ((1280/RW - 1) << 9) | $00C7`; 160 → `$0EC7`. PWIDTH encoding verified in `cobweb/sim/docs/spec/VIDEO_TIMING.md:204-219`.
- ☠ **`video.c:749` `video_rearm_irq` re-asserts `VMODE = 0x06C7` unconditionally** and `gpu.c:433` calls it from `gpu_sync`. Miss it and the picture silently doubles in width mid-session. Same literal at `hangbeacon.c:52`. And `gpu.c:423-431` records that VMODE writes disturb the video timing generator at *half* frame throughput — so the mode switch must live on the title↔game transition only, never per-frame.
- ☠ **`main.c:96` is `#define FOCAL_Y (FOCAL * RENDER_H / 240)`** — FOCAL_Y is **derived from** FOCAL. The survey's claim that "the FOCAL/FOCAL_Y split already exists" is false in the direction that matters: halving FOCAL silently halves FOCAL_Y and doubles the vertical FOV. The split must actually be introduced.
- FOCAL has **four independent copies in three define systems**: `main.c:93`, `main.c:357` (`PCL_FOCAL 190`, with a "MUST match the kernel" comment), `gpu_geotex.gas:272` (`FOCAL .equ 190` — *not* `RENDER_W/2`, and not equal to main.c's 160), `dsp_pose.das:306`. `jas` takes `-d` only in `.if` comparisons, so each needs its own enumerated ladder. Precedent for getting this wrong: `Makefile:892`'s `dsp_pose` rule does not pass VRESN, so `RX_FOCAL_Y` is a 120-line value in a VRESN=60 ROM today — harmless only because `RX_*` are dead.
- `video.c:38 BASE_X 16 → 8` (XPOS is an OP line-buffer index; a ~352-position line becomes ~176) — silent off-centre failure if missed.
- `gpu_geotex.gas:271` `CENTER_X 160 → 80`; `gpu_geotex.gas:2347` clamps to x=319; A2 `WID` encoding `$4200 → $3A00` (verified: `$4200>>9 = 33 = (4+1)<<6 = 320`; WID160 = `$3A00`).
- `main.c:6935 gpu_geotex_setclip(0,319,0,239)` **must stay 319** — that is the title path.
- `tools/beacon_box.py`'s box moves (beacon at fb x 32..63 is 20% across at 160 wide, not 10%).

**Reversibility gate:** `HRESN=320` must produce a **byte-identical ROM**. That is this project's standard no-op check and it is non-negotiable.

**Two variants, and the cheap one is not the surveyed one.** `RENDER_W` is the shared *stride* for the title, ring menu, loading art and the FMV decoder (`gpu_jvdec.gas:126 movei #320,r23`; `main.c:514 g_vidscratch[320*240]`; `tools/gen_titlebg.py` DST_W 320). Halving `RENDER_W` breaks all of them and shifts 115 KB of BSS. **Keep the 320-byte stride and narrow only the render**: set the object's `IWIDTH=20` while `DWIDTH` stays 40 (`video.c:267/301/376/469` already build both from `SCREEN_PWIDTH`). `IWIDTH<DWIDTH` is [HW]-proven on *unscaled* objects (jag_s3k, jag_sonic2) and **unverified on a scaled TYPE-1 object** — that is the one open question, and it is a one-turn yes/no. Fallback if it fails: treat the over-allocated buffer as packed 160-wide in game mode and 320-wide in title mode (`jaguar.h:80 BLIT_WID320` and its 13 `blit.c` uses become a per-mode runtime value).

### PHASE 4 — the quality decision, which is the user's

Present, side by side, on the TV: `320×60` (today's `playable`), `160×120` (same pixel count, vertical detail restored), and `320×120` (`pretty`). This is a perception call and the plan does not pre-empt it.

---

## 5. WHAT WE ARE NOT DOING

**Closed this round — three of the ledger's nine open items retire, with the evidence to close them permanently.** These are ledger edits, not experiments; they cost no build and no rig turn.

| item | was | now | one-line reason |
|---|---|---|---|
| **PHRASEDST** | open list **#1**, "one rig turn, yes/no" | **CLOSED on arithmetic + spec + this project's own RUN 14 note** | `BLITTER.md:296` (TRM): XADDPHR *truncates X to the next phrase boundary* — silicon does not mask a partial phrase, so a correct form must keep pixel-mode ends. At the corrected 15-px span, a span at uniform offset contains at most 1 whole aligned phrase; correct-form saving is ~1–2% of frame, sub-rung by 10×. And `AUTORUN_STATE.md:80-84` (yesterday) already found the built probe **only swaps `A2_FLAGS_VAL` at `gpu_geotex.gas:161` and never phrase-aligns `xl`** — the `XL &= ~7` exists only in the dead PHRASESHADE path (`:3678`). The +9–11% offline number is a build that paints x=8..23 for a span at x=13: fewer, larger, **wrong** blits. |
| **FOURBPP** as a fill lever | open list #2 | **CLOSED by hardware** | `JAGUAR_PORTING_NOTES.md:1355-1379` [HW], peer probe with five agreeing controls: 4bpp src → 8bpp dst, real Jaguar writes `$01234567` where jagemu writes `$00010203` — **Tom does not convert pixel sizes**, which is exactly the free conversion a 4bpp rehost needs. That kills the planned next step (A1 `XADDPIX` fixes an addressing mismatch that is not the defect). Independently: `blit.rs:619-621` forces one access/pixel in pixel mode **at any bpp** (`xadd != 0 ⇒ ppp = 1`), so the access count — what the Blitter is billed for — does not move. And a 4bpp *destination* needs `DSTEN` (`BLITTER.md:334`), taking 2 accesses/px to 3: **+50% transfer**. |
| **GPU-direct fill for short spans** | open list **#7**, *"the only structurally new kernel idea in the record"* | **CLOSED — it was built and measured on silicon** | `gpu_bltex.gas:8-17`, in this repo: *"gpu_textured : per span, a PER-PIXEL GPU loop (loadb texel, loadw palette, storew). HW-verified but ~1.8 fps."* The ledger's "UNTESTED, never built" is factually wrong. Peer [HW] corroborates at ~360 Tom cycles/px vs the Blitter's 11.2. And the interesting variant ("fill during bwait") is impossible: BLITTER.md — the Blitter holds the bus for the whole operation. ☠ **Record with it that jagemu would likely say YES**: `blit_busy` never reaches `risc/timing.rs::ext_access`, so an emulator A/B prices a GPU store during a blit as a quiet-bus access. |
| **Lara/enemies as OP sprites** | open list **#8**, *"the largest untried architectural idea"* | **CLOSED by hardware, verified locally** | `video.c:196`: *"QUIRK (HW-verified): nothing may follow a scaled object but STOP."* VRESN **is** that scaled object. Two peers measured the same (`object-processor.md:864-878`). Plus 368 KB of sprite sheets for one wolf against a 102,368 B margin. OP sprites and VRESN are mutually exclusive, and VRESN is the biggest lever this project has. |

**Killed in refutation this round — do not re-propose.**

- **`spanshade-remove-bake` / `shadebake`** (bake shading into the atlas) — the ship version needs `RAMP_PAL=0`, **+200,704 B**, against a margin I measured today at **102,368 B** (`out/OPENLARA.elf __bss_end = 0x001e3020` vs the `Makefile:1455-1457` guard `0x1FC000`). It also deletes per-vertex Gouraud level-wide (`pack_uv` puts a per-vertex k in every corner's u). ☠ **The 311,920 B margin everyone is quoting is the demo24-era GYMSD table; GYMSD has been off since 2026-08-16 and ~202 KB has been spent since.**
- **`shadeblit-phrase`** — a phrase dest aligns start down and rounds width up, ORing k into up to 7 px each side. `SHADEEXCL` exists (`gpu_geotex.gas:3358-3366`) because **one** pixel of OR overlap produced a reported flickering-tile artefact. Also OR's k into background index 0 at silhouettes → coloured fringes.
- **`shade-launch-defer` / `spanshade-pipeline`** — the Blitter must do texture(≈121)+shade(≈69) whichever order Tom launches; only its idle gap is recoverable, and most of that gap is the shade register writes, which `gpu_geotex.gas:3323` records **cannot** be hoisted ("black wedges, measured"). This is the software-pipeline version `BWOVER` (0.76 pp) says not to build.
- **`vcdrain-rightsize`** — real (~1.4–3.6%) but re-opens A1, the flagship silicon-only bug, and the "118-cycle requirement" is falsified by the shipped result (8,000 cycles took racing px only −37%).
- **`stage-defer-uv`** — ~0.6%. Exactly the MICROOPT class, which is measured at its ceiling (+0.8% for the whole stack).
- **`phraseclear` as a campaign item** — ≤2.1% (hard-bounded by `NOCLEAR`'s +2.4%). Keep it; it is built, plumbed and free. **Ride it as a passenger in a stacked batch; never headline it or spend a turn on it.**
- **`optax-iwidth-probe`** — a strict subset of the horizontal lever, not a stacking partner; and IWIDTH 40→20 blanks **half the screen**, not "32 columns".
- **`opband-hud`** — its +4% is VRESN=64, already free on the ladder; and it puts a second object on the A10 fault line (`jaguar.ld:47-51` reserves exactly 64 B at ALIGN(32), fully consumed by lists A and B under `OPDBL=1`).
- **The five Jerry candidates** (`jerry-async-handoff`, `jerry-prepass-pipelined`/JXPIPE, `jerry-window-reclaim`, `jerry-isr-restore`, `jerry-throughput-under-render`) — JXPIPE **is** JERRYX ([HW] null, 4.59 vs 4.59, DSP half deleted, ~1,606 B needed vs 720 free); the handoff caps at 2.23% of wall with Tom already idle; window reclaim enables two dead consumers; the ISR restore is 0.2% against a documented silicon death. **And RUN 14 added the decisive one: Jerry is busier than Tom.**
- **`phase-swap-visibility-block`** — sign undetermined: the exchange rate `k` derives to 1.5–2.7 from jagemu's 11 ms sort and to 0.875 from `build_cof.sh`'s 20 ms sort. Two of this project's own numbers straddle `k=1`. Its two nearest relatives (`COLLECTEARLY` −18%, `PIPESTAGE=0` black) both died on silicon.
- **`floorscan-candidate-list`** — half already shipping as `room_within3`'s cached `inset[64]`; the rest is ~1–2.5% and its matched precedent (`BFSCACHE`) read flat 4-for-4.
- **`frame-schedule-field-audit`** — its headline ("only even field counts, nobody has looked") is contradicted by `JAGUAR_FINDINGS.md:300/318`, the instrument it proposes exists (`HLP(i)`, `pp_per`/`pp_pmax`), and its bug class was already found and fixed here (`video.c:975-1000`, unmasked VC, 12–22 ms/frame). *The field-histogram capture in Phase 0 is a much narrower version of this and is worth doing.*
- **`serial-window-attribution`** — commit `aa1a37a` **is** that attribution and its own read says the largest line is 5.73% of wall and it is `main` itself. Adding `-Map` is fine hygiene; it is an instrument, not a lever.
- **`hudtext-mechanism-refuted`** — the mechanism correction is right and valuable (`menu_ink_w` at `main.c:4501-4509` rescans every glyph cell with a runtime `div`, paying `__mulsi3`/`__divsi3` per iteration — ~90 ms for 8 glyphs, matching the measured delta), but HUDTEXT is already off. Zero remaining gain. **Write the correction back; do not build on it.**
- **`blitter-truth-probe-one-turn`** — its critique is correct (an `x mod 8` histogram cannot detect a uniform-within-phrase defect) but the probe is unnecessary once PHRASEDST closes on arithmetic.

**Standing traps from the ledger, unchanged:** frustum culling (built, provably inert — `CURONLY=1` gives bit-identical face counters); draw distance / `FARCLIP` / `ROOMCAP` (bit-identical, B6 CLOSED — the *sliver window* is a different mechanism and is the exception); face-count reduction (`SUBDIV_MAX` byte-identical; median face is exactly one sector); further resolution steps (60 is the bottom of the hardware ladder — `Makefile:1073`, `VSCALE = 7680/N` must be whole); Z-buffer / hardware occlusion (16bpp-only, writes zeros at 8bpp); front-to-back reordering (unconditional write, no dst read); the ordered table (built, 2.4× slower, concluded); atlas size (−60% measured identical); `M68DIET` (quarantined, −62%).

---

## 6. RISKS

**A10 boot lottery.** Any change re-rolls it. Ranked by exposure:
- Phase 1 and Phase 2 arms: **no exposure by construction.** `jaguar.ld:47-51` places `op_list` at `ALIGN(32)` *before* `*(.bss)`, so BSS movement cannot disturb it, and none of those arms changes BSS, `op_list`, or main()'s layout.
- Phase 3, `IWIDTH` variant: **BSS byte-identical** — that is the main reason to prefer it.
- Phase 3, `RENDER_W` variant: shifts ~115 KB of BSS *and* main()'s constant pool. Manageable via `PADTEXT`, but it costs a pad-lottery re-verification (`gbuild.sh` builds 6 pads; today's ROM builds 6/6 at 1,543,484 B). Budget for it.
- The `op_list` reservation is exactly 64 B and fully consumed by lists A and B under `OPDBL=1`. **Any design needing a third object grows that reservation and re-enters the A10 class.**

**Measurement invisibility below one field.** The rung is ~16.7% at `playable`, and the mode is *bimodal* (6/7) — which cuts both ways. Sub-rung savings do cash partially by shifting the mix (`NOCLEAR` banked +3.1% that way), so "invisible" is not quite right at VRESN=60; but a 2% lever will read inside the ±0.3 fps run-to-run scatter and cannot be judged alone. **Every sub-rung item goes into a stacked, together-measured batch — never a solo arm.**

**Things that could make it slower.**
- **Removing 68k work.** Reproducibly negative here. Nothing in this plan removes 68k work; `PHRASECLEAR` *reduces 68k-issued bus traffic*, which is the direction that pays.
- **Control-flow reorgs.** `jag_s3k` [HW]: a real cycle saving made silicon **17% slower** because JRISC pays a refill per taken branch. Any change that trades straight-line work for branches is a regression until `jump_refill` says otherwise — and refill is already 14.8% of Tom here. The `B_COUNT` halving adds one instruction and no branch (the zero re-check reuses the existing `sp_skip` path). That is deliberate.
- **`GPUHALT=1` is not verified on silicon** (`gpu_geotex.gas:4351`: *"the rig went down before the A/B ran"*). It is a measurement arm, never a ship config — it changes DRAM arbitration.
- **The park's 83%-of-instret figure does not reconcile**: 1,969,011 instr / 500 frames = 3,938 instructions per frame for a renderer doing ~1,200 spans. Either the `moveq #2` arm rendered less than the control or the denominator is fields, not frames. **Re-take it in Phase 0 before quoting it.**

**Instrument risks specific to this plan.**
- An offline A/B of the horizontal lever reads **1–2 points optimistic** on the OP term (jagemu charges `charge_op_tax` off `iwidth_phrases`; silicon's `SLITDISPLAY` moved Tom 0.2%) and **unknown points pessimistic** on the freed-bus term (`risc/timing.rs:369` ignores cross-master page thrash — the M68A2 mechanism, fourth occurrence). Neither number is the verdict.
- **jagemu ignores PWIDTH entirely** — `VM_PWIDTH_SHIFT`/`MASK` are defined at `mem.rs:116-117` and referenced nowhere. A PWIDTH8 build renders perfectly offline no matter what silicon's pixel clock does. This is the one part that is 100% silicon-only, and the mitigation is the peer existence proof (jag_quake ships `VMODE=$EC1`).
- `tools/overdraw.py` and `fps_offline.py` remain broken (`fps_offline.py` launches `jagemu serve` with **no `--fidelity`** and discards the warning — every number it has produced is from the functional timeline). Do not use either.

**Process risk.** `build_cof.sh` was unable to build a ROM for 9 days (`54e55a6`→`fb2b316`) and **exited 0** the whole time. Before any arm: confirm a ROM exists and screenshot the measurement window. `COBWEB_REV` stays pinned at `9da2f99` for builds — bumping it re-rolls A10; local cobweb (`03b4ef5`, 1 commit) is for **measuring only**.

---

## 7. THE FIRST CONCRETE STEP

**Queue the Phase-1 rig batch with `jagq` — four arms, control first — and write the job number into `AUTORUN_STATE.md` before doing anything else. Then run Phase 0 offline while it waits.**

The four arms, in order: **control** (ship recipe + `AUTOSTART=1 PADMUTE=1 FASTBOOT=1`), **`NOFILL=1`**, **`NOBLIT=1`**, **`SLIVERW=56 SLIVERH=24`**. All four are `-D`/kernel-only, none touches assets, BSS, `op_list` or the projection, and all four can be built in one sitting. `rm -rf build` between each; grep the emitted command line for the flag before flashing.

**Why this one first, and not the horizontal lever it is meant to justify:**

1. **Two of the three numbers the whole plan rests on have never been measured at the shipping resolution.** Fill is 28.4% of frame at 120 lines and 25.4% at 80; at **60 lines it has never been taken**, and RUN 14 already showed the Blitter share falling from 34.6% to 20.3% across that same move. If fill at 60 is 12% rather than 22%, the horizontal lever is worth ~5% and this plan's centrepiece is dead — and one arm says so before a week of touching `FOCAL`, `FOCAL_Y`, `PCL_FOCAL`, `CENTER_X`, `BASE_X` and four `VMODE` sites.

2. **`SLIVERW=56 SLIVERH=24` is the only unspent measured win in the record and it is a free arm.** +17%, fully textured, no visible holes, pure runtime defines. It is the difference between a stack that crosses the rung and one that does not (§2, Stack A), and it has sat un-re-measured since era C while five campaigns were run past it.

3. **It is the gate TRAPEZOID never took.** TRAPEZOID was authorised by a "48% ceiling" the implementation could not exploit, built in full, pixel-exact, and net negative. The rule that came out of it is that a gate measurement must count a quantity the implementation can actually cash. `NOFILL` counts exactly the quantity a width halving cashes, at the exact configuration that ships, on the oracle that decides.

4. **It costs one shared lease and blocks nothing.** Phase 0 — the `--map` lines, the park subtraction, the field histogram, and the `Makefile:1403-1408` guard fix — is free, changes no ROM byte, and runs while the batch waits. If the rig slips, no work is idle.

**What we do not know, and how it gets found cheaply:** whether the field quantum at VRESN=60 is one field or two (free, Phase 0 — and it moves the rung between 16.7% and 33%); what fraction of Tom's 367,266 cycles/field is the park (free, Phase 0 — and it re-normalises every percentage in the current baseline); and whether real Tom honours `IWIDTH < DWIDTH` on a *scaled* TYPE-1 object (one rig turn, Phase 3 — and it decides between a 60-line change and a 107-site one).