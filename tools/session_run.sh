#!/usr/bin/env bash
# session_run.sh — the per-run ritual for jag_openlara.
#
#   tools/session_run.sh start    peek at the world before doing work
#   tools/session_run.sh end "<one-line summary of what this run did>"
#                               commit + push + advance the run counter
#   tools/session_run.sh status   where am I, is a report due
#
# WHY THIS EXISTS (user, 2026-08-16): "Create a way to continue running
# immediately after the context is complete. Then, every 25 runs, you show me
# exactly where you're at with your progress... Each run, you commit and push
# to your repo."
#
# The hard part is not the counter, it is that a context ends without warning.
# Anything a future run needs must be ON DISK, not in the conversation. So
# `end` refuses to complete quietly if AUTORUN_STATE.md was not touched: the
# handoff note IS the deliverable, and a run that advanced the counter without
# updating it has lost whatever it learned.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
STATE="$HERE/AUTORUN_STATE.md"
SHARED="${SHARED:-/home/jvilla/Documents/Git/jaguar-shared}"
REPORT_EVERY=25

# ☠️ PUSH TO `wip`, NOT `origin`. origin is bslop/tr1-jaguar and it is PUBLIC;
# pushing there IS the release, which the user has explicitly gated on his own
# sign-off and wants coordinated with a video. `wip` is bslop/jag_openlara,
# private, and is what "commit and push to your repo" means here.
PUSH_REMOTE="${PUSH_REMOTE:-wip}"

runno() { grep -m1 '^RUN:' "$STATE" 2>/dev/null | awk '{print $2}' | tr -cd '0-9'; }

case "${1:-status}" in

start)
    echo "=== jag_openlara run $(runno) ==="
    # 1. the oracle. Standing order: cobweb updates change what is POSSIBLE
    #    here, not just what is buggy, so a stale copy means routing around a
    #    limitation that no longer exists.
    "$HERE/tools/cobweb_check.sh" 2>&1 | sed 's/^/  /'
    # 2. the shared notes. Five sessions write hardware facts into this repo;
    #    reading it is how we avoid re-measuring what someone already paid for.
    if git -C "$SHARED" fetch -q origin 2>/dev/null; then
        n=$(git -C "$SHARED" rev-list --count HEAD..origin/main 2>/dev/null || echo 0)
        if [ "${n:-0}" != "0" ]; then
            echo "  jaguar-shared: $n NEW commit(s) — read before assuming anything"
            git -C "$SHARED" log --oneline --no-decorate HEAD..origin/main | head -12 | sed 's/^/    /'
        else
            echo "  jaguar-shared: up to date"
        fi
    else
        echo "  jaguar-shared: fetch failed (offline?)"
    fi
    # 3. who holds the rig. Cheap, read-only, and stops us planning a hardware
    #    step that is not ours to take.
    [ -f "$SHARED/hw/jaghw" ] && "$SHARED/hw/jaghw" status 2>/dev/null | head -4 | sed 's/^/  /'
    echo "  --- next step, from AUTORUN_STATE.md ---"
    sed -n '/^## NEXT STEP/,/^## /p' "$STATE" 2>/dev/null | head -14 | sed 's/^/  /'
    ;;

