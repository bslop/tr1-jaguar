/*
 * jaguar.ld - link a Jaguar RAM executable at $4000
 *
 * The Jaguar has 2MB of DRAM at $0. Vectors and loader scratch occupy
 * the bottom; $4000 is the conventional homebrew load address. The
 * stack is set to $200000 (top of DRAM) by startup.S.
 */
OUTPUT_ARCH(m68k)
ENTRY(_start)

SECTIONS
{
    . = 0x4000;

    .text : {
        *startup.o(.text)       /* _start must sit at the entry address */
        *(.text)
        *(.text.*)
    }

    .rodata : {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        . = ALIGN(4);
        *(.data)
        *(.data.*)
    }

    . = ALIGN(4);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    . = ALIGN(4);
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
