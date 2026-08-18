#!/usr/bin/env python3
"""jv_decode.py - decode a JV04 (VQ) clip back to PNG frames, and score it.

    tools/jv_decode.py EIDOS.JV --out /tmp/eidos_dec [--limit 40]
    tools/jv_decode.py EIDOS.JV --ref /tmp/fmvsrc/FMV/INTRO.STR   # + PSNR

WHY: the encoder printed a byte rate and nothing else, so every quality
judgement about the front-end clips has been "it looks grainy" against a
memory of the PSX original. This decodes what the Jaguar will actually put on
screen - same palette, same 4x4 codebook, same skip tokens - so an encoder
change can be scored instead of argued.

☠️ THE DECODER IS THE ENCODER'S MIRROR, NOT THE KERNEL'S. It reproduces the
stream semantics (skip = keep the previous frame); if the on-Jaguar player
diverges from this, that divergence is itself the bug and this file is where
to prove it.
"""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

A = sys.argv
SRC = A[1]
OUT = A[A.index("--out") + 1] if "--out" in A else None
REF = A[A.index("--ref") + 1] if "--ref" in A else None
LIM = int(A[A.index("--limit") + 1]) if "--limit" in A else 0


def unjag16(v):
    r = (v >> 11) & 31
    b = (v >> 6) & 31
    g = (v >> 1) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def decode(path, limit=0):
    d = open(path, "rb").read()
    assert d[:4] == b"JV04", "not a JV04 (VQ) clip: %r" % d[:4]
    W, H, FPS, N = struct.unpack_from(">HHHH", d, 4)
    pal = np.array([unjag16(struct.unpack_from(">H", d, 16 + i * 2)[0])
                    for i in range(256)], dtype=np.uint8)
    cb = np.frombuffer(d, dtype=np.uint8, count=256 * 16, offset=1024).reshape(256, 16)
    bw, bh = W // 4, H // 4
    nb = bw * bh
    grid = np.zeros(nb, dtype=np.uint8)
    off = 1024 + 4096
    frames = []
    for f in range(N):
        vlen, alen = struct.unpack_from(">II", d, off)
        off += 8 + alen
        p = d[off:off + vlen]
        off += vlen + ((4 - vlen % 4) % 4)
        i = 0
        bi = 0
        while i < len(p) and bi < nb:
            t = p[i]; i += 1
            if t >= 128:
                bi += t - 127                      # skip: previous persists
            else:
                n = t + 1
                grid[bi:bi + n] = np.frombuffer(p[i:i + n], dtype=np.uint8)
                i += n; bi += n
        # blocks -> pixels
        cells = cb[grid]                                   # (nb,16) palette idx
        img = cells.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(H, W)
        frames.append(pal[img])
        if limit and len(frames) >= limit:
            break
    return frames, W, H, FPS, N


def reference(src, n, fps, W, H):
    """The same frames the encoder saw, WITHOUT its denoise - the thing a
    viewer compares against."""
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", src, "-vf",
                        "crop=320:208:0:0,fps=%d,scale=%dx%d:flags=lanczos" % (fps, W, H),
                        os.path.join(td, "r_%04d.png")], check=True)
        names = sorted(f for f in os.listdir(td) if f.startswith("r_"))[:n]
        return [np.asarray(Image.open(os.path.join(td, f)).convert("RGB"),
                           dtype=np.uint8) for f in names]


def main():
    frames, W, H, FPS, N = decode(SRC, LIM)
    print("%s: %dx%d @ %dfps, %d frames (%d decoded), %d B"
          % (os.path.basename(SRC), W, H, FPS, N, len(frames), os.path.getsize(SRC)))
    if OUT:
        os.makedirs(OUT, exist_ok=True)
        for i, f in enumerate(frames):
            Image.fromarray(f).save(os.path.join(OUT, "d%04d.png" % i))
        print("  wrote %d PNGs to %s" % (len(frames), OUT))
    if REF:
        ref = reference(REF, len(frames), FPS, W, H)
        n = min(len(ref), len(frames))
        ps = []
        for i in range(n):
            e = (frames[i].astype(np.float32) - ref[i].astype(np.float32)) ** 2
            mse = e.mean()
            ps.append(10 * np.log10(255.0 * 255.0 / mse) if mse > 0 else 99.0)
        ps = np.array(ps)
        print("  PSNR vs source: mean %.2f dB  worst %.2f  best %.2f  (%d frames)"
              % (ps.mean(), ps.min(), ps.max(), n))


main()
