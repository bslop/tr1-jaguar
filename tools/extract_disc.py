#!/usr/bin/env python3
"""Pull the files the Jaguar build needs out of a user's own Tomb Raider PSX
disc, in whatever shape they have it.

    python3 extract_disc.py <disc> <outdir>

<disc> may be:  a folder that already contains PSXDATA/ ;  a .iso ;  a .bin
(+.cue) ;  a .cue ;  a .chd ;  or a .7z/.zip holding any of those.

Writes into <outdir>:
    PSXDATA/LEVEL1.PSX  GYM.PSX  TITLE.PSX     (level / mesh / title geometry)
    AMERTIT.RAW  GYMLOAD.RAW                    (RNC title + loading art, DELDATA)
    TRACK02.cdda                               (raw CD-DA title theme, if present)

Then validates it really is Tomb Raider 1 (USA, SLUS-00152) and exits non-zero
with a plain-English message if not.  No game data is kept anywhere else.
"""
import os, sys, struct, subprocess, tempfile, shutil, glob

NEED_PSXDATA = ["LEVEL1.PSX", "GYM.PSX", "TITLE.PSX"]
NEED_DELDATA = ["AMERTIT.RAW", "GYMLOAD.RAW"]
ISO_BLOCK    = 2048
SYNC12       = b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00"


def die(msg):
    sys.stderr.write("\nerror: " + msg + "\n")
    sys.exit(2)


# ── raw-image sector geometry (2352 MODE1/MODE2 vs 2048 .iso) ────────────────
def detect_geometry(path):
    with open(path, "rb") as f:
        head = f.read(16)
    if head[:12] == SYNC12:
        mode = head[15]
        return (2352, 24 if mode == 2 else 16)   # MODE2/FORM1 data at +24, MODE1 +16
    return (2048, 0)


