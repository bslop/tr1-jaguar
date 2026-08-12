# CAVES — mechanical parity with the PS1, room by room

**The reference is `res/…Part 2…mp4`, 3m20 → 23m24** (the level, end to end —
dated by the stats screen at 23m24, never by "it started looking man-made").
See `reference_ps1_footage`. Graphics are deliberately scaled back; **mechanics
are not** (user, 2026-08-11: *"Mechanically speaking, it should match."*).

## THE MAP — our 38 rooms, BFS from Lara's spawn

Spawn: **room 0** at (75264, 3072, 3584). Every room is reachable; adjacency is
`mrt_adj`, contents from `mrt_spawn.h`.

| room | exits | contents | PS1 landmark |
|---|---|---|---|
| 0 | 1 | — | canyon start, 3m20 |
| 1 | 0,2 | 4x DART_EMITTER | |
| 2 | 1,3,4 | — | |
| 3 | 2,5,6 | 3x BAT | |
| 4 | 2,7 | — | |
| 5 | 3,8 | — | |
| 6 | 3,9 | — | |
| 7 | 4 | MEDIKIT_SMALL | |
| 8 | 5 | BAT, MEDIKIT_SMALL | |
| 9 | 6,10 | — | cave/cavern, ~5m00–7m40 |
| 10 | 9,11 | — | |
| 11 | 10,12 | **DOOR_4 + SWITCH** | first lever/door puzzle |
| 12 | 11,13,14 | BAT | |
| 13 | 12,15 | — | |
| 14 | 12,16,17 | **DOOR_1+DOOR_2**, 4x DART | stone double doors, ~9m23 |
| 15 | 13,18 | — | |
| 16 | 14,19,20 | — | |
| 17 | 14 | DOOR_1+DOOR_2 | |
| 18 | 15,21,22,23 | — | |
| 19 | 16,24 | **2x TRAP_FLOOR** | collapsing tiles |
| 20 | 16 | WOLF, MEDIKIT_BIG, SWITCH | |
| 21 | 18 | — | |
| **22** | 18,25 | **12x BRIDGE**, 2x WOLF | ★ **the wooden bridge, 10m20** |
| 23 | 18,26 | — | |
| 24 | 19,27 | WOLF | |
| 25 | 22,28 | DOOR_3 | |
| 26 | 23,29,30 | **CRYSTAL** | the blue savegame crystal, ~12m40 |
| 27 | 24,31 | 2x DART | |
| 28 | 25,32 | **BEAR**, MEDIKIT_SMALL | |
| 29 | 26,32 | — | |
| 30 | 26,33 | — | |
| 31 | 27,34 | — | |
| 32 | 28,29 | 2x BAT | |
| 33 | 30,35 | MEDIKIT_SMALL | |
| 34 | 31,35,36,37 | DOOR_1+DOOR_2, SWITCH | ivy rooms, ~15m40–17m40 |
| 35 | 33,34 | — | |
| 36 | 34 | 2x WOLF, MEDIKIT_SMALL | |
| 37 | 34 | MEDIKIT_BIG | temple, ~20m00–23m20 |

★ r22 (the bridge, 2 wolves) and r34/r36 (ivy) are also the level's **slowest
rooms** — see `project_room_fps_sweep`.

## MECHANICS — what the PS1 does vs what we have

Audited by reading `main.c`, not by grepping filenames.
☠️ The first pass of this audit used `grep -E` with BRE `\|` alternation and
reported **every mechanic ABSENT**. A broken test is not a result.

| mechanic | PS1 (video) | ours | state |
|---|---|---|---|
| run / walk / turn | throughout | `g_layaw`, tank controls | ✅ |
| jump (fwd/back/side) | 4m40, 7m20 | `g_jfwd`/`g_jdir`, 47 refs | ✅ |
| slide / skid on slopes | canyon 3m40–5m00 | 29 refs | ✅ |
| vault / pull-up | 6m40, 8m20 | `g_vault`, 22 refs | ✅ |
| ledge hang + grab | 7m20 | `GRABREACH`/`g_autograb`, 19 refs | ✅ |
| roll | — | `ROLL_TICKS`, 11 refs | ✅ |
| death + health | wolves ~8m00 | `g_dead`/`g_health`, 16 refs | ✅ |
| pistols fire | 20m00 | facing cone, TR1 hp, real SFX | ✅ |
| levers / switches | 9m26 | oriented plane-crossing test | ✅ |
| doors swing on hinges | 9m23–9m28 | hinged double panels | ✅ |
| bridges walkable | 10m20 | 12 pieces in r22 | ✅ |
| dart traps | 1, 14, 27 | 5 refs | ✅ |
| pickups | 15m20 | medikits = real sprites | ✅ |
| **collapsing floor** | r19 has 2 | **NO RUNTIME HANDLER** | ☠️ **GAP** |
| **draw / holster guns** | 20m00, guns in hand | fires, but **no gun model and no draw anim** | ☠️ **GAP** |

