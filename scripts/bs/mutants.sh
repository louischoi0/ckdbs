#!/bin/bash
# BS-S8: a mutation check on one subsystem, the suite's teeth rather than a
# gate (instructions/v3.0.0/workorder-bs-test-hardening.md). This file's
# mutants are assertions' and foreign keys'; the next milestone's subsystem
# replaces the list at the bottom.
#
#   scripts/bs/mutants.sh [rounds]     default 3 (BS_MUTANT_ROUNDS)
#
# Builds build-bs-noassert/ (scripts/bs/config.sh noassert build, which must
# have run once) and runs `rounds` times three ckdbs-sim seeds, whose
# workload writes p0/c0/a0 (BS-A12), and the suite's assertion and
# foreign-key cells: first on the unmutated tree, which must pass or no
# count means anything, then once per mutant, each changing one line of the
# engine. Under concurrency one run proves nothing, so each is repeated;
# the interleavings are the cells' own (AssertionRaceTest stages its holds
# on one thread and on two cores). The file is restored from a snapshot
# taken before the change - never from git, which would discard uncommitted
# work in it - also on an interrupt; the snapshot is kept beside the logs,
# so a run killed past its traps leaves it there and the next run refuses
# to start. The tree is rebuilt last; an interrupted run leaves the
# mutant's binaries until the next build.
# Prints one line per run: how many sim runs and cell rounds failed, which
# for a mutant are its kills. Never run by the push hook (§6.1).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROUNDS="${1:-${BS_MUTANT_ROUNDS:-3}}"
JOBS="${BS_JOBS:-$(nproc)}"
BUILD="$ROOT/build-bs-noassert"
CELLS='Assertion|ForeignKey|FkCrossCore|FkParent'
LOGS="$BUILD/mutants"
SNAPSHOT="$LOGS/snapshot"
case "$ROUNDS" in '' | *[!0-9]* | 0) echo "usage: mutants.sh [rounds >= 1]" >&2; exit 2 ;; esac
cd "$ROOT" || exit 2
[ -x "$BUILD/ckdbs-sim" ] || { echo "no $BUILD/ckdbs-sim: run scripts/bs/config.sh noassert build" >&2; exit 2; }
mkdir -p "$LOGS" || exit 2
if [ -e "$SNAPSHOT" ] || [ -e "$SNAPSHOT.target" ]; then
    echo "a previous run stopped with a mutant applied to $(cat "$SNAPSHOT.target" 2>/dev/null):" \
        "its original is $SNAPSHOT; restore it, then remove $SNAPSHOT and $SNAPSHOT.target" >&2
    exit 2
fi

TARGET=""
restore() {
    [ -n "$TARGET" ] || return 0
    local file=$TARGET
    TARGET=""
    if cp "$SNAPSHOT" "$file" && cmp -s "$SNAPSHOT" "$file"; then
        rm -f "$SNAPSHOT" "$SNAPSHOT.target"
    else
        # The snapshot stays: it is the only copy of the original.
        echo "RESTORE FAILED: $file; its original is $SNAPSHOT" >&2
        exit 3
    fi
}
trap restore EXIT
trap 'exit 130' INT TERM HUP

build() {
    cmake --build "$BUILD" -j"$JOBS" --target ckdbs-sim kds_tests > "$LOGS/$1-build.log" 2>&1
}

# rounds <name>: prints what failed; returns nonzero if anything did.
rounds() {
    local name=$1 sim=0 cells=0 round i seed rc
    for round in $(seq 1 "$ROUNDS"); do
        for i in 1 2 3; do
            seed=$((i + 10 * round))
            "$BUILD/ckdbs-sim" --seed "$seed" --ops 1500 --iterations 2 \
                --mode clean --profile colliding > "$LOGS/$name-sim-$seed.log" 2>&1
            rc=$?
            # 1 is a failure the run found; 2 is a usage error, the harness's own.
            [ "$rc" -eq 2 ] && { echo "$name: ckdbs-sim usage error ($LOGS/$name-sim-$seed.log)" >&2; exit 2; }
            [ "$rc" -eq 0 ] || sim=$((sim + 1))
        done
        # --no-tests=error: a broken registration is a failure, not a survivor.
        ctest --test-dir "$BUILD" -j"$JOBS" --no-tests=error -R "$CELLS" -LE bs-nightly \
            > "$LOGS/$name-cells-$round.log" 2>&1 || cells=$((cells + 1))
    done
    echo "$name: sim failed $sim/$((3 * ROUNDS)) runs; cells failed $cells/$ROUNDS rounds"
    [ "$sim" -eq 0 ] && [ "$cells" -eq 0 ]
}

# mutant <name> <file> <old line text> <new line text>: <old> must occur once.
mutant() {
    local name=$1 file=$2
    printf '%s\n' "$ROOT/$file" > "$SNAPSHOT.target" && cp "$file" "$SNAPSHOT" ||
        { echo "$name: no snapshot of $file; stopped" >&2; exit 2; }
    TARGET=$file
    if ! python3 -I - "$file" "$3" "$4" <<'EOF'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"{path}: the mutant's line occurs {text.count(old)} times")
open(path, "w").write(text.replace(old, new))
EOF
    then
        echo "$name: not applied"
    elif ! build "$name"; then
        echo "$name: build failed ($LOGS/$name-build.log)"
    else
        rounds "$name"
    fi
    restore
}

build baseline || { echo "the unmutated build failed ($LOGS/baseline-build.log)"; exit 1; }
rounds baseline || { echo "the unmutated tree fails: no mutant is run"; exit 1; }

mutant admit-everything src/exec/assertion_check.cpp \
    '        if (!admitted.value().admitted) {' '        if (false && !admitted.value().admitted) {'
mutant bound-off-by-one src/exec/bound_cabin.cpp \
    'result.admitted = next <= bound_;' 'result.admitted = next <= bound_ + 1;'
mutant holds-ignored src/exec/assertion_check.cpp \
    'held.key != key || held.value <= 0) {' 'held.key != key || held.value <= 0 || true) {'
mutant parent-delete-unchecked src/server/command_dispatcher.cpp \
    '                return Status::FkViolation("row id=" + std::to_string(parent_tuple_key) +' \
    '                if (false) return Status::FkViolation("row id=" + std::to_string(parent_tuple_key) +'

build restore || { echo "the restored build failed ($LOGS/restore-build.log)"; exit 1; }
