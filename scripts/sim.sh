#!/bin/sh
# The simulation corpus (bench/workplan-teststrategy SIM07's corpus half).
#
#   scripts/sim.sh [fresh-seed-count]
#
# Two halves, and the pairing is the point:
#
#   (a) **every committed seed, forever** — tests/testdata/sim_seeds.txt is
#       regression-mandatory, and a seed leaves it only with the
#       justification it would take to delete a test;
#   (b) **N fresh seeds derived from today's date**, so the corpus explores
#       forward. Date-derived rather than random on purpose: a failure found
#       today is reproducible by anyone running the same day, and the seed
#       that found it goes into the corpus by hand.
#
# A failing run appends its seed and verdict to the artifacts file for
# triage and the script exits nonzero. Shrink one with:
#
#   build-release/ckdbs-sim --seed <n> --ops <n> --mode <m> --minimize \
#       --out case.sim && build-release/ckdbs-sim --replay case.sim
#
# `scripts/test.sh` stays the unit gate; this is the sweep beside it. The
# sanitizer matrix and the wall-clock smoke threshold are SIM13's and are
# deliberately not here.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Release by preference: Debug has reported the wrong sign twice (CLAUDE.md),
# and a sweep in Debug costs minutes it does not need to.
BIN="${SIM_BIN:-}"
if [ -z "$BIN" ]; then
    if [ -x "$ROOT/build-release/ckdbs-sim" ]; then
        BIN="$ROOT/build-release/ckdbs-sim"
    else
        BIN="$ROOT/build/ckdbs-sim"
    fi
fi
[ -x "$BIN" ] || { echo "sim.sh: no ckdbs-sim at $BIN; build it first" >&2; exit 2; }

FRESH="${1:-4}"
OPS="${SIM_OPS:-1500}"
ITERATIONS="${SIM_ITERATIONS:-2}"
ARTIFACTS="${SIM_ARTIFACTS:-$ROOT/sim-artifacts.txt}"

COMMITTED=$(grep -v '^#' "$ROOT/tests/testdata/sim_seeds.txt" | grep -v '^$')
TODAY=$(date +%Y%m%d)
FRESH_SEEDS=""
i=0
while [ "$i" -lt "$FRESH" ]; do
    FRESH_SEEDS="$FRESH_SEEDS $((TODAY * 1000 + i))"
    i=$((i + 1))
done

failures=0
runs=0

run_one() {
    # run_one <label> <args...>
    label="$1"
    shift
    runs=$((runs + 1))
    if out=$("$BIN" "$@" 2>&1); then
        echo "ok   $label"
    else
        failures=$((failures + 1))
        echo "FAIL $label"
        echo "$out" | sed 's/^/     /'
        {
            echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) $label"
            echo "$BIN $*"
            echo "$out"
            echo
        } >> "$ARTIFACTS"
    fi
}

for seed in $COMMITTED $FRESH_SEEDS; do
    for mode in clean sync-crash crash; do
        for faults in none io; do
            # All three value profiles, not just the default: `colliding`
            # (v over [0,4]) is what makes a FilterScan set interesting, and
            # it is where the Cabin finding in docs/inflight/known-gaps.md lives. A
            # sweep that ran `uniform` only would have missed it.
            for profile in uniform zipfian colliding; do
                run_one "seed=$seed mode=$mode faults=$faults profile=$profile" \
                    --seed "$seed" --ops "$OPS" --iterations "$ITERATIONS" \
                    --mode "$mode" --profile "$profile" --faults "$faults" --fault-rate 40
            done
        done
    done
    # The advisory-feature pairing (SIM06): one op stream, two instances
    # differing only in the three switches.
    run_one "seed=$seed pair" --seed "$seed" --ops "$OPS" --iterations 1 --pair
done

echo "sim: $runs run(s), $failures failure(s)"
if [ "$failures" -ne 0 ]; then
    echo "sim: failures appended to $ARTIFACTS"
    exit 1
fi
