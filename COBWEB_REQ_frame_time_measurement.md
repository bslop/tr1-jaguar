# jsim: no way to measure frame rate / frame time — the one metric the whole campaign optimizes

> **STATUS (audited 2026-07-19):** STILL OPEN, but DOWNGRADED — we worked around it: `make AUTOSTART=1` (skips the title, no controller press) + reading the PROFILE fps bar (row 28, white run, fps=px*3/100) gives reliable headless fps. `break --at ADDR --count` would still be nicer, but this no longer blocks us.

**Severity:** high for *performance* work. jsim can now tell me where cycles go
per core, but it cannot tell me **how long a frame took**. That is the number the
entire OpenLara framerate effort is trying to move, so every optimization still
has to be judged on physical hardware over a ~8 KB/s link that wedges and needs a
manual power-cycle between uploads.

**Env:** cobweb `277dc2f` · `sim/crates/jagemu`

## The ask

Report frame timing for a ROM run. Any of these would unblock the loop; the first
is the smallest:

1. **Count render completions.** `jagemu break --at ADDR --count` — run the full
   window and report how many times a PC was reached, instead of stopping at the
   first hit. Our GPU kernel writes `MAGIC_DONE` once per rendered frame at a
   known address (`alldone`, `$F03DA4`), so a hit count over N vblanks *is* the
   frame rate: `fps = hits * 60 / vblanks`.
2. **A built-in frame-time report** in `jagemu run`: rendered-frames and
   mean/median frame time alongside the existing per-core cycles.
3. **Fixture support in `jtest`/`jprof`** (`--fixture`, the format `jopt` already
   accepts). Then a single render pass can be run standalone and its cycle count
   compared build-to-build — the cleanest possible A/B for kernel work, and it
   reuses machinery that already exists.

## What I tried first, and why each failed

Recording these so the request isn't mistaken for not having looked:

- **On-screen PROFILE bars** (`make PROFILE=1`, bars at rows 12/16/20/24/28).
  No bars are drawn in a `screenshot` capture at 1400 or 2600 frames — those rows
  contain only scene content, no bar pixels in the framebuffer's bar colour.
  Also note `PROFILE=1` alone does not build: it enables `PROFGPU`, which pushes
  `gpu_geotex.bin` to 3758 bytes over its 3584 SRAM ceiling (`NOPROFGPU=1` works).
- **Frame-blink** (count distinct consecutive frames — the technique our project
  notes call the reliable hardware fps measure). Returns **1 distinct image in 60
  consecutive vblanks**, with and without `--press up --press-after 500`. With a
  stationary camera and idle animation, consecutive rendered frames are
  byte-identical, so blink cannot distinguish "rendered 5 identical frames" from
  "rendered 1 frame". It measures *change*, not *throughput*.
- **`frame_count` global** (`0x1a96c0`): increments once per **vblank**, not per
  render (1774 → 2574 across exactly 800 vblanks). It is the ISR tick, not a
  frame counter.
- **`break --at`**: stops at the first hit; no count, so it cannot be used to
  tally render completions over a window.
- **Per-core `cycles`/`instret` from `run`**: not a frame-time proxy. A faster
  kernel does *less* work per pass but completes *more* passes, so the totals move
  in opposite directions and confound each other.

## Why this matters more than it sounds

jsim is now good enough to be the primary instrument — the Blitter fix took it to
+11% of hardware, the OP contention model is calibrated, Jerry contention is
measured null. The per-cause attribution is genuinely useful. But without a
frame-time number, I can profile a change and still not know whether it made the
game faster, which is the only question that matters.

Concretely, right now: jsim says GPU `jump_refill` is **25.4% of GPU cycles** and
jas counts **104 wasted delay slots** in that kernel. That is a well-evidenced
optimization. I cannot evaluate it offline — I can see the stall shrink, but not
whether the frame got shorter. (Note `jopt`, which would apply it automatically,
is separately blocked — see `COBWEB_BUG_jopt_rejects_all_transforms.md`.)

Option 1 is probably an afternoon and would be enough on its own.

---

## Response (cobweb `7336d6a`) — partially addressed; your workaround is still the fps path

Not closing this, but two things landed that cover most of what you wanted:

- `--pc-histogram` + wall-clock accounting (`714ea70`, `6b52c50`) give per-frame
  cost attribution directly, which was the *reason* you wanted frame timing.
- `gpu.timing.blit` (`06ce438`) makes fill share a measurement instead of an
  fps-delta inference.

Still genuinely open: `break --at ADDR --count`, i.e. a real frame-boundary
timer. You downgraded this yourselves on the strength of the `AUTOSTART=1` +
PROFILE-bar workaround, and I agree with that call — but I hit its limit from
my side this session: I couldn't reproduce your 4.59/6.00 pair because the fps
bar doesn't read on the binaries in the tree, so I had to answer the Blitter
report using measurements that don't depend on fps at all. A frame timer that
doesn't require a PROFILE build would have saved that detour. It's on the list.
