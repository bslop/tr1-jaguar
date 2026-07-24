# jsim: panics (process abort) on an out-of-range bus access instead of handling it

**Severity:** medium — robustness. Real Jaguar hardware returns garbage for a wild
address; jsim aborts the process, which kills long profiling runs and makes any
experiment that puts a kernel into a bad state unusable.

**Env:** cobweb `8ca3fc0` · `sim/crates/jag-core/src/bus.rs:56`

```
thread 'main' panicked at crates/jag-core/src/bus.rs:56:44:
index out of bounds: the len is 65536 but the index is 65536
```

Reproduced with `jagemu screenshot <rom> --frames 2600 --fidelity silicon`.

## How we hit it

We built a diagnostic OpenLara variant that suppresses the GPU kernel's Blitter
register writes (`NOSPAN=1`) to isolate their wall-time cost. That leaves the
Blitter's A1/A2 registers holding stale values, and the kernel evidently derives
an address from them — so this is genuinely our bug, not jsim's. **But the
emulator should not abort.** An index exactly one past the end of a 64KB region
(65536 into len 65536) is a classic off-by-one at a region boundary and would be
harmless on silicon.

## Ask

Mask or bounds-clamp the access (wrap to the region size, or return 0 / open-bus)
rather than indexing directly. Optionally surface it as a counter (e.g.
`oob_access`) so the ROM author can see they went out of range — that would
actually have told us our diagnostic build was broken, instead of us reading a
Rust backtrace.

The 65536-into-65536 shape suggests a `>=` vs `>` bound or a missing mask on a
region whose size is a power of two.

---

## Response (cobweb `7336d6a`) — fixed

You were right on the principle: real hardware floats the bus and returns
garbage for an unmapped access, it doesn't halt the machine.

`Window`'s six accessors indexed the backing array directly. The `65536` in your
trace is the giveaway — that's a 16-bit access straddling the final byte of a
64 KB window, not a wildly wrong address, so it needed only a slightly
off-the-end pointer to abort the whole process. Now bounds-checked: reads off
the end return 0, writes are dropped, in-range access is unchanged.

Regression test covers one-past-the-end, straddling 16- and 32-bit reads, a
fully wild address, and that writes off the end don't corrupt the tail.

Your repro (`screenshot --frames 2600 --fidelity silicon`) runs to completion.

Worth saying: "makes any experiment that puts a kernel into a bad state
unusable" was the part that made this worth doing properly rather than clamping
one call site — a debugging tool that dies when the thing you're debugging
misbehaves is backwards.
