#!/usr/bin/env python3
"""
P2b VERIFICATION: run gpu_sflats.gas in the JRISC simulator on REAL room data
and diff its span list against tools/p2_flats_fixed.py, span for span.

★★★★★ WHY THIS EXISTS — THE OFFLINE LOOP IS NOT ACTUALLY DOWN
    jagemu freezes on the TITLE (the vertical interrupt never fires), so the
    whole-ROM offline loop is dead and every visual check costs a flash + a
    physical reset.  But a GPU kernel does not need the boot path: `jtest run
    --assemble` loads a bare kernel, executes it, and `jtest golden` dumps a
    DRAM region.  So the flats kernel CAN be verified offline, exactly, before
    it ever reaches silicon.
    ⇒ Do not conclude "no offline loop" from the jagemu note.  The right
    question is always "which layer does this bug live in, and is THAT layer
    simulable?"

WHAT IS COMPARED
    The kernel is assembled with -d SFTEST=1, which swaps span_flush's Blitter
    fill for a DRAM log of (row, x0, x1, texid).  The SPAN LIST is the right
    thing to check: it is what the geometry produces, it is the quantity the
    82%-setup cost is counted in, and diffing it needs no Blitter model.

    Both sides get the SAME visplane records, run lists, camera and yaw — the
    Python side reads them out of the very tables emitted into the .gas file,
    so a data-packing bug cannot hide by being made twice.

Usage:
    python3 tools/p2_flats_sim.py [sr_sr.bin] [--room N] [--yaw D] [--all]
"""
import sys, os, math, subprocess, struct, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
JAG = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import p2_flats_ref as ref
import p2_flats_fixed as fx

JTEST = os.path.expanduser("~/Documents/Git/cobweb/sim/target/release/jtest")
KERNEL = os.path.join(JAG, "gpu_sflats.gas")
LOGADDR = 0x00100000
MAXSPANS = 4096
FB = 0x00200000


def build_tables(room, cam, sa16, ca16):
    """Emit the exact bytes the 68k would build, and return both the assembly
    text and the same structures for the Python model — one source of truth."""
    planes = fx.build_planes(room, cam[1])
    zmajor = 1 if abs(sa16) > abs(ca16) else 0

    L = []
    L.append("tparams:")
    for v in (FB, 0, len(planes), cam[1], cam[0], cam[2], sa16, ca16, 0, 0, zmajor):
        L.append("\tdc.l\t%d" % v)
    # patch [1] to point at the plane table
    L[2] = "\tdc.l\ttplanes"

    L.append("tplanes:")
    for i, p in enumerate(planes):
        r0, r1 = fx.plane_rows(p, cam, sa16, ca16, zmajor)
        p['rows'] = (r0, r1)
        L.append("\tdc.l\t%d" % p['h'])
        L.append("\tdc.l\t%d" % p['tex'])
        L.append("\tdc.l\t%d" % len(p['rx']))
        L.append("\tdc.l\trx%d" % i)
        L.append("\tdc.l\t%d" % len(p['rz']))
        L.append("\tdc.l\trz%d" % i)
        L.append("\tdc.l\t%d" % r0)
        L.append("\tdc.l\t%d" % r1)
    for i, p in enumerate(planes):
        for tag, runs in (('rx', p['rx']), ('rz', p['rz'])):
            L.append("%s%d:" % (tag, i))
            for (x0, x1, z0, z1) in runs:
                assert 0 <= x0 < 32768 and 0 <= x1 < 32768, "world x out of s16"
                assert 0 <= z0 < 32768 and 0 <= z1 < 32768, "world z out of s16"
                L.append("\tdc.l\t$%08X" % ((x0 << 16) | x1))
                L.append("\tdc.l\t$%08X" % ((z0 << 16) | z1))
    return planes, zmajor, "\n".join(L)


def make_source(tables):
    """Prologue + the REAL kernel source (header stripped) + the data tables.
    Concatenating the actual file is deliberate: the test can never drift from
    the kernel it is meant to verify."""
    src = open(KERNEL).read().split("\n")
    out, seen = [], False
    for ln in src:
        s = ln.strip()
        if not seen and (s.startswith(".gpu") or s.startswith(".org")):
            continue
        if s.startswith("start:"):
            seen = True
        out.append(ln)
    return ("\t.gpu\n\t.org\t$F03000\n"
            "SFTEST\t.equ\t1\n"
            "\tmovei\t#$00100000,r1\n\tmoveq\t#0,r2\n\tstore\tr2,(r1)\n"
            "\tmovei\t#tparams,r0\n"
            "\tmovei\t#start,r23\n\tjump\t(r23)\n\tnop\n"
            + "\n".join(out) + "\n" + tables + "\n")


def run_kernel(text, budget=200000000):
    with tempfile.TemporaryDirectory() as d:
        s = os.path.join(d, "t.s")
        g = os.path.join(d, "g.bin")
        open(s, "w").write(text)
        n = 4 + MAXSPANS * 8
        r = subprocess.run([JTEST, "golden", s, "--assemble", "--capture",
                            "0x%X:%d" % (LOGADDR, n), "--golden", g,
                            "--update", "--budget", str(budget)],
                           capture_output=True, text=True, cwd=JAG)
        if not os.path.exists(g):
            open("/tmp/p2_flats_sim_fail.s", "w").write(text)
            raise SystemExit("jtest failed (source at /tmp/p2_flats_sim_fail.s):"
                             "\n%s\n%s" % (r.stdout, r.stderr))
        if os.environ.get("SIMVERBOSE"):
            sys.stderr.write(r.stdout + r.stderr)
        b = open(g, "rb").read()
    cnt, = struct.unpack_from(">I", b, 0)
    if cnt > MAXSPANS:
        raise SystemExit("span log overflowed (%d > %d)" % (cnt, MAXSPANS))
    spans = []
    for i in range(cnt):
        a, c = struct.unpack_from(">II", b, 4 + i * 8)
        spans.append((a >> 16, a & 0xFFFF, c & 0xFFFF, c >> 16))   # y,x0,x1,tex
    return spans


