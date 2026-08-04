# Reference footage — PS1 Tomb Raider (in `res/`)

Two ~24 min PS1 playthroughs the user supplied 2026-08-01, 642x480 @ 25 fps.
These are the **authority** for how the real game looks; prefer them over
memory or prose when deciding what a screen or a room should look like.

    res/Let's Play Tomb Raider (PS1) Part 1 [gHjTsxfx18A].mp4   front end + Lara's Home
    res/Let's Play Tomb Raider (PS1) Part 2 [nWfx8-5brqc].mp4   Level 1 (Caves)

Pull a frame with:

    ffmpeg -ss <seconds> -i "res/<file>.mp4" -frames:v 1 -y out.png

## Part 1 — front end and Lara's Home

| t (s) | what |
|-------|------|
| 0-90    | Eidos / Core logos, then the FMV intro |
| 120     | FMV: Lara in the plane / goggles |
| 230     | **Title ring, "Game" selected**, `Select` prompt bottom-left (VERIFIED) |
| **250** | **CONTROLS PAGE** — see below (VERIFIED exactly) |
| 260-280 | **Title ring, "Lara's Home" selected** — exact label + ring position (VERIFIED) |
| ~290    | **LOADING screen with progress bar** (follows selecting Lara's Home) |
| 300-330 | **DEMO MODE** attract loop — `Press any button to exit` top-left |
| 390-420 | House: main hall, stained-glass windows, organ |
| 450-480 | Library — bookshelves, reading desk |
| 510     | Upper gallery / landing |
| 540     | Corridor to the **pool** (blue water) |
| 570     | Stairwell, looking up |
| 630     | **Music room — grand piano** |
| 690-750 | Gym gallery, balcony rail, chequered-floor hall |
| 810     | Dark room with chandelier |
| 870-930 | **Gym** — vaulting horse, beams, training course |

Also in Part 1 at ~65 s: the **loading screen** — the mansion exterior with the
fountain, the word `LOADING`, and a **red-on-black progress bar** across the
bottom. That is the model for task #7.

### The Controls page (t=250, verified full-res)

★ It is an **OVERLAY on a DIMMED title screen**, not a separate screen — the
TOMB RAIDER logo, the ring and Lara's face all stay visible behind it, heavily
darkened. Build it as a draw-on-top pass over the existing title, which is much
cheaper than a new screen.

Layout: a centred header with left/right arrows, then binding rows where the
LEFT label is right-aligned and the RIGHT label is left-aligned, with the pad
glyph(s) between them in the middle column:

              <- Control Method 1 ->

      Step Left  [L2]  [R2]  Step Right
           Look  [L1]  [R1]  Walk
    Draw Weapon  [/\]
           Jump  [ ]   ( )   Roll
                 (X)   Action

    (X) Select                (O) Go Back
              Controls

`Select` bottom-left and `Go Back` bottom-right are the standard page footer —
the passport page uses the same pair. `Controls` is centred below them.
Glyphs are PS1 shapes; ours must be the **Jaguar pad** instead (task #6).

## Part 2 — Level 1 (Caves)

| t (s) | what |
|-------|------|
| 60      | Cutscene: carved stone face |
| 120     | **Cutscene: Lara's face, letterboxed** — best head/hair reference |
| 180     | Snow canyon, her footprints in the snow |
| 240     | Jump up to the ledge |
| 300     | The dark cave mouth |
| 360     | Narrow snow canyon (good full-body rear view of the model) |
| 420     | First large rocky cavern |
| 480-540 | Vines, first stone masonry |
| 600-660 | **The wooden bridge over the drop** |
| 720-840 | Vine walls, stone corridors |
| 900     | Temple room, carved wall pattern |
| 960-1020| Vine-covered doorway, water |
| 1080-1200 | Stone rooms, vine stairs, temple interior |
| 1260-1380 | Long corridor, pillared room, carved end wall |

## What the footage settles

- **Her hair is a smooth, CLOSED dark teardrop from behind** — no light patch
  anywhere inside its silhouette. Ours (LHEAD4) still shows a pale vertical bar
  through the middle of the hair; that is the HEADFILL hull glaring through, and
  the modal-texel fix for it is in `tr2jag_multiroom.py` but was **not in the
  flashed LHEAD4**. Compare with `cmp3.png` in the session scratchpad.
- The ring labels are exactly `Game` and `Lara's Home`, with `Select` as the
  bottom-left prompt (tasks #3/#4).
- Lara's Home is one connected mansion: hall, library, music room, pool,
  gallery, chequered hall, gym. Useful for judging how much of `gym_*` we
  actually reproduce.
- Level 1's route is canyon -> ledge -> cave -> cavern -> **wooden bridge** ->
  vine/stone rooms -> temple. The bridge is a good landmark for judging how far
  our progression really gets (task #9).
