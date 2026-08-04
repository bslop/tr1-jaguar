# PSX_DYNAMIC_RE — a debugger for the TR1 PSX disc (note from jag_sotn, 2026-08-01)

**PCSX-Redux is built and headless-driveable** at
`~/Documents/Git/jag_sotn/tools/pcsx-redux/pcsx-redux` (from `grumpycoders/pcsx-redux`;
gitignored there, ~880 MB with submodules).

This matters to *this* project specifically because the asset pipeline in `tools/`
currently recovers PSX formats **by inference plus a validation check**, and this
turns that into **direct observation**.

```
pcsx-redux -no-ui -stdout \
    -bios <psx-bios> \
    -iso "../../../../tr1_psx/Tomb Raider (USA) (v1.6).cue" \
    -dofile script.lua -run
```

Lua API:
`PCSX.addBreakpoint(addr, 'Read'|'Write'|'Exec', width, cause, callback, label)`,
`PCSX.getRegisters()` (`.pc`, `.GPR`), `PCSX.getMemPtr()`.
Return `false` from the callback to keep running instead of pausing, so a whole
loop is captured rather than just its first hit. Worked example:
`~/Documents/Git/jag_sotn/tools/trace_tilemap.lua`.

## Why this is worth the detour here

Two of this project's hardest-won asset facts were reached by *inferring* a layout
and then finding a check that agreed with it:

- **The TR1 PSX animation frame is 40 words, not the PC's 39**, and the joint-angle
  decode needs a skipped leading word with `(w1,w0)` swapped. The TRosettaStone PC
  decode is simply **wrong** for PSX files. This was pinned by
  baked-frame-0-bbox ≡ stored-bbox (±2u).
- **`soundMap` was located by a validated scan** in `tr2jag_sound.py`.

Both are good work, and both are *inference validated after the fact*. A
read-watchpoint on the frame block, or on the sound map, shows **the real engine
decoding its own data** — the stride, the word order and the base address fall out
of the loop directly. Where a validation check tells you "this interpretation is
consistent", a watchpoint tells you "this is what the game actually does".

Concretely useful next targets:
- Watchpoint the anim frame data to confirm the 40-word stride and swap **from the
  consumer**, retiring the last inference in the skinning path.
- Watchpoint `soundMap` to replace the scan with an address read out of the code.
- For anything still unknown in the room/texture data, break on the **GPU primitive
  path** (`GP0`/`GP1` at `0x1F801810`/`0x1F801814`, or the DMA ordering table) and
  walk *backwards* to whatever feeds it — that finds a renderer directly instead of
  hunting for it through the data.

## Gotchas that cost real time (do not re-derive)

- ## ☠️☠️ CORRECTION (2026-08-01, jag_openlara): **YOU CANNOT RE-ARM `AfterPollingCleanup` FROM INSIDE THE HANDLER.**
  The advice below — repeated verbatim in `jag_sotn/CLAUDE.md` and
  `jag_bubsy3d/CLAUDE.md` — **does not work**, and following it reproduces the
  exact "hang" it claims to cure. Read `src/core/ui.cc:78-92` in full:
  ```cpp
  L.getfield("AfterPollingCleanup", LUA_GLOBALSINDEX);
  if (!L.isnil()) {
      try { L.pcall(); } catch (...) {}   // <- your handler runs, re-arms
      L.push();                            // <- push nil
      L.setfield("AfterPollingCleanup", LUA_GLOBALSINDEX);  // <- CLOBBERS IT
  }
  ```
  The nil is written **after** the call returns, unconditionally, so any re-arm
  the handler performed is overwritten. **MEASURED:** a minimal handler that
  re-arms as its very first statement and appends to a file still ran **exactly
  once** (heartbeat = 1). `PCSX.nextTick()` (`src/core/pcsxffi.lua:192`) only
  chains onto the same global and is clobbered identically.
  ⇒ `AfterPollingCleanup` is usable for **one** action at an unpredictable time,
  and nothing else. It is not a frame driver. `jag_sotn`'s
  `tilemap_trace.every()` / `.auto()` headless drivers cannot work as written;
  that project's real results came from breakpoint callbacks, which do persist.
- **Still open: what the repeating headless driver actually is.** Verified along
  the way: the CPU *is* executing (`PCSX.getRegisters().pc` read `0xBFC02B58`,
  inside the BIOS), so this is not a dead emulator. But a `Write` watchpoint on
  **GP1 `0x1F801814` never fires, with or without `-debugger`** — MMIO writes
  appear not to go through the breakpoint path. Untried and most promising:
  the **built-in web server** (`src/core/web-server.cc:692,706` already serve
  screenshots), which would let an outside process poll frames over HTTP and
  skip the Lua hook problem entirely.
- **`-debugger` exists and gates breakpoints** (`src/main/main.cc:259`). Without
  it `DebugSettings::Debug` is false. Enabling it did not rescue the MMIO
  watchpoint above, but any breakpoint work should pass it.
- **`-softgpu` is required for `PCSX.GPU.takeScreenShot()` headlessly.** The base
  class throws `"Not yet implemented"` (`src/core/gpu.h:170`); only
  `src/gpu/soft/interface.h:85` implements it. Worse, `UI::tick()` swallows the
  exception, so without `-softgpu` a screenshot silently kills the handler.
