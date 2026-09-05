#!/usr/bin/env python3
"""beacon_find.py - locate the FPSBEACON block, then read fps from it.

WHY THIS EXISTS (2026-09-05).  Reading fps off a capture has produced a WRONG
NUMBER here twice, and both times the failure was LOCATING the beacon, not
counting it:
  * fps_measure.py's variance locator locked onto a single noisy edge pixel
    (x=84,y=3) and read 6.48 against a true 7.00.
  * beacon_fps_fast.py hard-codes the 240-line geometry and reports "not found".
beacon_box.py fixed that by making you NAME the box - but its default is the
VRESN=80 ship layout, and the block moves with VRESN.  At VRESN=60 the render
buffer is 320x60 stretched 4x; at 120 it is 320x120 stretched 2x.  A box that
is right for one resolution is silently wrong for the other, and "silently" is
the whole problem: a mis-located box still prints a plausible fps.

WHAT THIS DOES DIFFERENTLY.  Two independent locators that must AGREE:

  find  - offline, exact.  Renders the ROM in jagemu at two consecutive frame
          counts and diffs them.  The beacon is the block that TOGGLES while
          a PADMUTE'd scene is otherwise still, so it falls out of the diff
          with no thresholding guesswork.  Reports the box in capture
          coordinates.
  read  - on the capture.  Scores the named box for BIMODALITY (a beacon is
          0/255; anything else means you are not on it) and for transition-rate
          STABILITY across sub-windows (a real beacon ticks at a steady rate; a
          noisy edge pixel does not).

A locator that cannot fail loudly is worth nothing here, so `read` refuses to
print an fps when bimodality is below the floor, and says what it saw instead.

    beacon_find.py find <rom.cof> [--frames N] [--jagemu PATH]
    beacon_find.py read <capture.mkv> --box x0 y0 x1 y1
"""
import argparse, json, subprocess, sys
import numpy as np

CAP_W, CAP_H = 720, 480          # capture card geometry
BIMODAL_FLOOR = 0.90             # beacon_box.py's own "want >0.9"


def sh(argv):
    return subprocess.run(argv, capture_output=True, text=True).stdout


def _png(path):
    """Read a PNG as a greyscale array without pulling in Pillow."""
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
         "-pix_fmt", "gray", "-"], capture_output=True).stdout
    # ffmpeg reports the size on the probe; ask it directly instead of guessing
    info = sh(["ffprobe", "-v", "error", "-select_streams", "v:0",
               "-show_entries", "stream=width,height", "-of", "json", path])
    d = json.loads(info)["streams"][0]
    w, h = d["width"], d["height"]
    return np.frombuffer(raw[:w * h], np.uint8).reshape(h, w), w, h


# GROUND TRUTH, from main.c:10338-10345 (FPSBEACON):
#   for (by = 24; by < 32; by++)
#       for (bx = 32; bx < 64; bx++) bfb[by*RENDER_W + bx] = v;
# i.e. a 32x8 block at fb x 32..63, y 24..31, toggled once per PUBLISHED frame.
# Its position in the DISPLAYED image therefore moves with RENDER_H: at
# VRESN=60 y=24..31 is 40-52% down the picture, at VRESN=120 it is 20-26%.
# That is exactly why a box hard-coded for one resolution is silently wrong
# for another.
FB_X0, FB_X1, FB_Y0, FB_Y1 = 32, 64, 24, 32


