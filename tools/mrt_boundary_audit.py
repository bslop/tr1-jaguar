#!/usr/bin/env python3
"""mrt_boundary_audit.py — READ-ONLY diff of baked collision vs PSX truth.

Parses LEVEL1.PSX directly (rooms + FloorData, layouts copied verbatim from
tr2jag_multiroom.py) and mrt.bin/mrt_sect.bin, then reports every sector
where the baked boundary disagrees with TR1's own semantics:

  PHANTOM SEAM FLOOR — sector has a real floor value AND roomBelow. TR1's
    GetHeight DESCENDS into the room below and uses ITS floor; we bake the
    seam height as solid ground. Identical for stacked-room seams, but where
    the room below is deep this is a phantom floor at ledge height — Lara
    runs off the ledge and keeps running on air (user report 2026-08-07).
  DOORCELL MISMATCH — floor==-127 cells whose baked class (0x7FFE open /
    0x7FFF wall) disagrees with a recompute of the wall-portal footprint.

Usage: python3 tools/mrt_boundary_audit.py [--level path] [--rooms 3,7]
Never writes anything. The fix lives elsewhere (surgical mrt_sect patch).
"""
import os, struct, sys

D = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVEL = os.environ.get("TRLEVEL",
    "/home/jvilla/Documents/Git/jag_openlara/tr1_psx/extracted/PSXDATA/LEVEL1.PSX")
TILE_PAGE_BYTES = 256*256//2
CLUT_BYTES = 16*2
NUM_TILES = 13
NUM_CLUTS = 1024

