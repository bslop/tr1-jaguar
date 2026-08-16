# jag_openlara — autorun state

RUN: 9

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

**✅✅✅ LARA'S HOME WORKS THROUGH THE REAL MENU, AND THE RELEASE NOW SHIPS IT.**

Run 8 verified the path the user actually reported broken — not the GYMTEST
shortcut. Filmstrip: black → **TOMB RAIDER title with the ring menu** → the
mansion interior renders and stays stable. `illegal=0`, `vector64 = 0x00004158`.

New test hook **`AUTOGYM=1`** (plumbed in the Makefile, verified on the compile
line): breaks out of the ring loop exactly where a real A-press on page 4 does,
so it runs the whole menu exit — loading screen, level setup, `gpu_jvdec_done()`
— unlike `GYMTEST`, which jumps to `menu_done` from far earlier and skips all
of it. **Use AUTOGYM, not GYMTEST, for anything about the mansion from now on.**

☠️ **`AUTOSTART` now yields to `AUTOGYM`** (`#if defined(AUTOSTART) &&
!defined(AUTOGYM)`). Both hooks `break` out of the ring and AUTOSTART is FIRST,
so an AUTOGYM arm built with AUTOSTART also on silently landed in the CAVES and
would have read as "the mansion is broken again". The flag reached the compile
line and was still preempted — *landing is not the same as taking effect*.

**`tools/build_cof.sh` no longer passes `GYMSD`.** Under GYMSD the ring
deliberately REFUSES Lara's Home (null stubs), so the item was unreachable, not
just absent — that is what "Lara's house is broken" looked like from the title
screen. It now fits: 6 of 6 pads, ROM 1,554,268 B.

### What to do next
1. **Run `tools/build_cof.sh` END TO END.** It has changed in runs 6, 7 and 8
   and has NOT been executed once since. This is now the highest-risk item: the
   release recipe is unproven. Confirm the `gym_lskin == mrt_lskin` guard fires
   correctly, all six pads build, and the four videos still convert.
2. **Docker container build** — same recipe, from the user's disc.
3. Then the queue: #3 title audio (SFX jumps + music skips on menu moves — an
   emulator-answerable item via `jagemu audio`), #12 flat-shaded floor fps A/B,
   #2 full run-through (rig, batched; capture card still unplugged).

☠️ Only `gbuild.sh` enforces the stack-headroom guard — a bare `make` does not.
Count the `.cof` files.
☠️ The mansion needs `TEXSCALE=4` + `RAMP_PAL` + **no** `STATICS`.

---

## WHAT CHANGED IN RUN 8 (2026-08-16)

- ✅✅ **Mansion verified through the real menu path** and the release now ships
  it selectable (`GYMSD` dropped from `build_cof.sh`).
- ✅ **`AUTOGYM=1` added** — the first offline test that covers the shipping
  menu-exit path rather than the GYMTEST shortcut.
- ☠️ **Caught a hook-precedence trap**: AUTOSTART preempted AUTOGYM even though
  `-DAUTOGYM` was on the compile line. Made the precedence explicit in source so
  no caller can hit it by flag ordering.
- ★ The measurements that made shipping it possible, in one place: gym_lskin
  alias −107,040 B; TEXSCALE=4 + RAMP_PAL + no STATICS = 254,296 B payload;
  6 of 6 pads at 1,554,268 B.

---

## WHAT CHANGED IN RUN 7 (2026-08-16)

- ✅ **Mansion now fits: 6 of 6 pads, ROM 1,554,268 B**, by dropping STATICS
  from its extraction (−107,368 B). Boots clean, vector intact.
- ☠️ **Corrected a wrong claim I made twice** — runs 5 and 6 both reported the
  mansion linking, from a bare `make` that skips the stack-headroom guard.
  Under `gbuild.sh` it built **nothing**.
- ✅ **`tools/build_cof.sh`** now carries the exact working combination and the
  measurements behind it, plus `$MRTENV_NOSTATICS`.
- ★ Found that under **GYMSD the menu refuses Lara's Home on purpose** — so
  shipping it playable means building without GYMSD.

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
