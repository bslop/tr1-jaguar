# jag_openlara — autorun state

RUN: 6

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

**✅ LARA'S HOME NO LONGER CRASHES — she renders. Now make it look right.**

Run 5 fixed it. Verified on the GYMTEST p0 build:

    vector 64 = 0x00004158 (vblank_stub, INTACT)   was 0x0A3DD05E
    illegal   = 0                                   was 1
    gpu instret at f900 = 249,067,610               was frozen at 908,590
    filmstrip: Lara stands in the mansion, stable across 900 frames

**ROOT CAUSE (closed):** a kick launches **whatever is resident in GPU SRAM**.
`g_kernel_cur` cached which kernel that was, but `gpu_jvdec_load()` replaced
SRAM without invalidating it — so `gpu_kernel_select()` skipped the copy and the
world kick **ran the VIDEO kernel**. The param blocks overlap with different
meanings: `params[6]` is `atlas_width` (256 = **0x100**) to the world kernel and
the **mailbox address** to jvdec. So jvdec wrote its magics to `$100`/`$104` =
68k **vectors 64 and 65**; vector 64 is TOM/VBLANK, the next vertical interrupt
jumped to `0x0A3DD05E`, and the 68000 died inside the ISR.

Fix: `gpu_kernel_ensure()` called after `G_CTRL = 0` in all six kicks
(`gpu.c`), plus `gpu_jvdec_load()` now invalidates and `gpu_jvdec_done()`
restores. Also fixed on the way: **every** kick now sets `params[1]` (the
mailbox pointer) — `gpu_jvdec_kick` reuses that slot as `prevLen` and six kicks
never restored it. Published as
`jaguar-shared/techniques/gpu-kernel-residency.md` (d958f35).

### What to do next

1. **⬜ THE BIG ONE — is this the A10 BOOT LOTTERY?** The recorded A10 root
   cause is *"VI never fires"*, and `video_flip_force`'s `VECDIAG` probe exists
   purely to ask "is vector 64 still `vblank_stub`?". A clobbered vector 64
   produces exactly that, and residency depends on layout/timing — i.e. a
   lottery. **Test: `jagemu peek <rom> --at 0x100 --len 4` on each `PADTEXT`
   roll of a CAVES build, before and after this fix.** If black pads showed
   `0A3DD05E` and now do not, this fix just closed the boot lottery too. Cheap,
   entirely offline, and by far the highest-value item open.
2. **Make the mansion look right.** It renders but is dark and sparse. Check
   the `TEXSCALE=4` atlas (90,112 B, 256x352) is being sampled correctly and
   whether the mansion needs its own lighting/shade band.
3. **Re-verify the Caves did not regress** — `gpu_kernel_ensure()` is on every
   kick now. Rebuild an alias1-equivalent caves ROM and compare fps/screens.
4. Then return to the queue: #3 title audio, #12 flat-floor fps A/B.

☠️ Note the GYMTEST caveat: `GYMTEST` jumps past `gpu_jvdec_done()`, which is
why the restore-based fix alone did not work and the kick-based one was needed.
The kick-based fix is the correct one regardless — some path will always skip a
restore.
☠️ Rebuild the exact pad before symbolising. p0 command is in run 3's notes.

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
