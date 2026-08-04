# OpenLara-Jaguar — next-release fixes & optimizations

Consolidated from the 2026-07-18/19 perf campaign. Ordered by expected fps
impact. Every fps number here is measured on a **real Atari Jaguar** (Skunkboard
upload, Elgato video-capture of the on-screen PROFILE bar); the engine-internal
breakdowns are from the cobweb `jsim` emulator with `--fidelity silicon`.

---

## ALREADY DONE this session (in tree)

- **Native 320×240 is the default** (dropped `HALFRES`). Hardware-measured cost
  vs the old half-height + line-double path: **4.79 vs 4.83 fps in Caves — <1%.**
  Full resolution is effectively free because the frame is transform-bound, not
  fill-bound. README updated. *This is the headline visual win — ship it.*
- **Fast integer isqrt** (`main.c:1914`). The old `while ((r+1)*(r+1)<=r2) r++`
  bounding-radius loop reached r≈70k/room (64-bit mults) and dominated level
  load; replaced with a bit-by-bit isqrt (~32 steps). Load-time win on HW too.
- **`AUTOSTART` build flag** (`-DAUTOSTART`) — title auto-selects New Game after
  20 frames (forces page 0 / Caves). For headless profiling; harmless in ship
  builds if left off.

## The bottleneck (measured, settled)

The frame is **resolution-independent** (320×240 ≈ 320×120, HW). So the cost is
**per-vertex transform + per-frame logic**, NOT rasterization or Blitter fill:

- Removing 100% of the Blitter fill (`NOFILL`) gained only **+13%** (4.83→5.45).
  Fill is ~12% of the frame. Overdraw/fill work caps out there.
- HW PROFILE bars: **rooms (room render) ≈54%**, wait ≈36%, 68k ≈8%, lara ≈1%.
- The room render cost is the **per-vertex transform/projection**, resolution-free.

## OPTIMIZATIONS, by impact

### 1. Offload more of the room transform to Jerry (BIGGEST lever)
**Finding (corrected — measure IN-RENDER, which starts ~frame 336 in jsim with
AUTOSTART; earlier-frame samples are title/load and give false negatives):**
Jerry **does** pose Lara — `jerry_pose_kick`→`cmd_pose` fire every frame (hit
@336). What it does **not** do is co-transform rooms: `jerry_roomx_kick`/`rx_vert`
never fire because **`njx=0`** — no room gets assigned to Jerry. So **Tom
self-transforms the visible room(s)**, re-transforming shared verts once per face
(the kernel's own "Shared verts are re-transformed per face" note). Clean
in-render profile: Tom ~71% busy, Jerry ~100% (but that 100% is mostly its
resident `main_loop` poll + the Lara pose — it has real spare capacity for room
transforms).

**Why `njx=0` here:** the assignment at `main.c:2769-2781`
(`if (ndrawn>0 && rvc<=512 && rqc<=448 && rtc<=256 && njx<5)`) hands Jerry only
the 2nd..Nth *visible* room, and only if it fits the caps. In a view where one
room dominates (or a big room exceeds `rvc<=512`), nothing goes to Jerry and Tom
does all the transform. So the co-transform helps multi-room junctions but not
the common single-big-room view — exactly where the transform hurts most.

**The lever:** let Jerry transform the room(s) Tom would otherwise self-transform
— e.g. hand it the *first* visible room too (currently always Tom via
`ndrawn>0`), and/or raise/remove the `rvc<=512` cap (split a big room across
Jerry+Tom). The vertex-cache path already exists end-to-end (`jerry_roomx_kick` →
`cmd_roomx`/`rx_vert` → cache → kernel VCPTR gate at `gpu_geotex.gas:489`);
`cmd_pose` proves Jerry's command dispatch works. Watch the documented hazards:
the "flag-dance livelock under bus load" (why the current code finishes all Jerry
work before Tom starts) and the "Jerry DRAM write storm vs vblank OP-list rebuild"
(`video_wait_safe_vc`). Validate the fps win in-render (past frame 336) and on HW,
not on early-frame samples.

### 2. Transform-once (vertex cache), even without Jerry
Independent of #1: Tom re-transforms shared vertices per face (3–4× typical
waste). A per-room screen-vert cache filled on first use (by Tom or Jerry) and
reused cuts the transform proportionally.

