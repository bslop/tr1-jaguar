/* Embedded multi-room (Caves 0,6,4,2,5) shared atlas + per-room geometry
 * and sector blobs. Phrase-aligned (Blitter atlas + GPU DRAM reads). */
	.section .rodata
	.balign 8
/* LEVSD=1: BOTH level sets live on the CARD, not in the image.
   ☠️ GYMSD below moves only the mansion, and on its own that is a NET-ZERO
   DRAM WIN: you delete 254 KB of rodata and then need a 254 KB buffer to load
   it into, and the Caves cannot be that buffer because they are ROM-resident
   and cannot be reclaimed. The saving only exists when BOTH sets are on the
   card sharing one arena sized for the larger - 754,400 B - which frees the
   smaller, 254,400 B, and takes ~1 MB out of the ROM image.
   ★ That ROM saving stands on its own: `jaggd -ux` was measured failing at
   about 1.6 MB and this ROM is 1.47 MB, which is why the GameDrive wedges on
   roughly every other flash. ~0.5 MB clears the cliff.
   ☠️ mrt_lskin is DELIBERATELY OUTSIDE this guard - see its note below. */
#ifndef LEVSD
	.globl mrt_index
mrt_index:
	.incbin "disc/mrt.bin"
	.balign 8
	.globl mrt_geom
mrt_geom:
	.incbin "disc/mrt_geom.bin"
	.balign 8
	.globl mrt_sect
mrt_sect:
	.incbin "disc/mrt_sect.bin"
	.balign 8
	.globl mrt_atlas
mrt_atlas:
	.incbin "disc/mrt_atlas.bin"
	.balign 8
	.globl mrt_pal
mrt_pal:
	.incbin "disc/mrt_pal.bin"
	.balign 8
	.globl mrt_lara
mrt_lara:
	.incbin "disc/mrt_lara.bin"
	.balign 8
#else
	.balign 8
	.globl mrt_index
mrt_index:
	.long 0
	.globl mrt_geom
mrt_geom:
	.long 0
	.globl mrt_sect
mrt_sect:
	.long 0
	.globl mrt_atlas
mrt_atlas:
	.long 0
	.globl mrt_pal
mrt_pal:
	.long 0
	.globl mrt_lara
mrt_lara:
	.long 0
	.balign 8
#endif
/* ☠️ mrt_lskin STAYS RESIDENT UNDER LEVSD. It is Lara's skeleton - level
   independent - and gym_lskin is a .set alias onto it, so putting it in a
   per-level card image would duplicate 110,016 B into BOTH images and then
   load whichever copy happened to be current. It is shared data; it does not
   belong in a per-level arena. */
	.globl mrt_lskin
mrt_lskin:
	.incbin "disc/mrt_lskin.bin"
	.balign 8
	.globl title_img
title_img:
	.incbin "disc/title.bin"
	.balign 8
	.globl title_pal
title_pal:
	.incbin "disc/title_pal.bin"
	.balign 8
	.globl pass_geom
pass_geom:
	.incbin "pass_geom.bin"
	.balign 8
	.globl pass_atlas
pass_atlas:
	.incbin "pass_atlas.bin"

#ifdef PASS2ALIAS
| DIAGNOSTIC: alias the opened passport onto the closed one's data instead of
| linking its own 81 KB.  Every line of the new title code still builds and
| runs; only the ROM size goes back to what the last BOOTING build had.  It
| renders wrong on purpose - it exists to split "the image got 81 KB bigger"
| from "the new code runs" as the cause of a silicon boot hang.
	.globl pass2_geom
	.globl pass2_atlas
	.set pass2_geom, pass_geom
	.set pass2_atlas, pass_atlas
#else
	.balign 8
	.globl pass2_geom
pass2_geom:
	.incbin "pass2_geom.bin"

	.balign 8
	.globl pass2_atlas
pass2_atlas:
	.incbin "pass2_atlas.bin"
#endif
	.balign 8
	.globl ctrl_geom
ctrl_geom:
	.incbin "ctrl_geom.bin"

	.balign 8
	.globl ctrl_atlas
ctrl_atlas:
	.incbin "ctrl_atlas.bin"

	.balign 8
	.globl photo_geom
photo_geom:
	.incbin "photo_geom.bin"
	.balign 8
	.globl photo_atlas
photo_atlas:
	.incbin "photo_atlas.bin"
