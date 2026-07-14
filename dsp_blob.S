	.section .rodata
	.balign 8
	.globl dsp_kernel
dsp_kernel:
	.incbin "build/dsp_pose.bin"
	.globl dsp_kernel_end
dsp_kernel_end:
	.section .note.GNU-stack,"",%progbits
