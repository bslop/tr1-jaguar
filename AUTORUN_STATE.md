# jag_openlara — autorun state

RUN: 14

**This file is how work survives a context ending.** A context can end without
warning; anything the next run needs must be here, not in the conversation.
`tools/session_run.sh end` **refuses to advance the counter** if this file was
not touched, because a run that learned something and did not write it down has
lost it.

Every run: `tools/session_run.sh start` → work → `tools/session_run.sh end "<summary>"`.
Every 25 runs the script prints a PROGRESS REPORT DUE banner — the user reviews
direction at that point and decides whether it is still valid. **Do not
summarise that checkpoint away.**

### ☠️→✅ ENTLISTS WAS NEVER COMPILED — NOW WIRED, HONESTLY +4.1% (2026-08-27)
The micro-opt audit's first hit was a **plumbing bug**: `ENTLISTS=1` sat in
`build_cof.sh` BUILD_FLAGS but the **Makefile had no `ifdef ENTLISTS`** and no
generic var→`-D` passthrough, so `-DENTLISTS` never reached the compiler and
every `#ifdef ENTLISTS` block compiled to the `#else` full-scan. PROVEN: the
shipped `main.c` compile line carries no `-DENTLISTS`; adding it moves `main.o`
146,964→149,980 B. **⇒ the recorded "+2.5% (#2045→#2046)" was two BYTE-IDENTICAL
ROMs — pure beacon noise.** (M68A2 IS plumbed, via `-DM68D_A2`; VCDRAIN rides
GEOTEX_DEFS; only ENTLISTS was orphaned.)
FIXED: added the `ifdef ENTLISTS` block (Makefile) + completed the lever —
`54e55a6` left the wolf/bear draw loops and the pickup-collect loop unconverted;
`ent_lists_build` now builds `g_el_wolf/g_el_bear` and all three loops iterate
the lists. OFF = byte-identical to shipped; ON differs (+1232 B ROM).
**HONEST SILICON A/B (FASTBOOT, VRESN=80, PADMUTE, spawn, 40s):**
jagq #2048 base **7.975** → #2049 ENTLISTS **8.300** = **+4.1%**, bimodal 1.00
both, every 5 s window separated; frame-change instrument (beacon-blind) agrees
**8.65→9.03 = +4.3%**. The real lever beats its own ghost. ★★★★★ **`make FOO=1`
does NOTHING unless the Makefile maps FOO→-DFOO — grep the compile line for
`-DFOO` before trusting ANY A/B.** ⬜ ships on the next container build
(BUILD_FLAGS already lists ENTLISTS=1; now it will actually take effect).
Committed on `perf-sliver-4bpp`. Beacon box for this capture rig = (185,148,209,180).

