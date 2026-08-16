# jag_openlara — autorun state

RUN: 7

**This file is how work survives a context ending.** A context can end without
warning; anything the next run needs must be here, not in the conversation.
`tools/session_run.sh end` **refuses to advance the counter** if this file was
not touched, because a run that learned something and did not write it down has
lost it.

Every run: `tools/session_run.sh start` → work → `tools/session_run.sh end "<summary>"`.
Every 25 runs the script prints a PROGRESS REPORT DUE banner — the user reviews
direction at that point and decides whether it is still valid. **Do not
summarise that checkpoint away.**

---

## NEXT STEP

**✅✅ LARA'S HOME WORKS AND LOOKS RIGHT.** Warm stone walls, patterned tiled
floor, window openings, Lara standing — a proper mansion interior, `illegal=0`,
stable. Two runs closed it: run 5 stopped the crash, run 6 fixed the shading.

### Verified this run
- **Caves did NOT regress** from `gpu_kernel_ensure()` on every kick:
  `cavesfix_p0` vs the pre-fix `alias1_p0` = **0 of 25,600 pixels differ**,
  `illegal=0`, all pads boot. ★ Byte-identical is the CORRECT result here (the
  caves never hit the bug) — not the "arm is unwired" signal from run 3. The
  code is demonstrably live because the mansion's behaviour changed completely.
- **Release recipe fixed** (`tools/build_cof.sh`): the mansion now extracts with
  the **full** `$MRTENV` + `TEXSCALE=4`. `RAMP_PAL` is back on — the old comment
  saying it overflows the palette and kills the extractor was **stale**, fixed
  by run 1's 8-slot flat band. Without it the mansion rendered nearly black.
  TEXSCALE=4 is required to link (atlas 134,144 B vs 188,416 at TEXSCALE=2).
  ROM with the mansion: **1,638,252 B**, well under the 0x1FC000 guard.

### ☠️ NEGATIVE RESULT — the A10 lottery hypothesis is NOT confirmed
Vector 64 was checked on **all six CAVES pads of the PRE-FIX build**: every one
read `0x00004158` (`vblank_stub`), **intact**. So the vector-64 clobber never
happens on the caves path, and this fix does not explain the boot lottery.
Note also that jagemu boots every pad, so the lottery (a silicon phenomenon) is
not reproducible offline at all — **this cheap test cannot settle it either
way**. Do not re-run it expecting a different answer; if the lottery is to be
linked to vector 64 it needs a human reading the TV on a black pad.

### What to do next
1. **Re-verify the mansion through the real MENU path, not GYMTEST.** Everything
   so far used `GYMTEST=1`, which jumps past `gpu_jvdec_done()` and the level
   init. The kick-based fix is path-independent so it *should* hold, but the
   user's original report was via the menu — confirm it.
2. **Full-recipe container build** — `tools/build_cof.sh` changed; run it end to
   end so the release path is proven, and confirm the `gym_lskin == mrt_lskin`
   guard passes.
3. Then the queue: #3 title audio (SFX jumps + music skips on menu moves),
   #12 flat-shaded-floor fps A/B, #2 full run-through (rig, batched).

☠️ Rebuild the exact pad before symbolising. p0 command is in run 3's notes.
☠️ The mansion needs `TEXSCALE=4`; at 2 it does not link.

---

## WHAT CHANGED IN RUN 6 (2026-08-16)

- ✅✅ **The mansion looks right** — re-extracted with the full recipe including
  `RAMP_PAL` (whose "extractor dies" note was stale), atlas 90,112 → 134,144 B.
  Went from nearly black to a properly lit interior.
- ✅ **Proved no caves regression** — pixel-identical render, all pads boot.
- ✅ **`tools/build_cof.sh` updated** so the release build reproduces it.
- ☠️ **Recorded a clean negative**: the A10-boot-lottery link is unproven, and
  the offline test cannot decide it. Better than leaving the hypothesis dangling.

---

## WHAT CHANGED IN RUN 5 (2026-08-16)

- ✅✅ **Lara's Home is fixed** — no crash, vector intact, Lara renders. The bug
  that made it "broken" is closed.
- ✅ **`gpu_kernel_ensure()`** added and called from all six kicks; jvdec load
  invalidates residency, jvdec done restores it.
- ✅ **All kicks now set `params[1]`** (mailbox pointer), closing a second,
  independent instance of the same overlap hazard.
- ✅ **Published `techniques/gpu-kernel-residency.md`** to jaguar-shared — the
  general lesson (a kick launches whatever is resident; overlapping param slots
  turn a stale kernel into vector-table corruption) plus the diagnostic that cut
  through it: dump GPU SRAM and byte-compare against every built kernel.
- ★ Method note: the bug wore three disguises — "renderer bug" (the stripes were
  the crash beacon), "GPU wedge" (exc_catch halts Tom itself), and "corrupt
  pointer" (it was a constant, +2). Runs 2-4 each chased one.

---

## WHAT CHANGED IN RUN 4 (2026-08-16)

- ✅ **Root cause found and proven**: 68k vector 64 overwritten with
  `MAGIC_DONE` by the GPU's completion store through a bad `r30`. Caves vector
  verified intact as the control.
