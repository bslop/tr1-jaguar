#!/usr/bin/env python3
"""
P2b OFFLINE MODEL of gpu_sflats.gas — the EXACT integer arithmetic the kernel
does, checked against the float oracle in p2_flats_ref.py.

WHY THIS FILE EXISTS
    The offline emulator loop is down (jagemu freezes on the title), so every
    visual check costs a physical flash + reset (~208 s) and a trip to the
    board.  Fixed-point sign, overflow and quantisation bugs are exactly the
    class of bug that would eat a dozen of those bounces.  This runs the SAME
    operations the assembly runs — 16x16 imult, unsigned div with manual sign,
    arithmetic shifts — and asserts every intermediate fits the register width
    the instruction actually provides.
    ⇒ If this disagrees with p2_flats_ref.py, the kernel is wrong.  Fix it HERE.

★★★★★ THE ONE DESIGN IDEA — CLIP IN SCREEN SPACE, NOT WORLD SPACE
    The reference solves for t (a WORLD lateral offset) and then projects:
        t  = (x0 - ox)/ca ;  sx = CX + FOCAL*t/Z
    Composing those two:
        sx - CX = (x0 - ox) * FOCAL / (Z*ca)
    i.e. the slab clip and the projection are ONE multiply by a reciprocal
    whose denominator is  Z*ca.  And:
        * Z  is constant for the ROW
        * ca is constant for the FRAME
    ⇒ so 1/(Z*ca) is ONE divide per (row, plane) — and it does the job of the
    projection divide AND both x-slab divides AND both z-slab divides.
    Per RUN: zero divides, four imults, some compares.  That is the "no divides
    in the run loop" rule the kernel header states, satisfied structurally
    rather than by hoisting five separate reciprocals.

★★★★★ THE SECOND DESIGN IDEA — PAINTER ORDER IS FREE
    The old row walk in gpu_sflats.gas picked ONE nearest plane per row.  That
    is wrong: two floors at different heights can both be visible on the same
    row, at different columns.  The reference resolves per COLUMN.
    But for a horizontal plane,  Z = FOCAL_Y*(h-cam_y)/(y-CY), so at a given
    row the depth ORDER of the planes depends only on |h - cam_y| — and that
    ordering is THE SAME FOR EVERY ROW.
    ⇒ the 68k sorts the visplane list ONCE PER FRAME by |h - cam_y| descending,
    and the kernel just draws them in list order, far to near.  Correct
    occlusion with no z-buffer, no per-row sort, no per-column state.

★★★★ THE TWO THINGS THAT CLOSE THE GAP TO THE ORACLE  (measured, room 26, 8 yaws;
    spans/frame, lower is better — setup is 82% of Tom's per-face cost)
        naive                 232      1.81x vs the polygon path
        + axis-matched runs   202      2.08x
        + greedy row merge    170      2.47x     <- what this model implements
        oracle (exact)        163      2.58x
        polygon path today    420      1.00x
    1. AXIS-MATCHED RUN LISTS.  A row's world line runs along (ca, -sa).  If
       the plane's cells are merged into rects elongated ACROSS that direction,
       one row crosses many rects and emits many adjacent spans.  Merging the
       same cells the other way makes it one rect.  Both merges are STATIC per
       room, so we store BOTH and the 68k points at one per frame:
       |ca| >= |sa| -> x-major, else z-major.  Runtime cost: zero.
       (This alone is 280 -> 134 spans at yaw 90.)
    2. GREEDY ROW MERGE, NO SORT.  Within one (row, plane) keep a single
       PENDING interval; extend it when the next run touches or overlaps,
       otherwise flush and start again.  Runs arrive in near-screen order, so
       this catches almost everything a full sort would, for ~8 instructions
       per run and no memory.  Out-of-order arrivals cost an extra flush, never
       correctness.  (This is what fixes the diagonal yaws: 309 -> 204 at 135.)

Usage:
    python3 tools/p2_flats_fixed.py [sr_sr.bin] [--room N] [--yaws 8]
"""
import sys, math, struct

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import p2_flats_ref as ref

FOCAL, FOCAL_Y = 160, 80
RENDER_W, RENDER_H = 320, 120
CX, CY = 160, 60
NEARZ = 64
FARZ = 32000                 # Z must fit s16: it is an imult operand
DMIN = 32                    # |Z*ca| below this -> the axis is degenerate
RNUM = 655360                # FOCAL * 16 * 256  (see rec() below)
SOLID = -127
NONE = 0xFFFF