### ✅ THE CONTAINER BUILT THE ROM OF RECORD (2026-08-26, after three fixes)
`out/OPENLARA.COF` 1,540,812 B, QUALITY=playable, PADTEXT=136, cobweb 9da2f99:
**0** dropped-destination moves (`23f9` scan) · `op_list` 0 mod 32 ·
jagemu OP misaligned hits **0**, stray writes 0 · **no** stale wolf UV table
(fresh enemy skins) · renders with its own SD payload (`--sd out`, frame 1500).
It took three container fixes to get here — **the container had produced NO
ROM since the 08-19 `disc/` move and exited 0 anyway**: patch tools wrote to
the root, `build_cof.sh` stat'd bare names, title tools read `title_pal.bin`
from the root. All routed through `disc/`; a run with no COF now exits 1. The
59e5896 cobweb pin is RETIRED (its "ABI regression" was the jas reloc bug).
✅ The clip defect was **numpy** (the `JV_VQ=1` encoder imports it; the image
lacked it) — `9c9d3e9`. All four clips convert fresh now.
**`demo31` STAGED: `WORK_ROMS/demo31/` (full SD payload) + `demo31_p136.cof`,
md5 `00872e51f9ab…`, container run 4 (20:28).** ✅ **CONFIRMED ON SILICON (jagq #2025): CORE → cafe → intro → laptop → TV →
TITLE ring**, clips fresh. Holds at the title (no AUTOSTART in a release).
**`demo31` IS THE ROM OF RECORD** — PLAY_BUILD.md updated.

### (was) ⬜ NEXT — a container build is the ROM of record for all of this (2026-08-26)
Everything measured today was built LOCALLY (stale `disc/` enemy skins and
all). The shipping artifact is the container: `Dockerfile` now pins cobweb
`9da2f99` (both jas fixes), `build_cof.sh` BUILD_FLAGS carry `M68A2=1
VCDRAIN=1`, `jaguar.ld` places `op_list`, `main.c` has the sleep fixes.
**Run the container (QUALITY=playable, PADTEXT=136 — PADTEXT no longer
matters for booting) and put its ROM on the rig once**: expect boot on any
pad, ~8 fps at the spawn, no whole-face flicker, fully-skinned enemies. That
ROM becomes `demo31`. Nothing else is owed before that.

### 2026-08-26 — THE RESOLUTION LADDER WITH TODAY'S LEVERS (silicon, spawn, PADMUTE+FASTBOOT)
120 lines **6.63** fps (was 6.33 without levers, +4.7%) · 96 lines **7.35** ·
80 lines **7.98** (jagq #2018/#2019/#2021 + #1990). ☠️ The +14% the levers give
at 80 lines does NOT transfer to 120 — more pixels, longer Tom render, less
68k overlap to remove. **80 lines (QUALITY=playable) STAYS the shipped
setting** — a "that's unplayable, hard to see" comment received today was
meant for ANOTHER Claude instance (user, same day: "What we had was fine").
The numbers stand; the verdict does not apply here.

### 2026-08-27 — ENTLISTS shipped (+2.5%), OT concluded (2.4× slower), micro-opt audit running
* ✅ **ENTLISTS=1 in the ship recipe (`54e55a6` on perf-sliver-4bpp): +2.5% silicon
  (7.90→8.10)**, pixel-identical, byte-identical off. Type-indexed entity lists built
  once instead of 13 rescans/frame of all 60 entities. A 68k-bus-contention lever
  ([[project_68k_contention_lever]]); sub-rung alone, STACKS. Next: 68k __udivsi3/
  __mulsi3 (~10%), blit_band fb clear (~3%).
* ☒ **Ordered-table renderer (OTLIST) CONCLUDED: 2.4× slower (3.25 vs 7.90), dead.**
  Renders correctly but two GPU kicks + un-overlapped sort = ~18 fields vs 8; GPUHALT/
  per-kick/SRAM-list all no-change (field quantization). Its benefits were already won by
  M68A2 (sort) + VCDRAIN (flicker). Parked on `ot-campaign`. See [[project_ot_campaign]].
* ⬜ Exhaustive micro-opt audit workflow in flight (kernel per-vert/face + 68k contention).
* ☠️ RIG FLAKY 2026-08-27: repeated EXECUTE LIBUSB_ERROR_TIMEOUT every few jobs; USB reset
  + `jagpower cycle` recovers it (device re-enumerates), but it re-wedges. May be hardware.

### ✅✅✅ 2026-08-26 — A10 SOLVED: TWO BUGS, BOTH FIXED, PAD 0 RENDERS (interactive session)

1. **A10 = OP scaled-object ALIGNMENT.** The OP fetches a TYPE-1 object as one
   32-byte burst and dies on a straddle (jag_quake silicon, cobweb `905188f`).
   `op_list` was `aligned(16)`. jsim now reproduces our whole silicon history:
   shipped pads 0/408/544 are 16 mod 32 → 37366 misaligned hits, black;
   136/272/816 clean. **Fix: `op_list` is defined in `jaguar.ld` under
   `ALIGN(32)`** (`aligned(32)` in C does not survive `jas`). Commit `1cc0b9f`.
   The 07-29 "VI never fires" was the consequence, not the cause.
2. **`jas` dropped the DESTINATION reloc of `move.l sym,sym2`** — every
   global-to-global copy in a jcc68k TU stored into the exception vector table
   (24 sites; `fs_ph1/2/3` stayed zero → height-0 object → **black on every
   pad** since video.c moved onto jcc68k; `ship_p136` dodged it via `GCCHOT=1`).
   **Fixed in cobweb `ba9c680`** with a regression test. Pull cobweb and rebuild
   jas before building ANYTHING.

**Proof:** `final_p0.cof` and `final_p136.cof` render Lara in the caves in
jagemu (mean 0.0854, 0 OP hits, 0 stray writes). **Pad 0 has never lit before.**

✅ **CONFIRMED ON SILICON — jagq #1988, `WORK_ROMS/final_p0.cof`:** CORE
logo → intro clip → title ring → Lara in the caves, on the pad that had never
lit. `~/.jagq/jobs/1988/frame_0[0-7].png`. Capture card works.

**✅✅ +13.9% ON SILICON — `M68A2=1` (jagq #1989 base vs #1990): 7.000 → 7.975
fps** (beacon box, bimodal, every 5 s window; frame-change 7.12 → 8.09 agrees).
The O(n²) room selection sort (main.c:9877-9888, `#else` branch) was 31.8% of
68k main-line cycles and jagemu priced it at ZERO frames — the win is BUS
CONTENTION while Tom renders under PIPELINE. Order-identical (pixel diff 0 /
12 px). Added to `build_cof.sh` BUILD_FLAGS. ☠️ `fps_measure.py` locked onto an
edge pixel at VRESN=80 and read 6.48 for a true 7.00; use `tools/beacon_box.py`.

**✅✅✅ A1 HALF-FIXED — `VCDRAIN=1` (jagq #1998, 38-room tour, rooms paired
by CONTENT, `tools/tour_pair.py`):** racing px **9797 → 6219 (−37%)**; the
whole-wall face in PSX room 20: **2698 → 12**; rooms 3/10: 832 → 93, 484 → 35;
fps 6.83 → 6.75. **Mechanism:** Tom's face loop read back vertex-cache words
the self-transform pre-pass had JUST stored (jsim: 118-cycle min gap) and
silicon returned the stale word — one wrong vertex = a face toggling. Only
rooms dispatching >8 rooms (Jerry's cache cap) self-transform, which is why it
was room-specific. Fix = ~8k-cycle drain after the pre-pass, self-transform
path only. **Now in `build_cof.sh`.**
✅ **The "second mechanism" in PSX room 32 was a WOLF.** jagemu live session
at the hold: camera block bit-identical for 96 fields, floor-patch luma steps
48.6 → 44 → 41 → 54 at a fixed game time; unchanged by NEARLOW, BEXIT off,
TRAPFLOOR off, sort hysteresis; **constant 48.6 with ENEMIES off.** The tour
parks a PADMUTEd Lara beside a wolf that wakes and walks through the patch.
⇒ **A1 accounting after VCDRAIN: 6219 − ~4300 (wolf) ≈ 1900 racing px
level-wide, from 9797 — the whole-face class is gone and no room stands out.**
✅ **The wolf's flat BLACK polygon is a LOCAL-ASSET artifact, not a shipping
bug.** `disc/mrt_entex.h` here is dated Aug 11 — before the all-three-enemies
skin work — and carries 121/173 wolf quads (and 100% of bear/bat) as `0xFFFF`
flat faces; those fall back to the tone swatch, which samples black against
the current atlas. With ENEMYTEX off the wolf renders whole. **The shipping
`ship_p136.cof` does NOT contain this wolf UV table (byte-signature search);
`final_p0`/`tour_*` (built here) do.** The container regenerates it
(`build_cof.sh` MRT_ENEMYTEX pass, default models `wolf,bat,bear`). ⇒ Before
judging ANY enemy on a locally-built ROM, regenerate `disc/` with the
build_cof.sh recipe. None of today's A10/M68A2/VCDRAIN results depend on it.
☠️ Still-camera flicker metrics must run with ENEMIES off (or mask entities);
`tools/tour_pair.py`/`race_px.py` do not know a wolf from a wall.
☠️ Index-paired segments lied (the drain "fixed 94%" of a room that was a
different room) — pair by content, always.

**A1 — the earlier hypotheses, for the record**
* ☠️ **Refuted: "Tom's 3 ms per-room guard times out and self-transforms with
  different arithmetic than Jerry."** `JXWAIT=1` makes the 68k wait (polite
  150 µs backoff) for every Jerry room flag before Tom's first dispatch. The
  wait executed (jagemu: 336 backoffs / 600 frames) and changed NOTHING on
  silicon: racing world px **469 vs 463** (M68A2 control), fps 8.00 vs 7.98
  (jagq #1993). Jerry is already done by dispatch; the race is elsewhere.
* ☠️☠️ **THE FIRST METRIC WAS BROKEN** — its hand-placed beacon box missed the
  block's top rows, so 536/463/469/482 were ~90% beacon edge + Lara breathing
  (the mask IMAGE showed it). `tools/race_px.py` now derives the beacon mask
  from the data. **Corrected, world only: base 60 → M68A2 30 → +JXWAIT 31 →
  +COLLECTEARLY 31.** ⇒ M68A2 HALVES the still-camera flicker at the spawn
  (the sort's DRAM traffic was a real A1 contributor, as PIPESTAGE=0's 89%
  predicted); neither experiment adds anything beyond it; the residual ~30 px
  do not respond to the 68k going quiet at all — Tom-internal. The spawn is a
  weak A1 site; the user's "worst flickering" spot is elsewhere.
* ☠️ **Refuted: `COLLECTEARLY=1`** (jagq #1997): racing px **482** vs 463,
  fps **6.55 (−18%)**. The 68k sleeping through most of Tom's render does NOT
  quiet the wedge, so "68k bus pressure" is not the A1 mechanism either.
  ☠️ And the flag had a BUG: its `#if … && !defined(COLLECTEARLY)` gate at the
  old collect ran to the `#endif` after the deferred `video_flip`, so it
  compiled out the flip and the HUD paints — black on silicon (jagq #1995)
  and in jagemu. Fixed (gate no longer excludes COLLECTEARLY; the old collect
  is a no-op under it). Flag stays off.
* ★★★★★ **THE A1 MAP (from the 38-room tour, frame-rate-fair metric,
  `tools/race_px.py` masks): level-wide the two arms are EQUAL** (10,014 vs
  9,797 racing px, 14 rooms ≥20 each) — M68A2 is not a regression, it shuffles
  which near-tied faces flip. **Two shapes, two rooms:**
  - **tour seg 19 = PSX room 32: SPECKLE** across the floor (5227 base / 4278
    a2) — per-pixel nondeterminism; M68A2 visibly thins it ⇒ the bus-pressure
    class PIPESTAGE=0 attacked.
  - **tour seg 26 = PSX room 20: a WHOLE WALL FACE** toggling (2960/2698) —
    painter-order flip between overlapping rooms; the idle camera breathes,
    near-tied rooms swap. Candidate fix: hysteresis in the room sort.
  ☠️ A 60-field window at 5 fps holds 5 frames: a ≥4-toggle threshold is
  biased toward the faster arm. Use toggles ≥ 50% of frames rendered.
* ⇒ Both 68k-side hypotheses are dead. What is left is nondeterminism INSIDE
  Tom's frame: the kernel's own "stale-BUSY window" (a Blitter status poll
  that sails through before BUSY asserts) is the named candidate — jsim's
  `blitter.bcmd_poll_in_settle` counter is the instrument to read first.

**✅✅ 38-ROOM TOUR CONFIRMS IT, LARGER: jagq #1991 base vs #1992 M68A2** —
32 valid rooms each, median **5.30 → 6.83 fps (+29%)**, mean 5.04 → 6.46;
rooms 0–17 pair cleanly and are ALL faster (+17.6% … +52.2%, median ≈ +33%).
The spawn's +14% was the small end. Per-segment tables in `sweeps/`, tool =
`tools/tour_box.py` (tour_fps.py's locator does not work at VRESN=80).

**Gameplay profile (jagemu, silicon fidelity, frames 300-1200, `final_p136`)**
— kernel attribution verified by `cmp` of Tom SRAM against `gpu_geotex.bin`:
* Tom busy 73.6% of wall; **~30% of Tom's cycles are WAITING**: `ss_bw` +
  `bwait` Blitter spins ~17%, the `halt` idle spin 12.4%. Blitter-bound in the
  span phase, as the closed perf memory says — now quantified per PC.
* 68k: 43.5% asleep in STOP, 56.5% awake, 99% of that main-line. ~20% of
  awake time is ONE comparison loop in `main` (`0xFE64`–`0x10298`: stack-table
  lookups at sp+0x614/+0x814, cmp/blt/beq) — the per-frame depth/paint sort.
  ~150k cycles/frame ≈ 11 ms ≈ 8% of a 133 ms frame IF serial with Tom.
* ☠️ `m68k_dram_poll_max` = **5710** at `0xF08A` (budget 128): a 96-field
  `frame_count` busy-wait in `main` after a `(8,8)` call — one-shot, not
  per-frame, but a DRAM spin the detector flags. Find and STOP-sleep it.
* ☠️ GPU store→load round trips: **36** in `stage_vert+0x16..0x2a` (the
  vertex-cache reads of words the vc_loop pre-pass STORED, min gap 258 cyc).
  Silicon can return stale/0 there — the same shape as the A1 "PIPELINE race"
  flicker. Lead, not fixed.
* `imul32` is NOT hot in geotex (INLINEMUL did its job); my first read said
  otherwise because I attributed with the geomdirect map. Verify the resident
  kernel by dumping SRAM before trusting any GPU PC name.

⬜ `PLAY_BUILD.md` recipe is stale (needs `GUNS BLOBCACHE JCENT JOVL SECTLONG`);
build from `tools/build_cof.sh`'s `BUILD_FLAGS` (+`VRESN=80 AUTOSTART=1`).
⬜ jas still leaves `.bss` `sh_addralign` at 16 under `.align 32` (filed in
jaguar-shared) — keep the linker-script placement.
★ Method: a commit bisect 18→19 Aug found NOTHING — the assembler was the
variable. Diff linked bytes against generated asm.

### What may interrupt the 25 — and what may not (user, 2026-08-17)

> **The full working agreement lives in `/home/jvilla/Documents/Git/jaguar-shared/DEVELOPMENT.md`** — read it
> before your first run. This is the project-local copy of the loop rule.

Run the 25 autonomously. Exactly **two** things break the loop early, and only
one of them ends it:

1. **A question for the user.** Direction, an irreversible change, a fact only
   they can supply. Stop, ask, wait.
2. **Needing the real Jaguar — this does NOT stop the loop.** Queue it and
   carry on:

   ```sh
   jagq run <rom> --frames 3   # queue → flash → capture → hand back
   jagq status                 # who holds it, who is waiting
   ```

   `jagq run` blocks until your turn arrives, so *waiting for the rig is part of
   the run*, not a reason to end it. Exit 0 = ran; 3 = job failed
   (`jagq log <id>`); 4 = broker down (`jagq up`).

⭐ **The arbiter is now `jagq`, not `jaghw`** (user's call, 2026-08-17).
`jag_gd.sh` keeps working unchanged — `hw/jaghw` forwards to `jagq`, and `jagq`
exports `JAGHW_HELD` as well as `JAGQ_HELD` so its re-entrant nesting still
composes. Two things that were this project's problem are now the broker's:

- **The turn cap is enforced centrally** — 300 s for every lease, session token
  and hold, whatever the caller asks for.
- **The capture card is inside the exclusion.** `climb_matrix.sh:110`'s five
  acquisitions with the settle and the `grab` *unlocked* — the shape that
  photographs another project's frame — cannot happen through `jagq run`, which
  flashes and captures inside one turn. Worth re-reading that loop against
  `jagq run --capture`.

See the migration section at the top of `jaguar-shared/hw/PROTOCOL.md`.

---

## NEXT STEP

# ✅ PIPELINE VERIFIED TO THE END OF CAVES.JV — ⬜ THE LEVEL DOES NOT COME UP
# (user, 2026-08-19, on the fixed build from the CARD)

Startup → EIDOS → CORE → INTRO → title ring → Start Game → **CAVES.JV plays to
the end** → **no Caves level**. Everything up to and including the pre-level
cinematic is good; the hand-off into the level is where it stops.

### ★ FIRST SUSPECT, AND IT IS MINE: `CAVSLOAD.DAT` IS NOT ON THE CARD
The Caves loading screen STREAMS from SD, and the order is
CAVES.JV → loading screen → level. I restored the card after it was wiped and
wrote OPENLARA.COF + EIDOS/CORE/INTRO/CAVES.JV + MUSIC.PCM + GYMLOAD.DAT, but
**not CAVSLOAD.DAT** — `build_cof.sh` only ever emits GYMLOAD.DAT, so the gap
was inherited. It is now generated from the disc's AZTECLOA.RAW (76,800 B, in
`SD_RESTORE/CAVSLOAD.DAT`) and the push was cancelled mid-queue at the rig
handover. **One push, do it first.**
☠️ Caveat, so nobody over-reads this: main.c:7444 says a failed loading-art
read "falls through to the text panel", so a missing file SHOULD degrade, not
hang. If the level still does not come up with the file present, the lead is
dead and the level-load path itself is next.

### ✅ WHAT THE SAME RUN PROVED
  * the FMV fix holds on the shipping build, from the CARD, all four clips
  * title ring renders and takes input
  * ⬜ the title MUSIC CUTS OUT when stepping between ring options (user) —
    MUSDIAG is built for exactly this: `musdiag_p{136,0,272,408}.cof`,
    panel rows[5] = musgap<<24 | musloop<<16 | musfill



# ☠️☠️☠️ **THE FMV CORRUPTION IS A jcc68k MISCOMPILE — NOT THE CONSOLE, NOT THE
# CODE, NOT THE ASSETS.** Found 2026-08-18 by the USER'S hypothesis ("it worked
# before the automation"), which three sessions of bus-contention theory had
# talked past. The user nearly discarded a working $600 Jaguar over run 12's
# "marginal console" write-up. That write-up was WRONG.

### THE BRACKET (all measured the same night, same console, same card, same clip)
    demo30_p0.cof (2026-08-12)                                   0.00%   job 436
    same C code @ 3aded3d, today's toolchain                    19-21%   job 485
    same C code + demo30's OWN assets, today's toolchain        19-21%   job 498
    same C code + today's assets + jcc68k @ 84d9fc2 (07-27)      0.00%   job 506
  => code, assets, flags, kernels and the assembler are ALL innocent.
  => the ONLY variable that moves it is the jcc68k revision.

### THE FIX
    JCC68K=/home/jvilla/Documents/Git/jag_openlara/jcc68k-0727   (cobweb 84d9fc2)
  good = 84d9fc2 (2026-07-27)   bad = 59e5896 (2026-08-16)
  jcc68k compiles EVERY C TU except main.c - including video.c (OP list, flip
  protocol, ISR repair values), gpu.c and blit.c. Prime suspects in the window:
  b21f1e1 "thirteen wrong-code fixes" and beb2c15 "an odd-sized global sent
  every runtime helper to an odd address" (odd alignment = hardware-only fault).
  ⬜ Bisect 84d9fc2..59e5896 to name it, then file in COBWEB_ISSUES_OPENLARA.md.
  ⬜ Pin JCC68K in the Makefile AND COBWEB_REV in the Dockerfile.

### ☠️ WHY EVERY OFFLINE GATE PASSED FOR 13 RUNS
  jsim EXECUTES THE MISCOMPILED BINARY. An emulator cannot disagree with the
  compiler that fed it. Every check agreed with itself while the TV showed
  static. ⭐ The transferable rule: when hardware and emulator disagree and the
  SOURCE is identical, suspect the TOOLCHAIN before the code.

### ☠️ FALSE INSTRUMENT THAT COST THREE SESSIONS
  A flat field scores 0.00% on a local-median metric whether or not the fault
  is present (run 7 proved it). Run 6's "STOP fixed it 54.65% -> 0.00%" was
  measured on a flat test card and is therefore UNPROVEN. Tonight the same trap
  scored a flat YELLOW HANG SCREEN as "perfectly clean" - caught only because
  the user looked at the TV and said "I just see a yellow screen".

### ☠️ THREE HYPOTHESES KILLED ON SILICON (do not re-run them)
  OP scaler TYPE-1 vs TYPE-0     11.59% vs 11.15%   no effect
  OP fetch 240 vs 120 lines      ~20%   vs ~20%     no effect (120 = what the
                                                    GAME asks, and the game is
                                                    clean - so fetch is not it)
  cartridge streaming off        19.7%  vs 20.2%    no effect



# ★★★★★ **THE CONSOLE IS EXONERATED** - the user swapped in a DIFFERENT JAGUAR
# and reported "same issue", and the sweep agrees. ⏸ A different GAMEDRIVE is
# now in, but it does NOT enumerate on USB yet.

### ✅ THE NEW-CONSOLE SWEEP (job 315, full shipping build with video, pad 136)
    t=12s 54.8% · 18s 57.4% · 24s 55.2% · 30s 43.0% · 36s 43.3% · 42s 38.4%
    t=48s 37.4% · 54s 36.2% · 60s 43.4% · 66s+ NO VIDEO SIGNAL
Corrupt across the whole first minute, same appearance as the old console.
⇒ **not the console.** (User, verbatim: *"Same issue, different Jaguar."*)
☠️ It also drops the video signal entirely at ~66 s - worth chasing separately;
1 KB grabs are NO SIGNAL, not a black picture.

### ☠️☠️ AND I MUST WITHDRAW THE "IT CLEARS AT 70 s" RESULT
Job 299's clean frames came from `sg1024_p272.cof`, which carried the run-8
**object-shape regression** (plain TYPE-0 instead of scaled TYPE-1). So "clean
at t=70 s" cannot be attributed to time - it may have been the regressed object
shape rendering *correctly*, which would be a finding of the opposite kind.
⬜ **THAT IS NOW A LEAD, NOT A RETRACTION:** if a TYPE-0 plain object renders
CLEAN where the shipping TYPE-1 scaled object renders corrupt, the fix is to
stop displaying the clips through the game's scaled object - which is exactly
what the user suspected on their first question ("a high video mode so you
don't need to switch"). Re-test it DELIBERATELY, at one pad, with the mask.

### ⬜ RUN 14 STARTS HERE
1. ⏸ **The new GameDrive is not enumerating** - `jagq` says
   `GameDrive OFFLINE (03eb:800e)`, `lsusb` shows zero matches, and a
   `jagpower cycle` did not bring it back. Reported to the user; do not hammer
   the link. `jagq status` will show it when it returns.
2. ⬜ **The TYPE-0 vs TYPE-1 A/B, done properly.** `g_op_plain240` is now
   correctly declared and gated on `JVDECMASK` bit 16. Build masks 0 and 16 at
   ONE pad (they differ by one byte) and capture both at the SAME delay.
   This is the single most promising open lead and it is one turn.
3. ⬜ Then the NOBOOTCLIPS "title early" test (`WORK_ROMS/nbc_p*.cof`) to
   separate the display from the FMV path.

### ✅ SOLID, CONSOLE-INDEPENDENT, ALREADY SHIPPED IN THE TREE
  * Two 68k DRAM spins fixed (pacing loop; `gpu_jvdec_wait`'s 240,000-read
    mailbox poll). Offline gate: our ROM scores **5** where the pre-fix ROMs
    score 4216/6100, against a hardware budget of 128.
  * The FMV audio ring overran `g_arena` by 7,224 B every clip - fixed with a
    compile-time `sizeof(g_arena)` assert.
  * The jcc68k use-before-definition regression - fixed and filed upstream.
  * Pushed: cobweb `f377c95`; jaguar-shared `ff74328`, `674b403`, `be79e43`,
    `ee0e4d6`, `6c278b4`.
  * ☠️ Method: never A/B across two PADTEXT values · never across two capture
    times · never judge a capture by file size · LOOK at every capture ·
    rebuild the last known-good commit FIRST · and **check the job log before
    reading a blank as an A10 miss** (a USB failure looks identical).

### ⬜ ALSO OPEN
  1. ✅ `r22_try_p408.cof` DID run - jagq job 125, 07:18 today. It is not a rig
     fault any more. But the grab landed 35 s in, which is still **inside the
     boot FMV**, so the bridge room was never photographed. ☠️ THE ROM IS GONE
     WITH /tmp - rebuild it, and this time either build it `FASTBOOT=1` or ask
     jagq for a longer settle, because 35 s does not clear the clips.
  2. The 8 untestable door seats; `HW_TESTCARD`.

### ⭐ jaguar-shared MOVED - re-read on the user's instruction (2026-08-18)
14 new commits. The four that touch this project:
  * **`hw/POWER.md`** - the Kasa cycle is now a shared, documented PROCESS, and
    the driver moved to `hw/jagpower`. Use
    `jagq exec --lease 120 -- ~/Documents/Git/jaguar-shared/hw/jagpower cycle`
    rather than this repo's `./jag_power.sh`, and walk its "is it actually
    hung?" table first - reaching for the plug too early has cost sessions their
    turn.
  * **"a wedged USB link looks like your own bug"** - SIX turns failed across
    FIVE projects on 2026-08-17 before anyone said so. That is exactly what my
    `exit -6` and "GameDrive not found" were; jag_viewpoint's reboot failed in
    the same minute. **Check `jagq history` across projects before blaming your
    ROM or re-queueing.**
  * **"one clone per project" + "a foreign commit in your clone can invalidate
    every number you have"** - jag_quake's cobweb had a stray commit in the
    OBJECT PROCESSOR model, the part of jsim that sets display bus load. ✅
    CHECKED OURS: `jag_openlara/cobweb` is 12 behind origin/main, **0 ahead,
    clean** - no stray, so this run's fps numbers stand. ⭐ The rule to carry:
    after ANY shared-instrument change (cobweb pull, jas fix, new jagemu),
    RE-MEASURE THE BASELINE before trusting a comparison against it.
  * **video: the OP-list rebuild must follow the field WRAP** (jag_rr, measured
    on hardware) and **jsim reports PAL while the rig is NTSC**. Not our symptom,
    but the same family as the FMV grain: a display-path defect the emulator
    cannot show.
★ A LEAD FOR THE GRAIN FROM THAT TABLE: "per-field gate compares raw VC - bit 11
is the FIELD flag, so it misses every second field and the capture AVERAGES the
two". `gpu_jvdec.gas` writes the BACK buffer only and re-applies the previous
frame's tokens to keep the pair coherent. If that re-apply is not exact, buffers
A and B differ by the blocks that changed two frames ago - and a display
alternating between them is speckle on a CRT and in a capture, while jagemu
grabs ONE buffer and looks clean. **Test offline first: grab consecutive
published frames during the FMV and see whether the picture alternates between
two versions.**

### ☠️ THE RIG, LEARNED THE HARD WAY THIS RUN
  * **`jaggd ... exit -6` means the USB link is WEDGED, not that the ROM is
    bad.** It began right after a `--no-reboot` turn left a ROM running and
    another upload went in on top of it. `jagq reboot` did NOT clear it.
    What did: `jagq exec --lease 120s -- ./jag_power.sh cycle` (the Kasa plug),
    then the very next upload succeeded. ★ Take the power cycle UNDER A LEASE -
    raw `jag_power.sh` yanks the board out from under whoever holds it.
  * The rig is genuinely busy now - five projects, a real queue. `--no-wait`
    plus `jagq status` beats blocking on `jagq run`.

### ☠️ CLOSED - DO NOT REOPEN
    mansion holes (50-59) · pickups (65) · mid-walk LOADING (67) ·
    caves black wedges (69) · caves room crossing (72) · swimming IN (78) ·
    7 "dead doors" (74) · caves 11->12 + switch/door (79-82) ·
    all mansion door failures (85) · all Caves door failures (86-88) ·
    **mansion pool climb-out (93)** ·
    **Lara's Home soft-lock + "the mansion is silent" (116) - one bug, the
    slide travelled uphill; do not re-open either as an AUDIO question**

### ✅ WHAT IS DONE
    climbing   CAVES 24/24 ledges, 6/6 walls · MANSION 24/24, 6/6
    walking    CAVES 51 doors · MANSION 9 · zero defects either level
    swimming   enter/swim/room 18/renders · climb-out HALF FIXED (see above)
    switches   fire, animate, open the door, and she walks through
    enemies    BEAR and WOLVES render on shipping flags
    pickups    MEDIKIT_SMALL collected on contact, verified against a control
    frames     asserted by checkshot at BUILD time and at every driven capture
    rig        leases capped at 5 min; `jag_gd.sh endturn` reboots at turn end
    release    /tmp/cofout11 - 9/9 in the EMULATOR on BOTH levels; hw pending
    slopes     she slides DOWNHILL and faces the slope; mansion lock closed

### Toolchain — STILL PINNED to 59e5896 (`COBWEB_DIR=/tmp/cobweb-old`)
`beb2c15` does NOT fix the jcc68k regression (16-bit param read moved +2,
breaking gcc/jcc mixed links). `tools/toolchain_smoke.sh` guards updates.
⬜ cobweb moved to `8d09c43` (6 commits: jcc68k unsigned literal suffixes, a
jcc68k graphical smoke test, a jagemu profiling-coverage fix, the green-is-six-
bits bench). **None of them touch the 16-bit parameter ABI**, checked by log -
so the pin stands and taking the update buys nothing this project needs today.
ROMs: `/tmp/conf.cof` (Caves), `/tmp/gym.cof` (mansion, AUTOGYM, **no PADMUTE**).

### Instruments (all offline, no rig)
    conformance.py    driven per-spot sweep; best-height verdict, per-level
                      black baseline, jump drive for JUMPGRAB
    room_black.py     per-room black% baseline (caves 51.2 max, gym 36.3)
    floor_coverage.py collision-vs-mesh scan + patch (--prefix, --faces)
    mrt_boundary_audit.py  seam-floor audit + patch (--prefix)
    toolchain_smoke.sh     build a ROM and LOOK at it after a toolchain move

### ⬜ AWAITING THE USER (run-25 checkpoint)
  1. Capture card replugged? 2. Ship Lara's Home? 3. Release or keep polishing?
Nothing pushed to `origin` (public `tr1-jaguar`).

---

## WHAT CHANGED IN RUNS 9-11 (2026-08-16)

- ✅✅ **Release recipe proven end to end** with all four videos, both guards, and
  a real SD-card boot in jagemu.
- ✅ **Title-music refill fixed** to read the DRAM mailbox and stamp stale —
  correct per the codebase's own law, but **offline-unverifiable**; needs silicon.
- ✅ **Filed a real emulator divergence** to jaguar-shared: jagemu lets the 68k
  read a running core's local SRAM honestly, so this whole bug class is invisible
  offline. Asked for a counter, not a behaviour change, because other projects
  read those ranges today.
- ✅ **`session_run.sh end` guards against mid-build commits** — hit for real with
  three of four videos converted.
- ★ Lesson worth keeping: *a fix that produces no measurable change is not
  automatically wrong* — but it is also not confirmed. Say which one it is.

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

- ✅✅ **THE CAPTURE CARD IS BACK** (verified 2026-08-17) — this answers run-25
  checkpoint question #1. `/dev/video0` and `/dev/video1` both enumerate, and a
  grab through the broker returned the **GameDrive menu listing our own
  `OPENLARA.COF`, `CAVSLOAD.BIN`, `GYMLOAD.BIN`, `INPUT.BIN`** off the SD card —
  183 KB PNG, where a no-signal frame is ~1 KB. **The `WORLDCOUNT` framebuffer
  diagnostic is viable again**, and rig time no longer produces a picture only a
  human can read. The 2026-08-16 "physically unplugged" finding is superseded.
  ⭐ What survives from it: there is still **no channel to read a number
  *directly* off the board** — no jaggd memory-read, no GD BIOS FWRITE, bulk IN
  times out, CDC has no Jaguar-side write path. Telemetry still has to be
  painted into the framebuffer and read off a capture; the difference is that
  the capture now works.
- **The rig is shared, and arbitrated by `jagq`.** A session not on the roster
  is **queued, not refused**. `jaguar-shared/hw/RESOURCES.md` is the agreement,
  `hw/GD_ACCESS.md` is this project's gateway, and
  `/home/jvilla/Documents/Git/jaguar-shared/DEVELOPMENT.md` is the working
  agreement over both.
- **Batch what needs the console.** Items #2, #8, #10, #11 plus visual
  confirmation of the enemy skins and the mansion are ONE console session, not
  six — now bounded by the 5-minute turn, so batch only what fits in a turn.
- Claim the rig **only** for work the emulator genuinely cannot answer.
