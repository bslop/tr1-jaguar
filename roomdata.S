/* Embed the converted TR1 room geometry (big-endian) produced by
 * tools/tr2jag.cpp. The 68k reads it in place from ROM/DRAM. */
	.section .rodata
	.balign	4
	.globl	room_data
	.globl	room_data_end
room_data:
	.incbin	"room.bin"
room_data_end:

	.section .note.GNU-stack,"",%progbits