| INV_SOUND (model 96, TITLE.PSX) - the Sound ring item (task: 4th launch item)
	.balign 8
	.globl sound_geom
sound_geom:
	.incbin "sound_geom.bin"
	.balign 8
	.globl sound_atlas
sound_atlas:
	.incbin "sound_atlas.bin"
| type 95 "Screen Adjust" (PSX title sunglasses)
	.balign 8
	.globl detail_geom
detail_geom:
	.incbin "detail_geom.bin"
	.balign 8
	.globl detail_atlas
detail_atlas:
	.incbin "detail_atlas.bin"
	.balign 8
	.globl font_load
font_load:
	.incbin "font_load.bin"
	.balign 8
	.globl sfx_bank
sfx_bank:
	.incbin "sfx.bin"
	.balign 8
	.globl music_bank
music_bank:
	.incbin "music.bin"
	.balign 8
	/* loading-screen IMAGES stream from SD (CAVSLOAD/GYMLOAD.BIN -
	   76800B each was the difference between fitting the 2MB budget
	   and smashing the 68k stack, 2026-08-06). Palettes stay resident. */
	.globl gymload_pal
gymload_pal:
	.incbin "disc/gymload_pal.bin"
	.balign 8
	.globl cavesload_pal
cavesload_pal:
	.incbin "disc/cavesload_pal.bin"
	.balign 8
/* GYMSD=1: Lara's Home lives on the CARD, not in the image.
   ☠️ 316,336 BYTES of gym_atlas/geom/sect/index/pal were linked into every
   CAVES ROM - a level that is never in memory at the same time as the caves.
   That single block is larger than everything the budget has been fighting
   over all session (the wolf fur, the medikit sprites, the collapsing floor
   and a gun model, several times over). The GameDrive is a mass storage
   device; treat it like one. */
#if !defined(GYMSD) && !defined(LEVSD)
	.globl gym_index
gym_index:
	.incbin "disc/gym.bin"
	.balign 8
	.globl gym_geom
gym_geom:
	.incbin "disc/gym_geom.bin"
	.balign 8
	.globl gym_sect
gym_sect:
	.incbin "disc/gym_sect.bin"
	.balign 8
	.globl gym_atlas
gym_atlas:
	.incbin "disc/gym_atlas.bin"
	.balign 8
	.globl gym_pal
gym_pal:
	.incbin "disc/gym_pal.bin"
	.balign 8
#else
	.balign 8
	.globl gym_index
gym_index:
	.long 0
	.globl gym_geom
gym_geom:
	.long 0
	.globl gym_sect
gym_sect:
	.long 0
	.globl gym_atlas
gym_atlas:
	.long 0
	.globl gym_pal
gym_pal:
	.long 0
#endif
/* ☠️ gym_lara used to sit OUTSIDE the GYMSD guard entirely, so a build that
   stubbed the rest of the mansion still carried its 8,568 B. Gated now. */
#ifndef LEVSD
	.globl gym_lara
gym_lara:
	.incbin "disc/gym_lara.bin"
	.balign 8
#else
	.balign 8
	.globl gym_lara
gym_lara:
	.long 0
	.balign 8
#endif
/* ☠️☠️ gym_lskin USED TO .incbin ITS OWN 110,016 B COPY - AND IT SAT OUTSIDE
 * THE GYMSD #endif, so every shipping ROM carried it even though GYMSD stubs
 * out the rest of the mansion. It is a pure duplicate: _lskin is "skeleton +
 * ALL-animation joint angles" (tr2jag_multiroom.py), which is Lara's SKELETON
 * and is therefore level-independent - a freshly extracted gym_lskin.bin is
 * BYTE-IDENTICAL to mrt_lskin.bin, and stays identical across TEXSCALE 2 vs 4
 * (no atlas dependence at all). The per-level binding that DOES differ lives
 * in gym_lara.bin, which is still its own blob below.
 * build_cof.sh cmp's the two files and FAILS THE BUILD if they ever diverge,
 * so this alias cannot silently render Lara wrong in the mansion. */
	.globl gym_lskin
	.set gym_lskin, mrt_lskin
	.section .note.GNU-stack,"",%progbits

/* jcc68k keeps the statically-dead single-room data references that
 * gcc -O2 eliminates under MULTIROOM (see Makefile OBJS comment); the
 * paths are unreachable at runtime, so empty stubs satisfy the link. */
	.section .rodata
	.globl	rooms_data
	.globl	lara_data
rooms_data:
lara_data:
	.long	0
