#!/usr/bin/env python3
"""OCCLUSION step-0: simulate (a) tile-coverage face-skip and (b) span-merge
freerider against real JAGEMU_BLIT_TRACE span streams.

Faces are delimited by the v4b per-face rect-shade blits (cmd=01C00E08 at
pkt_done) -- REAL kernel face boundaries, not column heuristics (the trapezoid
step-0 poison).  k=0 faces lack a delimiter and merge into the next face
(~1% in Caves, noted).

Candidate (a) win pool = faces that contribute ZERO final pixels (fully
occluded by later-painted content).  Simulated by a reverse-painter pass
accumulating coverage; a face whose bbox tiles are all covered at its turn is
skippable.  Coverage semantics variants:
  X  exact per-pixel tile coverage (un-implementable ceiling)
  R  inscribed-rect per face (realistic cheap kernel update: max(xl),min(xr))
Tile sizes 16x16 and 32x16 simulated (SRAM scraps=28B < 38B needed for 16x16).

Candidate (b) = count same-row x-adjacent same-texture span pairs whose u/v
continue exactly (constant (du,dv)*w offset across the face pair's shared
rows, +-1 trace-truncation tolerance).
"""
import re, sys, collections
import numpy as np

W, H = 320, 240

line_re = re.compile(
    r"BLIT cmd=([0-9A-F]+) O=(\d+) I=(\d+) \| DST base=([0-9A-F]+) x=(-?\d+) y=(-?\d+).*?"
    r"\| SRC base=([0-9A-F]+) x=(-?\d+) y=(-?\d+)")

def parse(path):
    """returns list of renders; render = list of faces; face = list of spans
    (x,y,w,srcbase,sx,sy).  Renders keyed per-fb-base (PIPELINE-safe): a
    full-screen clear (O=240 I=320, GPU 01800E01 title path or 68k 01800200
    play path) on base B closes B's current render and opens a new one."""
    renders = []
    cur = {}          # fbbase -> {'faces': [...], 'spans': [...]}
    for ln in open(path):
        m = line_re.search(ln)
        if not m: continue
        cmd, O, I, db, dx, dy, sb, sx, sy = m.groups()
        cmd = int(cmd, 16); O = int(O); I = int(I)
        db = int(db, 16); dx = int(dx); dy = int(dy)
        sb = int(sb, 16); sx = int(sx); sy = int(sy)
        if O == 240 and I == 320 and cmd in (0x01800E01, 0x01800200):
            c = cur.get(db)
            if c is not None:
                if c['spans']: c['faces'].append(c['spans'])
                renders.append(c['faces'])
            cur[db] = {'faces': [], 'spans': []}
            continue
        c = cur.get(db)
        if c is None: continue
        if cmd == 0x01800801 and O == 1:
            c['spans'].append((dx, dy, I, sb, sx, sy))
        elif cmd == 0x01C00E08:                 # pkt_done shade = face boundary
            if c['spans']:
                c['faces'].append(c['spans']); c['spans'] = []
    for c in cur.values():
        if c['spans']: c['faces'].append(c['spans'])
        renders.append(c['faces'])
    return renders

def face_bbox(spans):
    x0 = min(s[0] for s in spans); x1 = max(s[0] + s[2] - 1 for s in spans)
    y0 = min(s[1] for s in spans); y1 = max(s[1] for s in spans)
    return max(x0,0), min(x1,W-1), max(y0,0), min(y1,H-1)

def inscribed_rect(spans):
    """[max(xl), min(xr)] x [y0,y1] if rows contiguous, else None"""
    ys = sorted(s[1] for s in spans)
    if ys[-1] - ys[0] + 1 != len(set(ys)) or len(set(ys)) != len(ys):
        # duplicate or gapped rows: bail (kernel would too -- needs contiguity)
        return None
    xl = max(s[0] for s in spans); xr = min(s[0] + s[2] - 1 for s in spans)
    if xr < xl: return None
    return max(xl,0), min(xr,W-1), max(ys[0],0), min(ys[-1],H-1)

def simulate_a(renders, tw, th, semantics):
    """reverse-painter coverage pass.  Returns aggregate dict."""
    TX, TY = (W + tw - 1)//tw, (H + th - 1)//th
    agg = collections.Counter()
    for faces in renders:
        grid = np.zeros((TY, TX), dtype=bool)
        if semantics == 'X':
            pix = np.zeros((H, W), dtype=bool)
            # per-tile pixel budget (tiles clipped at screen edge have fewer px)
            tilearea = np.zeros((TY, TX), dtype=np.int32)
            for ty in range(TY):
                for tx in range(TX):
                    tilearea[ty,tx] = (min((ty+1)*th,H)-ty*th) * (min((tx+1)*tw,W)-tx*tw)
        for spans in reversed(faces):
            bx0, bx1, by0, by1 = face_bbox(spans)
            t = grid[by0//th:by1//th+1, bx0//tw:bx1//tw+1]
            agg['faces'] += 1
            agg['spans'] += len(spans)
            agg['px'] += sum(s[2] for s in spans)
            if t.size and t.all():
                agg['skip_faces'] += 1
                agg['skip_spans'] += len(spans)
                agg['skip_px'] += sum(s[2] for s in spans)
                continue                      # skipped faces add no coverage
            # draw: update coverage
            if semantics == 'X':
                for x, y, w, *_ in spans:
                    if 0 <= y < H:
                        pix[y, max(x,0):min(x+w,W)] = True
                # refresh grid over touched tiles only
                for ty in range(by0//th, by1//th+1):
                    for tx in range(bx0//tw, bx1//tw+1):
                        if not grid[ty,tx]:
                            blk = pix[ty*th:(ty+1)*th, tx*tw:(tx+1)*tw]
                            if int(blk.sum()) == tilearea[ty,tx]:
                                grid[ty,tx] = True
            else:                              # 'R' inscribed rect
                r = inscribed_rect(spans)
                if r:
                    rx0, rx1, ry0, ry1 = r
                    # tiles FULLY inside the rect
                    tx0 = (rx0 + tw - 1)//tw; tx1 = (rx1 + 1)//tw  # [tx0,tx1)
                    ty0 = (ry0 + th - 1)//th; ty1 = (ry1 + 1)//th
                    if tx1 > tx0 and ty1 > ty0:
                        grid[ty0:ty1, tx0:tx1] = True
    return agg

def simulate_b(renders, tol=0):
    """count exact u/v-continuation same-row x-adjacent same-texture span pairs.
    tol = allowed |gap/overlap| in x-adjacency (diagnostic; correctness needs 0)"""
    tot_spans = 0; adj_pairs = 0; merges = 0; adj_anytex = 0
    for faces in renders:
        rows = collections.defaultdict(list)   # y -> [(x,w,sb,sx,sy,fid)]
        for fid, spans in enumerate(faces):
            tot_spans += len(spans)
            for x, y, w, sb, sx, sy in spans:
                rows[y].append((x, w, sb, sx, sy, fid))
        # collect candidate row-pairs grouped by face pair
        pair_rows = collections.defaultdict(list)  # (fidA,fidB) -> [(du_off,dv_off)]
        for y, lst in rows.items():
            lst.sort()
            for a, b in zip(lst, lst[1:]):
                if abs(a[0] + a[1] - b[0]) <= tol and a[5] != b[5]:
                    adj_anytex += 1
                    if a[2] != b[2]: continue
                    adj_pairs += 1
                    pair_rows[(a[5], b[5])].append((b[3]-a[3], b[4]-a[4], a[1]))
        for (fa, fb), lst in pair_rows.items():
            # exact continuation: (uB-uA, vB-vA) constant (+-1) across shared
            # rows AND consistent with w*du for a single du (w may vary by row:
            # if w varies, du_off must scale with w)
            if len(lst) == 1:
                du, dv, w = lst[0]
                # single row: accept if offsets look like w*step, |step|<=4
                if abs(du) <= 4*w+1 and abs(dv) <= 4*w+1:
                    merges += 1
                continue
            ok = True
            # fit du_step = du_off/w, dv_step = dv_off/w constant across rows
            base_du = lst[0][0]/max(lst[0][2],1); base_dv = lst[0][1]/max(lst[0][2],1)
            for du, dv, w in lst[1:]:
                if abs(du/max(w,1) - base_du)*w > 1.5 or abs(dv/max(w,1) - base_dv)*w > 1.5:
                    ok = False; break
            if ok:
                merges += len(lst)
    return tot_spans, adj_pairs, merges, adj_anytex

def report_scene(name, path, fps, dispatched, lastn=15):
    renders = parse(path)
    # in-game renders = ones containing face delimiters (title has none) and
    # enough spans
    ingame = [r for r in renders if len(r) > 30 and sum(len(f) for f in r) > 500]
    sample = ingame[-lastn:]
    n = len(sample)
    spans_pr = sum(len(f) for r in sample for f in r)/n
    px_pr = sum(s[2] for r in sample for f in r for s in f)/n
    faces_pr = sum(len(r) for r in sample)/n
    frame_cyc = 26.59e6 / fps
    print(f"\n=== {name}: renders={len(ingame)} sampled={n} "
          f"faces/render={faces_pr:.0f} spans/render={spans_pr:.0f} px/render={px_pr:.0f}")
    print(f"    frame={1000/fps:.1f}ms = {frame_cyc/1e6:.2f}M GPU-cyc @26.59MHz (silicon {fps} fps)")

    # cost model (documented constants)
    WALK_FRAC, FILL_FRAC, STAGE_FRAC = 0.27, 0.136, 0.20
    cyc_span = WALK_FRAC*frame_cyc/spans_pr
    cyc_px   = FILL_FRAC*frame_cyc/px_pr
    staged   = dispatched*0.75            # STAGEDIET culls ~25% pre-stage
    cyc_stage_face = STAGE_FRAC*frame_cyc/staged
    print(f"    derived: walk={cyc_span:.0f}cyc/span fill={cyc_px:.1f}cyc/px "
          f"stage={cyc_stage_face:.0f}cyc/face (staged~{staged:.0f})")

    print("\n--- CANDIDATE (a): tile-coverage face-skip (win pool via reverse-painter pass)")
    for tw, th in [(16,16), (32,16)]:
        for sem in ['X', 'R']:
            agg = simulate_a(sample, tw, th, sem)
            sf = agg['skip_faces']/n; ss = agg['skip_spans']/n; sp = agg['skip_px']/n
            tested = agg['faces']/n
            # savings: walk+fill always; stage only if test can precede staging
            save_wf = ss*cyc_span + sp*cyc_px
            save_st = sf*cyc_stage_face
            # costs: 20cyc test on every walked face; grid update:
            #  R: 4cyc/span min/max + 25cyc/face tile-OR;  X: not implementable
            cost = tested*20 + (0 if sem=='X' else (spans_pr-ss)*4 + (tested-sf)*25)
            net_lo = (save_wf - cost)/frame_cyc*100
            net_hi = (save_wf + save_st - cost)/frame_cyc*100
            print(f"  tiles {tw}x{th} sem={sem}: skip {sf:.1f}/{tested:.0f} faces "
                  f"({100*sf/tested:.1f}%)  spans {100*ss/spans_pr:.1f}%  px {100*sp/px_pr:.1f}%  "
                  f"NET {net_lo:+.2f}% (walk+fill) .. {net_hi:+.2f}% (+stage)")

    print("\n--- CANDIDATE (b): span-merge freerider (exact u/v continuation)")
    for tol in (0,1,2):
        tot, adj, merges, adjat = simulate_b(sample, tol)
        tot/=n; adj/=n; merges/=n; adjat/=n
        print(f"  tol={tol}: x-adj any-tex={adjat:.1f}  same-tex={adj:.1f} "
              f"({100*adj/tot:.2f}%)  exact-continuation merges={merges:.1f} ({100*merges/tot:.2f}%)")
    tot, adj, merges, _ = simulate_b(sample, 0)
    tot/=n; adj/=n; merges/=n
    # savings: one span setup+launch removed per merge.
    for setup_cyc in (60, 120):
        gross = merges*setup_cyc/frame_cyc*100
        # honest mechanism: pending-row buffer check per span (DRAM, contended)
        for chk in (15, 30):
            net = (merges*setup_cyc - tot*chk)/frame_cyc*100
            print(f"  save/merge={setup_cyc}cyc: GROSS {gross:+.2f}%   "
                  f"net w/ {chk}cyc/span check: {net:+.2f}%")
    return sample

if __name__ == "__main__":
    base = "/tmp/claude-1000/-home-jvilla-Documents-Git-jag-openlara/65f61037-d2a5-46c3-bbba-d55b0e3d270c/scratchpad"
    report_scene("SPAWN (trace_spawn_raw, TRAP_base=PLAY_PS cfg)", f"{base}/trace_spawn_raw.log",
                 19.5, 914)
    report_scene("JUNCTION r12 (trace_psr12, PLAY_PS_r12)", f"{base}/trace_psr12.log",
                 9.4, 2129)