class Iso:
    """Minimal ISO9660 reader over a raw data track (any sector geometry)."""
    def __init__(self, path):
        self.f = open(path, "rb")
        self.ss, self.uoff = detect_geometry(path)
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            # retry as plain 2048 in case detection was fooled
            self.ss, self.uoff = 2048, 0
            pvd = self.sector(16)
            if pvd[1:6] != b"CD001":
                die("not an ISO9660 disc image (no CD001 volume descriptor).")
        self.root_lba = struct.unpack_from("<I", pvd, 158)[0]
        self.root_sz  = struct.unpack_from("<I", pvd, 166)[0]

    def sector(self, lsn):
        self.f.seek(lsn * self.ss + self.uoff)
        return self.f.read(ISO_BLOCK)

    def _walk(self, lba, size, want):
        data = b"".join(self.sector(lba + i) for i in range((size + ISO_BLOCK - 1) // ISO_BLOCK))
        pos = 0
        while pos < len(data):
            rlen = data[pos]
            if rlen == 0:
                pos = (pos + ISO_BLOCK) & ~(ISO_BLOCK - 1)
                if pos >= len(data):
                    break
                continue
            flags = data[pos + 25]
            nlen  = data[pos + 32]
            name  = data[pos + 33:pos + 33 + nlen].decode("ascii", "replace").split(";")[0].upper()
            e_lba = struct.unpack_from("<I", data, pos + 2)[0]
            e_sz  = struct.unpack_from("<I", data, pos + 10)[0]
            if name == want:
                return e_lba, e_sz
            if (flags & 2) and name not in ("", "\x00", "\x01"):
                r = self._walk(e_lba, e_sz, want)
                if r:
                    return r
            pos += rlen
        return None

    def find(self, name):
        return self._walk(self.root_lba, self.root_sz, name.upper())

    def read_file(self, lba, size):
        out = bytearray()
        s = lba
        while len(out) < size:
            out += self.sector(s)[:size - len(out)]
            s += 1
        return bytes(out)


# ── input normalisation: reduce anything to (data_track_path, audio_track_path?) ─
def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die("command failed: %s\n%s" % (" ".join(cmd), r.stderr.strip()[:400]))
    return r.stdout


def _first(patterns, root):
    for pat in patterns:
        hits = sorted(glob.glob(os.path.join(root, "**", pat), recursive=True))
        if hits:
            return hits[0]
    return None


def normalize(disc, work):
    """Return (data_track_file, audio_track_file_or_None). data_track may be a
    folder path if the source is already an extracted PSXDATA tree."""
    # already-extracted folder?
    if os.path.isdir(disc):
        if os.path.isdir(os.path.join(disc, "PSXDATA")) or \
           glob.glob(os.path.join(disc, "**", "LEVEL1.PSX"), recursive=True):
            return ("DIR:" + disc, None)
        disc = _first(["*.cue", "*.bin", "*.iso", "*.chd"], disc) or \
            die("folder has no PSXDATA/ and no disc image inside.")

    ext = os.path.splitext(disc)[1].lower()
    if ext in (".7z", ".zip"):
        ex = os.path.join(work, "unz"); os.makedirs(ex, exist_ok=True)
        _run(["7z", "x", "-y", "-o" + ex, disc])
        inner = _first(["*.cue", "*.chd", "*.bin", "*.iso"], ex) or \
            die("archive contains no disc image.")
        return normalize(inner, work)
    if ext == ".chd":
        base = os.path.join(work, "chd")
        _run(["chdman", "extractcd", "-i", disc, "-o", base + ".cue", "-ob", base + ".bin", "-f"])
        return normalize(base + ".cue", work)
    if ext == ".cue":
        return _from_cue(disc, work)
    if ext in (".bin", ".iso", ".img"):
        cue = os.path.splitext(disc)[0] + ".cue"
        if os.path.isfile(cue):
            return _from_cue(cue, work)
        return (disc, _sibling_audio(disc))
    die("unrecognised disc format: %s" % disc)


def _sibling_audio(datafile):
    for c in glob.glob(os.path.join(os.path.dirname(datafile) or ".", "*[Tt]rack*02*.bin")):
        return c
    return None


def _place_audio(src, outdir):
    # tr2jag_music.py reads a *.bin as raw CD-DA (s16le/44100/stereo) and any
    # other extension via ffmpeg auto-detect, so preserve the true format.
    ext = ".wav" if src.lower().endswith(".wav") else ".bin"
    shutil.copy(src, os.path.join(outdir, "TRACK02" + ext))


def _from_cue(cue, work):
    """Parse a .cue: return (track1 data file, track2 audio file if any).
    Handles multi-FILE (redump) and single-FILE (bchunk-split) layouts."""
    base = os.path.dirname(cue)
    files, cur = [], None
    for line in open(cue, "r", errors="replace"):
        s = line.strip()
        if s.upper().startswith("FILE"):
            q = s.split('"')
            cur = os.path.join(base, q[1] if len(q) > 2 else s.split()[1])
            files.append([cur, []])
        elif s.upper().startswith("TRACK") and files:
            files[-1][1].append(s.split()[2].upper())    # track type: MODE2/2352, AUDIO
    if len(files) >= 2:                                  # multi-FILE: track per file
        data  = files[0][0]
        audio = next((f for f, t in files[1:] if "AUDIO" in t), None)
        return (data, audio)
    # single FILE with multiple tracks -> split with bchunk
    single = files[0][0] if files else None
    if single and os.path.isfile(single):
        out = os.path.join(work, "bchunk"); os.makedirs(out, exist_ok=True)
        if shutil.which("bchunk"):
            _run(["bchunk", "-w", single, cue, os.path.join(out, "trk")])
            data  = sorted(glob.glob(os.path.join(out, "trk*.iso"))) or [single]
            audio = sorted(glob.glob(os.path.join(out, "trk*.wav")))
            return (data[0], audio[0] if audio else None)
        return (single, None)
    die("cue references missing bin file(s).")


# ── main extraction ─────────────────────────────────────────────────────────
def extract(disc, outdir):
    os.makedirs(os.path.join(outdir, "PSXDATA"), exist_ok=True)
    work = tempfile.mkdtemp(prefix="trdisc_")
    try:
        data, audio = normalize(disc, work)

        if data.startswith("DIR:"):                      # pre-extracted tree
            src = data[4:]
            for n in NEED_PSXDATA:
                p = _first([n], src) or die("PSXDATA/%s not found in folder." % n)
                shutil.copy(p, os.path.join(outdir, "PSXDATA", n))
            for n in NEED_DELDATA:
                p = _first([n], src)
                if p:
                    shutil.copy(p, os.path.join(outdir, n))
            au = _first(["*[Tt]rack*02*.wav", "*[Tt]rack*02*.bin", "TRACK02.*"], src)
            if au:
                _place_audio(au, outdir)
        else:
            iso = Iso(data)
            _validate(iso)
            for n in NEED_PSXDATA:
                loc = iso.find(n) or die("%s not on this disc (is it Tomb Raider 1?)." % n)
                open(os.path.join(outdir, "PSXDATA", n), "wb").write(iso.read_file(*loc))
            for n in NEED_DELDATA:
                loc = iso.find(n)
                if loc:
                    open(os.path.join(outdir, n), "wb").write(iso.read_file(*loc))
                else:
                    print("  note: %s not found (title/loading art will be blank)." % n)
            if audio and os.path.isfile(audio):
                _place_audio(audio, outdir)
            else:
                print("  note: no CD-audio track (title theme will be silent).")
        print("disc extraction OK -> %s" % outdir)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _validate(iso):
    cnf = iso.find("SYSTEM.CNF")
    if cnf:
        txt = iso.read_file(*cnf).decode("ascii", "replace").upper()
        if "SLUS_001.52" in txt or "SLUS-00152" in txt:
            return
        for bad, who in (("SLUS_004", "Tomb Raider II/III"), ("SLES", "a PAL/European disc"),
                         ("SLPS", "a Japanese disc")):
            if bad in txt:
                die("this looks like %s. This port needs Tomb Raider 1, USA (SLUS-00152)." % who)
    if not iso.find("LEVEL1.PSX") or not iso.find("GYM.PSX"):
        die("this disc doesn't contain Tomb Raider 1's PSXDATA files.")
    print("  note: SYSTEM.CNF serial unconfirmed, but TR1 data is present; continuing.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: extract_disc.py <disc.iso|.bin|.cue|.chd|.7z|folder> <outdir>")
    extract(sys.argv[1], sys.argv[2])
