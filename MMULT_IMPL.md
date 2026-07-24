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

### SILICON RESULT — side A (MMULTX=0) baseline, 2026-07-24
Flashed `AB_MMULT_OFF.cof` (upload 191 s @ 7 KB/s, then a ~240 s console window).
14 `fpsT` samples (fps x100): 481 436 455 437 344 407 390 405 340 397 398 410 359 500
- **median 406 = 4.06 fps**, mean 4.11, range 3.40 - 5.00.
- `fps100` on the same blocks read 2033/2022/1538/... vs `fpsT` 437/344/... —
  ratio ~4.65, **re-confirming the documented ~4.7x fps100 inflation on silicon**.
- **STALLS ARE REAL AND PRE-EXISTING (baseline build, no MMULT):** `maxvbl` per
  block = 4 4 4 4 5 6 **13** 5 **15** **13** 5 5 3 — most frames 4-6 vblanks but
  worst-frame excursions to 13-15 (215-250 ms). `spind` spikes on the same blocks
  (194/103/102/98/.../196/142). User observed the stutter directly. This is a
  SMOOTHNESS problem independent of the transform campaign (GOVERNOR=1 exists in
  the tree for exactly this; see PERFHUNT variance work).

Side B (`AB_MMULT_ON.cof`) NOT YET FLASHED — needed for any A/B verdict.

---

# ⛔ HANDOFF — MMULTX IS BROKEN ON SILICON, ROOT CAUSE UNFOUND (2026-07-24)

**STATE: do NOT ship MMULTX=1. MMULTX=0 is unaffected and byte-identical to the
shipped kernel — the tree is safe.** Console was restored to `AB_MMULT_OFF.cof`.

## The failure (silicon only, user-observed, reproducible)
With `MMULTX=1` on the play config: **"parts of Lara everywhere — body and face
scattered around the screen"**, scenery **popup**, and after walking to *the dip*
the **screen goes black** (hang/crash). Degrades WITH MOVEMENT — spawn looks
comparatively OK, walking makes it worse.

## What is ELIMINATED (with evidence)
1. **Operand layout / MMULT basics — FINE.** Phase 0 `p_mmult` passes on silicon
   (rd=230,530,830), and cobweb's own probe passes too after my fixes.
2. **Drain / settle time — NOT the cause.** Hypothesised that chaining 3 MMULTs
   too tightly corrupted results; rebuilt with cobweb's validated **8 nops after
   every mmult** (kernel 3648) and flashed: **Lara still scattered.** Fix kept
   anyway (it is the validated spacing), commit f81b711.
3. **s16 truncation of (dx,dy,dz) — NOT OBSERVED.** `MMXDIAG=1` ORs
   |dx|,|dy|,|dz| into $1C0010. Reads **$00007FFF at spawn AND while walking
   (--press up, 2200f)** => no component ever reaches 32768. NOTE: weak evidence
   — the emu never reproduces the scatter at all, so this only rules it out for
   emu-reachable states.
4. **The emulator CANNOT reproduce this.** jagemu renders MMULTX=1 nearly
   correctly (3.7% px, one wall). Every emu gate is therefore blind here.

## REMAINING SUSPECTS (ranked, for the next session)
1. **MTXBUF/WBUF ALIAS — TOP SUSPECT.** I aliased the 9-long matrix onto `WBUF`
   ($F03E60), reasoning it is "free during the pre-pass". But `stage_vert` in the
   FACE loop writes staged world verts into WBUF. In **dispatch mode (several
   rooms + Lara per kick)** the order is room1 pre-pass -> room1 FACE loop
   (**WBUF/matrix destroyed**) -> room2 pre-pass (re-copies) ... **VERIFY the
   matrix is really re-copied for EVERY room/blob entry, including Lara's.** If
   any entry path re-enters the pre-pass *below* my copy (or skips vc_self), that
   entry transforms with a garbage matrix = scattered geometry. This fits the
   symptom (Lara worst) better than anything else. **Move the matrix somewhere
   private before anything else** — even if it costs a byte diet elsewhere.
