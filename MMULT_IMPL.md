# MMULT vertex-transform — IMPLEMENTATION (Phase 1 verified, Phase 2 specced)

Follows MMULT_SCOPE.md. Phase 0 PASSED on silicon (cobweb calib p_mmult,
2026-07-24): GPU MMULT computes 3×3·vec correctly; two hardware quirks that
jsim hides — (a) the bank-1 vector must be `moveta`'d in the instrs IMMEDIATELY
before each `mmult`; (b) the SRAM matrix element is read from the LOW 16 bits on
silicon (jsim reads HIGH). Reference idiom: Black Ice White Noise engine.bin
$5486 (`store r12,(r6); moveta; moveta; mmult; move r0,r3`).

## The transform (unchanged semantics)
vc_loop currently does the yaw+pitch rotate as 8 imul32 (vcf1..vcf8):
```
rx  = (dx·cY − dz·sY) >> 12
rz  = (dx·sY + dz·cY) >> 12           ; intermediate, rounded
ry  = (dy·cP − rz·sP) >> 12
rz2 = (dy·sP + rz·cP) >> 12
```
Precompose yaw·pitch into a flat 3×3 Q12 matrix R so (rx,ry,rz2)=R·(dx,dy,dz):
```
row0 (rx):   cY,        0,     −sY
row1 (ry):  −(sY·sP),   cP,    −(cY·sP)
row2 (rz2):  (sY·cP),   sP,     (cY·cP)     ; cross-products round-to-nearest
```

## Precision — VERIFIED (host + Python, 300k random poses)
Round-to-nearest cross products ((x*y+2048)>>12): **max cam-space diff vs the
imul32 path = (rx,ry,rz2) = (0,3,3)**; ~1–2px projected at typical depths (worst
~8px only at the near plane with max off-axis d — rare). Truncation gave (0,5,5)
/ ~14px, so round-to-nearest is worth the +2048. Accumulator max |Σ| ≈ 7.9e8 <
2^31 (safe). ALL 9 coeffs fit s16 (verified, zero overflow).

## Phase 1 (68k precompute) — helper VERIFIED on host, ready to wire
Each coeff is duplicated into BOTH 16-bit halves so silicon (low) and jsim
(high) both read it correctly (verified: zero half-mismatch):
```c
static uint32_t mtx_pack(int c){ return ((uint32_t)c<<16)|(uint32_t)(c&0xFFFF); }
static void build_xform_mtx(uint32_t *m,int cY,int sY,int cP,int sP){
    int sYsP=(sY*sP+2048)>>12, cYsP=(cY*sP+2048)>>12;
    int sYcP=(sY*cP+2048)>>12, cYcP=(cY*cP+2048)>>12;
    m[0]=mtx_pack(cY);    m[1]=mtx_pack(0);   m[2]=mtx_pack(-sY);
    m[3]=mtx_pack(-sYsP); m[4]=mtx_pack(cP);  m[5]=mtx_pack(-cYsP);
    m[6]=mtx_pack(sYcP);  m[7]=mtx_pack(sP);  m[8]=mtx_pack(cYcP);
}
```
Wiring: extend the camera block from 8 to 16 longs (idx 0–6 cam as now, idx 7
was the =0 spare → now the 9 matrix longs at 7..15). camblk fill sites in
main.c: **2087, 3258, 4218, 4266** (+ the `static uint32_t camblk[8]` locals at
2014, 2123 and the `[2][8]` at 1309 all widen to [16]). Guard the extra compute
+ the kernel path behind a matching `-DMMULTX` (C) / `-d MMULTX=1` (jas) so
MMULTX=0 stays byte-identical (kernel) and cost-free (68k). NB multiple render
paths (single gpu_geotex, batch dispatch, geomxform) — confirm the ACTIVE one
first and wire it; a camblk_fill() helper de-dups all sites.

