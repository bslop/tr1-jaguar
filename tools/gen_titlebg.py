#!/usr/bin/env python3
"""Title + loading background extractor for the Jaguar TR1 port.

The PSX disc stores the American title screen and the Lara's-Home loading
screen RNC-compressed in DELDATA (AMERTIT.RAW, GYMLOAD.RAW) as 384x256 15-bit
BGR images.  The Jaguar wants a 320x240 8-bit indexed image + a 256-entry RGB16
CLUT, so we: RNC-decompress -> BGR555 -> resize 320x240 -> quantize to 256 ->
emit  <name>.bin (indices) and <name>_pal.bin (jag RGB16, big-endian).

Deterministic (fixed quantizer/resampler, no randomness) so every build from
the same disc produces identical output.  Emits blank-but-valid files if a RAW
is absent, so the game still links.

Runs BEFORE tr2jag_title.py, which maps the 3D passport/photo textures onto
the palette this tool writes to title_pal.bin.

  env:  TR_DELDATA  dir holding AMERTIT.RAW / GYMLOAD.RAW
                    (default assets/extracted)
        TR_OUTDIR   output dir (default: the jaguar/ tree)
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rnc import rnc_unpack
from PIL import Image

_REPO   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DELDATA = os.environ.get("TR_DELDATA", os.path.join(_REPO, "assets/extracted"))
OUTDIR  = os.environ.get("TR_OUTDIR", _REPO)

SRC_W, SRC_H = 384, 256          # native size of the decompressed disc image
DST_W, DST_H = 320, 240          # Jaguar hi-res title buffer


def jag16(r5, g5, b5):
    """PSX 5-bit channels -> Jaguar RGB16 (R<<11 | B<<6 | G<<1); matches
    tr2jag_multiroom.jag16 and the title_pal unpack in tr2jag_title.py."""
    return ((r5 & 31) << 11) | ((b5 & 31) << 6) | ((g5 & 31) << 1)


def bgr555_to_rgb888(blob, w, h):
    """Little-endian PSX BGR555 bytes -> a Pillow RGB image."""
    im = Image.new("RGB", (w, h))
    px = []
    for i in range(w * h):
        c = blob[i * 2] | (blob[i * 2 + 1] << 8)
        px.append((((c) & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3))
    im.putdata(px)
    return im


def emit(img_q, name):
    """img_q: a 'P'-mode 320x240 image (<=256 colors). Writes name.bin + name_pal.bin."""
    idx = img_q.tobytes()
    assert len(idx) == DST_W * DST_H, len(idx)
    pal = img_q.getpalette() or []
    pal += [0] * (256 * 3 - len(pal))
    out_pal = bytearray()
    for i in range(256):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        out_pal += struct.pack(">H", jag16(r >> 3, g >> 3, b >> 3))
    open(os.path.join(OUTDIR, name + ".bin"), "wb").write(idx)
    open(os.path.join(OUTDIR, name + "_pal.bin"), "wb").write(bytes(out_pal))


def convert(raw_name, out_name):
    path = os.path.join(DELDATA, raw_name)
    if not os.path.isfile(path):
        # blank fallback: solid image + a 1-colour palette -> still links/boots
        print("  %-12s MISSING (%s) -> blank %dx%d" % (out_name, raw_name, DST_W, DST_H))
        img = Image.new("P", (DST_W, DST_H), 0)
        img.putpalette([0, 0, 0] + [0] * (768 - 3))
        emit(img, out_name)
        return
    raw = rnc_unpack(open(path, "rb").read())
    need = SRC_W * SRC_H * 2
    if len(raw) < need:
        raise ValueError("%s: decompressed %d bytes, expected >=%d (%dx%d BGR555)"
                         % (raw_name, len(raw), need, SRC_W, SRC_H))
    src = bgr555_to_rgb888(raw, SRC_W, SRC_H)
    src = src.resize((DST_W, DST_H), Image.LANCZOS)
    q = src.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                     dither=Image.Dither.NONE)
    emit(q, out_name)
    print("  %-12s <- %s (RNC %dx%d -> %dx%d, 256 colours)"
          % (out_name, raw_name, SRC_W, SRC_H, DST_W, DST_H))


def main():
    print("title/loading backgrounds -> %s" % OUTDIR)
    convert("AMERTIT.RAW", "title")
    convert("GYMLOAD.RAW", "gymload")


if __name__ == "__main__":
    main()
