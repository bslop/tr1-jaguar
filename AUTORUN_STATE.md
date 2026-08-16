# jag_openlara — autorun state

RUN: 1

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

**Lara's Home renders GARBAGE — and it is offline-reproducible, so no rig needed.**

    tools/gbuild.sh gymtest2 ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 \
      BLOBCACHE=1 JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80 TRAPFLOOR=1 \
      GUNS=1 PROBE_AHEAD=256 GYMTEST=1 PADMUTE=1
    jagemu screenshot build_gymtest2/gymtest2_p0.cof --frames 1500 -o /tmp/g.png

Observed: a striped block pattern at f1500; **4 distinct colours** at f3000 and
f6000; `illegal:1` (the Caves gives 0). It is genuinely the mansion — 99.7% of
pixels differ from a Caves frame — so the level switch works and the *content*
is wrong.

Prime suspects, cheapest first:
1. **`atlasW` vs the TEXSCALE=4 atlas.** `atlasW` comes from `S_index` (gym.bin,
   160 B). The atlas was re-extracted at TEXSCALE=4 (90,112 B) — confirm gym.bin
   was regenerated in the SAME pass, or every UV is scaled wrong.
2. **The 4-colours-at-f3000 reading suggests it is not drawing the world at
   all** — possibly stuck on the loading screen, which reads `GYMLOAD.DAT` off
   the GameDrive. **jagemu has no GD card**, so that read fails and may hang.
   Test by checking whether the Caves path takes the same branch.
3. `illegal:1` — find the illegal instruction; jagemu reports it, so it is
   locatable.

☠️ Do NOT judge a model change from this arm: `SPAWNAT_ROOM` renders Lara
deformed and always has.

---

## WHAT CHANGED IN RUN 1 (2026-08-16)

- ✅ **Enemies fully skinned — 553/553 faces** (bat and bear were 0/41 and
  0/261). Two stale defaults: `MRT_ENEMYTEX_MODELS` defaulted to `"wolf"` on an
  expired budget note, and the extractor dropped every *coloured* face
  (`tex<256` is still an objtex entry — a 1×1 rect whose texel IS the colour).
  ★ Check the SPLIT not the total: 34 of the bat's 41 faces are coloured, so
  texturing alone helped it least. **Not yet seen on silicon.**
- ✅ **−107,040 B from every ROM.** `gym_lskin` `.incbin`'d a duplicate of
  Lara's skeleton *and* sat outside the `GYMSD` `#endif`. Now a `.set` alias,
  guarded by a `cmp` in `build_cof.sh`.
- ✅ **Lara's Home LINKS again** (1,524,188 B) via that alias + `TEXSCALE=4`.
  It renders garbage — see NEXT STEP.
- ✅ **`GYMTEST`/`CAVETEST` plumbed** in the Makefile; they never had been, so a
  GYMTEST arm silently rendered the Caves.
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
