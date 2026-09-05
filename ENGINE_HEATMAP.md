# Engine heat map — what runs well on the 68000, the OP and the Blitter

Measured 2026-09-05 on the shipping configuration (**160×120, `HRESN=160`**),
not inherited from earlier numbers. That distinction is the whole point of this
document: every previous conclusion about the 68000 was measured at 320×60, and
`SLIVERW` proved what happens when you carry a number across a configuration
change — a **+17%** win measured in the 4-fps era read **−0.6%** at 8.65 fps,
because the bottleneck had moved underneath it.

Re-measure before you trust anything here. The method is at the bottom.

---

## 0. The baseline these numbers come from

`out_h160game` = ship recipe + `HRESN=160 FASTBOOT=1 PADMUTE=1 FPSBEACON=1`,
`jagemu run --fidelity silicon --pc-histogram --core 68k --start 600 --frames 900`.

```
68k total charged   199,486,801
   asleep in STOP   142,525,231   71.4%   <- was 63.9% at 320x60
   awake             56,961,570   28.6%
     vblank ISR         966,808    1.7% of awake  (82,041 instrs)
     main line       55,994,762   98.3% of awake  (5,093,822 instrs)

Tom   blit 17.9% of GPU cycles  (transfer 84.9% of that, launch 15.1%)
      blit_wait 3.8%  jump_refill 14.6%  mem_external 14.5%  contention 0.5%
      887 blits/field
68k   op_tax 8,481 cycles/field lost to Object-Processor DRAM occupancy
```

⭐ **The 68000 idles MORE at 160×120 than it did at 320×60 — 71.4% vs 63.9%.**
The frame is longer in fields (8.00 vs 6.94) and the 68k's per-frame work did
not grow with it. There is more headroom than the old figure implies, and this
is the first thing that changed in the 68k's favour in a year.

---

## 1. THE RULE — and it is not "spare CPU cycles"

**68000 *cycles* are nearly free. 68000 *DRAM accesses during Tom's render* are
extremely expensive.** The currency is bus traffic and *when it happens*. Every
68k result this project has is explained by that one sentence and by nothing
else:

| change | result | why |
|---|---|---|
| `M68A2=1` — hoist the O(n²) room sort out of the render window | **+13.9%** (7.00→7.98) | removed 68k DRAM traffic *during* Tom's render |
| `HUDTEXT` off — stop painting 8 glyphs | **+56%** | `menu_ink_w` rescans every glyph cell with a runtime `div` |
| `ENTLISTS=1` — precompute type-indexed entity lists | **+4.1%** | killed 13 rescans/frame of `mrt_ent[].type` |
| `GCCHOT` — 22% fewer 68k instructions | **0.00%** | fewer *instructions*, same *bus traffic* |
| `M68DIET` | **−62%, QUARANTINED** | moved work into the render window |
| 68k eviction campaign — remove 68k work | **SLOWER** | Tom is bandwidth-bound; the 68k was already asleep |

⇒ Ask of any candidate: **how many DRAM words does it touch, and does it touch
them while Tom is rendering?** Not "how many cycles does it take".

---

## 2. The 68000 — measured hot spots