- **`print()` from inside a running-emulator callback is DISCARDED under
  `-no-ui`**: `PCSX::TUI::addLuaLog()` is an empty function
  (`src/main/textui.cc`). Only output emitted at `-dofile` time reaches stdout.
  Debug by writing to a FILE, or you cannot tell "never ran" from "said nothing".
- ☠️ **This project's `.cue` is unusable as shipped**: it references 37 track
  `.bin` files and only **2** exist, so PCSX-Redux reports *"cuesheet references
  a file that can't be found"* and every CDDA track comes up length 0. Write a
  one-track cue pointing at `Tomb Raider (USA) (v1.6) (Track 01).bin`
  (`MODE2/2352`) — that loads clean (`[+cue]`). No music, fine for visual work.
- **The original (now known to be incomplete) note:**
  **`AfterPollingCleanup` is a ONE-SHOT Lua hook.** `PCSX::UI::tick()`
  (`src/core/ui.cc:81-89`) calls it, then pushes nil back into the global. A handler
  that does not **re-arm itself** runs exactly once — indistinguishable from the
  emulator hanging. Two runs timed out with zero output before this was found by
  reading the source.
- **`-testmode` suppresses stdout.** Never pair it with `print()` debugging.
- **capstone's `disasm()` STOPS at the first undecodable byte.** PS1 overlays are
  mostly data; disassembling from offset 0 halts almost immediately and reports
  *zero* of everything — which reads as "no references exist" rather than "the tool
  did nothing". Drive from detected code regions and resume past bad words. This
  produced a confident, entirely false `0 references` result.
- **Find a binary's load base by POINTER FIT**, never assume it: for each candidate
  base, count how many `0x80xxxxxx` words land in-file, and take the winner. In
  jag_sotn this gave 96.2 % (stage overlays) and 94.1 % (main engine overlay), and
  it caught two bases that had been guessed wrong from byte statistics.
- **Byte statistics cannot tell MIPS code from packed data.** A bit-15 density
  heuristic put jag_sotn's code region in entirely the wrong place; only a
  decode-density pass corrected it.

## Reusable tooling

In `~/Documents/Git/jag_sotn/tools/`:
- `psx_disasm.py` — R3000A: `--info` (code/data map by decode density), `--xref`
  (reconstructs `lui`+`addiu`/`ori`/load-offset pairs to find what code touches what
  data), `--base`, `--disasm`, `--funcs`.
- `overlay_map.py` — pointer-table topology: finds tables by signature rather than
  by hardcoded offset.
- `room_table.py` — worked example of locating a per-stage structure whose **offset
  differs in every file**, by signature instead of address.

Full derivations with the evidence for each claim:
`~/Documents/Git/jag_sotn/docs/FORMATS.md`.

## Caveat, stated plainly

A watchpoint tells you what the game does *on the path you exercised*. It is
stronger evidence than inference, but it is not proof of the general format — a
different level or entity type can still take a different path. Treat a trace as a
strong lead to confirm across cases, not as a closed question.

> ### CORRECTION (2026-08-01, same day): the re-arm advice above is WRONG
> An earlier version of this note said to re-arm `AfterPollingCleanup` at the end
> of the handler. **That does not work.** Reading `PCSX::UI::tick()`
> (`src/core/ui.cc:81-89`) properly: it calls the hook and then pushes nil into
> the global **after the call returns**, so a self-re-arm inside the handler is
> unconditionally clobbered. The hook is strictly one-shot and **cannot be used
> for polling at all.**
>
> Compounding it: the `pcall` is wrapped in `catch (...) {}` and
> `PCSX::TUI::addLuaLog()` is an **empty function**, so under `-no-ui` a Lua error
> produces *no output whatsoever*. A script that errors looks exactly like a hang.
>
> **What to do instead:** do not poll. **Breakpoints persist by themselves** — arm
> them once at load time over the fixed address range you care about and let the
> game come to you. If you genuinely need periodic work, use an `Exec` breakpoint
> on a known per-frame function rather than the tick hook.

> ### CRITICAL (2026-08-01): breakpoints REQUIRE `-interpreter`
> PCSX-Redux defaults to `CPU type: Dynarec (x86-64)`, and **the dynarec does not
> check breakpoints**. They arm without error, report success, and then *never
> fire* — including an `Exec` breakpoint on the MIPS general exception vector
> (`0x80000080`), which executes on literally every interrupt.
>
> Combined with the silent-Lua-error problem above, this is the single most
> misleading failure mode in the tool: the script says "armed N watchpoints",
> the emulator runs happily for minutes, and you conclude *the data is never
> touched* — a strong, confident, completely false negative. It cost several
> multi-minute runs here and sent the investigation down two wrong paths.
>
> **Always pass `-interpreter` (and `-debugger`) for any breakpoint/watchpoint
> work.** Verify in the boot log: it must say `CPU type: Interpreted`. If it says
> Dynarec, your breakpoints are decorative.
>
> Also note: input IS scriptable even though PCSX-Redux exposes no pad API to
> Lua. `PCSX.getMemPtr()` gives arbitrary RAM access, and a PS1 game using the
> BIOS pad driver has its pad buffer written every frame (idle digital pad =
> `00 41 FF FF`: status, type, then buttons ACTIVE-LOW). Locate that buffer and
> overwrite the button word to inject input.
