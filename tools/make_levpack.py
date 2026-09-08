#!/usr/bin/env python3
"""make_levpack.py - pack one level's blobs into a single card image.

WHY. Both level sets are linked into every ROM, and only one is ever live:
the Caves occupy 754,304 B while you are in Lara's Home and the mansion
occupies 254,296 B while you are in the Caves. Moving BOTH to the GameDrive
and loading whichever is selected into one shared arena sized for the larger
frees the smaller (254,296 B of DRAM) and takes ~1 MB out of the ROM image.

☠️ MOVING ONLY THE MANSION IS A NET-ZERO DRAM WIN, which is the trap this
file exists to avoid repeating: you delete 254 KB of rodata and then need a
254 KB buffer to load it into, and the Caves cannot serve as that buffer
because they are ROM-resident and cannot be reclaimed. The saving only
appears when both sets share one arena.

★ The ROM saving matters on its own. `jaggd -ux` uploads were measured
failing at about 1.6 MB (0 of 6 at or above, 203 of 327 below) and the ROM is
1.47 MB - 92% of that cliff, which is why the GameDrive wedges on roughly
every other flash and needs a power cycle. Dropping to ~0.5 MB clears it.

FORMAT. Big-endian throughout (68000). Every blob is 32-byte aligned in the
file so a DMA-friendly load into a 32-byte-aligned arena preserves the
alignment the .balign 8 directives in mrt_data.S rely on - and so nothing
lands 16-mod-32, which is the alignment class that caused the A10 blackouts.

    magic   'OLLV'                4
    version                       2
    nblob                         2
    per blob: offset, length      8 * nblob
    (pad to 32)
    blobs, each padded to 32

BLOB ORDER IS FIXED and matches the S_* assignment in main.c:
    0 index   1 geom   2 sect   3 atlas   4 pal   5 lara

☠️ mrt_lskin is NOT here. It is Lara's skeleton, level-independent, and
gym_lskin is a .set alias onto it - so it stays resident rather than being
duplicated into both card images.

    make_levpack.py <prefix> <discdir> <out.LEV>
"""
import os, struct, sys

BLOBS = ("", "_geom", "_sect", "_atlas", "_pal", "_lara")
ALIGN = 32


def pad(n, a=ALIGN):
    return (a - (n % a)) % a


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pfx, disc, out = sys.argv[1], sys.argv[2], sys.argv[3]

    parts = []
    for suf in BLOBS:
        # the index blob is `mrt.bin` / `gym.bin`, not `mrt_index.bin`
        name = "%s%s.bin" % (pfx, suf)
        path = os.path.join(disc, name)
        if not os.path.exists(path):
            sys.exit("!!! missing %s - cannot pack %s" % (path, pfx))
        parts.append((name, open(path, "rb").read()))

    hdr = 8 + 8 * len(parts)
    hdr += pad(hdr)
    off = hdr
    table = []
    for _, data in parts:
        table.append((off, len(data)))
        off += len(data) + pad(len(data))
    total = off

    with open(out, "wb") as f:
        f.write(b"OLLV" + struct.pack(">HH", 1, len(parts)))
        for o, l in table:
            f.write(struct.pack(">II", o, l))
        f.write(b"\0" * pad(8 + 8 * len(parts)))
        for _, data in parts:
            f.write(data)
            f.write(b"\0" * pad(len(data)))

    assert os.path.getsize(out) == total, "packer wrote a size it did not plan"
    print("   %s -> %s  %d B" % (pfx, os.path.basename(out), total))
    for (name, data), (o, l) in zip(parts, table):
        print("      %-18s %8d B  @ %d" % (name, l, o))
    return total


if __name__ == "__main__":
    main()