# ---- the machine ------------------------------------------------------------
def s16(v, what):
    assert -32768 <= v <= 32767, "%s = %d does not fit s16 (imult operand)" % (what, v)
    return v


def s32(v, what):
    assert -(1 << 31) <= v < (1 << 31), "%s = %d overflows s32" % (what, v)
    return v


def imult(a, b, what):
    """Tom IMULT: signed 16x16 -> 32.  BOTH operands must fit s16."""
    return s32(s16(a, what + ".a") * s16(b, what + ".b"), what)


def tdiv(n, d):
    """Tom DIV is UNSIGNED 32/32; the kernel takes |n|,|d| and re-applies the
    sign, which truncates TOWARD ZERO (not Python's floor)."""
    assert d != 0
    q = abs(n) // abs(d)
    return -q if (n < 0) != (d < 0) else q


def sharq(v, n):
    """Tom SHARQ: arithmetic shift right = floor.  Python >> matches."""
    return v >> n


def rec(d):
    """RA = RNUM/d.  Guarded by |d| >= DMIN so the result fits s16:
       655360/32 = 20480."""
    r = tdiv(RNUM, d)
    return s16(r, "reciprocal(%d)" % d)


# ---- visplane build, WITH run merging ---------------------------------------
def build_planes(room, cam_y):
    """Group cell surfaces by (height, texture, kind), merge their cells into
    maximal rects BOTH WAYS, and sort the planes FAR->NEAR by |h - cam_y|.

    The merge is what turns 296 flat surfaces into 19 visplanes with a short
    run list; the sort is what makes painter order correct for every row; and
    keeping both merges is what lets the 68k hand the kernel run rects that lie
    ALONG the row's world line instead of across it (see the header)."""
    xS, zS, cells = room['xS'], room['zS'], room['cells']
    groups = {}
    for sx in range(xS):
        for sz in range(zS):
            ft, ct, fy, cy = cells[sx * zS + sz]
            if fy == SOLID:
                continue
            for kind, h_click, tex in (('f', fy, ft), ('c', cy, ct)):
                if tex == NONE or h_click == SOLID:
                    continue
                groups.setdefault((h_click * 256, tex, kind), set()).add((sx, sz))

    planes = []
    for (h, tex, kind), cellset in groups.items():
        planes.append(dict(h=h, tex=tex, kind=kind,
                           rx=merge_cells(cellset),
                           rz=merge_cells_z(cellset)))
    # FAR -> NEAR for every row simultaneously: see the header.
    planes.sort(key=lambda p: -abs(p['h'] - cam_y))
    return planes