⇒ **Two mechanical gaps**, both already identified independently:
`TRAP_FLOOR` is the only entity type with no handler at all, and the guns are
hitscan with nothing in her hands. Everything else EXISTS; whether it *behaves*
like the PS1 is what the driven tests below are for.


## ★★★★★ WHAT THE TRIGGER TABLE SAYS — dug out of the data, not the video

`mrt_trig` (110 triggers) + `mrt_trigcmd` (147 words) are already extracted, so
switch-to-door pairing is TR1's real trigger data, not proximity — a switch can
open a door rooms away, and one does.

| door | room | fired by |
|---|---|---|
| e9 DOOR_4 | 11 | trig33 SWITCH in r11 |
| **e12 DOOR_1** | **17** | ☠️ **NOTHING** |
| **e13 DOOR_2** | **17** | ☠️ **NOTHING** |
| e28 DOOR_3 | 25 | trig99 PAD in r28 |
| e35/e36 DOOR_1+2 | 14 | trig61 SWITCH in **r20**, + ANTIPADs in r17 |
| e43/e44 DOOR_1+2 | 34 | trig108 SWITCH in r34 |

★ The r20 switch opening the r14 doors is confirmed real, and the r17 ANTIPADs
*close* them behind Lara — TR1's "the door shuts behind you" mechanic, already
in our data.
☠️ **e12/e13 in r17 are the only doors nothing fires.** r17's sole exit is back
to r14, so it is a sealed dead-end alcove. Whether TR1 opens them by a trigger
our extractor drops, or they are permanently shut, is the one thing here the
video has to settle — check what is behind the r14 doors at ~9m23.

## ⬜ THE SCRIPTED CAMERAS ARE EXTRACTED AND THEN THROWN AWAY

User: *"notice how the level starts and you have a view of Lara from the side.
We need this too."* Confirmed in the footage — **Part 2 @ 3m16** is a low
side-on shot of Lara in the doorway; by 3m22 it has settled into the normal
follow camera.

The level carries **12 camera commands** and we extract every one:

    CAMERA_TARGET  9   (8 of them a 2x4 block of sectors in ROOM 2, all
                        aiming at VIEW_TARGET e4 - a scripted view)
    CAMERA_SWITCH  3   (trig61 with the r20 switch, trig108 with the r34
                        switch - the classic cut to show the door opening)

Both `VIEW_TARGET` entities are in `mrt_ent` (e4 in r2, e41 in r14).

☠️ **`ent_fire` handles action 0 (ACTIVATE) and NOTHING ELSE.** CAMERA_SWITCH is
explicitly skipped (`else if (a == 1) k++;` — it only steps over the parameter
word) and CAMERA_TARGET falls through silently. So every scripted shot in the
level is parsed and discarded. The camera is always the follow camera.
⇒ **This is a rendering/camera job, not an extraction job** — the data is
already there.

## ⬜ STACKED ROOMS (from the plan view)
Six room pairs share a footprint at different heights: 14/16, 18/22, 23/32,
26/29, 33/35, 34/36. Five are adjacent; **23/32 is not** — worth a look, though
a corridor passing over another is legitimate TR level design.

## ⬜ THE DRIVEN TEST PLAN (not yet run)
★★★★★ **The game can be driven from the host**: `GDPAD=1` + `tools/gdpad.sh`
(masks: UP 1 DOWN 2 LEFT 4 RIGHT 8 A=jump 16 B=action 32 C=walk 64), and
`SPAWNAT_ROOM/X/Y/Z` + `FASTBOOT=1` puts Lara in any room in ~8s. So each
mechanic can be tested where the PS1 does it, and captured:

1. **skid** — spawn r2/r3 (canyon slopes), run downhill, compare slide distance
   and whether she keeps footing at the bottom.
2. **jump** — r4→r7 gap. Running jump distance vs the PS1's.
3. **pull-up** — r6/r9 ledges. Does she vault a 1-click step and hang-grab a
   2-click one at the same heights TR1 uses (step 256, vault 768, reach 720)?
4. **shoot** — r20 (1 wolf) and r36 (2 wolves). Hits-to-kill is already exact
   (wolf 6); what is untested is whether she auto-faces and stops to fire.
5. **death** — r22 (2 wolves) or r28 (bear). Damage rate and the death anim.
6. **bridge** — r22, walk the 12 pieces end to end without falling through.
7. **collapsing floor** — r19, currently nothing happens: the tiles are solid.
8. **level-start camera** — r0/r2, the side-on opening shot above.
9. **r17** — walk in through the r14 doors and confirm whether the alcove is
   meant to open.

☠️ `gdpad` writes INPUT.BIN over the control endpoint **while the game runs** —
never point it at a build that streams video or music from the cart, or the
write races its own `gd_fread`s and locks the console. FASTBOOT builds stream
neither.
