/* Embed Lara's baked-pose mesh (big-endian) from tools/tr2jag.cpp. */
	.section .rodata
	.balign	4
	.globl	lara_data
	.globl	lara_data_end
lara_data:
	.incbin	"laramesh.bin"
lara_data_end:

	.section .note.GNU-stack,"",%progbits
