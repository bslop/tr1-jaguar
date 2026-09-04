# jag_openlara — Progress ledger

<!-- The console reads this file: `## M<n> — <title>` and a ✅ when done.
     Reconstructed 2026-09-04 from the repo's own records — the tags,
     PLAY_BUILD.md's release history and AUTORUN_STATE.md — not from memory.
     Where a milestone is marked done, PLAY_BUILD.md names the ROM that did it. -->

## M0 — boot on real silicon ✅

- [x] GameDrive bring-up, ROM boots on a real Jaguar
- [x] framebuffer, OP list and vblank flip stable on hardware

## M1 — polygon renderer ✅

- [x] textured, lit polygons on silicon — tag `stable-2026-08-01-polygon-renderer`
- [x] affine per-span UV via the Blitter, at scale
- [x] span shading + head fix — tag `good-2026-08-01-headfix-spanshade`

## M2 — Lara ✅

- [x] runtime-skinned model carrying her complete animation set
- [x] pistols draw and fire; movement set (walk, roll, sidestep, jump)

## M3 — the front end, off the same disc ✅

- [x] Eidos and Core logos, attract cinematic
- [x] 3D passport ring menu, title theme streamed from SD
- [x] resident-DSP mixer + SD music streaming

## M4 — the Caves, playable end to end ✅

- [x] full first level, textured and lit
- [x] wolves and bats hunt; collapsing floors; switches open doors rooms away
- [x] `demo31` — the ROM of record, container build, cobweb `9da2f99`

## M5 — Caves completeness and the fastest rung 🔄 in progress

- [x] level END trigger, LEVEL COMPLETE, loop
- [x] HUD health bar, secrets with chime and count, dart traps
- [x] ANTIPAD doors shut behind Lara, save crystal stays in-world
- [x] YOU DIED card, CAMERA_SWITCH door-reveal cuts
- [x] VRESN=60 — 9.4 fps, +13.6% — `demo32` release candidate built and
      verified offline (0 `23f9` stores, op-aligned, clean 200-frame jagemu run)
- [ ] ⬜ silicon full-chain boot confirm for `demo32` — rig was flaky

## M6 — the demo endpoint 🔄 the actual goal

☠ **68k/kernel micro-perf is at its ceiling** (2026-08-27, measured). After
M68A2 (+14%) and ENTLISTS (+4.1%) every remaining saver reads FLAT on silicon:
BFSCACHE is 8.300 vs 8.30/8.30/8.30/8.275, the full stack is +0.8% — sub-field
against the +3.3% a rung needs. A field is 12.5%; the remainder is <1% each.

- [ ] the next rung needs a **structural** lever, not more micro-opts —
      Blitter fill is 30% of the frame and overdraw is 2.07×
- [ ] bank ENTLISTS (+4.1%) into the next container ROM of record
- [ ] ☠ **the origin push IS the gated release — the user calls that moment.**
      Nothing here pushes to origin on its own.

## M7 — the open blockers (see OPEN_ISSUES.md, which is authoritative)

- [ ] **A10 — a build can black-screen silicon from t=0** with no crash and no
      emulator symptom. The top blocker; 12 flashes spent. A semantics-preserving
      register shuffle (`-frename-registers`, zero source change) is sufficient
      on its own to trigger it. Mitigation ships (`noinline` helper), but the
      next feature re-rolls the same lottery.
- [ ] **A1 — Lara's head notches.** Silicon-only. Cull-sign theory dead in both
      directions; divide-by-zero refuted as the cause. Next lead is the PIPELINE
      race, currently untested rather than refuted. ⚠ Any future metric must be a
      silhouette/convex-deficiency measure — three metrics have already failed on
      this bug by measuring the wrong thing.
- [ ] A2 floating shaded poly chunks · A3 grey outline · A5 black poly at the
      cave mouth · A6 passport pose · A9 `front_fb` is not `volatile`
