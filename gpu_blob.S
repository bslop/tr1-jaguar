/* Embed the assembled minimal GPU kernel (raw blob, origin $F03000).
 * Built by rmac from gpu_hello.gas; the 68k copies it into GPU SRAM. */
	.section .rodata
	.balign	4
	.globl	gpu_kernel
	.globl	gpu_kernel_end
gpu_kernel:
#if defined(GEOTEX) || defined(MULTIROOM)
	.incbin	"build/gpu_geotex.bin"
#elif defined(BLTEX)
	.incbin	"build/gpu_bltex.bin"
#elif defined(TEXTURED)
	.incbin	"build/gpu_textured.bin"
#elif defined(GEOMDIRECT)
	.incbin	"build/gpu_geomdirect.bin"
#elif defined(GEOMXFORM)
	.incbin	"build/gpu_geomxform.bin"
#elif defined(GEOMWALK)
	.incbin	"build/gpu_geomwalk.bin"
#else
	.incbin	"build/gpu_spanfill.bin"
#endif
gpu_kernel_end:

	.section .note.GNU-stack,"",%progbits
