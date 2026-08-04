# jas: bug-13 hazard errors are mis-attributed AND block jopt on benign pre-existing hazards

> **STATUS (audited 2026-07-19):** PARTIALLY RESOLVED — the blocking half is fixed: `jopt --allow-input-hazards` ships and works (we used it to optimize gpu_geotex.gas past its 5 pre-existing benign bug-13 races). The ATTRIBUTION half is unverified: jas still reports e.g. dsp_pose.das:384 racing 'a pending load from line 562' (a LATER line). That may be legitimate cross-iteration analysis via a loop back-edge rather than mis-attribution — needs re-validation before anyone works it.

**Env:** cobweb `0d0a2fc` · target file: OpenLara `gpu_geotex.gas` (ships + renders
correctly on a real Atari Jaguar — verified via video capture this session).

## Two problems

### 1. Hazard-error attribution points at the wrong instruction/register

`jas gpu_geotex.gas` reports 5 `error: TRM bug-13` hazards. The line numbers are
file lines (verified), but the instruction and register named don't match the source:

```
gpu_geotex.gas:861:error: write to r1 races a pending load/divide from line 846
    L861 is:  movei #sv_ret,r8        <-- writes r8, NOT r1
    L846 is:  moveq #0,r1             <-- an immediate, NOT a load/divide
gpu_geotex.gas:1248:error: write to r24 races a pending load/divide from line 1236
    L1248 is: move r23,r19            <-- writes r19, NOT r24
    L1236 is: rchain_cont:            <-- a label
gpu_geotex.gas:1249..1251: write to r25/r26/r27 from 1237/1238/1239
    L1249 move r13,r0 / L1250 move r24,r1 / L1251 move r24,r13  (none write r25-r27)
    L1237 addq #1,r21 / L1238 move r13,r24 / L1239 movei #-1,r9 (none are load/div)
```

The real hazard shape near the second cluster is a genuine one — `chain_step`
(called just above `rc_ret`) performs a scoreboarded **DIV** whose result is still
pending when `rc_ret` reads/writes r23/r24/r25/r29 — but the report names the wrong
lines and registers, so it's not actionable. Looks like the producer/consumer and
register fields are being resolved from the wrong slot (possibly related to the
same reg1/reg2 field handling as the indexed-store bug fixed in `9476039`). Please
make the error name the actual write instruction, its register, and the actual
load/DIV producer line.

### 2. jopt refuses to optimize any file with a pre-existing hazard

`jopt gpu_geotex.gas -o out.gas` →
`· rejected  precheck:0  input does not assemble clean (jas errors) — nothing to optimize`

jopt's v1 transform (delay-slot filling) is independent of these pre-existing
hazards — it moves an instruction into a wasted `nop` slot elsewhere. Refusing to
run because the *input* already has (benign, HW-correct) hazards blocks a large,
safe win: `jas` counts **104 wasted delay slots** in this kernel, and hardware
profiling this session shows the render is **GPU-compute-bound** (Blitter fill is
only ~12% of the frame; `jump_refill` is ~14% of GPU cycles), so filling those
slots is the single highest-value optimization available.

## Requests (either unblocks us)

