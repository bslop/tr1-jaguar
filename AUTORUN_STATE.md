# jag_openlara — autorun state

RUN: 4

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

**Lara's Home: the 68000 crashes FIRST. Run 2's "GPU wedge" was wrong.**

☠️ **CORRECTION TO RUN 2.** Run 2 concluded the GPU wedged and a watchdog halted
it. Run 3 disproved that with a frame timeline:

    f 40  gpu pc=0xF03000 instret=0          illegal=0
    f 80  gpu pc=0xF03D84 (halt) instret=9556 illegal=0   <- normal, clean halt
    f160  gpu pc=0xF03D84 (halt) instret=9556 illegal=0
    f170  gpu pc=0xF030E8 instret=438664      illegal=0   <- GPU RUNNING fine
    f180  gpu pc=0xF0317C instret=908590      illegal=1   <- 68k faults HERE
    f400  identical to f180

`illegal` flips to 1 at the same instant the GPU freezes, and
`startup.S:exc_catch` **itself** does `clr.l 0xF02114` (G_CTRL=0). So the 68k
crashed and *exc_catch* stopped Tom mid-render — the GPU PC 0xF0317C is
**incidental**, wherever Tom happened to be. Do not chase it as a wedge site.

Also corrected: `gpu_sync`'s `G_CTRL=0; return 1` is the **normal completion**
path (`cpu_stop_unless` returns 1 when the mailbox reads MAGIC_DONE), not a
timeout path. Run 2 misread it.

**THE ACTUAL FAULT: an `rts` returns to `0x0A3DD05E` (= MAGIC_DONE).**
Faulting PC `0x0A3DD060` is that value + 2. `cpu_stop_unless` (`cpu68k.S:19`)
is clean and its args are correct (`addr=0x001D33A0` = mailbox, `val=MAGIC_DONE`).
The caller pushes exactly those two longs:

    move.l #$0A3DD05E,-(a7)      ; MAGIC_DONE   <- ends up 8(sp)
    move.l #$001D33A0,-(a7)      ; &mailbox[0]
    jsr    ($0000420C).l         ; cpu_stop_unless
    adda.l #$00000008,a7

⇒ **a stack imbalance of exactly 8 bytes** — the size of those two args — makes
`rts` pop MAGIC_DONE as a return address. Find who returns with `sp` 8 high.

NEXT PROBES, cheapest first:
1. **Catch it live.** `jagemu serve --rom build/openlara.cof` + `jagemu ctl <inst>
   run 175`, then `step`/`disasm`/`peek` across the fault. This is the precise
   tool and it was not used yet — `break <rom> --at 0xADDR` also exists.
2. **Suspect the interrupt path, not `cpu_stop_unless`.** `stop #$2000` inside it
   sleeps with interrupts unmasked; an ISR that returns with `rts` instead of
   `rte`, or that mismatches its own pushes, lands exactly here. Check
   `vblank_stub` / the INT1 handler's stack discipline.
3. **Why only the mansion?** Likely it is slower per frame, so `gpu_sync` spins
   past `i >= 200 && (i & 63) == 63` and calls `video_rearm_irq()`
   (`video.c:692`) — a path the Caves rarely reaches. It writes `VMODE` and
   `cpu_irq_on()` mid-loop. Build with that call disabled and see if the mansion
   survives; that is a one-line A/B and would localise it immediately.

RULED OUT: `gym.bin` vs the TEXSCALE=4 atlas (256x352 = 90,112 exactly); the
room list (structurally identical to the Caves'); the loading-screen SD read
(shared path, falls through); `cpu_stop_unless` itself.

★ **Technique that worked** — the block had no labels, so mapping a PC to an
instruction was guesswork. Inserting `_wpNN:` labels on every instruction and
re-assembling gave an exact map, and `cmp` proved the binary **byte-identical**
(labels emit nothing). `gpu_probe.gas` is left in the tree for reuse.

☠️ Do **not** A/B against `ARCHIVE_PRERELEASE_2026-08-15/gym_before` — its
`gym_lskin.bin` differs from `mrt_lskin.bin`, so with the run-1 alias the
mansion would silently get the Caves skeleton. Re-extract at TEXSCALE=2 instead.

☠️ Rebuild the exact pad you run before symbolising (`PADTEXT` shifts every
address; `build/` keeps only the last pad linked). Working p0 build = gbuild's
BASE flags + `ENTITIES=1 SWANIM=1 DOORTEX=1 ENEMIES=1 ENEMYTEX=1 BLOBCACHE=1
JCENT=1 JOVL=1 SECTLONG=1 NOPCLIP=1 VRESN=80 TRAPFLOOR=1 GUNS=1 PROBE_AHEAD=256
GYMTEST=1 PADMUTE=1 PADTEXT=0`.

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
