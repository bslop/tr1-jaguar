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
        __data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        __data_end = .;
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

    /* PRISTINE .data SNAPSHOT - deliberately AFTER __bss_end, which is what
       makes it work: startup.S clears exactly __bss_start..__bss_end, so
       anything past that survives a soft reboot.
       ☠ WHY THIS EXISTS. _start clears .bss and CANNOT restore .data. A soft
       reboot re-enters it with .data still holding the previous run's values -
       516 bytes of function statics that are caches and "already done"
       trackers - so they end up pointing into a .bss that was just zeroed. The
       renderer then skips work it believes is staged, and assets disappear,
       worse on every restart (user, 2026-09-08: the loading picture went, then
       the world, then the palette). Snapshot on the first boot, restore on
       every later one. Sized from the section itself so it cannot go stale. */
    . = ALIGN(4);
    __data_save = .;
    . += 4 + (__data_end - __data_start);


    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