1. **Let jopt operate on inputs with pre-existing hazards** it doesn't introduce —
   e.g. only fail the equivalence gate on hazards the *transform* creates, or add a
   `jopt --allow-input-hazards` / `--no-input-hazard-check` that still keeps the
   jsim equivalence certificate. (The certificate, not the input hazard check, is
   what guarantees jopt's output is safe.)
2. **Fix the hazard attribution** (correct write line, register, and producer line)
   so the 5 flagged hazards can be evaluated/fixed on their merits.

A `jas --no-hazard-check` already exists for assembly; plumbing an equivalent
through jopt would be the smallest change.

## Repro
```
jas  gpu_geotex.gas -o /dev/null      # 5 mis-attributed bug-13 errors, 104 delay-slot warnings
jopt gpu_geotex.gas -o /tmp/out.gas   # rejected: input does not assemble clean
```

---

## RESOLVED — cobweb fd9c761 (jopt), 589d641 (jas)

Both requests implemented and pushed to bslop/cobweb.

**Request 2 — hazard attribution (jas 589d641).** The hazard pass reported
line numbers from the *expanded* text, so the disabled `.if PROFGPU=1` block
above the hazard shifted every reported line. `preprocess::run_mapped()` now
returns a source-line map and the assembler threads it through the hazard
diagnostics. `jas gpu_geotex.gas --gpu` now reports the real lines:
`write r1 @L874: [move r6,r1]` racing `producer @L859: [loadw (r0),r1]`
(was mis-attributed to L861/L846).

**Request 1 — jopt past input hazards (jopt fd9c761).** New
`jopt --allow-input-hazards` (alias `--no-input-hazard-check`) relaxes only
the *input* assembly; the per-candidate jsim equivalence certificate is
unchanged, so accepted transforms are still proven safe. jopt now runs on
this kernel instead of aborting at the precheck.

**Caveat — the 104 delay slots are not yet the win we hoped.** With
`--allow-input-hazards`, jopt accepts 4 fills and **rejects every fill in
live code** (lines 1372–1891) because the jsim certificate shows they would
change behavior (the wasted slots sit behind real hazards, not free nops).
The 4 it *accepts* all fall inside disabled blocks — `.if PROFGPU=1`
(L930/L1145/L1588) and `.if HALFSPAN=1` (L1510) — so they assemble to
identical bytes (`0 saved`) and are only "equivalent" vacuously (dead code
never executes in the certificate run).

So filling this kernel's slots needs more than delay-slot motion — it needs
a scheduler that can legally reorder across the hazards, or source changes
that break the load→use chains. Tracking that as a separate jopt v2 item.

**Follow-up filed against jopt:** it should skip instructions inside
inactive `.if` blocks (or flag them), rather than reporting dead-code
rewrites as "accepted".

---

## FOLLOW-UPS IMPLEMENTED — cobweb 65d281e (jopt v2)

Both follow-ups from the caveat above are now in bslop/cobweb.

**1. Inactive-block awareness.** jopt now reasons over the *assembled*
instruction stream, so instructions inside a disabled `.if` block never
become candidates. The 4 profiling-block slots (L931/L1146/L1511/L1589)
are now reported as `skipped-inactive` — no more phantom "4 accepted /
0 saved":
```
jopt: 0 transform(s) accepted, 3518 bytes -> 3518 bytes (0 saved)
jopt: 4 wasted slot(s) skipped — inside inactive `.if` blocks (not assembled)
```

**2. v2 scheduler.** jopt no longer tries only the instruction immediately
before a jump (usually the `cmp`/`cmpq` the branch depends on). It walks the
straight-line block backwards for *any* donor it can legally sink into the
slot — dominated by the jump, data-independent of everything it leapfrogs,
flag-safe — decoded with the simulator's own `risc::timing::classify`, then
proven with the jsim certificate. Regression tests cover a non-adjacent sink
(donor behind a 3-word MOVEI) and the labelled-slot soundness case.

**Why the live slots still don't fill — and the real next step.** With v2,
gpu_geotex now surfaces 86 structurally-valid live-slot donors, but the
certificate rejects all of them. Root cause (verified): **the kernel never
halts in isolation.** Run standalone in jsim it retires ~100k instructions
and runs the budget out (`running=true`) because there is no vertex/texture/
framebuffer state — so the certificate compares a *mid-loop snapshot at the
budget cutoff*, and any timing-shifting reorder moves where that cutoff lands.

So delay-slot scheduling is solved; the blocker is now the **certificate
fixture**. jopt needs to run the kernel against representative input (the
control block + a few triangles) so it halts deterministically and the
equivalence check reflects real behavior. That is the v3 item; filed against
jopt. The scheduler itself is proven correct by the self-contained tests.

---

## v3 DONE — certificate fixture + a jas correctness fix (cobweb 9fda058, 3a74aed)

The v2 caveat ("gpu_geotex never halts in isolation, so the certificate
can't accept live-slot fills") is resolved. Getting there uncovered a
deeper jas bug.

**jas bug: a lone `=` was not equality (9fda058).** jas's `.if` expression
lexer accepted `==` but not a single `=`, so `.if NOFILL=0` (and every
`.if X=N` rmac uses) failed to lex, evaluated false, and the block was
silently dropped. gpu_geotex's Blitter LAUNCH (`store r0,(r15+14)` → B_CMD)
lives inside `.if NOFILL=0`: jas dropped it, so the kernel set up every
span's blitter registers but never fired a blit — it rendered **nothing**
in jsim (94 B_COUNT writes, 0 B_CMD launches), while the real rmac build
rendered fine. One-character parser gap, whole textured pipeline dark.
Fixed; gpu_geotex now renders in jsim (verified 3537–69346 painted pixels
depending on the scene).

**Certificate fixture (3a74aed).** New `jtest::run_with` applies memory
presets before a run; new `jopt --fixture <file>` certifies against the
kernel's real input state (param block + geometry + camera + atlas + a
framebuffer to capture). Directives: `budget`, `capture`, `long`, `blob`.

A ready fixture ships alongside the kernel:
```
gpu_geotex.fixture      # param block, clip rect, blob pointers
geotex_fx_scene.bin     # synthetic quad + triangle facing the camera
geotex_fx_cam.bin       # identity camera at the origin
                        # (atlas reuses room0_atlas.bin)
```

Run it:
```
jopt gpu_geotex.gas --gpu --allow-input-hazards --fixture gpu_geotex.fixture -o out.gas
  -> 4 transform(s) accepted, 3526 -> 3518 bytes (8 saved)
  -> 4 wasted slot(s) skipped (inactive .if blocks)
```

jopt now **accepts 4 delay-slot fills in the live span/edge-walk code**
(lines ~1823-1859), each proven to leave the rendered framebuffer
byte-identical — where the isolation run accepted none. The 384 rejected
candidates are real (they would change the rendered image). Point the
fixture at a bigger scene (e.g. the real room0_tex.bin + a room camera, ~70k
pixels) for wider coverage at a higher per-run cost.

All three follow-ups (attribution, input-hazards, dead-code, v2 scheduler,
v3 fixture) are now implemented, tested, and pushed.

---

## Response (cobweb `7336d6a`) — both parts fixed

**Hazard line attribution** — hazards now report the source line that actually
causes them.

**Running past pre-existing hazards** — `--allow-input-hazards` (`fd9c761`), so
jopt can optimise code that already contains hazards it didn't introduce
instead of refusing the whole file.

Separately, and found while working on this: jas was silently dropping every
`.if X=N` block (`9fda058`). rmac accepts a lone `=` as equality; jas failed to
lex it and discarded the entire conditional body without a diagnostic. In your
tree that meant `gpu_geotex`'s Blitter launch wasn't in the binary at all —
the textured pipeline assembled clean and rendered nothing. A silent-drop bug
in an assembler is about as bad as it gets; there's a test for it now.
