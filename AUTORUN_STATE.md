# jag_openlara — autorun state

RUN: 5

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

**Lara's Home: 68k VECTOR 64 IS OVERWRITTEN WITH `MAGIC_DONE`. Half fixed.**

Run 4 found the actual mechanism, proved it, fixed one of two causes, and
localised the second precisely.

### What is proven

    mansion  $100 (vector 64) = 0A3DD05E  (MAGIC_DONE)
             $104 (vector 65) = 0A3D0001  (MAGIC_HELLO)
             $108/$10C        = 00004054  (exc_catch, untouched)
    caves    $100             = 00004158  (vblank_stub, INTACT)

The GPU kernel signals completion with `alldone: store r0,(r30)` where **r30 is
the mailbox pointer**, and `mailbox[1] = MAGIC_HELLO` is stored at `r30+4`. Both
magics landed on **vectors 64 and 65**, so `r30` was `0x100`. Vector 64 is the
TOM/VBLANK vector, so the next vertical interrupt "returned" to `0x0A3DD05E` and
the 68000 died **inside the ISR** — the stack carries TWO exception frames, the
outer one taken at `cpu_stop_unless`'s `stop #$2000`.

★★★★★ This is why the failure looked like a GPU wedge AND like a rendering bug.
The stripes on screen were `exc_catch`'s crash beacon; the halted GPU was
`exc_catch`'s own `clr.l 0xF02114`. **Neither symptom was the bug.**

⬜ **Does this explain the A10 BOOT LOTTERY?** `video_flip_force`'s `VECDIAG`
probe exists solely to ask "is vector 64 still `vblank_stub`?", and the recorded
A10 root cause is *"VI never fires"*. A clobbered vector 64 would produce exactly
that, and would be codegen/layout dependent — i.e. a lottery. **Worth checking on
a failing caves pad**: peek `$100` on each `PADTEXT` roll. If a black pad shows
`0A3DD05E`, this bug and the boot lottery are the same bug.

### FIXED this run (keep — correct regardless)

`params[1]` is the kernel's `r30` source (`load (r1),r30 ; [1] mailbox`), and
**`gpu_jvdec_kick` reuses `params[1]` as `prevLen`**. Six kick paths set
`params[0,8,12,...]` but never restored `params[1]`:
`gpu_geomxform_kick`, `gpu_textured_kick`, `gpu_geotex_kick`,
`gpu_geotex_dispatch`, `gpu_geomdirect` — all now set
`*(G_PARAMS + 4) = (uint32_t)mailbox`. Verified: `params[1]` at the world kick
went from a stale `0x001D33A0` to `0x001D33F0`, which **is** the C `mailbox`
symbol (`nm`: `001d33f0 b mailbox`).

### NOT fixed — the remaining cause

The vector is **still** clobbered with `params[1]` correct, so **`r30` is being
destroyed INSIDE the kernel**. Evidence: at halt `r30 = 0x0015A050`, which is
`params[5]` (the atlas pointer) — not a mailbox at all. The clobber happens
between **f166 and f172**, on the first world kick.

NEXT PROBES:
1. **Find who clobbers r30 in `gpu_geotex.gas`.** It is documented as callee-
   saved: line 1496 *"r30 = mailbox"* as an input, line 3729 *"Preserves
   r10-r21, r4-r6, r30"*. Grep every write to r30 and check each routine on the
   list-mode path honours that. `grep -n "r30" gpu_geotex.gas`.
2. **Use the probe-label trick** (run 3) to catch it live: `jagemu serve` +
   `ctl run 166` then step, watching r30 in `ctl state`.
3. **Cheap containment while hunting**: have the kernel reload r30 from
   `params[1]` immediately before `alldone`'s store. That is a 2-instruction
   guard; if the vector then stays intact it confirms the diagnosis exactly and
   makes the mansion testable while the real culprit is found.
4. **Then check the A10 link** (see above) — potentially the bigger prize.

☠️ Do not chase GPU PC `0xF0317C` or "the GPU wedged" — run 3 disproved both.
☠️ Do not A/B against `ARCHIVE_PRERELEASE_2026-08-15/gym_before` (its
`gym_lskin` differs from `mrt_lskin`; the run-1 alias would feed the mansion the
Caves skeleton).
☠️ Rebuild the exact pad before symbolising. p0 command is in run 3's notes.

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
