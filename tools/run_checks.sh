#!/usr/bin/env bash
# Run every porydaw --*check harness against fresh scratch copies of a
# decomp project (the harnesses write into the project, so each one gets a
# brand-new copy). Point it at an ASAN build (-DPORYDAW_ASAN=ON) to turn
# silent memory bugs into aborts with stack traces.
#
# usage: tools/run_checks.sh <porydaw-binary> <decomp-checkout> [songsmk-fork]
#
#   <decomp-checkout>  a pokeemerald checkout; savecheck/onboardcheck/
#                      roundtrip also want tools/mid2agb/mid2agb built in it
#   [songsmk-fork]     a songs.mk-only (pre-midi.cfg) fork checkout for
#                      --mkcheck, which refuses midi.cfg projects by design;
#                      omitted -> mkcheck is skipped (and says so)
#
# env: PORYDAW_SAMPLE_CORPUS  a BUILT decomp tree (with generated sample
#      .bin files) for samplecheck's corpus pass; omitted -> self-contained
#      samplecheck only. ASAN_OPTIONS defaults to detect_leaks=0 (Qt's
#      process-lifetime allocations drown real leaks in noise).
set -u

usage() {
    echo "usage: tools/run_checks.sh <porydaw-binary> <decomp-checkout> [songsmk-fork]" >&2
    exit 2
}
[ $# -ge 2 ] || usage
BIN=$(readlink -f "$1")
SRC=$(readlink -f "$2")
FORK=""
[ $# -ge 3 ] && FORK=$(readlink -f "$3")
[ -x "$BIN" ] || { echo "run_checks: not executable: $BIN" >&2; exit 2; }
[ -f "$SRC/sound/song_table.inc" ] || {
    echo "run_checks: not a decomp project (no sound/song_table.inc): $SRC" >&2
    exit 2
}
if [ -n "$FORK" ] && [ ! -f "$FORK/sound/song_table.inc" ]; then
    echo "run_checks: fork is not a decomp project: $FORK" >&2
    exit 2
fi

export QT_QPA_PLATFORM=offscreen
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT
LOG="$TMPROOT/log"

copy_tree() { # src dst: the working tree minus .git
    mkdir -p "$2"
    local entry
    for entry in "$1"/* "$1"/.[!.]*; do
        [ -e "$entry" ] || continue
        [ "$(basename "$entry")" = ".git" ] && continue
        cp -r "$entry" "$2/"
    done
}

copy_tree "$SRC" "$TMPROOT/base"
[ -n "$FORK" ] && copy_tree "$FORK" "$TMPROOT/forkbase"

fails=()

report() { # name exit-status
    if [ "$2" -ne 0 ]; then
        fails+=("$1")
        echo "FAIL: $1 (exit $2)"
        tail -40 "$LOG"
    else
        echo "ok: $1 — $(tail -1 "$LOG")"
    fi
}

run() { # name base|- harness-args... (SCRATCH placeholder = fresh copy of base)
    local name="$1" base="$2"
    shift 2
    local scratch="$TMPROOT/scratch"
    rm -rf "$scratch"
    [ "$base" != "-" ] && cp -r "$TMPROOT/$base" "$scratch"
    local args=() a
    for a in "$@"; do
        [ "$a" = "SCRATCH" ] && a="$scratch"
        args+=("$a")
    done
    "$BIN" "${args[@]}" >"$LOG" 2>&1
    report "$name" $?
}

run roundtrip        base --roundtrip SCRATCH
run editcheck        base --editcheck SCRATCH
run viewcheck        base --viewcheck SCRATCH
run selftest         base --selftest SCRATCH mus_littleroot
run savecheck        base --savecheck SCRATCH mus_abandoned_ship
run onboardcheck     base --onboardcheck SCRATCH
run vgcheck          base --vgcheck SCRATCH mus_b_factory
run vgsavecheck      base --vgsavecheck SCRATCH mus_abandoned_ship
run exportcheck-loop base --exportcheck SCRATCH mus_abandoned_ship
run exportcheck-tail base --exportcheck SCRATCH mus_obtain_item
run sessioncheck     base --sessioncheck SCRATCH mus_abandoned_ship
run tabcheck         base --tabcheck SCRATCH mus_abandoned_ship mus_petalburg
run eventviewcheck   base --eventviewcheck SCRATCH
run rollcheck        base --rollcheck SCRATCH mus_abandoned_ship
run loopcheck        -    --loopcheck
run ignorecheck      -    --ignorecheck SCRATCH
run primecheck       -    --primecheck
run transportcheck   -    --transportcheck
run polycheck        -    --polycheck

# samplecheck builds its own fake projects and requires a scratch dir that
# does NOT exist yet.
rm -rf "$TMPROOT/scratch"
"$BIN" --samplecheck "$TMPROOT/scratch" \
    ${PORYDAW_SAMPLE_CORPUS:+"$PORYDAW_SAMPLE_CORPUS"} >"$LOG" 2>&1
report samplecheck $?

if [ -n "$FORK" ]; then
    run mkcheck forkbase --mkcheck SCRATCH mus_aqua_magma_hideout
else
    echo "skip: mkcheck (pass a songs.mk-only fork checkout as the 3rd argument)"
fi

echo
if [ ${#fails[@]} -ne 0 ]; then
    echo "run_checks: FAIL (${#fails[@]}): ${fails[*]}"
    exit 1
fi
echo "run_checks: PASS (all harnesses)"
