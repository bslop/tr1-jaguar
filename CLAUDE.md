# jag_openlara — Agent Instructions

Tomb Raider 1 on the Atari Jaguar — the real first level on real hardware.
[`README.md`](README.md) is what it is; [`ARCHITECTURE.md`](ARCHITECTURE.md) is
how it is built; [`PROGRESS.md`](PROGRESS.md) is the milestone ledger.

Process is `~/Documents/Git/jaguar-shared/DEVELOPMENT.md`; the hardware and
silicon knowledge is `~/Documents/Git/jaguar-shared/CLAUDE.md` and its symptom
router. **Do not re-derive what is written there** — most of it cost a real
debug session. Arrive at `JAGUAR_PORTING_NOTES.md` with a *symptom*, via its
"Find it by symptom" table.

## ⭐ Write findings BACK — near the top on purpose

Anything another project would want — a silicon fact, a toolchain trap, a
measured cost — goes into `~/Documents/Git/jaguar-shared`, **committed and
pushed by you**, not held for a relay. An audit on 2026-09-04 found that the
projects whose write-back rule was buried contributed nothing to the shared
brain, and the ones where it sat near the top contributed 6–8 commits in a day.
Same intent, different position on the page.

⭐ **This project has the largest unbanked debt of any of them.**
[`JAGUAR_FINDINGS.md`](JAGUAR_FINDINGS.md) is 59 KB of "things that cost a day
each to learn" and it is local to this repo. It is a shipped, hardware-verified
engine, so its findings are `[HW]` — the strongest tag there is. When you touch
a section of it, carry that section across.

## Loop state — this project already had one, and it stays

⚠ **[`AUTORUN_STATE.md`](AUTORUN_STATE.md) is the loop state. Do not replace it
with a block in this file.** It predates the shared structure, it is 43 KB of
real history, and `tools/session_run.sh end` **refuses to advance the run
counter** if it was not touched — which is a stronger guarantee than anything a
convention can offer. It is the same idea, enforced better.

    RUN COUNTER, MILESTONE, IN FLIGHT, NEXT   →  AUTORUN_STATE.md
    every run: tools/session_run.sh start → work → session_run.sh end "<summary>"
    RIG JOB:   record the job NUMBER in AUTORUN_STATE.md if one is queued when
               you stop — the capture outlives the session and an unread one is
               a wasted lease.

## Where this lives

Branch **`main`** on [`bslop/tr1-jaguar`](https://github.com/bslop/tr1-jaguar).

☠ **The origin push IS the gated release. The user calls that moment.** Commit
freely; never push to origin unless the user has just said to, in this session.

## The devkit

**Cobweb (jas/jsim/jagemu)** — one checkout per project, in `cobweb/`. Pull it
at the start of every run, before any measurement you will record, after any
long block, and after every commit; then rebuild and re-measure your baseline.
It moves fast, and a number measured against a stale devkit is not a number.

## Standing rules for every run here

1. **Work in runs of 50**, then stop and report. Only a question for the user
   ends the loop early — needing the hardware does not, you queue it with `jagq`
   and carry on.
2. **Commit every run.** The loop's memory is on disk; uncommitted disk is one
   `rm` from gone.
3. ☠ **Never commit game-derived material.** Our `.md`, `.c`, `.h` and `.s`
   ship; anything extracted from the original disc does not. `.gitignore`
   enforces it.
4. **[`OPEN_ISSUES.md`](OPEN_ISSUES.md) is authoritative** for what is broken,
   and it records what has been *refuted* as carefully as what is open. Read the
   refutations before proposing a theory — several of them were expensive, and
   three separate metrics have already failed on A1 alone by measuring the wrong
   quantity.
5. **Silicon is the oracle.** This project has 8+ cases of "jagemu passes,
   silicon dies". A jagemu result is a lead; a capture from the rig is a fact.
