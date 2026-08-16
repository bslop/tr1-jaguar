#!/usr/bin/env python3
"""extract_fmv.py — pull the FMV/ movies out of a raw-sector PSX disc image.

    python3 extract_fmv.py "<Track 01>.bin" <outdir> [NAME.FMV ...]

Walks ISO9660 on 2352-byte raw MODE2 sectors (user data at +24 for form 1),
finds the FMV directory, and dumps each movie as RAW 2352-byte sectors
(ceil(size/2048) of them) — the shape ffmpeg's STR demuxer accepts directly,
which is what tr2jag_video.py feeds on. Names default to every file in FMV/.
"""
import os, struct, sys

RAW = 2352
U0 = 24                     # user-data offset inside a raw MODE2 sector

def rd_sector(f, lba):
    f.seek(lba * RAW)
    return f.read(RAW)

def iso_dir_entries(f, lba, size):
    """Yield (name, lba, size, is_dir) from an ISO9660 directory extent."""
    left = size
    while left > 0:
        sec = rd_sector(f, lba)[U0:U0+2048]
        off = 0
        while off < 2048:
            ln = sec[off]
            if ln == 0:
                break
            ext_lba = struct.unpack_from("<I", sec, off+2)[0]
            ext_sz = struct.unpack_from("<I", sec, off+10)[0]
            flags = sec[off+25]
            nlen = sec[off+32]
            name = sec[off+33:off+33+nlen].decode("ascii", "replace")
            name = name.split(";")[0]
            if name not in ("\x00", "\x01"):
                yield name, ext_lba, ext_sz, bool(flags & 2)
            off += ln
        lba += 1
        left -= 2048

def main():
    dump(sys.argv[1], sys.argv[2], sys.argv[3:])


def dump(disc, outdir, want=()):
    """Dump the named movies (default: all) as RAW 2352-byte sectors.
    Importable so the disc extractor can call it while it still holds the
    normalized data track - that track lives in a temp dir it deletes."""
    want = [w.upper() for w in want]
    os.makedirs(outdir, exist_ok=True)
    f = open(disc, "rb")
    pvd = rd_sector(f, 16)[U0:U0+2048]
    assert pvd[1:6] == b"CD001", "no ISO9660 PVD at sector 16 (+24)"
    root_lba = struct.unpack_from("<I", pvd, 156+2)[0]
    root_sz = struct.unpack_from("<I", pvd, 156+10)[0]
    # FMV/ holds the cutscenes; MOVIES/ holds INTRO.STR, which OPENS WITH THE
    # EIDOS LOGO - so the whole front end comes off the user's own disc and
    # nothing has to ship with the repo. SRCDIR picks one (default: both).
    wantdirs = [d.upper() for d in os.environ.get("SRCDIR", "FMV,MOVIES").split(",")]
    dirs = []
    for name, lba, sz, isdir in iso_dir_entries(f, root_lba, root_sz):
        if isdir and name.upper() in wantdirs:
            dirs.append((name.upper(), lba, sz))
    assert dirs, "no FMV/ or MOVIES/ directory on this disc"
    n = 0
    for _dn, _dl, _ds in dirs:
     for name, lba, sz, isdir in iso_dir_entries(f, _dl, _ds):
        if isdir: continue
        if name in ("\x00", "\x01"): continue
        if want and name.upper() not in want: continue
        nsec = (sz + 2047) // 2048
        out = os.path.join(outdir, name.upper())
        with open(out, "wb") as o:
            for s in range(nsec):
                o.write(rd_sector(f, lba + s))
        print(f"{name}: lba {lba}, {sz} B ISO -> {nsec} raw sectors "
              f"({os.path.getsize(out)} B)")
        n += 1
    print(f"extracted {n} movies to {outdir}")

if __name__ == "__main__":
    main()
