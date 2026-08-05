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

# JV03: audio muxed in. Per-frame record = u32 vlen | u32 alen |
# audio[alen pad4] | video[vlen pad4]; audio = s8 mono 11025Hz chunks
# (1376/frame at 8fps - 4-aligned, 0.15% drift over a 10s clip). The
# player hands audio to the DSP voice-0 gapless queue (the music path's
# proven engine) and Tom decodes the video payload unchanged.
ACHUNK = None   # set per-fps below: round(11025/fps/4)*4

SRC = sys.argv[1]
OUT = sys.argv[2]
FPS = int(sys.argv[3]) if len(sys.argv) > 3 else 12
VQ  = os.environ.get("JV_VQ", "0") == "1"
W, H = int(os.environ.get("JV_W","160")), int(os.environ.get("JV_H","120"))
CROP = os.environ.get("JV_CROP", "2,20,1066,800")   # l,t,r,b in source px
PAYLOAD_MAX = 48 * 1024
l, t, r, b = [int(v) for v in CROP.split(",")]

def jag16(r8, g8, b8):
    return (((r8 >> 3) & 31) << 11) | (((b8 >> 3) & 31) << 6) | (((g8 >> 3) & 31) << 1)

def packbits(d, prev):
    # JV02 tokens: 0..99 colour-run (n=t+2, 2..101); 100..127 SKIP-run
    # (n=(t-99)*4, 4..112: keep the previous frame's pixels - the stage
    # persists across frames); 128..255 literal (n=t-127, 1..128).
    out = bytearray(); i = 0; n = len(d)
    while i < n:
        if prev is not None:
            k = i
            while k < n and d[k] == prev[k] and k - i < 112:
                k += 1
            if k - i >= 4:
                out.append(100 + (k - i) // 4 - 1); i += ((k - i) // 4) * 4
                continue
        j = i
        while j + 1 < n and d[j + 1] == d[j] and j - i < 99:
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

def encode_vq():
    # JV04: native 320x240 VECTOR QUANTIZATION (user: "the original videos
    # from the disc"). 64-colour CLUT + 256-entry 4x4-block codebook (4KB,
    # its own sector-aligned section after the header). Frame stream =
    # skip/copy tokens over the 80x60 block grid:
    #   t < 128  -> copy t+1 blocks: that many index bytes follow
    #   t >= 128 -> skip t-127 blocks (previous frame persists; the kernel
    #               writes BOTH framebuffers so ping-pong stays coherent)
    # Records keep the JV03 audio mux + 4-align + sector-pad laws.
    import numpy as np
    W2, H2, K = 320, 240, 256
    ACH = (11025 // FPS // 4) * 4
    with tempfile.TemporaryDirectory() as td:
        apath = os.path.join(td, "a.raw")
        r2 = subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", SRC, "-vn",
                             "-f", "s8", "-ar", "11025", "-ac", "1", apath])
        audio = open(apath, "rb").read() if r2.returncode == 0 and os.path.exists(apath) else b""
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", SRC,
                        "-vf", ("crop=%d:%d:%d:%d,hqdn3d=4:3:12:12,fps=%d,"
                                "scale=%dx%d:flags=lanczos")
                        % (r - l, b - t, l, t, FPS, W2, H2),
                        os.path.join(td, "f_%04d.png")], check=True)
        names = sorted(f for f in os.listdir(td) if f.startswith("f_"))
        fr = [np.asarray(Image.open(os.path.join(td, f)).convert("RGB"),
                         dtype=np.float32) for f in names]
        print("%d frames @ %dfps (VQ %dx%d)" % (len(fr), FPS, W2, H2))
        NCOL = int(os.environ.get("JV_VQCOLS", "128"))
        mont = Image.fromarray(np.concatenate(
            [f.astype(np.uint8) for f in fr[::max(1, len(fr)//24)]], axis=0))
        pimg = mont.quantize(colors=NCOL)
        pal = np.array(pimg.getpalette()[:NCOL*3], dtype=np.float32).reshape(NCOL, 3)
        idxf = [np.asarray(Image.fromarray(f.astype(np.uint8)).quantize(
                    palette=pimg, dither=Image.Dither.NONE), dtype=np.uint8)
                for f in fr]
        def blocks(a):
            return a.reshape(H2//4, 4, W2//4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
        allb = np.concatenate([blocks(a) for a in idxf[::2]], axis=0)
        vec = pal[allb].reshape(len(allb), -1)
        rng = np.random.default_rng(7)
        # k-means++ seeding: spread centroids by distance, not luck
        cent = np.empty((K, vec.shape[1]), dtype=np.float32)
        cent[0] = vec[rng.integers(len(vec))]
        d2min = ((vec - cent[0])**2).sum(1)
        for k in range(1, K):
            p = d2min / d2min.sum()
            cent[k] = vec[rng.choice(len(vec), p=p)]
            d2min = np.minimum(d2min, ((vec - cent[k])**2).sum(1))
        for _ in range(14):
            assign = np.empty(len(vec), dtype=np.int32)
            for i in range(0, len(vec), 8192):
                dd = ((vec[i:i+8192, None, :] - cent[None, :, :])**2).sum(2)
                assign[i:i+8192] = dd.argmin(1)
            for k in range(K):
                m = assign == k
                if m.any(): cent[k] = vec[m].mean(0)
        cb = np.empty((K, 16), dtype=np.uint8)
        for k in range(K):
            cells = cent[k].reshape(16, 3)
            cb[k] = ((cells[:, None, :] - pal[None, :, :])**2).sum(2).argmin(1)
        cbvec = pal[cb].reshape(K, -1)
        # temporal deadband in CODEBOOK space: a block is "changed" only if
        # its new entry is visually far from the one on screen - glow noise
        # otherwise flips entries every frame (EIDOS hit 43KB/s without it)
        VQDB = float(os.environ.get("JV_VQDB", "350"))
        KEYF = int(os.environ.get("JV_KEYF", "30"))
        cdist = ((cbvec[:, None, :] - cbvec[None, :, :])**2).sum(2) / 16.0
        enc2 = []
        prev = None
        for fidx, a in enumerate(idxf):
            bl = blocks(a); v = pal[bl].reshape(len(bl), -1)
            assign = np.empty(len(bl), dtype=np.int32)
            for i in range(0, len(bl), 8192):
                dd = ((v[i:i+8192, None, :] - cbvec[None, :, :])**2).sum(2)
                assign[i:i+8192] = dd.argmin(1)
            if prev is None or (KEYF and fidx % KEYF == 0):
                # KEYFRAME: full repaint - self-heals any accumulated
                # drift (deadband residue, buffer-invariant slips)
                changed = np.ones(len(bl), bool)
            else:
                changed = cdist[assign, prev] > VQDB
                # settling pass: spend quiet-frame budget on the worst
                # sub-threshold offenders so fades converge to truth
                if changed.sum() < 1500:
                    resid = np.where(~changed, cdist[assign, prev], 0)
                    idxs = np.argsort(resid)[::-1][: int(1500 - changed.sum())]
                    add = idxs[resid[idxs] > 40]
                    changed[add] = True
                assign = np.where(changed, assign, prev)
            prev = assign.copy()
            out = bytearray(); i = 0; n = len(bl)
            while i < n:
                if not changed[i]:
                    j = i
                    while j < n and not changed[j] and j - i < 128: j += 1
                    out.append(128 + (j - i) - 1); i = j
                else:
                    j = i
                    while j < n and changed[j] and j - i < 128: j += 1
                    out.append((j - i) - 1)
                    out += bytes(int(assign[k2]) for k2 in range(i, j)); i = j
            enc2.append(bytes(out))
        rate = (sum(len(p) for p in enc2) + len(enc2)*(8+ACH)) * FPS // len(enc2)
        worst = max(len(p) for p in enc2)
        print("VQ rate %dB/s worst %dB" % (rate, worst))
        assert worst <= 24576
        hold = bytes([255]) * 38   # 38 x skip-128 = 4864 >= 4800 blocks
        while audio and len(enc2) * ACH < len(audio):
            enc2.append(hold)
        with open(OUT, "wb") as o:
            o.write(b"JV04")
            o.write(struct.pack(">HHHH4x", W2, H2, FPS, len(enc2)))
            for i in range(256):
                if i < NCOL:
                    o.write(struct.pack(">H", jag16(int(pal[i][0]), int(pal[i][1]), int(pal[i][2]))))
                else:
                    o.write(struct.pack(">H", 0))
            o.write(b"\0" * (1024 - 16 - 512))
            o.write(cb.tobytes())            # 4096B codebook = 8 sectors
            for fi, p in enumerate(enc2):
                a = audio[fi*ACH:(fi+1)*ACH]
                if a and len(a) < ACH: a = a + b"\0" * (ACH - len(a))
                o.write(struct.pack(">II", len(p), len(a)))
                o.write(a)
                o.write(p)
                o.write(b"\0" * ((4 - len(p) % 4) % 4))
            o.write(b"\0" * ((512 - o.tell() % 512) % 512))
        print("wrote %s: %d frames VQ, %dB" % (OUT, len(enc2), os.path.getsize(OUT)))

if VQ:
    encode_vq()
    sys.exit(0)

with tempfile.TemporaryDirectory() as td:
    ACHUNK = (11025 // FPS // 4) * 4
    print("audio chunk %dB/frame @ %dfps" % (ACHUNK, FPS))
    apath = os.path.join(td, "a.raw")
    r2 = subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", SRC, "-vn",
                         "-f", "s8", "-ar", "11025", "-ac", "1", apath])
    audio = open(apath, "rb").read() if r2.returncode == 0 and os.path.exists(apath) else b""
    print("audio: %d bytes (%.1fs)" % (len(audio), len(audio) / 11025.0))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", SRC,
                    "-vf", (("crop=%d:%d:%d:%d,hqdn3d=4:3:14:14,fps=%d,"
                             "scale=%dx%d:flags=lanczos")
                            % (r - l, b - t, l, t, FPS, W, H)
                            if os.environ.get("JV_STRETCH", "1") == "1" else
                            ("crop=%d:%d:%d:%d,hqdn3d=4:3:14:14,fps=%d,scale=%d:%d:"
                             "force_original_aspect_ratio=decrease:flags=lanczos,"
                             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black")
                            % (r - l, b - t, l, t, FPS, W, H, W, H)),
                    os.path.join(td, "f_%04d.png")], check=True)
    frames = sorted(f for f in os.listdir(td) if f.startswith("f_"))
    print("%d frames @ %dfps" % (len(frames), FPS))
    for colors in [int(v) for v in os.environ.get("JV_COLORS","96,64,48,32").split(",")]:
        # shared palette from a sampled montage, then dither-free remap
        samp = frames[:: max(1, len(frames) // 12)]
        mont = Image.new("RGB", (W, H * len(samp)))
        for i, f in enumerate(samp):
            mont.paste(Image.open(os.path.join(td, f)).convert("RGB"), (0, i * H))
        palimg = mont.quantize(colors=colors)
        pal = palimg.getpalette()[: colors * 3]
        import numpy as np
        DB = int(os.environ.get("JV_DEADBAND", "16"))
        prgb = np.array(pal + [0] * (768 - len(pal)), dtype=np.int32).reshape(256, 3)
        enc = []
        worst = 0
        prev = None
        for f in frames:
            im = Image.open(os.path.join(td, f)).convert("RGB")
            q = np.frombuffer(im.quantize(palette=palimg,
                    dither=Image.Dither.NONE).tobytes(), dtype=np.uint8).copy()
            if prev is not None and DB:
                # temporal deadband (conditional replenishment): keep the
                # previous index wherever the source pixel is within DB of
                # the colour already on screen - MDEC noise otherwise flips
                # indices in static regions and starves the skip-runs.
                src = np.asarray(im, dtype=np.int32).reshape(-1, 3)
                d2 = np.abs(src - prgb[prev]).sum(1)
                keep = d2 <= DB * 3
                q[keep] = prev[keep]
            p = packbits(q.tobytes(), None if prev is None else prev.tobytes())
            prev = q
            worst = max(worst, len(p))
            enc.append(p)
        rate = sum(len(p) for p in enc) * FPS // max(1, len(enc))
        print("  colors=%d worst=%dB rate=%dB/s" % (colors, worst, rate))
        if worst <= PAYLOAD_MAX:
            break
    assert worst <= PAYLOAD_MAX, "frame too big even at 32 colours"
    # hold the last frame (all-skip payloads, ~172B) until the audio ends -
    # CORE's sting runs ~2.7s past its last video frame
    hold = bytes([127] * 171 + [99 + 12])          # 171*112 + 48 = 19200
    while audio and len(enc) * ACHUNK < len(audio):
        enc.append(hold)
    print("frames incl. audio-hold:", len(enc))
    with open(OUT, "wb") as o:
        # header+palette padded to 1024 and the tail to a 512 multiple:
        # the GD BIOS FREAD is only ever exercised with sector-aligned,
        # sector-sized reads (the music path's proven envelope) - the
        # 528-byte header read desynced/wedged the stream on silicon.
        o.write(b"JV03")
        o.write(struct.pack(">HHHH4x", W, H, FPS, len(enc)))
        for i in range(256):
            if i < colors:
                o.write(struct.pack(">H", jag16(pal[i*3], pal[i*3+1], pal[i*3+2])))
            else:
                o.write(struct.pack(">H", 0))
        o.write(b"\0" * (1024 - 16 - 512))
        for fi, p in enumerate(enc):
            # records padded to 4 bytes (length fields = TRUE lengths): the
            # player's rolling buffer then stays 4-aligned through
            # compaction, so every FREAD destination is aligned - an odd
            # destination address-errors the 68k inside the BIOS copy
            # (the frozen mid-clip frame, 2026-08-05).
            a = audio[fi * ACHUNK:(fi + 1) * ACHUNK]
            if a and len(a) < ACHUNK:
                a = a + b"\0" * (ACHUNK - len(a))
            o.write(struct.pack(">II", len(p), len(a)))
            o.write(a)                       # ACHUNK is 4-aligned already
            o.write(p)
            o.write(b"\0" * ((4 - len(p) % 4) % 4))
        o.write(b"\0" * ((512 - o.tell() % 512) % 512))
    print("wrote %s: %d frames, %d colours, %dB total" %
          (OUT, len(enc), colors, os.path.getsize(OUT)))
