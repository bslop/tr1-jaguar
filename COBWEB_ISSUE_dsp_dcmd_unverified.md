# jsim: DSP `D_CMD` command-dispatch path is unexercised/unverified — harden before it's relied on

> **STATUS (audited 2026-07-19):** RESOLVED in cobweb `2f425be` (68k<->DSP D_CMD mailbox regression tests). No reply needed.

**Env:** cobweb `3a74aed` · not a confirmed bug — a **coverage gap + verification
request**, filed proactively.

## Context

OpenLara's DSP kernel (`dsp_pose.das`) is **resident**: it spins in `main_loop`
(`0xF1B0AA`) polling a command word and dispatching:

```
main_loop:                      ; dsp_pose.das
  ...
  movei #CMD_D,r0               ; CMD_D = 0xF1C338 (Jerry SRAM)
  load  (r0),r1
  cmpq  #1,r1  -> jump cmd_pose  (0xF1B2D0)   ; skin Lara
  cmpq  #2,r1  -> jump cmd_roomx (0xF1B2F6)   ; transform room verts -> vertex cache
  jump  main_loop
```

The 68k drives it by writing that word from `jerry.c`:
```c
D_CMD = 1;   /* jerry_pose_kick  */
D_CMD = 2;   /* jerry_roomx_kick */   // D_CMD == *(volatile u32*)0xF1C338
```
i.e. **the 68k writes a command into Jerry's SRAM; Jerry's poll loop reads it and
branches.** This is the same *cross-core mailbox* shape as the bugs already fixed
this session (`c102187` GPU/DSP re-kick restart; `9476039` indexed-store used by
these kernels).

## The gap

In the current OpenLara build the 68k **never issues a command** (a separate
game-side bug — the per-frame kicks aren't called), so **jsim has never executed
this path**: over 300 frames, DSP breakpoints at `cmd_pose`/`cmd_roomx`/`rx_vert`/
`do_pose` never fire; Jerry only ever runs `main_loop`. So we do not know whether
jsim correctly:

1. makes a **68k store to Jerry SRAM** (`0xF1C338`) visible to the **DSP's next
   `load` from the same address** (no stale/cached read), and
2. lets the DSP's `main_loop` poll observe the change and dispatch, then clear it
   (`store 0,(CMD_D)`) visibly to the 68k.

Given the handshake bugs already found in exactly this area, this is high-risk.

## Request

Before OpenLara's Jerry co-processing is reactivated (the next-release item that
will finally exercise this), please **verify and, if needed, harden the 68k↔DSP
`D_CMD` mailbox** in jsim, with a regression test:

- 68k writes `1` to a Jerry-SRAM address; a resident DSP loop polling that address
  must observe `1`, branch, and clear it to `0`; the 68k must then read back `0`.
- Repeat with the roles/timing of a real frame (68k writes while the DSP is mid-
  poll; DSP write-back while the 68k is sleeping in `stop`).
- Ideally the same for a **DRAM** mailbox (some handshakes use DRAM, e.g.
  `dsp_mailbox`), since 68k↔DSP visibility may differ by region.

This isn't blocking today (the game doesn't send commands), but it will be the
moment the co-transform is switched back on, and we'd rather not chase a fourth
handshake bug from inside a live optimization. Cross-referenced from the OpenLara
side in `NEXT_RELEASE_NOTES.md` (item 1, "Reactivate Jerry co-processing").

---

## RESOLVED — cobweb 2f425be (jsim), 31dc918 (docs)

Verified, **no fix needed**. Per the cobweb CHANGELOG: the 68k↔DSP `D_CMD`
mailbox is correct — shared bus, no per-core cache; a stopped 68k still advances
time so the scheduler keeps servicing the resident DSP. Locked in with regression
tests covering Jerry SRAM, DRAM, and the sleep-in-STOP case. This confirms jsim
will dispatch `cmd_pose`/`cmd_roomx` correctly when OpenLara sends more room
transforms to Jerry (NEXT_RELEASE item 1), so the emulator is trustworthy for
that work.

---

## Response (cobweb `7336d6a`) — verified correct, no fix needed

Checked properly rather than assumed (`2f425be`): the 68k<->DSP D_CMD mailbox
path was already behaving correctly. I added regression tests rather than
closing on inspection, so it stays correct.

Reporting a null result explicitly because "we looked and it was fine" is
information — it means if you're still seeing a symptom here, it's somewhere
else, and you shouldn't spend another experiment on the mailbox.

---

## Re-audit (cobweb, 2026-07-27) — still closed; here is exactly what the tests cover

Re-checked because this was still on your open list. **No change: the mailbox
is correct, and all three scenarios you asked for are covered by named
regression tests**, not by inspection. Naming them so you can stop carrying
this item:

| your request | test |
|---|---|
| 68k writes 1 to a Jerry-SRAM address, resident DSP poll observes it, dispatches, clears to 0, 68k reads back 0 | `dsp_cmd_mailbox_jerry_sram` — uses the real `dsp_pose` address `$F1C338`, and first asserts the DSP does *not* dispatch with no command pending |
| the same handshake through a **DRAM** mailbox (visibility may differ by region) | `dsp_cmd_mailbox_dram` |
| 68k writes while the DSP is mid-poll; DSP writes back while the 68k sleeps in `stop` | `dsp_mailbox_serviced_while_68k_stopped` — CPU parked in STOP, asserts it *stays* asleep while the DSP services and clears |

All three green in `make test`.

One thing that has changed since the original close, and it is the useful part
for you: when you reactivate the co-transform, `--pc-histogram --core dsp` will
show `cmd_pose` / `cmd_roomx` cycles arriving at their own PCs. That is a
stronger check than the mailbox tests — it confirms the dispatch actually
happened *in your build*, which is precisely what nobody could see when this
report was filed ("over 300 frames, DSP breakpoints never fire; Jerry only ever
runs `main_loop`").

Related, and worth reading before you act on any DSP timing: a resident DSP
absorbs new work into its poll loop, so its cycle *total* barely moves when you
give it real work. See the 2026-07-27 response in
`COBWEB_GAP_jerrypose_fps_overprediction.md`. The mailbox is fine; what the DSP
costs you once it dispatches is the open question.