## Phase 2 (kernel MMULT rotate) — specced, byte budget checked
- MTXBUF = WBUF ($F03E60, 52B ≥ 36B). WBUF is face-loop scratch, FREE during the
  vertex pre-pass (vc_loop runs before the face loop) → safe alias. MTXA offset
  $E60 & 0xFFC = $E60 (in range).
- At pre-pass start (near line 637, where CAM_BUF cY/sY/cP/sP load): copy the 9
  matrix longs from the DRAM camblk (+28) into MTXBUF; `store #3,(G_MTXC=$F02104)`.
- Replace vcf1..vcf8 with, per output row j∈{0,1,2}:
  ```
  ; vector (dx,dy,dz) packed once: rVA = dx|(dy<<16), rVB = dz  (s16 each)
  store  #MTXBUF+12*j,(G_MTXA=$F02108)   ; matrix row j
  moveta rVA,rVA                         ; REQUIRED before each mmult
  moveta rVB,rVB
  mmult  rVA,rOUT                        ; rOUT = Σ row_j·vec  (Q12)
  sharq  #12,rOUT                        ; -> rx / ry / rz2
  ```
  (mmult dest read directly, like BIWN. Keep NEAR clamp + vcf9/vcf10 projection
  divides unchanged.) All under `.if MMULTX / .else <imul32 vcf1..8> .endif`.
- BYTE BUDGET: MMULTX=1 build DROPS 8 imul32 call-sites (~192B) and ADDS the
  matrix copy (~40B) + 3×(store+2moveta+mmult+sharq)≈60B ⇒ **net ≈ −90B** vs the
  current 3656/3680. Fits. (In-kernel precompute would ADD ~160B → would blow the
  24B ceiling; hence 68k-side precompute.)

## s16 RANGE GUARD (the binding correctness risk — TODO before shipping)
dx=wx−camx can exceed ±32767 in large rooms; the bank-1 pack truncates to s16 →
warped verts. Phase 3 plan: jsim pixel-diff the MMULTX path vs imul32 on the
real scene; if verts diverge, add a per-vert |d|>s16 → imul32 fallback (or
pre-scale). STAGEDIET's "camera exceeds s16" ok-flag is the precedent.

## Verify path
- Host: /tmp/mtxtest.c (this session) — precompose vs imul32, both-halves check.
- jsim: PROBE dump the 9 MTXBUF longs to DRAM at pre-pass start; diff vs a Python
  build_xform_mtx for the scene's cam angles. Then pixel-diff MMULTX vs imul32.
- Silicon: fps A/B + visual, flags-off byte-identity (c4f42fe3 discipline).

## STATUS (2026-07-24): wired + jsim correctness gate PASSED

Phase 2 landed (commit b4d05bd): MMULTX flag, gpu.c build_xform_mtx, kernel
matrix copy + 3-MMULT rotate. Build facts: MMULTX=0 assembles BYTE-IDENTICAL to
HEAD (3328B); MMULTX=1 = 3272B (-56B); full MULTIROOM+AUTOSTART cof links (jcc68k).

**jsim geometry gate PASSED** (jagemu screenshot, MULTIROOM AUTOSTART, frame 600
= a real Caves render, 185 colors): MMULTX=1 vs MMULTX=0 is **PIXEL-IDENTICAL**
(0/76800 px differ). The ±3-unit precompose rounding stays sub-pixel here.
Path-liveness PROVEN: a temp `addq #30,r21` in the MMULT rotate shifted 38% of
pixels → the MMULT path really drives the geometry (not skipped by the Jerry
co-transform gate; VCPTR=0 in this build).

**Perf: jsim instret is NOT a valid oracle here.** MMULTX=1 vs =0 steady-state
per-frame GPU instret (f600→620) = only -0.2%. Two reasons: (1) jagemu at this
fidelity counts ~1 cyc/instr and does NOT model imul32's multi-cycle `mult`
latency vs MMULT's systolic cost — the real cycle win is invisible to it; (2)
this Caves-walking scene is rasterization/face-loop-bound, whereas the "65% of
Tom pre-pass" was a SPAWN config. **Real perf = rig fps A/B** (flags-off byte-
identity already holds), ideally on the spawn scene where the pre-pass dominates.
NB: build with `make clean` between flag flips — make does not track -D changes
(a stale-object false-identical bit me once).

