# jag_openlara — autorun state

RUN: 3

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

**Lara's Home: the GPU WEDGES. The "garbage" was never a rendering bug.**

Run 2 identified the mechanism. The striped blocks on screen are the **crash
beacon** `startup.S:exc_catch` paints (row 0 = `A5A5A5A5` calibration, row 1 =
`EEEE0000`). Sequence: black → `LOADING...` panel (fine, the SD read falls back
correctly) → **GPU wedge** → `gpu_sync`'s watchdog times out → `G_CTRL = 0`
halts Tom → beacon painted → frozen (frames 300-1800 byte-identical).

Evidence, all reproducible offline with `build/openlara.cof` (PADTEXT=0 build):

    jagemu peek build/openlara.cof --at 0x820 --len 32 --frames 600
      -> EEEE0000  SSP=0x001FF2C8  SR=0x2200  PC=0x0A3DD060
    jagemu video build/openlara.cof --count 12 --every 150 --cols 3 -o film.png

☠️ `0x0A3DD05E` is **not** a corrupt pointer — it is `MAGIC_DONE` (`gpu.c:24`),
pushed as an argument to `cpu_stop_unless(&mailbox[0], MAGIC_DONE)`. Do not
chase it again.

**GPU PC at halt = `0xF0317C` = `wid_m_done + 0x78`, ~14 bytes before
`room_next`** — i.e. inside the SINGLE-DISPATCH room-list init
(`gpu_geotex.gas` ~line 549-578). ★ That block already carries a recorded
bus-wedge hazard: *"storing 1 into VCDEF bus-wedged every list-mode kick --
reload the address!"*

RULED OUT so far:
- `gym.bin` vs the TEXSCALE=4 atlas — consistent (256 x 352 = 90,112 exactly).
- The room list itself — structurally identical to the working Caves list:
  mansion `count=3` + 3 geom ptrs (0x13FB90/0x13B790/0x13C770), caves `count=2`.
- The loading-screen SD read — shared code path with the Caves, falls through.

NEXT PROBES, cheapest first:
1. **Get the exact wedging instruction.** Offsets were inferred by hand; emit a
   real listing/map and locate `0xF0317C` precisely within lines 549-578.
   `jas <kernel>.gas --map k.map` gives labels; the wedge is between
   `load (r0),r2` (list count) and the `DISPCUR`/`DISPCNT` stores.
2. **Compare GPU params.** mansion `params[]` @ `0xF03F00` =
   13FB90/1D33A0/1D33C0/1ACAE0/17297C/15A050/0100/F03F74; caves =
   023BB0/1A4740/1A4760/17DE80/14390C/07E780/0100/F03F74. The mansion's
   buffers sit MUCH higher (0x1D33A0 vs 0x1A4740) and SSP is 0x1FF2C8 — check
   whether a mansion buffer runs into the stack or past the 0x1FC000 guard.
3. Confirm pre-existing vs regression by re-extracting gym at **TEXSCALE=2 with
   the CURRENT extractor** (not the archive) and rebuilding.

☠️ Do **not** A/B against `ARCHIVE_PRERELEASE_2026-08-15/gym_before` — its
`gym_lskin.bin` DIFFERS from `mrt_lskin.bin`, so with the new alias the mansion
would silently get the Caves skeleton. Re-extract instead.

☠️ Rebuild the exact pad you are running before symbolising: `PADTEXT` shifts
every address and `build/` keeps only the last pad linked. Full p0 command is
in run 2's shell history — `gbuild.sh`'s BASE flags + the feature flags +
`PADTEXT=0`.

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