2. **Register liveness across my inserted blocks.** The matrix copy uses
   r0-r4,r22; the rotate uses r0,r1,r2,r3,r21,r24,r25. Verified r5 (vcount), r18
   (cache ptr), r11-r17 (camera), r30 (mailbox) are untouched — but this was
   eyeballed, not proven. `jas` hazard-checks; consider a targeted audit.
3. **Bank-1 interaction.** `moveta r0,r0 / moveta r1,r1` writes bank1 r0/r1.
   BANKDIET=0 in this config so nothing else claims them — re-check if BANKDIET
   is ever enabled with MMULTX.
4. **The black screen at "the dip"** is a separate, harder symptom (hang, not
   just wrong pixels) — likely a wild vertex feeding a huge blitter span. Any
   root-cause fix should be re-tested specifically by walking to the dip.

## Perf hint (INVALID as an A/B, but recorded)
ON v2 (broken geometry) read fpsT 490/443/430/455 (median ~449 = 4.49 fps) vs
side A's median 406 (4.06), with maxvbl 5-6 vs A's 13-15 and spind near zero.
**Do not quote this as the MMULT win** — it renders the wrong scene, so the work
performed differs. It is only weak evidence that the direction is not a slowdown.

## Next-session plan (recommended order)
1. Move MTXBUF off WBUF to private SRAM; re-verify the matrix survives every
   dispatch entry. Re-flash and look at Lara.
2. If still broken, BISECT on silicon rather than hypothesise: build variants that
   (a) run the MMULT rotate but THROW AWAY the result and use imul32's values
   (isolates "does merely executing MMULT corrupt state?"), then (b) use the
   MMULT result for rx only, then ry, then rz2. One flash each, ~7 min, but each
   one is decisive where reasoning has not been.
3. Only after correct geometry: redo the A/B against side A median 406.

Branch: mmult-phase1-precompose. Related: MMULT_SCOPE.md, CULLWALK_SCOPE.md.

---

# ✅ FIXED + MEASURED (2026-07-24) — and the win is a NULL RESULT

**Geometry: FIXED on silicon.** User at the rig: "Lara looks solid now." The
$F03F20/SHADEK clobber (commit f40156f) was the whole regression.

## A/B on silicon, PLAY_XB config, 14 fpsT samples each
| | MMULTX=0 | MMULTX=1 (v3) |
|---|---|---|
| median | 406 (4.06 fps) | 416 (4.16 fps) |
| mean | 411.4 | 406.9 |
| sd | ~47 | ~39 |
| maxvbl spikes | 13,15,13 | 12,14,14 |

**Median +2.5%, mean -1.1%, t ~ 0.28 => STATISTICALLY INDISTINGUISHABLE.**
The hardware MMULT vertex transform is **perf-neutral on this scene**, NOT the
~-28%-of-frame MMULT_SCOPE predicted. Do not quote the earlier "4.49 fps" — that
build rendered wrong geometry.

### Why the estimate was wrong (for the next estimate)
1. **Mandatory drain.** Silicon needs 8 nops after every mmult (3/vertex = 24
   nops/vertex) — pure overhead the scope's "~27 cyc/vert" never counted.
2. **The 65%-of-Tom pre-pass figure came from jsim on a SPAWN config.** This is a
   walking scene, and jagemu does not model imul32's mult latency, so the
   software rotate's modelled cost was probably overstated relative to silicon.
3. Frame time here is not pre-pass-bound; variance (sd ~10% of mean) is larger
   than the effect being chased.

### Verdict / options
- MMULTX=1 is CORRECT and slightly SMALLER (3654 vs 3668) but buys no measurable
  fps. Keep it flag-gated and default OFF; it is not a regression, just not a win.
- If revisited: measure on the SPAWN config (where the pre-pass actually
  dominates) and with more samples; or attack the drain (MTXA auto-advance is
  silicon-confirmed and would drop 2 stores/vertex, but the 24 nops dominate).
- The honest headline: **the biggest-lever estimate did not survive contact with
  silicon.** Frame time is elsewhere (raster/blit + the maxvbl stall spikes).