### 2b. MMULT the rotation (the biggest per-vertex win) — VALIDATED in jsim
The real per-vertex cost is **~10 `imul32` helper calls** in `vc_loop` (8 for the
yaw+pitch rotation, 2 for FOCAL projection), each ~18 instructions incl.
call/return overhead. Replace the 8 rotation `imul32`s with the JRISC **`MMULT`**
systolic matrix-multiply. **Proven working in jsim** — see `mmult_ref.s`
(dot([3,4,5]·[10,20,30])=260). Validated recipe:
- **Fold the two 2D rotations into one 3×3** on the 68k, once per frame:
  `M = [[cY,0,-sY],[-sY·sP,cP,-cY·sP],[sY·cP,sP,cY·cP]]` (.12 fixed), giving
  `(rx,ry,rz2) = M·(dx,dy,dz)` directly (rz intermediate not needed).
- **Rotate the LOCAL s16 vert** (`RVert x/y/z` are s16 → fit MMULT's 16-bit
  operands), then add a **per-room `M·(offset−cam)` constant** (computed once per
  room). Add it **before the `>>12`** to stay byte-exact vs the current code.
- MMULT setup: matrix rows as 16-bit high-halves in local SRAM by-row (stride 4),
  `MTXC`=3, `MTXA`=byte offset; vertex packed 2×16-bit in **bank-1** regs.
  **Switch banks via the FLAGS reg bit-14 (REGPAGE=0x4000), preserving IMASK**
  (`flags = (flags & 0x8) | 0x4000`) so no ISR fires mid-transform. MMULT reads
  bank-1 operand and writes Rd in the *current* bank.
- Per vertex: 3 MMULTs (rx,ry,rz2) + 3 translation adds + `>>12`, vs 8 imul32.
  Also frees SRAM (MMULT is 1 instr/dot-product vs the imul32 call sites).
**Remaining work:** wire the 68k 3×3 + per-room translation precompute; rewrite
`vc_loop` verts 589–669 with the bank-managed MMULT path; keep it byte-exact
(verify render-identical in jsim) and within the 3584-byte budget; flash for fps.

### 3. Cut the two projection divides per vertex
`render_pkt` does `sx = rx*FOCAL/rz`, `sy = ry*FOCAL_Y/rz` — two scoreboarded
divides per vertex. Compute one reciprocal `recip = (FOCAL<<k)/rz` and multiply
(`sx = rx*recip>>k`, `sy` similar): 1 divide + 2 mults instead of 2 divides.
NB: the earlier reciprocal attempt was rejected for the affine *fill* (overflow);
this is the *projection*, different math — pick `k` for the rz range.

### 4. jopt delay-slot filling — SMALL, do it for tidiness not speed
`jump_refill` is 36% of GPU cycles, but cobweb confirmed jopt can safely fill
only ~4 of the 104 wasted slots (the rest sit behind real data hazards, not free
nops). Run: `jopt gpu_geotex.gas --gpu --allow-input-hazards --fixture
gpu_geotex.fixture` (needs a fixture; cobweb's ships one). Cutting jump_refill
meaningfully needs control-flow restructuring of the transform loop, not slot
filling.

### 5. HW check: is the Tom-done→68k wake prompt?
`gpu_sync` (`gpu.c:86`) `stop #0x2000` sleeps until Tom raises CPUINT at
`alldone` OR the next vblank. If CPUINT isn't waking promptly on HW, the 68k
sleeps vblank-granular past Tom-done, wasting up to 16ms/sync. Cheap to verify on
silicon; potentially free fps.

## Tooling (cobweb) — resolved this session
Reported and FIXED by the cobweb maintainer: GPU/DSP re-kick restart
(`c102187`), JRISC indexed-store operand fields (`9476039`), Blitter XADDINC
16.16 affine DDA (`0d0a2fc`), jas hazard attribution (`589d641`), jas `.if X=0`
lexer (`9fda058`), jopt `--allow-input-hazards` + `--fixture` (`fd9c761`/`3a74aed`).
With these, `jsim` renders the real in-game path at 320×240 — full offline
correctness + compute profiling; hardware remains the ground truth for fps and
for anything DSP-command-handshake-related (jsim's Jerry D_CMD path unverified).

## HW measurement recipe (console-free, reliable)
Skunkboard USB on this rig is slow (~7KB/s, ~3.4 min/upload) and its console is
garbled; **don't read fps over `jcp -c`** (the SKUNK_CONSOLE build also stalls on
loading because setup `dbg_kv`s block on the slow host). Instead build **headless**
(`-DNO_GAMEDRIVE` without `NOGD`) + `PROFILE -DAUTOSTART`, `jcp file.cof`, then
capture the on-screen fps bar: `ffmpeg -f v4l2 -i /dev/video2 -frames:v 1 out.png`
(video2 = Jaguar; game width ≈527px of 720 → scale ÷1.647; fps = bar_px×3/100).
