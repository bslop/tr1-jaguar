# jopt: sinks a donor ACROSS a branch-target label — loop init moves inside the loop, silently corrupting code

> **STATUS (audited 2026-07-19):** RESOLVED in cobweb `8ca3fc0` (never sink across a branch-target label) + the vacuous-capture check we asked for. Verified by us: wid_e loop intact, 34 transforms, render 0/76800 px diff. No reply needed.

**Severity:** critical — jopt now *accepts* transforms (thanks for the label-relocation
fix in `0e14ab5`, that part works), but some accepted transforms **change program
semantics**. The output assembles, passes the certificate, and produces a broken
kernel. This is worse than the previous all-reject behaviour because it is silent.

**Env:** cobweb `7132058` · `sim/crates/jopt/src/lib.rs` (donor legality)

## What happened

Ran jopt on OpenLara's `gpu_geotex.gas`: **56 transforms accepted**, 3526 → 3416
bytes. Measured with clean rebuilds of both variants (identical flags, only the
kernel differs), `jagemu run --frames 620 --fidelity silicon`:

| clean build | GPU cycles | `jump_refill` | render |
|---|---|---|---|
| baseline kernel | 47,446,832 | 21.7% | — |
| jopt kernel | **251,226,220 (5.3x)** | 13.2% | **95.2% of pixels differ** |

`jump_refill` did drop as intended (21.7% → 13.2%) — the transform does what it
advertises — but the kernel now thrashes and renders garbage, which is exactly
what the broken `floor(log2 w)` below would cause.

## The defect

A donor may only be sunk into a delay slot if it executes the **same number of
times** in both positions. jopt moves donors **across branch-target labels**,
which changes execution count from once to once-per-iteration.

From the actual diff (`wid_e` computes `e = floor(log2 w)` for texture addressing):

```diff
 	moveq	#1,r3			; const 1
 	move	r0,r1			; t = w
-	moveq	#0,r2			; e = 0        <-- executes ONCE, before the loop
 wid_e:                                         <-- loop entry / back-edge target
 	cmp	r3,r1
 	movei	#wid_e_done,r22
 	jump	EQ,(r22)
 	nop
-	nop
 	jump	MI,(r22)		; t == 0 guard
-	nop
+        moveq	#0,r2                          <-- now executes EVERY iteration
 	nop
 	shrq	#1,r1
-	addq	#1,r2			; e++
 	movei	#wid_e,r22
 	jump	T,(r22)			; back-edge to wid_e
-	nop
+        addq	#1,r2
 	nop
```

`e = 0` is now re-executed on every iteration, so `e` never exceeds 1 and
`floor(log2 w)` is destroyed. The identical bug occurs immediately below with
`move r0,r9` sunk into the `wid_m` loop.

**`wid_e` is unambiguously a branch target** — the loop bottom does
`movei #wid_e,r22 / jump T,(r22)`. So the donor crossed a label that is the
destination of a backward branch.

For contrast, jopt gets the ordinary case right. This one is correct and should
keep working — donor sunk into a call's delay slot, no label crossed, and
`imul32` reads `r1`, which the delay slot sets before the branch takes effect:

```diff
 	move	r8,r0
-	move	r11,r1
 	movei	#vcf1,r28
 	movei	#imul32,r22
 	jump	T,(r22)
-	nop
+        move	r11,r1
```

## Suggested fix

When walking backwards for a donor, **stop at any label that is the target of a
branch** (in practice: any label referenced by a `movei #label,rN` or a `jr`
target). A donor above such a label is not execution-count-equivalent to the slot
below it, regardless of data-independence.

The existing three legality conditions (dominated by the jump, data-independent
of what it leapfrogs, flag-safe) are all about *data*; this is a *control-flow*
condition and appears to be missing. Note "dominated by the jump" is satisfied
here — the donor does dominate — which is exactly why it slipped through.
Dominance is the wrong test; the requirement is equal execution frequency
(post-dominance / same loop nest depth).

## Secondary issue: the certificate did not catch it

My fixture (whole-DRAM blob + real `PARAMS` + `capture 0x001CD4B0 76800`, the
framebuffer) certified all 56 transforms as equivalent, yet the real build is
visibly broken. So the fixture run cannot have exercised the affected code — most
likely the kernel does not reach the render under the fixture, and the capture is
identical because *nothing was written in either run*.

That is arguably the more dangerous finding: **an empty capture is
indistinguishable from a passing one.** Two suggestions:

1. **Report certification coverage** — e.g. "capture region unchanged during
   run" as a loud warning, and/or how many of the transformed instructions were
   actually retired during certification. A transform certified by a run that
   never executed it is not certified at all.
2. **Refuse to accept when the capture never changes**, or at minimum degrade to
   a warning that the fixture is not exercising the code.

I am happy to iterate on the fixture — it may well be mine that is wrong — but
jopt currently reports full confidence either way, which is what made this take a
while to spot.

## Repro

```
jopt gpu_geotex.gas --gpu --allow-input-hazards --fixture geotex.fixture -o out.gas
# 56 accepted; diff out.gas against the input and look at the wid_e / wid_m loops
```

Fixture and both kernel versions are in the OpenLara tree
(`gpu_geotex.gas.prejopt`, the fixture is reproduced in
`COBWEB_REQ_frame_time_measurement.md`'s sibling notes).

## Status

Reverted in OpenLara; the shipped kernel is unchanged at 3526 bytes. Happy to
re-run the moment the legality check lands — there are 104 wasted slots in this
kernel and 69 in `dsp_pose.das`, and the ordinary-case transforms above look
correct and useful (112 bytes saved, which also buys back GPU SRAM headroom).

---

## Response (cobweb `7336d6a`) — fixed. This was the serious one.

`8ca3fc0`. Two stacked mistakes on my side, and the second is the one worth
recording:

**1. Dominance was the wrong test.** I was checking that the donor dominates
the jump. It does — a loop initialiser dominates the loop's back-edge — and
sinking it into the delay slot is still wrong, because it then executes once
per *iteration* instead of once. The requirement is **equal execution
frequency**, not dominance.

**2. I documented the right condition and implemented it wrong.** The rule
"no label in (D, J]" was written correctly in the comments. But I checked the
*instruction* stream, and `wid_e:` is a label-only line that emits no
instruction — so it was invisible to the walk. The check now scans source
LINES, and donors must sit strictly below the nearest preceding label.

An optimiser that silently changes program meaning is the worst thing I can
ship, and this one shipped. What made it findable was that you sent a repro
rather than a symptom. The certificate should also have caught it and didn't —
that's the vacuous-capture hole covered in the sibling report, now failing
closed.
