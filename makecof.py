#!/usr/bin/env python3
"""
makecof.py - wrap a raw binary in a Jaguar COFF header.

Produces the .cof layout that Virtual Jaguar, BigPEmu and the Skunkboard
jcp loader all understand: a 20-byte file header, 28-byte a.out-style
optional header, three section headers (.text/.data/.bss), then the raw
image at file offset 0xA8, loaded contiguously at --addr.

usage: makecof.py input.bin output.cof [--addr 0x4000] [--entry 0x4000]
"""
import argparse
import struct
import sys

COFF_MAGIC = 0x0150          # m68k COFF
AOUT_MAGIC = 0x0107
F_FLAGS = 0x0003             # no relocs, executable
STYP_TEXT = 0x0020
STYP_DATA = 0x0040
STYP_BSS = 0x0080
HDR_SIZE = 20 + 28 + 3 * 40  # 0xA8


def section(name, paddr, vaddr, size, scnptr, flags):
    return struct.pack(">8sLLLLLLHHL",
                       name.encode("ascii"), paddr, vaddr, size,
                       scnptr, 0, 0, 0, 0, flags)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--addr", type=lambda v: int(v, 0), default=0x4000)
    ap.add_argument("--entry", type=lambda v: int(v, 0), default=None)
    ap.add_argument("--bss", type=lambda v: int(v, 0), default=0,
                    help="bss size (informational; startup clears bss itself)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        image = f.read()
    if len(image) & 1:
        image += b"\0"

    entry = args.entry if args.entry is not None else args.addr
    tsize = len(image)
    dsize = 0
    bss_addr = args.addr + tsize

    filehdr = struct.pack(">HHLLLHH",
                          COFF_MAGIC, 3, 0, 0, 0, 28, F_FLAGS)
    # rln writes the a.out magic as a 32-bit 0x00000107 (no vstamp);
    # BigPEmu checks it that way. Match rln byte-for-byte.
    aouthdr = struct.pack(">LLLLLLL",
                          AOUT_MAGIC, tsize, dsize, args.bss,
                          entry, args.addr, bss_addr)
    sections = (
        section(".text", args.addr, args.addr, tsize, HDR_SIZE, STYP_TEXT) +
        section(".data", bss_addr, bss_addr, 0, HDR_SIZE + tsize, STYP_DATA) +
        section(".bss", bss_addr, bss_addr, args.bss, 0, STYP_BSS)
    )

    with open(args.output, "wb") as f:
        f.write(filehdr + aouthdr + sections + image)

    print(f"{args.output}: {tsize} bytes at ${args.addr:06X}, "
          f"entry ${entry:06X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
