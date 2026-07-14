/* Embed the multi-room geometry (big-endian) from tools/tr2jag.cpp. */
	.section .rodata
	.balign	4
	.globl	rooms_data
	.globl	rooms_data_end
rooms_data:
	.incbin	"rooms.bin"
rooms_data_end:

	.section .note.GNU-stack,"",%progbits
