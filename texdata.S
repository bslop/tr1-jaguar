/* Embedded texture test data: 256x256 8bpp atlas subset + 256-entry RGB16 palette. */
	.section .rodata
	.balign 8
	.globl tex_atlas
tex_atlas:
	.incbin "tex_test.bin"
	.balign 8
	.globl tex_pal
tex_pal:
	.incbin "texpal.bin"
	.section .note.GNU-stack,"",%progbits
