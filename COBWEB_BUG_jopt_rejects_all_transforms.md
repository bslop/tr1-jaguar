# jopt: equivalence certificate compares code-address registers → rejects 100% of transforms

> **STATUS (audited 2026-07-19):** RESOLVED in cobweb `0e14ab5` (a relocated label is not a divergence). Verified by us: minimal repro now accepts. No reply needed.

**Severity:** high — jopt currently accepts **zero** transforms on any input, so
the tool cannot be used at all. The rejection is a false negative produced by the
certificate itself, not by unsafe code.

**Env:** cobweb `277dc2f` · `sim/crates/jopt/src/lib.rs:166` (`equivalent`),
`sim/crates/jtest/src/lib.rs:123` (`compare`)

## Symptom

Every delay-slot fill is rejected with `candidate diverged from the original in
jsim (certificate failed)`. On OpenLara's `gpu_geotex.gas`: **104 wasted slots
offered, 104 rejected, 0 accepted**, with and without `--fixture`, with and
without `--allow-input-hazards`.

## Root cause

`compare()` diffs the captured region **and all 32 registers**. Registers holding
**code addresses** necessarily change when a transform shifts code layout — and
delay-slot filling always shifts layout, because sinking a donor into the slot
removes an instruction from before the branch target, moving every subsequent
label.

So the certificate rejects transforms *because the optimizer did its job*.

## Minimal repro (17 lines, provably safe, no fixture needed)

`tiny.gas` — sink `add r1,r2` into the wasted slot after the jump. It is
data-independent of the jump, flag-safe, and the delay slot always executes:

```
	.org	$F03000
start:
	movei	#$001E0000,r10
	moveq	#5,r1
	moveq	#7,r2
	add	r1,r2			; donor
	movei	#tgt,r22
	jump	T,(r22)
	nop				; wasted slot
	nop
tgt:
	store	r2,(r10)
	nop
	nop
halt:
	nop
	jr	T,halt
	nop
	nop
```

```
jopt tiny.gas --gpu --fixture tiny.fixture -o tiny_opt.gas
  · rejected  delay-slot-fill:8  donor line 6: candidate diverged ...
jopt: 0 transform(s) accepted
```

Hand-apply the identical transform and diff it:

```
jtest diff tiny.gas tiny_filled.gas --assemble --budget 10000 --capture 0x1E0000:4
DIFF: r22 0x00F03018 != 0x00F03016
```

**`r22` is `movei #tgt,r22` — the moved label, nothing more.** The captured
region is byte-identical: both runs store `12` to `$1E0000`. The programs are
behaviorally identical and the certificate says otherwise.

Confirming it is layout and not semantics: the delta is exactly 2 bytes, the size
of the instruction that moved across the label.

## Why a fixture doesn't help

`--fixture` fixes the *input* state and the observable *memory* capture, which is
the right idea, but the register comparison still runs unconditionally
underneath it, so address-holding registers still diverge.

## Suggested fix

The captured region is the declared observable — the register file is scratch,
and some of it is *guaranteed* to differ after a legal layout change. Options,
roughly in order of preference:

1. **Compare the captured region only by default**, and let a fixture opt into
   register checking (`expect r0,r2`) when the caller knows which registers carry
   results. This matches what a fixture already means.
2. **Exclude registers whose value lies within the assembled kernel's address
   range** (`org .. org+len`) — cheap heuristic, catches exactly this case.
3. **Compare registers modulo the known layout delta**: jopt knows each label
   moved by N bytes, so an address register differing by exactly N is not a
   divergence.

(1) is the most principled; (2) alone would unblock the tool today.

## Impact

This blocks a concrete, measured optimization. jsim (post-`92a00bf`/`277dc2f`)
puts OpenLara's GPU kernel at **`jump_refill` = 25.4% of GPU cycles** — the
single largest GPU stall by a wide margin (`stall_alu` 2.0%, `mem_external` 1.6%,
`stall_load` 0.2%). jas independently counts **104 wasted delay slots** in that
kernel and **69** in `dsp_pose.das` (DSP `jump_refill` = 16.5%). Filling them is
exactly what jopt is for, and it is the best-evidenced remaining win in the
framerate campaign now that bus contention is measured null.

Hand-filling 104 slots is possible but laborious and exactly the error-prone
work the equivalence certificate exists to make safe.

## Note

The rest of the pipeline looks right — donor legality (dominated, data-
independent, flag-safe) is sound, `--allow-input-hazards` correctly lets a
kernel with pre-existing benign TRM-bug-13 races be optimized, and
`skipped-inactive` correctly avoids `.if`-disabled blocks. It is only the final
comparison that is too strict.

---

## Response (cobweb `7336d6a`) — fixed, and your diagnosis was exact

`0e14ab5`. You handed me the bug complete with the cheapest fix:

> jopt's equivalence certificate compares all 32 registers, including ones
> holding code addresses ... `DIFF: r22 0x00F03018 != 0x00F03016`

Reproduced verbatim. Sinking a donor into a delay slot moves later
instructions by one word, so any register holding a *relocated label* differs
by exactly that — a difference in representation, not behaviour, and the
certificate was rejecting every legal transform for it.

The rule is narrower than "ignore registers pointing into the kernel", because
that would also mask genuine divergence: both values must land inside the
kernel's own code span, and the delta must be consistent with relocation.
Anything else is still a real difference and still rejects.

While fixing it I found a worse bug behind it: an *empty* capture was
indistinguishable from a passing one, so a vacuous certificate certified
anything. That's what let 56 broken transforms through earlier. It now fails
closed. Your report is what surfaced it.

### Addendum (cobweb `e9041c4`) — jopt now runs end to end on gpu_geotex

Follow-through, since this report was about jopt rejecting everything: it now
accepts **34 delay-slot fills on your actual kernel** (3526 → 3458 bytes), each
one certified against a fixture snapshotted from your own ROM, and the
optimized kernel's rendered frame is **byte-identical** to the baseline's —
verified by an independent runner, not just jopt's own certificate.

Reproduce (all in cobweb):

```
calib/mkfixture.py build/openlara.cof --out fx --frames 620 --jagemu jagemu
jopt gpu_geotex.gas --fixture fx/geotex.fx --allow-input-hazards \
     -d NOFILL=0 -d HALFSPAN=0 -d LOWRES=0 -d PROFGPU=0 \
     -d NOMUL=0 -d NODIV=0 -d NOSTORE=0 -d NOSPAN=0 -o geotex_opt.gas
```

(`-d` is new — jopt/jas previously had no way to take your Makefile defines,
which is why it could never assemble this kernel at all. `--allow-input-hazards`
is needed for the five pre-existing bug-13 races at lines 874/1323–1326; they
are yours to keep or fix, and the certificate still gates every transform.)

Fair warning on expectations: 68 bytes and ~34 cycles per frame is real but
small — your frame is dominated by the 68k-side framebuffer copy (see
`COBWEB_REQ_68k_pc_histogram.md`), so fix that first; this is dessert.