def plane_rows(pl, cam, sa16, ca16, zmajor):
    """The ROW RANGE the kernel is handed at record +24/+28 — what the 68k
    computes once per frame per plane.

    ★★★★★ THIS IS THE CULL THAT MAKES THE KERNEL AFFORDABLE.  A row's world line
    is the locus of camera depth == Z, so a plane can only appear on rows whose
    Z falls inside the camera-space depth range of its bounding box.  That range
    depends on the bbox, the camera and the yaw — NOT on the row — so it is a
    per-frame constant, and Z = FOCAL_Y*dh/yr turns it straight into a row span.
    Measured, room 26: 2280 (row,plane) visits -> ~270.

    ☠️ CONSERVATIVE ONLY.  Widening costs a few visits that emit nothing (the
    per-run clip still rejects them, so the span list is unchanged).  Narrowing
    silently drops spans and nothing downstream would notice — which is why the
    +1 and the floor below are deliberate, not sloppy.
    """
    camx, camy, camz = cam
    runs = pl['rz'] if zmajor else pl['rx']
    x0 = min(r[0] for r in runs); x1 = max(r[1] for r in runs)
    z0 = min(r[2] for r in runs); z1 = max(r[3] for r in runs)
    ds = [((X - camx) * sa16 + (Zc - camz) * ca16) >> 14
          for X in (x0, x1) for Zc in (z0, z1)]
    zlo = max(NEARZ, min(ds))
    zhi = min(FARZ, max(ds))
    dh = pl['h'] - camy
    if zhi < zlo or dh == 0:
        return (0, 0)
    ad = abs(dh) * FOCAL_Y
    yr_hi = ad // zlo + 1                 # Z >= zlo  =>  |yr| <= ad/zlo
    yr_lo = max(1, ad // zhi)             # Z <= zhi  =>  |yr| >= ad/zhi
    if yr_lo > yr_hi:
        return (0, 0)
    if dh > 0:
        r0, r1 = CY + yr_lo, CY + yr_hi + 1
    else:
        r0, r1 = CY - yr_hi, CY - yr_lo + 1
    r0 = max(0, min(RENDER_H, r0))
    r1 = max(0, min(RENDER_H, r1))
    return (r0, r1) if r1 > r0 else (0, 0)


def merge_cells_z(cellset):
    """The same greedy merge with the axes swapped: maximal Z-runs per x, then
    stacked across x.  Rects come out elongated in Z."""
    r = merge_cells({(z, x) for (x, z) in cellset})
    return [(z0, z1, x0, x1) for (x0, x1, z0, z1) in r]


def merge_cells(cellset):
    """Greedy: maximal x-runs per z, then stack identical x-runs across z."""
    byz = {}
    for (sx, sz) in cellset:
        byz.setdefault(sz, []).append(sx)
    strips = []                       # (x0, x1, z) in SECTOR units, x1 exclusive
    for sz, xs in byz.items():
        xs.sort()
        i = 0
        while i < len(xs):
            j = i
            while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1:
                j += 1
            strips.append((xs[i], xs[j] + 1, sz))
            i = j + 1
    strips.sort(key=lambda s: (s[0], s[1], s[2]))
    runs, i = [], 0
    while i < len(strips):
        x0, x1, z0 = strips[i]
        z1 = z0 + 1
        j = i + 1
        while j < len(strips) and strips[j][0] == x0 and strips[j][1] == x1 \
                and strips[j][2] == z1:
            z1 += 1
            j += 1
        runs.append((x0 * 1024, x1 * 1024, z0 * 1024, z1 * 1024))
        i = j
    return runs


# ---- the kernel, transliterated ---------------------------------------------
def render_fixed(room, cam, sa16, ca16, tag=None, stats=None):
    """One frame of gpu_sflats.gas, instruction-for-instruction in intent."""
    camx, camy, camz = cam
    planes = build_planes(room, camy)
    if tag is None:
        tag = [None] * (RENDER_W * RENDER_H)
    spans = 0
    px = 0

    # ---- per-FRAME: which run list lies ALONG the row line? ----------------
    zmajor = abs(sa16) > abs(ca16)

    for sy in range(RENDER_H):
        yr = sy - CY
        if yr == 0:
            continue
        row_off = sy * RENDER_W

        for pl in planes:                                  # FAR -> NEAR
            dh = pl['h'] - camy
            if dh == 0 or (dh < 0) != (yr < 0):            # xor sign test
                continue
            # ---- the row divide:  Z = FOCAL_Y*dh / yr ----------------------
            Z = tdiv(imult(FOCAL_Y, dh, "focal*dh"), yr)
            if Z < NEARZ or Z > FARZ:
                continue
            # ---- the row's world line:  P(u) walks it as u sweeps the row --
            zs = sharq(imult(Z, sa16, "Z*sa"), 14)
            zc = sharq(imult(Z, ca16, "Z*ca"), 14)
            ox = camx + zs
            oz = camz + zc
            dA, dB = zc, zs
            ra = rec(dA) if abs(dA) >= DMIN else None
            rb = rec(dB) if abs(dB) >= DMIN else None
            key = (pl['h'], pl['tex'], pl['kind'])
            p0 = p1 = -1                                   # pending span

            for (x0, x1, z0, z1) in (pl['rz'] if zmajor else pl['rx']):
                lo, hi = -32768, 32767
                # ---- x slab -----------------------------------------------
                if ra is not None:
                    e0 = sharq(x0 - ox, 4)
                    e1 = sharq(x1 - ox, 4)
                    u0 = sharq(imult(e0, ra, "e0*ra"), 8)
                    u1 = sharq(imult(e1, ra, "e1*ra"), 8)
                    if u0 > u1:
                        u0, u1 = u1, u0
                    if u0 > lo: lo = u0
                    if u1 < hi: hi = u1
                elif not (x0 <= ox <= x1):
                    continue
                # ---- z slab (world z = oz - u*dB/FOCAL, hence z1 first) ----
                if rb is not None:
                    f0 = sharq(oz - z1, 4)
                    f1 = sharq(oz - z0, 4)
                    v0 = sharq(imult(f0, rb, "f0*rb"), 8)
                    v1 = sharq(imult(f1, rb, "f1*rb"), 8)
                    if v0 > v1:
                        v0, v1 = v1, v0
                    if v0 > lo: lo = v0
                    if v1 < hi: hi = v1
                elif not (z0 <= oz <= z1):
                    continue
                if hi <= lo:
                    continue
                # ---- to screen columns (the projection is ALREADY done) ----
                i0 = CX + lo
                i1 = CX + hi
                if i0 < 0: i0 = 0
                if i1 > RENDER_W: i1 = RENDER_W
                if i1 <= i0:
                    continue
                # ---- greedy merge: extend the pending span, or flush it ----
                if p0 < 0:
                    p0, p1 = i0, i1
                elif i0 <= p1 and i1 >= p0:
                    if i0 < p0: p0 = i0
                    if i1 > p1: p1 = i1
                else:
                    spans += 1; px += p1 - p0
                    for i in range(p0, p1): tag[row_off + i] = key
                    p0, p1 = i0, i1
            if p0 >= 0:
                spans += 1; px += p1 - p0
                for i in range(p0, p1): tag[row_off + i] = key
    if stats is not None:
        stats['spans'] = spans
        stats['blit_px'] = px
        stats['planes'] = len(planes)
        stats['runs'] = sum(len(p['rz' if zmajor else 'rx']) for p in planes)
    return tag


# ---- comparison against the float oracle ------------------------------------
def main():
    args = sys.argv[1:]
    path = args[0] if args and not args[0].startswith('--') else 'sr_sr.bin'
    def opt(n, d): return args[args.index(n) + 1] if n in args else d
    want = int(opt('--room', '26'))
    nyaw = int(opt('--yaws', '8'))

    rooms = ref.load(path)
    if want not in rooms:
        raise SystemExit("room %d not in blob; have %s" % (want, sorted(rooms)))
    rm = rooms[want]
    xS, zS, cells = rm['xS'], rm['zS'], rm['cells']

    # FARZ sanity: can this room even fit inside a s16 depth?
    diag = int(math.hypot(xS * 1024, zS * 1024))
    print("room %d: %dx%d sectors, diagonal %d units (FARZ %d) -> %s"
          % (want, xS, zS, diag, FARZ,
             "OK" if diag <= FARZ else "☠️ FAR CLIP WILL BE VISIBLE"))

    gx, gz = xS // 2, zS // 2
    ft, ct, fy, cyc = cells[gx * zS + gz]
    if fy == SOLID:
        for i, c in enumerate(cells):
            if c[2] != SOLID:
                gx, gz, (ft, ct, fy, cyc) = i // zS, i % zS, c
                break
    cam = (gx * 1024 + 512, fy * 256 - 512, gz * 1024 + 512)
    print("camera %s\n" % (cam,))

    print("  yaw   agree%%   ref_px  fix_px   spans  px/span  runs  planes")
    tot_agree = tot_ref = 0
    for k in range(nyaw):
        yaw = 360.0 * k / nyaw
        a = math.radians(yaw)
        sa16 = int(round(math.sin(a) * 16384))
        ca16 = int(round(math.cos(a) * 16384))

        tag_ref, _ = ref.render(rm, cam, yaw)
        st = {}
        tag_fix = render_fixed(rm, cam, sa16, ca16, stats=st)

        # the oracle tags (h,tex,kind); ours does too
        agree = same = refpx = fixpx = 0
        for i in range(RENDER_W * RENDER_H):
            r, f = tag_ref[i], tag_fix[i]
            if r is not None: refpx += 1
            if f is not None: fixpx += 1
            if r == f: agree += 1
        tot_agree += agree
        tot_ref += RENDER_W * RENDER_H
        print("  %3d   %6.2f   %6d  %6d  %6d   %6.1f  %4d   %5d"
              % (yaw, 100.0 * agree / (RENDER_W * RENDER_H), refpx, fixpx,
                 st['spans'], (st['blit_px'] / st['spans']) if st['spans'] else 0,
                 st['runs'], st['planes']))
    print("  ---")
    print("  MEAN pixel agreement with the float oracle: %.2f%%"
          % (100.0 * tot_agree / tot_ref))
    print()
    print("  Disagreement is expected only at SPAN EDGES (the oracle rounds")
    print("  with ceil(sx-0.5); the kernel truncates) — a few px per span.")
    print("  A large disagreement means a SIGN or OVERFLOW bug, not rounding.")


if __name__ == '__main__':
    main()