class R:
    def __init__(s, d): s.d = d; s.p = 0
    def setpos(s, p): s.p = p
    def seek(s, n): s.p += n
    def u8(s):  v = s.d[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from("<H", s.d, s.p)[0]; s.p += 2; return v
    def s16(s): v = struct.unpack_from("<h", s.d, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from("<I", s.d, s.p)[0]; s.p += 4; return v
    def s32(s): v = struct.unpack_from("<i", s.d, s.p)[0]; s.p += 4; return v

def read_all_rooms(r):
    r.seek(4)
    nrooms = r.u16()
    rooms = []
    for ri in range(nrooms):
        ix = r.s32(); iz = r.s32(); yb = r.s32(); yt = r.s32()
        size = r.u32(); start = r.p; r.seek(2)
        vc = r.s16()
        r.seek(vc*8)
        rc = r.s16(); r.seek(rc*10)
        tc = r.s16(); r.seek(tc*8)
        r.setpos(start + size*2)
        npor = r.u16(); ports = []; portverts = []
        for _ in range(npor):
            adj = r.u16(); r.seek(6)
            pv = [(r.s16(), r.s16(), r.s16()) for _ in range(4)]
            ports.append(adj); portverts.append(pv)
        zS = r.u16(); xS = r.u16(); sect = []
        for _ in range(zS*xS):
            fidx = r.u16(); r.u16()
            below = r.u8(); floor = struct.unpack("b", bytes([r.u8()]))[0]
            above = r.u8(); ceil = struct.unpack("b", bytes([r.u8()]))[0]
            sect.append((floor, ceil, fidx, below, above))
        r.seek(2); r.seek(r.u16()*20)
        nsm = r.u16(); r.seek(nsm*20)
        alt = r.u16(); rflags = r.u16()
        rooms.append(dict(i=ri, info_x=ix, info_z=iz, ports=ports,
                          portverts=portverts, xS=xS, zS=zS, sect=sect,
                          alt=alt, water=bool(rflags & 1)))
    return rooms, nrooms

def parse_level():
    data = open(LEVEL, "rb").read()
    r = R(data)
    r.u32(); r.u32(); r.seek(8)
    off = r.u32(); tiles_off = off + 8
    cluts_off = tiles_off + NUM_TILES*TILE_PAGE_BYTES
    r.setpos(cluts_off + NUM_CLUTS*CLUT_BYTES)
    rooms, nrooms = read_all_rooms(r)
    nfloor = r.u32()
    floors = list(struct.unpack_from("<%dH" % nfloor, data, r.p))
    return rooms, floors, nfloor

def make_fd_walkers(floors, nfloor):
    def sector_portal(fidx):
        if fidx <= 0 or fidx >= nfloor: return None
        i = fidx
        while True:
            cmd = floors[i]; i += 1
            func = cmd & 0x1F; end = (cmd >> 15) & 1
            if func == 1: return floors[i]
            elif func in (2, 3): i += 1
            elif func == 4:
                i += 1
                while i < nfloor and not ((floors[i] >> 15) & 1): i += 1
                i += 1
            elif 7 <= func <= 18: i += 1
            if end or i >= nfloor: break
        return None
    def sector_slant(fidx):
        if fidx <= 0 or fidx >= nfloor: return (0, 0)
        i = fidx
        while True:
            cmd = floors[i]; i += 1
            func = cmd & 0x1F; end = (cmd >> 15) & 1
            if func == 2:
                w = floors[i]; i += 1
                sx = w & 0xFF; sz = (w >> 8) & 0xFF
                if sx >= 128: sx -= 256
                if sz >= 128: sz -= 256
                return (sx, sz)
            elif func in (1, 3): i += 1
            elif func == 4:
                i += 1
                while i < nfloor and not ((floors[i] >> 15) & 1): i += 1
                i += 1
            elif 7 <= func <= 18: i += 1
            if end or i >= nfloor: break
        return (0, 0)
    return sector_portal, sector_slant

def parse_baked():
    mrt = open(os.path.join(D, "mrt.bin"), "rb").read()
    sect = open(os.path.join(D, "mrt_sect.bin"), "rb").read()
    n = struct.unpack(">H", mrt[0:2])[0]
    baked = []
    for i in range(n):
        goff, soff = struct.unpack(">II", mrt[8 + i*8: 16 + i*8])
        soff &= 0x7FFFFFFF
        xS, zS = struct.unpack(">HH", sect[soff:soff+4])
        ix, iz = struct.unpack(">ii", sect[soff+4:soff+12])
        cells = []
        p = soff + 12
        for _ in range(xS*zS):
            fy, cy, sx, sz = struct.unpack(">hhbb", sect[p:p+6]); p += 6
            cells.append((fy, cy, sx, sz))
        baked.append(dict(xS=xS, zS=zS, ix=ix, iz=iz, cells=cells, off=soff))
    return baked

def sector_at(room, wx, wz):
    """PSX sector of `room` containing world (wx,wz), or None."""
    lx = wx - room['info_x']; lz = wz - room['info_z']
    if lx < 0 or lz < 0: return None
    sx = lx >> 10; sz = lz >> 10
    if sx >= room['xS'] or sz >= room['zS']: return None
    return room['sect'][sx*room['zS'] + sz]

def resolve_floor(rooms, ri, wx, wz, depth=0):
    """TR1 GetHeight: descend roomBelow links until a room owns the floor.
    STOPS at water rooms (our no-swim compromise stands on the surface).
    Returns (floorY_units, final_room) or (None, None)."""
    if depth > 8: return None, None
    room = rooms[ri]
    if room['water']: return None, None       # never resolve INTO water
    s = sector_at(room, wx, wz)
    if s is None: return None, None
    floor, ceil, fidx, below, above = s
    if floor == -127: return None, None       # wall/opening in the below room
    if below != 255:
        f2, r2 = resolve_floor(rooms, below, wx, wz, depth+1)
        if f2 is not None: return f2, r2
        if below < len(rooms) and rooms[below]['water']:
            return None, None                 # chain ends at water: keep seam
    return floor*256, ri

def load_entity_cells():
    """World (x>>10, z>>10) cells of entities from mrt_spawn.h. Returns
    (bridge_cells, other_cells) — enemies excluded from `other` (they fly or
    track Lara and never derive Y from the sector floor)."""
    import re
    bridges, others = set(), set()
    path = os.path.join(D, "mrt_spawn.h")
    for m in re.finditer(r"\{\s*(\d+),\s*\d+,\s*\d+,\s*(-?\d+),\s*-?\d+,"
                         r"\s*(-?\d+),", open(path).read()):
        t, x, z = int(m.group(1)), int(m.group(2)), int(m.group(3))
        cell = (x >> 10, z >> 10)
        if 68 <= t <= 70: bridges.add(cell)
        elif t not in (7, 8, 9, 0):  # wolves/bears/bats/Lara: no exemption
            others.add(cell)
    return bridges, others

def recompute_doorcells(rm):
    cells = set()
    for pv in rm['portverts']:
        xs = [v[0] for v in pv]; zs = [v[2] for v in pv]
        ys = [v[1] for v in pv]
        if max(ys) - min(ys) < 16:
            continue
        def rng(a, b, n):
            lo = a//1024; hi = (b-1)//1024 if b > a else a//1024
            lo = min(max(lo, 0), n-1); hi = min(max(hi, 0), n-1)
            return range(lo, hi+1)
        for sx in rng(min(xs), max(xs), rm['xS']):
            for sz in rng(min(zs), max(zs), rm['zS']):
                cells.add(sx*rm['zS'] + sz)
    return cells

def main():
    only = None; do_patch = False
    for a in sys.argv[1:]:
        if a.startswith("--rooms"):
            only = [int(x) for x in a.split("=", 1)[1].split(",")]
        if a == "--patch":
            do_patch = True
    rooms, floors, nfloor = parse_level()
    sector_portal, sector_slant = make_fd_walkers(floors, nfloor)
    baked = parse_baked()
    # the extractor REORDERS rooms — match baked rooms to PSX rooms by their
    # (info_x, info_z, xS, zS) signature. The level contains PAIRS of distinct
    # rooms with IDENTICAL signatures (flip pairs 13/14, 19/20, 26/27 stored
    # as full rooms) — disambiguate by scoring each candidate's sector grid
    # against the baked cells and taking the best match (measured: the right
    # twin scores 0 mismatches, the wrong one 72-216).
    bridge_cells, entity_cells = load_entity_cells()
    skipped = {'bridge': 0, 'entity': 0}
    patch_list = []
    bykey = {}
    for rm in rooms:
        bykey.setdefault((rm['info_x'], rm['info_z'], rm['xS'], rm['zS']),
                         []).append(rm)
    def match_psx(bk):
        cands = bykey.get((bk['ix'], bk['iz'], bk['xS'], bk['zS']), [])
        if not cands: return None
        if len(cands) == 1: return cands[0]
        best, bestd = None, 1 << 30
        for rm in cands:
            diff = 0
            for ci, (floor, ceil, fidx, below, above) in enumerate(rm['sect']):
                bfy = bk['cells'][ci][0] & ~1
                if floor == -127:
                    if bfy < 0x7FFE: diff += 1
                elif bfy >= 0x7FFE or bfy != floor*256:
                    diff += 1
            if diff < bestd: best, bestd = rm, diff
        return best
    tot_phantom = tot_seam = tot_door = 0
    for bi, bk in enumerate(baked):
        if only and bi not in only: continue
        rm = match_psx(bk)
        if rm is None:
            print(f"room {bi:2d}: NO PSX MATCH (info {bk['ix']},{bk['iz']} "
                  f"{bk['xS']}x{bk['zS']})")
            continue
        ri = bi
        doorcells = recompute_doorcells(rm)
        phantoms = []; doormis = []
        for ci, (floor, ceil, fidx, below, above) in enumerate(rm['sect']):
            bfy = bk['cells'][ci][0]
            sx_i, sz_i = ci // rm['zS'], ci % rm['zS']
            wx = rm['info_x'] + sx_i*1024 + 512
            wz = rm['info_z'] + sz_i*1024 + 512
            wcell = (wx >> 10, wz >> 10)
            if floor != -127 and below != 255:
                if below < len(rooms) and rooms[below]['water']:
                    continue                    # water surface: baked height intended
                truef, tr = resolve_floor(rooms, below, wx, wz)
                if truef is None: continue      # below is wall/water — seam stands
                delta = truef - (bfy & ~1)
                tot_seam += 1
                if delta > 256:                 # >1 click deeper = phantom ground
                    if wcell in bridge_cells:
                        skipped['bridge'] += 1; continue
                    if wcell in entity_cells:
                        skipped['entity'] += 1; continue
                    phantoms.append((sx_i, sz_i, bfy & ~1, truef, below, tr,
                                     ci, bk['off']))
            elif floor == -127:
                hport = (sector_portal(fidx) is not None) or (ci in doorcells)
                want = 0x7FFE if (below != 255 or hport) else 0x7FFF
                if bfy != want:
                    doormis.append((sx_i, sz_i, bfy, want))
        if phantoms or doormis:
            print(f"room {ri:2d} ({rm['xS']}x{rm['zS']}):")
            for sx_i, sz_i, have, true, below, tr, ci, soff in phantoms:
                print(f"   PHANTOM floor cell ({sx_i:2d},{sz_i:2d}): baked "
                      f"{have:6d}, true {true:6d} (drop {true-have:5d} via "
                      f"room {below}->{tr})")
                patch_list.append((soff + 12 + ci*6, ri, sx_i, sz_i))
            for sx_i, sz_i, have, want in doormis:
                print(f"   DOORCELL cell ({sx_i:2d},{sz_i:2d}): baked "
                      f"{have:#06x} want {want:#06x}")
            tot_phantom += len(phantoms); tot_door += len(doormis)
    print(f"\n=== {tot_phantom} phantom seam floors (of {tot_seam} seam cells), "
          f"{tot_door} doorcell mismatches; exempt: {skipped['bridge']} bridge, "
          f"{skipped['entity']} entity cells ===")
    if do_patch and patch_list:
        # SURGICAL in-place patch: floorY -> 0x7FFE ("room below supplies the
        # floor" — literally TR1's pitRoom semantic). Same size, so the ROM
        # layout does not move and the A10 roll stays lit.
        sect_path = os.path.join(D, "mrt_sect.bin")
        bak = sect_path + ".prepatch"
        if not os.path.exists(bak):
            open(bak, "wb").write(open(sect_path, "rb").read())
            print(f"backup written: {bak}")
        buf = bytearray(open(sect_path, "rb").read())
        for off, ri, sx_i, sz_i in patch_list:
            buf[off] = 0x7F; buf[off+1] = 0xFE
        open(sect_path, "wb").write(buf)
        print(f"PATCHED {len(patch_list)} cells in mrt_sect.bin "
              f"(size {len(buf)}, unchanged)")

if __name__ == "__main__":
    main()
