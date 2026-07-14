#!/usr/bin/env python3
# tr2jag_music.py — build music.bin (title theme) for the Jerry mixer.
#
# music.bin (big-endian): u32 pcm_len, then s8 PCM @11025Hz (long-aligned
# tail pad). len==0 = no music (the title hook skips arming).
#
# Source (first that exists, or $MUSIC_SRC):
#   tr1_psx/Tomb Raider (USA) (v1.6) (Track 02).bin  — raw CD-DA
#     (44100Hz s16le stereo, no header; INDEX 01 at 00:02:00 = skip 2s pregap)
#   any .wav — decoded via ffmpeg
# Cap: $MUSIC_SECONDS (default 34 — RAM budget), 1s fade at the cut so the
# loop point doesn't click.
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.dirname(HERE)
SRC  = os.environ.get("MUSIC_SRC",
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets/Tomb Raider (USA) (v1.6) (Track 02).bin"))
SECS = float(os.environ.get("MUSIC_SECONDS", "34"))
RATE = 11025
LPF    = int(os.environ.get("MUSIC_LPF", "5000"))    # low-pass (Hz) to tame the 8-bit/11k "ringy" high end; 0 = off
DITHER = os.environ.get("MUSIC_DITHER", "1") != "0"  # TPDF dither -> smooths 8-bit quantization grit
import random as _random
_dr = _random.Random(0x5107)                         # SEEDED, so the dithered output stays reproducible

def to_s8(s16, gain):
    """gain -> optional TPDF dither -> round -> clamp, returned as signed-8-bit bytes.
    Rounding+dither replace the old truncation, which added the harsh/ringy distortion."""
    out = bytearray()
    for v in s16:
        x = v * gain
        if DITHER:
            x += _dr.random() + _dr.random() - 1.0      # triangular PDF, +-1 LSB
        q = int(round(x))
        out.append((-128 if q < -128 else 127 if q > 127 else q) & 0xFF)
    return bytes(out)

def write_blob(pcm):                       # pcm = list of s8 ints
    blob = struct.pack(">I", len(pcm)) + bytes((v & 0xFF) for v in pcm)
    while len(blob) & 3: blob += b'\0'
    open(os.path.join(OUT, 'music.bin'), 'wb').write(blob)
    print("music.bin: %d samples (%.1fs) -> %d bytes"
          % (len(pcm), len(pcm)/RATE, len(blob)))

if not os.path.exists(SRC):
    print("music source missing (%s) -> silent stub" % SRC)
    write_blob([])
    sys.exit(0)

# decode to mono s16le @11025 via ffmpeg (handles wav; raw CD-DA needs fmt)
cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
if SRC.lower().endswith(".bin"):           # raw CD-DA track
    cmd += ["-f", "s16le", "-ar", "44100", "-ac", "2", "-ss", "2", "-i", SRC]
else:
    cmd += ["-i", SRC]
cmd += ["-t", str(SECS + 1), "-ac", "1", "-ar", str(RATE),
        "-f", "s16le", "pipe:1"]
raw = subprocess.run(cmd, capture_output=True, check=True).stdout
s16 = struct.unpack("<%dh" % (len(raw)//2), raw)
n = min(len(s16), int(SECS * RATE))
peak = max(1, max(abs(v) for v in s16[:n]))
g = min(120.0/peak, 4.0)                   # normalize peak to ~120 in s8
pcm = [max(-128, min(127, int(v*g))) for v in s16[:n]]
fade = min(RATE, n)                        # 1s fade to the loop point
for k in range(fade):
    pcm[n-fade+k] = pcm[n-fade+k]*(fade-k)//fade
write_blob(pcm)

# ---- MUSIC.PCM: FULL-LENGTH raw s8 for GameDrive SD streaming (no
# header; the game gd_freads it sequentially and loops at EOF) ----
cmd2 = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
if SRC.lower().endswith(".bin"):
    cmd2 += ["-f", "s16le", "-ar", "44100", "-ac", "2", "-ss", "2", "-i", SRC]
else:
    cmd2 += ["-i", SRC]
cmd2 += ["-ac", "1"] + (["-af", "lowpass=f=%d" % LPF] if LPF > 0 else []) \
      + ["-ar", str(RATE), "-f", "s16le", "pipe:1"]
raw2 = subprocess.run(cmd2, capture_output=True, check=True).stdout
s16f = struct.unpack("<%dh" % (len(raw2)//2), raw2)
peakf = max(1, max(abs(v) for v in s16f))
gf = min(120.0/peakf, 4.0)
full = to_s8(s16f, gf)
while len(full) & 3: full += b'\0'
open(os.path.join(OUT, 'MUSIC.PCM'), 'wb').write(full)
print("MUSIC.PCM: %.1fs -> %d bytes (SD streaming)" % (len(s16f)/RATE, len(full)))
