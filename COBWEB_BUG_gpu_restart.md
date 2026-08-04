# jagemu bug: GPU (and DSP) never restart after a halt — re-kick ignored

> **STATUS (audited 2026-07-19):** RESOLVED — verified empirically, not by reply: the game now re-kicks the GPU once per frame and renders ~199 consecutive frames in jsim. A never-restarting GPU could not do that. Closed.

**Severity:** blocker for any re-kicked RISC workload (i.e. essentially every real
game). The GPU runs its kernel exactly **once** (the boot self-test) and never
again, so nothing renders.

**Env:** jagemu 0.1.0 · cobweb `d8499b9` (`d8499b92e701004d41c92722a4b96763ad285f23`)

---

## Summary

The 68k drives the GPU with the standard idiom every frame:

```
G_CTRL = 0            ; stop GPU
...write params...
G_PC   = 0xF03000     ; entry point
G_CTRL = 1            ; GO
poll mailbox for DONE ; (with timeout)
```

jagemu honors this on the **first** GO (cold core) but **ignores every
subsequent re-kick**: the GPU is left spinning at its previous `halt` PC, its
program counter is never reloaded from `G_PC`, the kernel prologue never re-runs,
the mailbox DONE is never written, and the 68k spins its whole timeout each frame.

On real silicon, writing `GPUGO` (bit 0 of `G_CTRL`) after it was cleared
restarts the core at `G_PC`. jagemu does not.

## Root cause (pinpointed)

Two interacting gates:

1. **`sim/crates/jag-core/src/scheduler.rs:168`** only calls `gpu.run()` while GO
   is set:
   ```rust
   if bus.tom.win.r32(mem::G_CTRL) & mem::RISCGO != 0 {
       gpu.run(bus, budget);
   }
   ```
   When the 68k writes `G_CTRL=0` during a kick, `gpu.run()` is **not called** for
   that window.

2. **`sim/crates/jag-core/src/risc.rs:331–347`** — the only place `self.running`
   is cleared on a GO-low is *inside* `run()`:
   ```rust
   let ctrl = self.win_r32(bus, self.kind.ctrl_addr());
   if ctrl & mem::RISCGO == 0 {
       if self.running { self.sync_back(bus); self.running = false; }
       return;
   }
   if !self.running {                 // fresh start — reload PC from G_PC
       self.pc = self.win_r32(bus, self.kind.pc_addr());
       ...
       self.running = true;
   }
   ```

Because (1) skips `run()` while GO is low, the falling edge of GO is never
observed, so `self.running` is never reset to `false`. When GO goes high again,
`!self.running` is false, the fresh-start block is skipped, and `self.pc` keeps
its stale `halt` value. The core resumes spinning at `halt` instead of restarting
at `G_PC`.

The 68k's `G_CTRL`/`G_PC` writes *do* land where `run()` reads them
(`bus.write32 → tom_write32 → tom.win.w32`, and `run()` reads
`win_r32(ctrl_addr())`), so this is purely the scheduler gate hiding the GO-low
window from the core.

The DSP path (`scheduler.rs:171`, `D_CTRL`) has the identical guard. It only
escapes the bug when the DSP is resident (GO held high, never toggled).

## Recommended fix

Call `run()` unconditionally and let the core observe control itself — `run()`
already early-returns cheaply when GO is clear (and now resets `self.running` on
the falling edge):

```rust
// scheduler.rs ~168 — was: if G_CTRL & RISCGO != 0 { gpu.run(...) }
gpu.run(bus, budget);   // run() checks RISCGO, stops+syncs on GO-low, restarts on GO rising
dsp.run(bus, budget);   // same for D_CTRL
```

Alternative (if you want to keep the scheduler gate for idle-skip): intercept the
`G_CTRL`/`D_CTRL` write in `tom_write32`/`jerry_write32` and, on a
GO-set-bit transition, force the target core to reload PC from `G_PC` on its next
`run()` (e.g. clear its `running`). The unconditional-`run()` fix is simpler and
exactly hardware-faithful.

## Reproduction

ROM: an OpenLara Jaguar build (`PROBE_full240.cof`, MULTIROOM 320×240, headless).
Can be provided; it kicks the GPU (`gpu_geotex`) every frame and polls a DRAM
mailbox for `MAGIC_DONE=0x0A3DD05E`.

```sh
# GPU never re-enters its kernel entry (0xF03000) during gameplay:
jagemu break ROM.cof --gpu --at 0xF03000 --frames 150 --press start --press-after 50
#   => {"stop":"frame_limit","hit_pc":null}          (NEVER restarts)

# Control — the same breakpoint DOES fire from boot (self-test kick):
jagemu break ROM.cof --gpu --at 0xF03000 --frames 60
#   => {"stop":"gpu_breakpoint", hit}                (entered exactly once)
```

Corroborating state after 200 frames (`run --fidelity silicon`):

| Observation | Value | Meaning |
|---|---|---|
| GPU `G_PC` (0xF02110) | `0x00F03DC0` | parked in the `halt` spin, not `0xF03000` |
| GPU `G_CTRL` (0xF02114) | `0x00000001` | GO still set |
| GPU `r4` (room base, loaded at prologue) | `0x00F03000` | the **self-test sentinel** — prologue last ran during boot |
| `params[0]` in GPU SRAM (0xF03F00) | `0x001A64C0` | a **real** room ptr the 68k wrote for the live kick |
| mailbox (0x1E2020) | `0x00000000` | 68k cleared it; GPU never wrote `DONE` |
| framebuffer (fb0/fb1) | all `0x00` | never drawn (screen shows flat CLUT[0]) |

`r4` (self-test sentinel) ≠ `params[0]` (real room) is the proof the prologue has
not re-executed since boot.

## Suggested regression test (jtest/calib)

Minimal isolation, no game needed:

1. A tiny GPU kernel: `load` a DRAM counter, `addq #1`, `store` it back, then
   `halt` (`jr halt`).
2. A 68k stub that kicks it **twice**: `G_CTRL=0; G_PC=entry; G_CTRL=1; <spin a
   few instrs>;` repeat.
3. Assert the DRAM counter == **2**. Today it is **1**.

## Secondary item to verify *after* this fix

Once the GPU restarts and actually renders, confirm jagemu drives the **Blitter
textured-span** path this kernel uses: `B_CMD = SRCEN|LFU_REPLACE|DSTA2`
(`0x01800801`) with `A1` as a fractional affine texture sampler (`A1_PIXEL/FPIXEL/
INC/FINC`) and `A2` as the 8bpp destination, `B_COUNT=(1<<16)|width`. Note
`tom_read32` reports the Blitter as always-idle (`BLIT_IDLE`) — verify that model
still writes destination pixels for this A1-affine/A2-dest mode, otherwise the
framebuffer will stay empty even after the restart is fixed. (Can't be exercised
until the restart lands.)

---

## Response (cobweb `7336d6a`) — agreed closed, and here's the mechanism

Confirming your audit rather than re-litigating it: your empirical close is
correct, and the reason it works is in `scheduler.rs`. `gpu.run()` is called
unconditionally every scheduler tick and reads its own control register,
early-returning cheaply when RISCGO is clear.

That's deliberate and load-bearing: gating the call on RISCGO would hide the
*falling* edge of GO from the core, so the every-frame re-kick idiom
(`G_CTRL=0; G_PC=entry; G_CTRL=1`) would never reset `running` and the core
would resume at its stale halt PC instead of restarting at G_PC. That is
precisely the failure you originally reported. There's a comment at the call
site now so nobody "optimises" the unconditional call away later.
