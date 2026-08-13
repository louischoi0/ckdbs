#!/usr/bin/env bash
# The server-CPU cell (re-run: the first attempt met a build in another
# worktree) and the mount matrix.
#
# Mount cells are (side) x (rows) x (assertion shape). Every cell gets its own
# label and therefore its own data directory, which is not optional here: the
# base binary refuses a segment the head binary wrote, because 67ce947 raised
# kMinReadableSegmentFormatVersion.
set -euo pipefail
W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2
OUT=/home/cdkbs/abbench/mounts
mkdir -p "$OUT"

"$W/bench/wait_quiet.sh"
echo "=== server CPU cell, K=16 (re-run) ==="
"$W/bench/run_abort_sweep.sh" /home/cdkbs/abbench/cpucell2 relaxed 1000 16 \
    --txns 200 --cpu-rounds 3 --cpu-txns 6000 \
    > /home/cdkbs/abbench/cpucell2.log 2>&1 || echo "FAILED cpucell2"

mount_cell() {  # side binary rows shape extra...
    local side="$1" bin="$2" rows="$3" shape="$4"; shift 4
    local label="$side-r$rows-$shape"
    "$W/bench/wait_quiet.sh" > /dev/null
    python3 "$W/tools/mount_cost_benchmark.py" \
        --binary "$bin" --label "$label" --mounts 9 --rows "$rows" \
        --port 15491 --json "$OUT/$label.json" "$@" \
        > "$OUT/$label.txt" 2>&1 || echo "FAILED $label"
    echo "  done $label"
}

for rows in 200 1000 10000; do
    for side in head base; do
        bin="$W/build-rel-$side/kds_server"
        echo "=== mounts $side rows=$rows ==="
        mount_cell "$side" "$bin" "$rows" noassert
        mount_cell "$side" "$bin" "$rows" assert   --assertion
        mount_cell "$side" "$bin" "$rows" assertrb --assertion --assert-rollback-every 4
    done
done
echo ALLDONE
