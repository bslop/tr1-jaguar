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

/* JV02 boot-FMV decoder kernel (BOOTVID; loaded only while the logos
 * play, then the title's kernel_select reloads the renderer). */
	.balign	4
	.globl	gpu_jvdec_kernel
	.globl	gpu_jvdec_kernel_end
gpu_jvdec_kernel:
	.incbin	"build/gpu_jvdec.bin"
gpu_jvdec_kernel_end:

/* GPU-driven Blitter probe kernel (BLITPROBE builds use it; embedding it
 * unconditionally keeps this file free of extra flag plumbing). */
	.balign	4
	.globl	gpu_probe_kernel
	.globl	gpu_probe_kernel_end
gpu_probe_kernel:
	.incbin	"build/gpu_blitprobe.bin"
gpu_probe_kernel_end:

	.section .note.GNU-stack,"",%progbits

/* TITLE-HQ kernel (task #4): same gpu_geotex assembled with LOWRES=0
 * (240-line projection). The 68k uploads this during the TITLE phase and
 * re-uploads gpu_kernel when the game starts. */
	.section .rodata
	.balign	4
	.globl	gpu_kernel_hq
	.globl	gpu_kernel_hq_end
gpu_kernel_hq:
#if defined(MULTIROOM)
	.incbin	"build/gpu_geotex_hq.bin"
#endif
gpu_kernel_hq_end:
