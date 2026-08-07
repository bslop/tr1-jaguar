#!/usr/bin/env python3
# jv_pacing_sim.py - discrete-event model of the boot FMV player's pacing,
# driven by a real .JV file's record sizes and the SILICON-MEASURED GD cost
# curve (GDPROBE 2026-08-06: gd_fread = 3.5ms fixed + 193KB/s). Validated:
# predicts the v8 low-water player at 76% on-cadence vs 74-78% measured on
# hardware. Use this to sweep read policies in seconds instead of 12-minute
# silicon cycles. Usage: python3 tools/jv_pacing_sim.py EIDOS.JV
import struct, sys, math, collections

d = open(sys.argv[1] if len(sys.argv) > 1 else "EIDOS.JV", "rb").read()
nf = struct.unpack(">H", d[10:12])[0]
recs = []
pos = 1024 + 4096
for i in range(nf):
    L, AL = struct.unpack(">II", d[pos:pos+8]); pos += 8
    recs.append((8 + AL + ((L + 3) & ~3), L, AL))
    pos += AL + ((L + 3) & ~3)

FIELD = 16.7
DECODE = 12.0
PARSE = 8.0
def rd(n): return 3.5 + n / 193000.0 * 1000.0

def sim(policy, name):
    t = 0.0; buf = 0
    remain = sum(r[0] for r in recs)
    displays = []; dprev = -1e9
    for fi, (rlen, L, AL) in enumerate(recs):
        while buf < rlen:                      # frame-top safety reads
            want = min(24576, 31488 - buf, remain)
            if want <= 0: break
            t += rd(want); buf += want; remain -= want
        buf -= rlen
        t += PARSE
        decode_done = t + DECODE
        for w in policy(fi, buf, remain):
            w = min(w, 31488 - buf, remain)
            if w > 0:
                t += rd(w); buf += w; remain -= w
        t = max(t, decode_done)
        tgt = max((fi + 1) * 4 * FIELD, dprev + 4 * FIELD, t)
        show = math.ceil(tgt / FIELD) * FIELD
        displays.append(show); dprev = show
        t = max(t, show - 2 * FIELD)
    durs = [displays[i+1] - displays[i] for i in range(len(displays) - 1)]
    c = collections.Counter(round(x / FIELD) for x in durs)
    good = sum(v for k, v in c.items() if 3 <= k <= 5)
    print(f"{name:28s} on-cadence {100*good/len(durs):3.0f}%  {dict(sorted(c.items()))}")

sim(lambda fi, b, r: [],                                  "frame-top only")
sim(lambda fi, b, r: [min(24576, 31488-b)] if b < 12288 else [], "v8 low-water 24KB")
sim(lambda fi, b, r: [7168] if b < 24576 else [],         "steady 7KB (SHIP TARGET)")
sim(lambda fi, b, r: [5120] if b < 24576 else [],         "steady 5KB")