def model_spans(planes, cam, sa16, ca16, zmajor):
    """The same walk as p2_flats_fixed.render_fixed, emitting the span list
    rather than a tag buffer.

    PLANE-MAJOR to match the kernel's emission order — but deliberately still
    scanning ALL 120 rows with the exact sign/near/far tests rather than the
    row bounds from plane_rows().  That is the point: if a row bound is too
    TIGHT the kernel drops a span the model still emits, and the diff catches
    it.  Bounding both sides here would make the check blind to exactly the
    failure that has no other symptom."""
    camx, camy, camz = cam
    out = []
    for pl in planes:
        for sy in range(fx.RENDER_H):
            yr = sy - fx.CY
            if yr == 0:
                continue
            dh = pl['h'] - camy
            if dh == 0 or (dh < 0) != (yr < 0):
                continue
            Z = fx.tdiv(fx.imult(fx.FOCAL_Y, dh, "f*dh"), yr)
            if Z < fx.NEARZ or Z > fx.FARZ:
                continue
            zs = fx.sharq(fx.imult(Z, sa16, "Z*sa"), 14)
            zc = fx.sharq(fx.imult(Z, ca16, "Z*ca"), 14)
            ox, oz = camx + zs, camz + zc
            ra = fx.rec(zc) if abs(zc) >= fx.DMIN else None
            rb = fx.rec(zs) if abs(zs) >= fx.DMIN else None
            p0 = p1 = -1
            for (x0, x1, z0, z1) in (pl['rz'] if zmajor else pl['rx']):
                lo, hi = -32768, 32767
                if ra is not None:
                    u0 = fx.sharq(fx.imult(fx.sharq(x0 - ox, 4), ra, "u0"), 8)
                    u1 = fx.sharq(fx.imult(fx.sharq(x1 - ox, 4), ra, "u1"), 8)
                    if u0 > u1: u0, u1 = u1, u0
                    lo, hi = max(lo, u0), min(hi, u1)
                elif not (x0 <= ox <= x1):
                    continue
                if rb is not None:
                    v0 = fx.sharq(fx.imult(fx.sharq(oz - z1, 4), rb, "v0"), 8)
                    v1 = fx.sharq(fx.imult(fx.sharq(oz - z0, 4), rb, "v1"), 8)
                    if v0 > v1: v0, v1 = v1, v0
                    lo, hi = max(lo, v0), min(hi, v1)
                elif not (z0 <= oz <= z1):
                    continue
                if hi <= lo:
                    continue
                i0 = max(0, fx.CX + lo)
                i1 = min(fx.RENDER_W, fx.CX + hi)
                if i1 <= i0:
                    continue
                if p0 < 0:
                    p0, p1 = i0, i1
                elif i0 <= p1 and i1 >= p0:
                    p0, p1 = min(p0, i0), max(p1, i1)
                else:
                    out.append((sy, p0, p1, pl['tex'])); p0, p1 = i0, i1
            if p0 >= 0:
                out.append((sy, p0, p1, pl['tex']))
    return out


def check(room, cam, yaw, verbose=True):
    a = math.radians(yaw)
    sa16 = int(round(math.sin(a) * 16384))
    ca16 = int(round(math.cos(a) * 16384))
    planes, zmajor, tables = build_tables(room, cam, sa16, ca16)
    got = run_kernel(make_source(tables))
    want = model_spans(planes, cam, sa16, ca16, zmajor)

    ok = got == want
    if verbose:
        print("  yaw %3d   kernel %4d spans   model %4d spans   %s"
              % (yaw, len(got), len(want), "MATCH" if ok else "*** DIFFER ***"))
        if not ok:
            for i in range(max(len(got), len(want))):
                g = got[i] if i < len(got) else None
                w = want[i] if i < len(want) else None
                if g != w:
                    print("    first diff at #%d:  kernel %s   model %s" % (i, g, w))
                    break
    return ok, len(got)


def main():
    args = sys.argv[1:]
    path = args[0] if args and not args[0].startswith('--') else 'sr_sr.bin'
    def opt(n, d): return args[args.index(n) + 1] if n in args else d
    want = int(opt('--room', '26'))
    rooms = ref.load(os.path.join(JAG, path) if not os.path.isabs(path) else path)
    rm = rooms[want]
    xS, zS, cells = rm['xS'], rm['zS'], rm['cells']
    gx, gz = xS // 2, zS // 2
    ft, ct, fy, cyc = cells[gx * zS + gz]
    if fy == fx.SOLID:
        for i, c in enumerate(cells):
            if c[2] != fx.SOLID:
                gx, gz, (ft, ct, fy, cyc) = i // zS, i % zS, c
                break
    cam = (gx * 1024 + 512, fy * 256 - 512, gz * 1024 + 512)

    print("gpu_sflats.gas vs the fixed-point model — room %d, camera %s"
          % (want, cam))
    yaws = [float(opt('--yaw', '0'))] if '--yaw' in args else \
           ([45.0 * k for k in range(8)] if '--all' in args else [0.0, 90.0])
    allok = True
    for y in yaws:
        ok, n = check(rm, cam, y)
        allok = allok and ok
    print("\n  %s" % ("ALL MATCH — the assembly is the model." if allok else
                      "☠️ MISMATCH — fix the ASSEMBLY, the model is the oracle."))
    return 0 if allok else 1


if __name__ == '__main__':
    sys.exit(main())
