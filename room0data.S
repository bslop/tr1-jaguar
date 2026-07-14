/* Embedded room 0 (Caves) textured geometry + compact atlas + palette. */
	.section .rodata
	.balign 8
	.globl room0_tex
room0_tex:
	.incbin "room0_tex.bin"
	.balign 8
	.globl room0_atlas
room0_atlas:
	.incbin "room0_atlas.bin"
	.balign 8
	.globl room0_pal
room0_pal:
	.incbin "room0_pal.bin"
	.balign 8
	.globl room0_sect
room0_sect:
	.incbin "room0_sect.bin"
	.section .note.GNU-stack,"",%progbits