REMAINING: (a) rig fps A/B + visual on the spawn config; (b) s16 range guard
(dx=wx−camx > s16 in big rooms → bank-1 pack truncates → warped verts; not seen
in the Caves frame but unverified at scale); (c) optional MTXA auto-advance
(silicon-confirmed) to drop the 2 per-row MTXA stores.

## RIG A/B — STAGED AND READY (2026-07-24)

**Builds** (in `probes/`, self-contained; assets embedded):

| build | flag | kernel | md5 |
|---|---|---|---|
| `AB_MMULT_OFF.cof` | MMULTX=0 | **3668**/3680 | `8211a82f534517037a4c7fbe85c66283` |
| `AB_MMULT_ON.cof`  | MMULTX=1 | **3612**/3680 | `b9380f43c431b989a467408b7e82b9b9` |

Config (identical both sides except MMULTX), **built with `bins_planes`** (STAGEDIET
requires FACE_PLANES bins — the live tree carries bins_statics, so `cp
probes/bins_planes/* .` before rebuilding, and `make clean` between flag flips):
```
MULTIROOM=1 GEOMDIRECT=1 SHADEPASS=1 JERRYPOSE=1 AUTOSTART=1 STAGEDIET=1 \
PIPELINE=1 PIPESTAGE=2 HOPDIAL=1 HOPBOOT=1 XCULL=1 BEXIT=1 \
NOGD=1 PROFILE=1 NOPROFGPU=1 QUIETFPS=1
```
A's kernel is **3668 = the documented PLAY_XB baseline exactly** → config reproduced.
NOGD=1 (Skunkboard console) is what carries `fpsT`; note NOGD builds CANNOT run in
jagemu (the skunk handshake spins with no board) — use NOGDONLY for emu work.

**Run:**
```sh
cd .../cobweb/calib   # skunkflash.sh is generic
./skunkflash.sh <path>/probes/AB_MMULT_OFF.cof 90 /tmp/ab_off.log
./skunkflash.sh <path>/probes/AB_MMULT_ON.cof  90 /tmp/ab_on.log
grep -a fpsT /tmp/ab_off.log /tmp/ab_on.log
```
**Read `fpsT` ONLY** (loop-top window, includes logic). fps100 is inflated ~4.7×
— see pacing-campaign. Compare medians over a settled stretch, same scene/pose;
wall-clock block cadence is the backup oracle.

**Emu pre-gate results (silicon fidelity, 1200f spawn, NOGDONLY twin):**
- 0 illegal both sides, both render (93/86 colors).
- GPU instret +0.88% for MMULTX=1 — **not a valid perf oracle** (jagemu doesn't
  model imul32's mult latency vs MMULT's systolic cost). The rig decides.
- **KNOWN VISUAL DELTA: ~3.7% of pixels differ — one wall face present in OFF,
  absent in ON**, stable across frames 1200/1500. NOT s16 overflow: a range probe
  (MMXDIAG=1, ORs |dx|,|dy|,|dz| into $1C0010) read **$00007FFF at spawn = no
  component ever exceeded s16**. Cause is a cull knife-edge: with BEXIT=1 a single
  vertex crossing NEAR (rz2≈64) aborts its WHOLE face, so the ±3-unit precompose
  rounding flips one face. Inherent to any precision change, not a transform bug —
  but **eyeball the ON build for a missing wall** during the run.

`MMXDIAG=1` is retained as a default-off diagnostic flag (kernel byte-identical off).

Branch: mmult-phase1-precompose. Related: MMULT_SCOPE.md, CULLWALK_SCOPE.md.
