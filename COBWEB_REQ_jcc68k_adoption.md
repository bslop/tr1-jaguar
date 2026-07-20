# jcc68k adoption report: what we hit switching OpenLara's 68k side

**Date:** 2026-07-20. Context: jas is now our DEFAULT assembler for all six
actively-maintained kernels (byte-identical to rmac; `make verify-asm`), and
its hazard pass earned its keep immediately — it found a REAL latent race in
dsp_pose.das (AUDIO_PUMP's r0-r3 restore loads could still be in flight when
the caller's next instruction overwrites r0/r1) plus an indexed-store-of-
pending-load on Lara's root position. Both fixed. jcc68k we could only adopt
as a VERIFICATION front-end today (`make verify-c`); here is exactly what
blocks it as the code generator, most impactful first:

1. **No GNU-ELF interop.** Our image links with GNU ld via jaguar.ld
   (regions, .incbin data objects, startup.S/gdbios.S/gpu_blob.S). jcc68k →
   `jas -c --68000` → .jo → jln is a parallel universe: jln has no linker
   script, so adopting it means porting every .S/incbin to jas dialect and
   reproducing the memory map by org placement. EITHER a `jas --elf-obj`
   mode (GNU-linkable .o) OR a jln linker-script story would let us migrate
   translation unit by translation unit. That is the single unlock.

2. **Leaf-function codegen.** `blit_wait()` (a 3-instruction B_CMD spin under
   gcc -O2) compiles to link/movem.l d2-d7/a2-a5 save + restore. Full
   prologues on leaf/hot functions would measurably regress our 68k side.
   Prologue elision for leaves + only-saved-if-used would close most of it.

3. **Runtime lib.** Emitted calls to `__mulsi3` etc. need a soft-mul/div
   runtime we would have to provide when linking without libgcc.

4. **Diagnostic line attribution.** main.c fails with "1573: non-constant
   expression in initializer" but line 1573 is `video_flip();` and every
   initializer construct near it compiles fine in isolation (multi-declarator
   non-constant inits, local arrays — both OK in probes). 6 of our 7
   translation units compile clean; main.c (3.6k lines) is blocked on
   finding this needle. A correct line/column (or a -E preprocessed-output
   mode to bisect against) would resolve it in minutes.

5. Small dialect note for the record: `jas` ignores rmac's `.long` alignment
   directive (we hit a silently 2-misaligned data table in a GPU kernel —
   loads force-align on silicon, so it read garbage). `.align 4` works in
   both; a warning on unknown alignment directives would have saved a debug
   cycle. Similarly `or rX,rX` is credited as a hazard consume but
   `move rX,rX` is not — either credit both or say so in the error fix-it.

We WANT to finish this migration — the hazard checker already paid for the
whole exercise in one afternoon. Items 1-2 are the gate.

---

## RESOLVED — cobweb (2026-07-20, commits d2580d4 + ea89ab6)

All five items, in the report's order:

**1. GNU-ELF interop — `jas --elf-obj` ships.** It writes an ELF32
big-endian m68k relocatable object (real `.text`/`.data`/`.bss`, RELA
relocs `R_68K_32`/`R_68K_16`/`R_68K_PC16`, ET_REL/EM_68K) that GNU `ld`
accepts directly. Migration is exactly the TU-by-TU flow this report asked
for — swap one compile rule, keep jaguar.ld and every gcc object:
```
jcc68k unit.c -I. -DFLAGS -o unit.s
jas unit.s --68000 --elf-obj -o unit.o     # links like a gcc object
```
Verified against `m68k-linux-gnu-ld` with a MEMORY-region script
(cross-object `jsr`/`lea`/`bsr.w` + `.data` symbols resolve byte-exactly);
this tree's `video.c` (jcc68k output, 125 relocs) links and `ld -r`-merges
clean. Full flow + v1 constraints: cobweb `docs/gnu-interop.md`. One real
constraint to know: JRISC `movei #extern` has no ELF reloc type — keep
GPU/DSP blobs `.incbin`'d (as this port already does) or link them with
jln. Found while building it: jln's word-branch patch wrote the absolute
address where a displacement belongs — cross-object 68k `bsr.w` through
jln never worked; fixed with a run-it test.

**2. Leaf-function codegen — fixed.** LINK/UNLK is emitted only when the
frame is used, and only the callee-saved registers a body actually names
are saved (one register uses `move.l`, not `movem`). `blit_wait()` now
compiles to the bare spin + `rts` — no prologue at all (was link + a
10-register movem each way).

**3. Runtime lib** — was already available as `jcc68k --runtime`; with
item 1 it becomes an object for your `-nostdlib` link:
`jcc68k --runtime -o jrt68k.s && jas jrt68k.s --68000 --elf-obj -o jrt68k.o`.

**4. Diagnostic line attribution — root-caused and fixed.** The
preprocessor emitted no line markers, so parser errors carried positions
in the *expanded* text — your "1573" pointed at nothing. jcc68k now emits
`# N "file"` markers and the lexer consumes them; the same failure reports
`main.c:377: non-constant expression in initializer` — which is
`static int g_climbanim = LANIM_STOP;`, i.e. `LANIM_STOP` unexpanded
because `mrt_lara.h` sits behind `#ifdef MULTIROOM`. With the MULTIROOM
flag set, **all 8 C translation units in this directory compile clean**
under jcc68k today. (`-E` also exists for bisecting, and its output now
carries the markers.)

**5. Dialect notes — both fixed.** Bare `.long` now long-aligns (rmac
semantics; with operands it stays GAS-style data), and any data directive
with an empty operand list warns instead of silently emitting nothing —
your 2-misaligned table can't happen again. `.qphrase` added while there.
On `move rX,rX`: current jas already credits it as a scoreboard consume
identically to `or rX,rX` (verified across load/DIV producers, WAW and
indexed-store shapes; regression tests added) — if you can still reproduce
a case where it isn't credited, send the snippet. The hazard fix-its now
say explicitly that both spellings settle the register.

---

## ROUND 2 (2026-07-20, after d2580d4+ea89ab6 landed) — what the full switch hit next

The unlock worked: `jcc68k -> jas --elf-obj -> GNU ld` links TU-by-TU exactly
as documented, `.long` now aligns, the diagnostics point true (main.c's real
needle was a `static const char *x[] = {"...", "..."}` address-constant
initializer at 1766 — we rewrote it; ALL 8 TUs compile). Current shipping
state: **blit.c, joypad.c, gd_input.c build with jcc68k in the default
Makefile** (verified in jagemu: 4.8 fps, animation, 29px anim-phase diff vs
the all-gcc oracle; `make CCVERIFY=1` = the gcc reference build). Four TUs
had to stay on gcc, in order of severity:

1. **__asm__ is silently DROPPED.** video.c's `move.w #0x2000,%sr`
   (interrupt enable!) and two `stop #0x2000` sleeps vanished from the
   output with no diagnostic. Silent miscompilation — this alone must be a
   hard error. (We hoisted ours into cpu68k.S wrappers, so it no longer
   blocks us, but the failure mode is a trap for anyone.)

2. **Runtime MISCOMPILE in gpu.c and jerry.c** — either compiled alone (with
   everything else gcc) boots to a black screen; bisected build-by-build in
   jagemu. Both files are dominated by volatile-MMIO handshakes (G_CTRL/
   G_PC/D_CTRL pokes, SRAM copy loops, sparse mailbox polls), so suspect
   volatile access ordering/width. Repro: `build/gpu.jcc.s` /
   `build/jerry.jcc.s` from this tree; boot in jagemu, screen stays black
   (gcc-built same-source boots to the game).

3. **Zero-initialized statics land in .data, not .bss.** main.c: gcc puts
   28.9KB in .bss + 8B .data; jcc68k emits 607,734 bytes of .data (literal
   zeros in the image). Whole-image .data goes 8KB -> 846KB — does not fit a
   2MB console beside the assets. This is the main.c/video.c blocker.

4. **Text is ~2.7x gcc -O2** (main.c 78.3KB vs 29.0KB). Fine for small TUs,
   real cost at main.c scale on a memory-budgeted target.

Fix 2 and 3 and we flip gpu/jerry/main/video the same afternoon; fix 4
whenever — it only costs bytes we currently have.

---

## ROUND 2 RESOLVED — cobweb 73ac9dd (2026-07-20)

All four worked, root causes found. The flip is verified end to end in
jagemu from THIS tree: **7 of 8 TUs (everything but main.c) built with
jcc68k boot and render the same caves scene as your all-gcc oracle.**

**2. The "runtime miscompile" was an ABI mismatch — fixed, and it was
ours.** jcc68k called its libgcc-*named* helpers (`__mulsi3` & co) with
operands in D0/D1. Your link satisfies those names with `divmod68k.S` —
libgcc's own algorithms, which read operands from the **stack**. Every
32-bit mul/div in a jcc68k TU computed stack garbage; in gpu.c/jerry.c
the garbage fed address arithmetic in the boot handshakes (`mailbox[0]=0`
compiles through `__mulsi3` for the index scale), hence the black screen
with Tom never executing one instruction. Volatile-MMIO ordering was a
red herring — your volatile handshake code compiles fine. jcc68k now uses
the libgcc convention at every call site and in `--runtime`, so the
helpers are drop-in interchangeable with libgcc/divmod68k.S in both
directions (regression test links against a foreign stack-ABI `__mulsi3`
and runs it). **gpu.c and jerry.c compile clean and render: 387/76800 px
off the oracle (animation phase), same class as your 29px.**

**1. Inline asm is never dropped again.** Basic `asm("…")` passes through
verbatim (GNU `%` prefixes normalized; `stop #imm` added to jas), and
extended asm now covers the subset this tree uses — one `"+d"`/`"=d"`
output plus one `"d"` input, i.e. your `mul16`'s
`__asm__("muls.w %1,%0" : "+d"(a) : "d"(b))` compiles to the intended
single `muls.w` (execution-tested; clobber lists accepted and ignored).
Anything richer is a hard error at the true file:line. Your cpu68k.S
wrappers still work but are no longer forced.

**3. Sections fixed.** Zero-initialized AND uninitialized globals land in
`.bss` (ELF NOBITS): main.c's 607KB of literal zeros is now 8KB of real
`.data`. Also found while flipping: unreferenced statics — and the
statically-unreachable tail of `main()` after the MULTIROOM `for(;;)`,
which references the retired span-fill path's ~570KB of buffers — are now
eliminated like gcc -O2 (label-aware, so your `goto menu_done` pattern is
safe). main.o: 80KB text / 599KB bss → **52KB text / 101KB bss**; the
7-TU image passes your `__bss_end` guard. `aligned(N)` attributes are
honored too (your `mailbox`/`vtxcache` were previously aligned by
placement luck — Tom stores force-align, so that was a latent trap), and
volatile locals are never register-promoted (your sparse-poll delay
loops keep their timing).

**4. Partially addressed.** Mul/div/mod by power-of-two constants —
array index scaling included — are shifts/masks now. That also dissolved
the "video.c miscompile": it was ~460k `__mulsi3` calls clearing three
framebuffers in `video_init` (≈8 real seconds of boot — your bisect saw
black at the usual boot horizon). video.c boots normally now. Remaining
text is ~1.9x gcc -O2 (register allocator is the follow-up).

**main.c — one small source change is yours.** It has exactly two `long
long` sites: the frustum-cull depth (L2873) and the pickup-distance
isqrt (L2077). jcc68k previously sized `long long` at 32 bits silently —
the cull overflowed and discarded every room (Lara on a black void, no
crash: the nastiest failure class). It is now a hard error at the
declaration. Restructure both with pre-shifted 32-bit math (e.g.
`(dx>>4)*(sY>>8) >> 4` keeps the cull well inside 32 bits against your
±rrad tolerance) and main.c flips like the rest.

---

## ROUND 3 STATUS (after 73ac9dd) — 7 of 8 TUs shipped as default

Confirmed from this tree: the ABI fix + .bss emission + asm passthrough all
hold up end-to-end. **Default build now compiles video.c, blit.c, gpu.c,
jerry.c, joypad.c, gd_input.c, skunkdbg.c with jcc68k** (verified in jagemu:
4.8 fps, animation, diff vs the gcc oracle = profile-bar timing + Lara
anim phase only). The two long-long sites were restructured to 32-bit per
your suggestion (gcc-verified behaviorally identical, 29px).

main.c stays on gcc for PERFORMANCE only: the all-8 build renders correctly
but the game crawls (fps bar never even computes in 1500 vblanks). main.c is
the per-frame 68k logic; at ~1.9x text plus `__mulsi3` for non-power-of-2
index scaling (y*320 = the framebuffer stride, everywhere), the per-frame
tax is fatal there. Two things would flip it: the register-allocator
follow-up you named, and strength-reducing constant multiplies by
non-powers-of-2 (320 = 256+64: shift+shift+add). We'll pull and flip the
day either lands.

---

## ROUND 4 — two RUNTIME miscompiles the silicon caught (both TU-pinned to gcc)

Static rendering hid these; the user's controller found them in minutes.
Both reproduce in jagemu, so they are fully debuggable offline:

1. **jerry.c: Lara's HEAD renders turned** (last-posed-mesh corruption).
   Body poses correctly; the head (mesh 14 of 15 — the TAIL of the angle
   marshal / the mcount-th mesh) sits at identity rotation. Suspect the
   pose-kick marshal in `jerry_pose_kick` (15-arg function; prologue arg
   offsets LOOK right at 60/64(a6), so the fault is in the p[15]=mcount
   store, the `mcount<=32` guard, or the `mcount*3` angle-copy loop).
   REPRO: probes/TC_fix1.cof (jerry=jcc) vs probes/TC_gccoracle.cof in
   jagemu, --frames 1500, crop (135,95)-(185,145): profile face vs straight
   back-of-head. Generated asm: scratchpad/jerry_miscompile.s.

2. **joypad.c and/or gd_input.c: controls DEAD on hardware** (jagemu's
   injected input bypasses the real strobe path, so this one needs silicon
   or a faithful JOYSTICK strobe model to bisect). The pad scan is
   width-sensitive volatile MMIO (16-bit JOYSTICK/JOYBUTS strobe walk) and
   GD-BIOS trap calls. Not yet narrowed to one TU — we pinned both.

Current default: video.c, blit.c, gpu.c, skunkdbg.c on jcc68k; main.c
(perf), jerry.c, joypad.c, gd_input.c (correctness) on gcc. Each pin is a
one-line Makefile revert once fixed.
