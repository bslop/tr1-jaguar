	.section .rodata
	.balign 8
	.globl dsp_ovl_ent
dsp_ovl_ent:
	.incbin "build/dsp_ovl_ent.bin"
	.globl dsp_ovl_ent_end
dsp_ovl_ent_end:
	.section .note.GNU-stack,"",%progbits