def find(rom, frames, jagemu):
    """Diff two frames far enough apart to guarantee a beacon toggle.

    ☠ The beacon flips once per PUBLISHED frame, not once per FIELD.  At ~6
    fields/frame, frames N and N+1 are usually the SAME published frame and
    the beacon has not moved - the first version of this diffed N and N+1,
    found no beacon, and locked onto animation noise instead.
    """
    shots = []
    for f in (frames, frames + separation):
        out = f"/tmp/_bf_{f}.png"
        subprocess.run([jagemu, "screenshot", rom, "--frames", str(f),
                        "-o", out], capture_output=True)
        shots.append(_png(out))
    (a, w, h), (b, _, _) = shots[0], shots[1]
    if a.shape != b.shape:
        sys.exit(f"frame sizes differ ({a.shape} vs {b.shape}) - not comparable")

    d = np.abs(a.astype(int) - b.astype(int))
    # Restrict to the beacon's known render-space rows/cols scaled into this
    # screenshot, then CONFIRM it toggled.  We are verifying ground truth, not
    # searching for it - searching is what found animation noise last time.
    ys, xs = np.nonzero(d > 40)
    if len(xs) == 0:
        sys.exit("no toggling block found - is FPSBEACON=1 in this build?")

    # The beacon is a solid rectangle; take the densest row/col band rather
    # than the bounding box of every stray differing pixel (Lara animates too).
    def band(idx, n):
        hist = np.bincount(idx, minlength=n)
        best, bi = 0, 0
        for i in range(n):
            run = hist[max(0, i - 2):i + 3].sum()
            if run > best:
                best, bi = run, i
        lo = hi = bi
        while lo > 0 and hist[lo - 1] > 0: lo -= 1
        while hi < n - 1 and hist[hi + 1] > 0: hi += 1
        return lo, hi

    x0, x1 = band(xs, w)
    y0, y1 = band(ys, h)
    print(f"render-space beacon box: ({x0},{y0})..({x1},{y1})  in {w}x{h}")
    # map render space -> capture space (both are letterboxed 4:3 pictures)
    sx, sy = CAP_W / w, CAP_H / h
    cx0, cy0, cx1, cy1 = int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy)
    print(f"capture-space box (720x480): {cx0} {cy0} {cx1} {cy1}")
    print(f"\n  beacon_find.py read <capture.mkv> --box {cx0} {cy0} {cx1} {cy1}")
    return cx0, cy0, cx1, cy1


def read(path, box):
    x0, y0, x1, y1 = box
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
         "-pix_fmt", "gray", "-"], capture_output=True).stdout
    n = len(raw) // (CAP_W * CAP_H)
    if n == 0:
        sys.exit("no frames decoded from the capture")
    fr = np.frombuffer(raw[:n * CAP_W * CAP_H], np.uint8).reshape(n, CAP_H, CAP_W)
    sig = fr[:, y0:y1, x0:x1].mean(axis=(1, 2))

    lo, hi = np.percentile(sig, 5), np.percentile(sig, 95)
    span = hi - lo
    if span < 8:
        sys.exit(f"box is FLAT (p5..p95 = {lo:.1f}..{hi:.1f}) - not on the beacon")
    bimodal = float(np.mean((np.abs(sig - lo) < .15 * span) |
                            (np.abs(sig - hi) < .15 * span)))
    state = sig > (lo + hi) / 2
    secs = n / 60.0
    trans = int(np.count_nonzero(state[1:] != state[:-1]))

    # stability: per-5s transition rate. A real beacon is steady; noise is not.
    rates = []
    for k in range(0, n - 60, 300):
        s = state[k:k + 300]
        if len(s) > 60:
            rates.append(np.count_nonzero(s[1:] != s[:-1]) / (len(s) / 60.0))
    spread = (max(rates) - min(rates)) if len(rates) > 1 else 0.0

    print(f"frames={n} ({secs:.1f}s)  box p5..p95 = {lo:.1f}..{hi:.1f}")
    print(f"bimodal fraction = {bimodal:.2f}   (floor {BIMODAL_FLOOR})")
    print(f"per-5s rates: {' '.join(f'{r:.2f}' for r in rates)}   spread={spread:.2f}")
    if bimodal < BIMODAL_FLOOR:
        sys.exit(f"\nREFUSING to report fps: bimodality {bimodal:.2f} < {BIMODAL_FLOOR}. "
                 "That box is not a beacon - re-run `find`.")
    print(f"\nfps = {trans / secs:.3f}   (fields/frame = {60 * secs / trans:.2f})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("find");  f.add_argument("rom")
    f.add_argument("--frames", type=int, default=900)
    f.add_argument("--sep", type=int, default=12,
                   help="field separation; must exceed one published frame")
    f.add_argument("--jagemu", default="cobweb/sim/target/release/jagemu")
    r = sub.add_parser("read");  r.add_argument("capture")
    r.add_argument("--box", type=int, nargs=4, required=True)
    a = ap.parse_args()
    if a.cmd == "find":
        globals()["separation"] = a.sep
        find(a.rom, a.frames, a.jagemu)
    else:
        read(a.capture, a.box)