- ✅ **Fixed a real latent bug in 5 kick functions** — `params[1]` (the kernel's
  mailbox pointer) was never restored after `gpu_jvdec_kick` reused it as
  `prevLen`. Verified corrected at the world kick.
- ✅ **Localised the remaining cause** to `r30` being clobbered inside the
  kernel (holds the atlas pointer at halt), between f166 and f172.
- ⬜ Raised the possibility that this **is** the A10 boot lottery ("VI never
  fires") — a concrete, cheap test is written above.

---

## WHAT CHANGED IN RUN 3 (2026-08-16)

- ✅ **Disproved run 2's root cause** with a frame-by-frame timeline. The 68k
  faults first; the halted GPU is `exc_catch`'s own doing. Recorded above so the
  next run does not re-derive it.
- ✅ **Localised the real fault** to an `rts` returning to `MAGIC_DONE`, i.e. a
  stack imbalance of exactly 8 bytes, and cleared `cpu_stop_unless` of blame by
  reading it.
- ✅ **New technique: probe labels.** Label every instruction in a block,
  re-assemble, `cmp` to prove byte-identical, and read the exact PC→instruction
  mapping off the map. Left as `gpu_probe.gas`.
- ★ Named the wedge candidate precisely (`movei #DISPCUR,r1`, one after
  `load (r0),r2`) before the timeline showed it was a red herring — worth
  keeping as the shape of "precise but wrong".

---

## WHAT CHANGED IN RUN 2 (2026-08-16)

- ✅ **Root-caused the mansion failure to a GPU wedge** in the kernel's room-list
  init, with the crash frame, the GPU PC, and the watchdog path all identified.
  Reframes the bug completely: it is not textures, UVs or the atlas.
- ✅ **Published `techniques/reading-a-crash-without-a-capture-card.md`** to
  jaguar-shared (c70e277) — how to read a crash with `jagemu peek` when the
  capture card is gone, the "your garbage may be your own crash beacon" warning,
  the `PADTEXT` symbolisation trap, and the sentinel-vs-corruption lesson.
- ✅ **cobweb ad713ec → b8dd333** (4 commits incl. GOURD intensity and the
  blitter BUSY settle window). Renderer re-assembled **byte-identical**;
  `COBWEB_REV` bumped in the Dockerfile.
- ★ Learned: the archived gym assets predate the current extractor and their
  `gym_lskin` differs — the `build_cof.sh` alias guard added in run 1 is what
  makes that safe.

---

## WHAT CHANGED IN RUN 1 (2026-08-16)

- ✅ **Enemies fully skinned — 553/553 faces** (bat and bear were 0/41 and
  0/261). ★ Check the SPLIT not the total: 34 of the bat's 41 faces are
  coloured, so texturing alone helped it least. **Not yet seen on silicon.**
- ✅ **−107,040 B from every ROM** — `gym_lskin` was a duplicate of Lara's
  skeleton *and* sat outside the `GYMSD` `#endif`. Now a `.set` alias.
- ✅ **Lara's Home LINKS again** (1,524,188 B) via that alias + `TEXSCALE=4`.
- ✅ **`GYMTEST`/`CAVETEST` plumbed** — they never had been, so a GYMTEST arm
  silently rendered the Caves.
- ✅ **Two rig-protocol violations fixed** (`climb_matrix.sh`, `vroll_game.sh`).

---

## STANDING RULES FOR EVERY RUN

- ☠️ **Check cobweb first** — `tools/cobweb_check.sh`. Its updates change what
  is POSSIBLE here, not just what is buggy.
- ☠️ **Read `jaguar-shared`** — five sessions write hardware facts into it.
  `session_run.sh start` does both of the above.
- ☠️ **Push to `wip` (private), never `origin`.** `origin` is
  `bslop/tr1-jaguar` and it is **PUBLIC** — pushing there IS the release, which
  the user gated on his own sign-off and wants coordinated with a video.
- ☠️ **`rm -rf build` before any A/B**, and **verify the flag reached the
  compile line** — not just that it exists. A build arm producing byte-identical
  output to another arm is not "working", it is **unwired**.
- ☠️ **`make FOO=0` turns FOO ON.** Omit the flag instead.
- ☠️ Never `git add -A` in `jaguar-shared` — it is one working tree shared by
  every session, and another project's half-finished edit is usually sitting in
  it.

## HARDWARE — READ BEFORE PLANNING ANY RIG STEP

- **The capture card is PHYSICALLY UNPLUGGED** (jag_quake, 2026-08-16): no
  `/dev/video*`, no `0fd9:` on any bus. And there is **no other channel to read
  a number off the board** — no jaggd memory-read, no GD BIOS FWRITE, bulk IN
  times out, CDC has no Jaguar-side write path. **Rig time currently produces a
  picture on a TV that a human must read.** This devalues the `WORLDCOUNT`
  framebuffer diagnostic for task #11 — plan around it.
- **Roster is five** (openlara, quake, bubsy3d, rr, resident). A session not on
  it is **queued, not refused**. `jaguar-shared/hw/RESOURCES.md` is the
  agreement; `hw/GD_ACCESS.md` is openlara's gateway.
- **openlara has the slot after `jag_bubsy3d`**, batched — items #2, #8, #10,
  #11 plus visual confirmation of the enemy skins and the mansion are ONE
  console session, not six.
- Claim the rig **only** for work the emulator genuinely cannot answer.