end)
    SUMMARY="${2:?usage: session_run.sh end \"<what this run did>\"}"
    N=$(runno); N=${N:-0}

    # ☠️ THE HANDOFF NOTE IS THE DELIVERABLE. A run that advanced the counter
    # without updating AUTORUN_STATE.md has thrown away everything it learned
    # the moment the context ends.
    #
    # ☠️ COMPARE CONTENT, NOT TREE STATE. The first version tested `git status
    # --porcelain -- AUTORUN_STATE.md` and refused a run that HAD updated the
    # file but had already committed it mid-run - the tree was clean, so the
    # check saw "untouched". A guard that misfires on correct behaviour gets
    # switched off, which is worse than no guard. Hash the file against the
    # value stamped at the end of the previous run instead: exact, and immune
    # to when (or whether) the change was committed.
    STAMP="$HERE/tools/.lastrun"
    NOWHASH=$(sha256sum "$STATE" 2>/dev/null | cut -d" " -f1)
    if [ -f "$STAMP" ] && [ "$NOWHASH" = "$(cat "$STAMP" 2>/dev/null)" ]; then
        echo "☠️ REFUSING: AUTORUN_STATE.md is byte-identical to last run."
        echo "   The next context starts from that file and nothing else."
        echo "   Update 'NEXT STEP' and 'WHAT CHANGED', then re-run."
        exit 1
    fi

    NEXT=$((N + 1))
    sed -i "s/^RUN: .*/RUN: $NEXT/" "$STATE"
    sha256sum "$STATE" | cut -d" " -f1 > "$HERE/tools/.lastrun"

    # ☠️ NEVER COMMIT WHILE AN ASSET BUILD IS IN FLIGHT.
    # `git add -u` stages every tracked file, and tools/build_cof.sh REGENERATES
    # tracked ones (CORE.JV, EIDOS.JV, sound.h, pass2.h, the mrt_* headers).
    # Closing a run mid-build captures a half-written asset set that still looks
    # like a clean commit. Hit for real in run 9, with three of four videos
    # converted. Wait for it, then close.
    # ☠️ Check the PID LOCK, not `pgrep -f build_cof.sh`: that string matches
    # any shell WAITING on the build, and the checking command itself, so the
    # first version blocked a clean commit by detecting itself.
    BCLOCK=/tmp/.build_cof.lock
    if [ -f "$BCLOCK" ] && kill -0 "$(cat "$BCLOCK" 2>/dev/null)" 2>/dev/null; then
        echo "☠️ REFUSING: tools/build_cof.sh is still running."
        echo "   It rewrites tracked assets; committing now would capture a"
        echo "   partially regenerated tree. Wait for it to finish, then re-run."
        exit 1
    fi

    # Commit BY PATH. Never `git add -A` — this tree carries disc-derived
    # assets that must not be committed (see .gitignore) and generated headers.
    git -C "$HERE" add -u 2>/dev/null
    git -C "$HERE" add AUTORUN_STATE.md tools/.lastrun 2>/dev/null
    # ☠️ `git add -u` ONLY STAGES FILES GIT ALREADY KNOWS. Every instrument this
    # campaign was built on — conformance.py, build_conf.sh, floor_coverage.py,
    # room_black.py and the recorded baselines — was written by a run, staged by
    # nothing, and sat UNTRACKED for six runs. The state file survives a context
    # ending; the tools it tells the next run to use were backed up nowhere.
    # Pick up new tools/ files by name, still never `add -A`.
    git -C "$HERE" add tools/*.py tools/*.sh 2>/dev/null
    git -C "$HERE" add tools/.black_* tools/.toolchain_baseline 2>/dev/null
    # Say plainly if anything is still untracked, rather than passing silently.
    UNTR=$(git -C "$HERE" ls-files --others --exclude-standard | head -8)
    if [ -n "$UNTR" ]; then
        echo "note: still untracked (add by name if it is work, .gitignore if not):"
        echo "$UNTR" | sed 's/^/    /'
    fi
    if git -C "$HERE" diff --cached --quiet; then
        echo "run $N: nothing to commit"
    else
        git -C "$HERE" commit -q -m "run $N: $SUMMARY" && echo "run $N committed"
    fi
    BR=$(git -C "$HERE" branch --show-current)
    if git -C "$HERE" push -q "$PUSH_REMOTE" "$BR" 2>&1; then
        echo "run $N pushed to $PUSH_REMOTE/$BR"
    else
        echo "☠️ push to $PUSH_REMOTE failed — work is committed locally, not backed up"
    fi

    if [ $((NEXT % REPORT_EVERY)) -eq 0 ]; then
    # ⭐ The 25-run STOP is the user's rule, and hw/loopmode is the user's
    # switch for suspending it (jaguar-shared DEVELOPMENT.md §1). Ask it
    # rather than hardcoding the answer: it expires by itself and fails
    # safe to STOP, so a missing or stale flag prints the banner below.
    LOOPMODE_BIN="${JAGUAR_SHARED:-$HOME/Documents/Git/jaguar-shared}/hw/loopmode"
        if [ -x "$LOOPMODE_BIN" ]; then
            "$LOOPMODE_BIN" banner "$NEXT"
        else
        echo
        echo "════════════════════════════════════════════════════════════"
        echo "  RUN $NEXT — PROGRESS REPORT IS DUE TO THE USER."
        echo "  He decides whether the direction is still valid; that is the"
        echo "  point of the checkpoint, so do not summarise it away."
        echo "  Show: what shipped, what is blocked, what the next 25 buys."
        echo "════════════════════════════════════════════════════════════"
        fi
    fi
    ;;

status)
    N=$(runno); N=${N:-0}
    echo "run $N; next report at $(( (N/REPORT_EVERY + 1) * REPORT_EVERY ))"
    git -C "$HERE" log --oneline -3 | sed 's/^/  /'
    ;;

*) echo "usage: session_run.sh {start|end \"<summary>\"|status}"; exit 2 ;;
esac
