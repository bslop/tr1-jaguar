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
        /* ☠️☠️☠️ op_list IS PLACED HERE, BY THE LINKER, ON PURPOSE (2026-08-26).
           The OP fetches a SCALED (TYPE-1) object as one 4-phrase = 32-BYTE
           burst and DIES if it straddles a 32-byte boundary — black field,
           wedged machine. That is the whole A10 / PADTEXT boot lottery.
           It cannot be expressed in C here: jcc68k emits `.align 32`
           correctly, but jas does not honour it in .bss AND leaves the
           section's sh_addralign at 16, so the linker may still land us at
           16 mod 32. Defining the symbol in the LINK SCRIPT makes 32-byte
           alignment an invariant no source or layout change can re-roll.
           64 bytes = uint32_t[16] (list A at +0, list B at +32 under OPDBL).
           ☠️ Keep this BEFORE *(.bss) and do not move it into C. */
        . = ALIGN(32);
        op_list = .;
        . += 64;
        . = ALIGN(16);
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    . = ALIGN(4);
    __bss_end = .;

    /* WARM-BOOT COOKIE - deliberately AFTER __bss_end, which is what makes it
       work: startup.S clears exactly __bss_start..__bss_end, so anything past
       that survives soft_reboot()'s re-entry into _start. The user asked for
       * + # to restart to the Menu/Title screen rather than sit through the
       logos, and the reset has to stay a real reset (Tom and Jerry halted,
       .bss cleared) -- so the request to skip the front end cannot live in a
       variable the reset destroys.
       ☠️ A cold power-on leaves DRAM as garbage, so the reader MUST check the
       magic and never merely a non-zero flag.
       ⚠ This sits above __bss_end, so the Makefile's DRAM guard - which
       measures headroom from __bss_end - now reports 8 bytes more than is
       really free. Of ~102 KB. */
    . = ALIGN(4);
    warm_cookie = .;
    . += 8;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
