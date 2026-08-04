# jsim: no GameDrive/SD BIOS — runtime-streamed assets can't load, so streamed audio is untestable offline

**Severity:** medium. Not a bug — jsim behaves correctly given what it models.
This is a **capability gap** that blocks a specific class of verification, and it
bit us today: an optimization touching the audio path could not be validated in
the emulator at all, forcing it onto a slow, flaky hardware link.

**Env:** cobweb `66cd9a2` · `sim/crates/jag-core/src/cart.rs`,
`sim/crates/jag-core/src/bios.rs`

## What's missing

`cart.rs` loads cartridge *formats* (ROM / COF / ABS / JAG / RAW) into memory.
`bios.rs` builds a minimal boot ROM and vector table. Neither provides a
**GameDrive / SD-card BIOS**, so a ROM that opens and reads files at runtime has
nothing to read from.

To be explicit about what is NOT broken: **jsim's audio capture path works
correctly.** `scheduler.rs:237` samples `L_I2S`/`R_I2S` at ~44.1 kHz and the wav
writer is fine. The silence we observed is the correct end state of a missing
input, not a defect in the audio backend.

## How it bit us

OpenLara streams its title music from the GameDrive SD (`MUSIC.PCM`) via GD BIOS
calls, mixed by a DSP mixer whose voices are fed from that stream. Under jsim the
stream never opens, so every voice stays idle, the mixer writes zeros to the DAC,
and `jagemu audio` faithfully captures **silence**.

We then made a change to the DSP audio pump (throttling its timebase poll to cut
bus traffic — see `COBWEB_GAP_tom_jerry_contention.md`). Audio corruption is the
single most likely way that change could go wrong. We captured audio from both
the baseline and modified ROMs:

```
base.wav    frames=294066 rate=44097 RMS=0.0 nonzero=0.0%
jpoll.wav   frames=294066 rate=44097 RMS=0.0 nonzero=0.0%
```

Both bit-identical silence — so the test **cannot distinguish a working build
from a completely broken one**. The riskiest part of the change is unverifiable
offline, and the only way to judge it is a human listening to real hardware.

## Request

A file-backed GD BIOS shim: attach a host directory or SD image
(`--sd <dir|img>`) and resolve the GameDrive BIOS entry points against it, so
`gd_fopen`/`gd_fread` return real data.

Implementation notes from this codebase's driver (`gdbios.c`, `gd_input.c`),
which may save you some reverse-engineering:

- **`gd_fread` returns 0 on SUCCESS — not a byte count.** This is genuinely
  counterintuitive and is documented in our project notes as a past source of
  bugs; a shim returning bytes-read will silently break callers.
- **There is no seek.** Looping a stream is done by *reopening* the file.
- Reads are issued in chunks from a resident streaming queue, so the shim needs
  to be re-entrant across frames rather than one-shot.

## Payoff

Any ROM that streams assets at runtime — audio, level data, FMV — becomes
testable offline. Concretely for this campaign it would let a regression check
assert "the music still decodes to non-silence with the expected RMS" instead of
delegating that to a person with a controller, which is the difference between a
check we run on every build and one we run when hardware happens to be working.

## Priority note

Lower priority than the contention gap. Contention blocks the core optimization
loop; this blocks one (important) correctness check. If you only take one, take
that one.

---

## COBWEB RESPONSE — accepted, and the fix is smaller than you asked for

Agreed this is a real capability gap, and your framing is right: jsim's audio
path is fine, the silence is the correct end state of a missing input. Since the
contention gap resolved to "do not model" (see that report), this is now the
active item rather than the lower-priority one.

### The architecture is different from the request — in our favour

You asked for a shim that resolves "the GameDrive BIOS entry points". Reading
`gdbios.S`, there are no BIOS entry points to resolve: the GameDrive is driven as
an **SPI device over Jerry registers**.

```
SPI_STATUS  $F16002      SPI_DATA  $F16004      SPI_DATAB  $F16005
FN_INIT 1   FN_CARDIN 9  FN_FOPEN 10  FN_FCLOSE 11  FN_FREAD 13  FN_FSIZE 16
```

`gd_install` does not install a vendor blob that we would have to emulate — the
driver *is* `gdbios.S`, in your ROM, speaking a packet protocol to hardware.

So the right fix in jsim is to **emulate the device, not shim the API**: intercept
`$F16002-$F16005` in the bus (the same way JERRY's joypad is already intercepted)
and implement the packet protocol against a host directory (`--sd <dir>`). Your
existing driver then drives it unmodified, which is both less work and a much
better test — it exercises your real SPI code path rather than bypassing it.

### Your two ABI traps stop being hazards

This is the nice consequence, and it is worth being explicit since you flagged
them as past sources of bugs:

- **`gd_fread` returning 0 on success, not a byte count** — that is the contract
  of *your* C wrapper over the SPI reply. We are not reimplementing that wrapper,
  so we cannot get it backwards. We implement `FN_FREAD`'s wire behaviour.
- **No seek; looping by reopening** — likewise a property of the driver/protocol,
  which is preserved automatically because the driver is unchanged. The device
  model just has to honour `FN_FOPEN` on an already-read file.

The one thing we *do* have to get right is re-entrancy: reads arrive in chunks
from your resident streaming queue across frames, so the device model holds
per-handle file state and position rather than servicing a request one-shot.

### Plan

1. GameDrive SPI device model in `jag-core` (bus interception + packet state
   machine for INIT / CARDIN / FOPEN / FSIZE / FREAD / FCLOSE).
2. `jagemu --sd <dir>` to attach a host directory; `gd_install` then succeeds and
   `MUSIC.PCM` opens.
3. Regression check of the kind you asked for: capture audio and assert
   non-silence with an expected RMS, so a broken audio path fails a build instead
   of needing a person with a controller.

We will point `--sd` at the existing `MUSIC.PCM` in the tree to prove the path
end-to-end rather than a synthetic file.
