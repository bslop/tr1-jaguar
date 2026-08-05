#!/usr/bin/env python3
# tr2jag_video.py - boot-logo FMV converter (2026-08-05, user: "have this
# video load after the game is booted").
#
#   python3 tools/tr2jag_video.py <in.mp4> <out.JV> [fps]
#
# .JV format (all big-endian, 68k-native):
#   u8[4] "JV01"; u16 W; u16 H; u16 fps; u16 nframes; u8 pad[4]   (16 B)
#   u16 pal[256]                      jag16 = R5<<11 | B5<<6 | G5<<1
#   nframes x { u32 len; u8 payload[len] }   PackBits-style RLE:
#       token t < 128  -> run: next byte repeated (t+2) times   (2..129)
#       token t >= 128 -> literal: copy the next (t-127) bytes  (1..128)
#
# Decoded frames are native 320x240 8bpp, drawn into the title framebuffer.
# The player streams sequentially (GD BIOS has no seek) from the GameDrive
# SD card and paces on the 60Hz VI clock; a payload must fit the 48KB
# stream buffer (the rblob staging area, free until the ring items stage).
#
# The source screencasts carry the emulator menu bar - cropped here.
# Quantization is dither-FREE (dither noise kills RLE); colours default 96
# and step down automatically until the worst frame fits PAYLOAD_MAX.
import os, struct, subprocess, sys, tempfile
from PIL import Image

SRC = sys.argv[1]
OUT = sys.argv[2]
FPS = int(sys.argv[3]) if len(sys.argv) > 3 else 12
W, H = int(os.environ.get("JV_W","160")), int(os.environ.get("JV_H","120"))
CROP = os.environ.get("JV_CROP", "2,20,1066,800")   # l,t,r,b in source px
PAYLOAD_MAX = 48 * 1024
l, t, r, b = [int(v) for v in CROP.split(",")]

def jag16(r8, g8, b8):
    return (((r8 >> 3) & 31) << 11) | (((b8 >> 3) & 31) << 6) | (((g8 >> 3) & 31) << 1)

def packbits(d):
    out = bytearray(); i = 0; n = len(d)
    while i < n:
        j = i
        while j + 1 < n and d[j + 1] == d[j] and j - i < 128:
            j += 1
        run = j - i + 1
        if run >= 2:
            out.append(run - 2); out.append(d[i]); i = j + 1
        else:
            j = i
            while j + 1 < n and (d[j + 1] != d[j] or (j + 2 < n and d[j + 2] != d[j + 1])) \
                  and j - i < 127:
                j += 1
            lit = j - i + 1
            out.append(128 + lit - 1); out += d[i:j + 1]; i = j + 1
    return bytes(out)

with tempfile.TemporaryDirectory() as td:
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", SRC,
                    "-vf", ("crop=%d:%d:%d:%d,fps=%d,scale=%d:%d:force_original_aspect_ratio="
                            "decrease:flags=lanczos,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black")
                    % (r - l, b - t, l, t, FPS, W, H, W, H),
                    os.path.join(td, "f_%04d.png")], check=True)
    frames = sorted(os.listdir(td))
    print("%d frames @ %dfps" % (len(frames), FPS))
    for colors in (96, 64, 48, 32):
        # shared palette from a sampled montage, then dither-free remap
        samp = frames[:: max(1, len(frames) // 12)]
        mont = Image.new("RGB", (W, H * len(samp)))
        for i, f in enumerate(samp):
            mont.paste(Image.open(os.path.join(td, f)).convert("RGB"), (0, i * H))
        palimg = mont.quantize(colors=colors)
        pal = palimg.getpalette()[: colors * 3]
        enc = []
        worst = 0
        for f in frames:
            im = Image.open(os.path.join(td, f)).convert("RGB")
            q = im.quantize(palette=palimg, dither=Image.Dither.NONE)
            p = packbits(q.tobytes())
            worst = max(worst, len(p))
            enc.append(p)
        rate = sum(len(p) for p in enc) * FPS // max(1, len(enc))
        print("  colors=%d worst=%dB rate=%dB/s" % (colors, worst, rate))
        if worst <= PAYLOAD_MAX:
            break
    assert worst <= PAYLOAD_MAX, "frame too big even at 32 colours"
    with open(OUT, "wb") as o:
        # header+palette padded to 1024 and the tail to a 512 multiple:
        # the GD BIOS FREAD is only ever exercised with sector-aligned,
        # sector-sized reads (the music path's proven envelope) - the
        # 528-byte header read desynced/wedged the stream on silicon.
        o.write(b"JV01")
        o.write(struct.pack(">HHHH4x", W, H, FPS, len(enc)))
        for i in range(256):
            if i < colors:
                o.write(struct.pack(">H", jag16(pal[i*3], pal[i*3+1], pal[i*3+2])))
            else:
                o.write(struct.pack(">H", 0))
        o.write(b"\0" * (1024 - 16 - 512))
        for p in enc:
            # records padded to 4 bytes (length field = TRUE length): the
            # player's rolling buffer then stays 4-aligned through
            # compaction, so every FREAD destination is aligned - an odd
            # destination address-errors the 68k inside the BIOS copy
            # (the frozen mid-clip frame, 2026-08-05).
            o.write(struct.pack(">I", len(p)))
            o.write(p)
            o.write(b"\0" * ((4 - len(p) % 4) % 4))
        o.write(b"\0" * ((512 - o.tell() % 512) % 512))
    print("wrote %s: %d frames, %d colours, %dB total" %
          (OUT, len(enc), colors, os.path.getsize(OUT)))