Top of the awake profile, symbolised (`nm` on the arm's own ELF):

| % awake | cyc/instr | symbol | what it is |
|---|---|---|---|
| 5.78% | **23,184** | `blit_band.Lelse_12+0x9c` | **spin** — waiting on the Blitter |
| 5.54% | 140 | `__udivsi3_internal` ×2 sites | **software 32-bit divide** |
| 8.10% | 76 | `__mulsi3_internal` ×3 sites | **software 32-bit multiply** |
| 3.15% | 12 | `jerry_pose_sync` | **spin** — waiting on Jerry |
| 6.14% | 10 | `main+0x2962/0x296a/0x2974/0x2d8e` | real main-line work |

Two findings fall straight out:

⭐ **~13.6% of the 68k's awake time is software `mul`/`div` helpers.** The 68000
has no 32-bit multiply and no 32-bit divide; gcc calls library routines at
76–140 cycles a go. Tom and Jerry both have hardware multipliers and a divide
unit. **Any candidate whose inner loop needs 32-bit `*` or `/` is on the wrong
processor**, and that is a *code shape* test you can apply before writing
anything.

⭐ **~8.9% of awake time is spinning** on the Blitter and Jerry. That is capacity
that already exists and costs nothing to reclaim — work placed there is free in
the strongest sense, provided it does not touch DRAM.

### 🟢 COLD — good candidates
- 8/16-bit logic, comparisons, branching, state machines
- Table lookups into a **small, resident** working set
- Anything already in registers or the 68k's own stack
- Work that can be scheduled into **vblank** or the pre-render window
- Turning per-frame recomputation into a **precomputed table** (the `ENTLISTS`
  shape: the win is fewer DRAM reads, not fewer instructions)

### 🟡 WARM — viable with care
- Per-entity logic at 60 entities (the level's real count — see
  `project_level1_entity_inventory`), *if* it runs outside the render window
- Anything touching DRAM **during vblank only**
- Trigger/collision bookkeeping over cached sector data

### 🔴 HOT — expect it to cost more than it saves
- Any inner loop with 32-bit `*` or `/` (76–140 cycles per operation)
- Per-pixel or per-span anything — 8+ orders off the Blitter
- Per-vertex transform (needs exactly the multiplies the 68k lacks)
- Scanning arrays every frame that could be indexed once

### ☠️ TOXIC — measured negative, do not
- DRAM-heavy loops **inside Tom's render window** (`M68DIET`, −62%)
- Text/glyph painting with a runtime divide per cell (`HUDTEXT`, +56% when off)
- O(n²) anything over rooms or entities in the render window (`M68A2`)

---

## 3. The Object Processor — free pixels, at a bus price

The OP composites during scanout, so what it does costs **zero Tom cycles**.
It is not free of *bus*: this build loses **8,481 cycles/field** of 68k time to
OP DRAM occupancy (`m68k_op_tax_cycles`), and that tax scales with how many
phrases per line it fetches.

### 🟢 What the OP is genuinely good for
- **Vertical scaling** — already load-bearing (`VSCALE`, the VRESN ladder)
- **Narrowing the fetch** — `IWIDTH < DWIDTH` renders fewer columns without
  changing the stride. Shipping now: `IWIDTH=20`, `DWIDTH=40`
- **PWIDTH** — widening the pixel clock is a bus **WIN** (fewer, wider pixels =
  fewer fetches). `VMODE=$0EC7`, [HW]-confirmed. **Not** the same thing as ⤵

### ☠️ What the OP cannot do here
- **Horizontal SCALING** (`HSCALE ≠ 1.0x`) — holds the bus for most of the
  active display, ~15–20×, and blacks the screen. `video.c:193`. ☠ This is the
  line that reads like advice about PWIDTH and is not; they are opposite in cost
- **Anything after a scaled object but `STOP`** — [HW] quirk, `video.c:196`. The
  VRESN scaler *is* that object, so **OP sprites and VRESN are mutually
  exclusive**. That closes "Lara/enemies as OP sprites" for as long as VRESN
  ships, which is the biggest lever the project has
- **A third object** — `jaguar.ld:47-51` reserves exactly 64 B at `ALIGN(32)`,
  fully consumed by lists A and B under `OPDBL=1`. Growing it re-enters the A10
  class

### ☠️ OP traps that fail silently
- A scaled object must be reached by **branch fall-through inside the active
  window** and is **four phrases, not three** — its outbound LINK is not
  honoured. This is where a black screen comes from when a narrow mode meets
  OP vertical scale
- Object lists must be **32-byte aligned** (scaled) / 16-byte (bitmap) — the OP
  fetches one burst. `op_list` lives in `jaguar.ld` under `ALIGN(32)`
- ☠ **jagemu cannot test any of this.** Its screenshot comes from the
  framebuffer, so a wrong OP object renders perfectly offline
  (`JAGUAR_FINDINGS.md` §1). It also ignores PWIDTH entirely
  (`mem.rs:116-117` define the fields and nothing reads them)

---

## 4. The Blitter — transfer is everything, launch is noise

Measured this build: blit is **17.9%** of Tom's cycles, of which **transfer
84.9%, launch 15.1%**, over **887 blits/field**. That reproduces the shared
corpus's 86/14 model.

### 🟢 What pays
- **Reducing pixels transferred.** The only thing that has ever moved this
  number. `HALFW` (halve `B_COUNT`) → **+14.1%** on silicon; the real
  `HRESN=160` → **+16.3%**
- **Phrase mode on a linear copy** — 8 bytes/tick vs 1. `blit_copy_phrase`,
  and the `vidrom` FMV path went 95 ms → 5 ms on exactly this
- **Bulk moves.** This is what the Blitter is *for*

### ☠️ What does not pay — all measured
- **Batching spans / amortising launches** — launch is 15% of blit cost.
  `RUNBATCH` and `TRAPEZOID` both null, and the kernel comment predicted it
- **Per-pixel arithmetic changes** — flat-shading floors measured 7.50 fps on
  *both* arms: same texel count, different addresses
- **Phrase mode on a gathered textured span** — `XADDPHR` *truncates* x to the
  phrase boundary (`BLITTER.md:296`), so a correct form must keep pixel-mode
  ends; at a real ~15-px span that is ~1–2% of frame. The built `PHRASEDST`
  probe never phrase-aligned `xl` at all, so its offline +9–11% was a build
  painting the wrong pixels
- **4bpp as a fill lever** — [HW]: Tom does **not** convert pixel sizes, and a
  4bpp *destination* needs `DSTEN`, taking 2 accesses/px to **3**
- **Z-buffer / hardware occlusion** — 16bpp-only; writes zeros at 8bpp
- **GPU-direct fill instead of the Blitter** — built and HW-measured at
  **~1.8 fps** (`gpu_bltex.gas:8-17`). ~360 Tom cycles/px vs the Blitter's 11.2

---

## 5. Ranked: what I would actually try next on the 68000

1. **Move the `mul`/`div` helpers off the 68k, or out of the frame.** 13.6% of
   awake time, and the code shape is diagnosable by grep. Either precompute the
   products/quotients into a table (trades DRAM reads for arithmetic — measure
   which way that lands) or move the owning computation to a core with a
   hardware multiplier. Find the callers first: they are what matters, not the
   helpers.
2. **Spend the spin.** ~8.9% of awake time is `blit_band` and `jerry_pose_sync`
   waiting. Register-resident, DRAM-free work placed there is free.
3. **Re-test one QUARANTINED item against the new profile.** Not `M68DIET`
   (−62%, needs the discriminator kit) — something cheaper whose verdict was
   taken at 320×60. `SLIVERW` is the standing proof that verdicts expire.

⚠ Every one of these must be measured as a **stacked, together-measured batch**
on silicon: the rung at 160×120 is one whole field from 8.00, and a 2% item
reads inside run-to-run scatter alone.

---

## 6. Method — how to re-measure this

```
# build an arm (three define paths must ALL carry the flag — check the
# compile lines, not that the ROM differs)
docker build -t tr-jaguar .        # ☠ the container builds from a baked /src
docker run --rm -v "$PWD/disc:/disc:ro" -v "$PWD/out_x:/out" \
  -e DISC_NAME="..." -e RES=120 -e HRES=160 -e VIDEO=0 \
  -e BUILD_FLAGS="<ship flags> FASTBOOT=1 PADMUTE=1 FPSBEACON=1" tr-jaguar

# 68k profile
jagemu run out_x/OPENLARA.COF --frames 900 --fidelity silicon \
  --pc-histogram --core 68k --start 600 --top 20 2>&1 >/dev/null

# symbolise (nm on THAT arm's own ELF — addresses differ per arm)
m68k-neogeo-elf-nm -n out_x/OPENLARA.elf
```

☠ `--fidelity silicon` or every `timing.*` reads 0.
☠ Redirect `>file 2>/dev/null`; hazard warnings on stderr corrupt the JSON.
☠ `cycles_per_field` as emitted is boot-contaminated — delta two runs.
☠ **Cycles offline, frames on silicon, and report fields/frame.** And note that
an offline *fill* A/B in this engine is a **floor**: `HALFW` read +7.3% offline
and +14.1% on hardware, because jsim charges a blit's own cost exactly but not
what it costs the other masters.

---

## 7. Open hazard, unrelated to placement but found while measuring

`jagemu` warns: **Jerry performs 400 store→load round trips on the same DRAM
word inside the hazard window** (min gap 78 cycles), at `$1883D4` (PC `$F1BA5C`)
and `$18EE68` (PC `$F1BA9A`). On silicon that load can return stale/0 under bus
traffic — three confirmed kills in the corpus. jsim lands stores instantly, so
it is invisible offline. This is the same class as the bug `VCDRAIN` fixed on
Tom. Keep the value in a register or local SRAM. **Not yet investigated.**
